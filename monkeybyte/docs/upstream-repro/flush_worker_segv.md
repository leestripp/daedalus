# daedalus bug report: flush worker SIGSEGV in Messages::save_impl (still present on main)

**Date:** 2026-09-02
**daedalus version:** main @ a671a4e (post-1.0.2)
**Host:** MonkeyByte (GTK4 AI code editor, daedalus via CMake FetchContent)
**Platform:** Ubuntu 26.04 x86_64, gcc 15, nlohmann_json 3.11.3, libstdc++

## Symptom

The host process dies with SIGSEGV on the daedalus Messages background
flush worker thread. Caught under gdb (full thread backtrace:
`flush_worker_backtrace.txt` in this directory):

```
Thread "monkeybyte" received signal SIGSEGV, Segmentation fault.
#0  std::_Rb_tree<string, pair<const string, nlohmann::json>, ...>::_M_root
    (this=0x0) at /usr/include/c++/15/bits/stl_tree.h:1358
... (nlohmann::json deep-copy frames: map copy ctor → json copy ctor) ...
#18 Messages::save_impl        Messages.cpp:855
#19 Messages::save             Messages.cpp:841
#20 Messages::flush_if_dirty   Messages.cpp:1146
#21 Messages::flush_worker_main Messages.cpp:1187
#22 Messages::Messages::<lambda()> Messages.cpp:375  (worker thread spawn)
```

## Root-cause analysis

`Messages::save_impl()` line 855 is:

```cpp
std::lock_guard<std::mutex> lk(messages_mutex_);
snapshot = messages_;              // <-- crashes HERE
dirty_.store(false, std::memory_order_release);
```

The deep copy of the chain dereferences a `std::map<string, json>` node
with a **null internal pointer** (`_M_root(this=0x0)`): one json value
inside `messages_` is moved-from or half-mutated.

The snapshot IS taken under `messages_mutex_`, so either:

1. some other code path mutates `messages_` (or a json already inside it)
   **without holding `messages_mutex_`**, racing the flush worker; or
2. a **moved-from json** gets appended to the chain and stays there
   (suspects: stream-recovery / synthesized-message / orphan tool_call
   cleanup paths — the host's session logs regularly show
   `[Messages] Removing orphan tool_call (no result): ...` entries).

The on-disk `messages.json` parses cleanly after the crash (191 entries),
so the corruption is created **in memory at runtime**, not loaded.

## Why the 1.0.2 fix did not cover this

1.0.2 (SANITIZE-NOTHROW / CRASH-6) wrapped `flush_if_dirty()` calls in
the worker with try/catch. A null-pointer dereference raises SIGSEGV —
**it is not a C++ exception and cannot be caught by try/catch**. The
worker still takes the whole host process down.

## Repro

Intermittent under real use with a long chain (~200 messages). Observed
twice within ~30 min of agent chat in the host app (each crash on an
auto-flush tick). No deterministic trigger identified; a chain that has
lived through interrupted streams / orphan tool_call cleanup seems to be
the precondition.

## Suggested fixes

1. **Audit every write to `messages_` for `messages_mutex_` coverage** —
   especially the stream-recovery, synthesized-message, and orphan-cleanup
   paths.
2. **Never leave a moved-from json in the chain**: after any `std::move`
   of a chain element, replace/reset the slot before releasing the lock.
3. **Defensive snapshot**: validate elements during the copy
   (skip + LOG_CERR entries whose object/array pointer is null) so a bad
   entry degrades to one dropped message instead of a host-killing SEGV.
4. **Host kill switch**: expose `MessageCompressionConfig::auto_flush_enabled`
   through the public API (it currently lives in the internal
   `src/core/Messages.h`, unreachable from `include/daedalus/`) so hosts
   can disable the background worker until they trust a fixed build.

## Host workaround

None — the worker thread is internal and cannot be disabled via public
API. On crash only the in-flight turn is lost; the persisted chain
survives.
