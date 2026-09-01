# daedalus bug reports

Bugs found in daedalus while running MonkeyByte (GTK4 host). daedalus is
upstream code — these are reports for upstream, NOT local patches.
MonkeyByte consumes daedalus via FetchContent tracking `main`.

**Reported against:** daedalus `main` @ a671a4e (post-1.0.2)
**Platform:** Ubuntu 26.04 x86_64, gcc 15, nlohmann_json 3.11.3, libstdc++

---

## BUG 1 — SIGSEGV: flush worker still crashes deep-copying `messages_` (1.0.2 fix incomplete)

**Severity:** crash — takes down the entire host process
**Caught:** 2026-09-02, live under gdb (full thread backtrace:
`docs/upstream-repro/flush_worker_backtrace.txt`; analysis:
`docs/upstream-repro/flush_worker_segv.md`)
**Found via:** MonkeyByte — app dies mid-chat with no error dialog.

### Backtrace (crashing thread)

```
Thread "monkeybyte" received signal SIGSEGV, Segmentation fault.
#0  std::_Rb_tree<string, pair<const string, nlohmann::json>, ...>::_M_root
    (this=0x0) at /usr/include/c++/15/bits/stl_tree.h:1358
... (nlohmann::json deep-copy frames: _Rb_tree copy ctor → std::map copy
     ctor → json_value::json_value → basic_json copy ctor) ...
#18 Messages::save_impl()        Messages.cpp:855
#19 Messages::save()             Messages.cpp:841
#20 Messages::flush_if_dirty()   Messages.cpp:1146
#21 Messages::flush_worker_main() Messages.cpp:1187
#22 Messages::Messages::<lambda()> Messages.cpp:375   (worker thread spawn)
```

### Root cause

`Messages::save_impl()` (Messages.cpp:852-857):

```cpp
json snapshot;
{
    std::lock_guard<std::mutex> lk(messages_mutex_);
    snapshot = messages_;   // <-- SIGSEGV here (line 855)
    dirty_.store(false, std::memory_order_release);
}
```

`_M_root(this=0x0)` means the copy constructor walked into a
`std::map<string, json>` whose internal tree pointer is null — i.e. one
json value inside `messages_` is **moved-from or half-mutated** at copy
time. The snapshot IS taken under `messages_mutex_`, so the bad entry got
into the chain earlier. Two candidate mechanisms:

1. **A mutation of `messages_` (or of a json element already inside it)
   without holding `messages_mutex_`.** Mutation sites to audit:
   - `messages_.push_back(...)` — Messages.cpp:443, 468, 480, 496, 513
   - `messages_.erase(...)` — Messages.cpp:1231, 1252
   - `arr = std::move(valid)` — Messages.cpp:831 (load-sanitize)
   - `messages_ = std::move(parsed)` — Messages.cpp:1771 (load)
   - Any path that takes a **reference/pointer to a chain element** and
     mutates it later (e.g. streaming append of content into the last
     assistant message) — a reference obtained under the lock but used
     after releasing it races the flush worker's deep copy.
2. **A moved-from json appended to the chain** — e.g. a
   `push_back(std::move(x))` where the source was ALREADY part of the
   chain, leaving the in-chain element moved-from. Suspects: the
   stream-recovery / synthesized-message / orphan tool_call cleanup paths
   (our session logs regularly show
   `[Messages] Removing orphan tool_call (no result): ...`).

The on-disk `messages.json` parses cleanly after the crash (191 entries),
so the corruption is created **in memory at runtime**, not loaded.

### Why the 1.0.2 fix did not cover this

1.0.2 wrapped the worker's `flush_if_dirty()` calls in try/catch
(Messages.cpp:1177-1184). A null-pointer dereference raises **SIGSEGV —
not a C++ exception** — so try/catch never sees it. The process dies
before any handler runs.

### Repro

Intermittent under real use with a long chain (~200 messages). Observed
twice within ~30 min of agent chat; each crash on an auto-flush tick.
Precondition appears to be a chain that has lived through interrupted
streams / orphan tool_call cleanup. No single-command trigger identified.

### Suggested fixes (for upstream)

1. **Audit every write to `messages_` for `messages_mutex_` coverage**
   (list above) — especially streaming-append paths that may hold a
   reference to a chain element across a lock release.
2. **Never leave a moved-from json in the chain**: after any
   `std::move` of a chain element, replace/reset the slot before
   releasing the lock.
3. **Defensive snapshot in `save_impl()`**: copy element-by-element and
   skip + `LOG_CERR` any entry whose object/array payload pointer is
   null, so one bad entry degrades to a dropped message instead of a
   host-killing SEGV.
4. **Host kill switch**: expose
   `MessageCompressionConfig::auto_flush_enabled` (currently in the
   internal `src/core/Messages.h`, unreachable from `include/daedalus/`)
   through the public API so hosts can disable the background worker
   until they trust a fixed build.

### Host workaround

None — the worker thread is internal and cannot be disabled via public
API. On crash only the in-flight turn is lost; the persisted chain
survives.

---

## BUG 2 — `execute_bash` never returns: two infinite hangs in `Subprocess::execute_combined`

**Severity:** hang (agent turn never completes; tool call stuck "running" forever)
**Reported against:** daedalus `main` @ a671a4e (post-1.0.2),
`src/core/Subprocess.cpp`. Confirmed with a standalone repro linked
against this exact source — `docs/upstream-repro/repro_subprocess.cpp`.
**Found via:** MonkeyByte 0.9.4 — user report "agent runs execute_bash,
it never returns". Reproduced 2026-09-01.

### Hang A — EOF branch waits forever on a live child (timeout never checked)

```cpp
// Subprocess.cpp, main read loop (~line 410)
if (stdout_eof && stderr_eof) {
    int exit_status;
    pid_t status_ret = waitpid(pid, &exit_status, 0);  // BLOCKS, no timeout
```

If the direct child closes its inherited pipe fds but keeps running (e.g.
`exec sleep 300 >&- 2>&-`, or any properly daemonizing process that closes
stdin/stdout/stderr), both pipes hit EOF while the child is still alive.
This `waitpid(..., 0)` then blocks until the child eventually exits —
the configured tool timeout is never consulted in this branch.

**Confirmed repro** (timeout_ms=5000; still blocked >15 s):

    exec sleep 300 >&- 2>&-

### Hang B — timeout path's drain loop never terminates when a survivor holds the pipes

```cpp
// Subprocess.cpp, timeout branch (~line 438)
kill_group(pid);
result.timed_out = true;
// Drain remaining output
while (!stdout_eof && !stderr_eof) {   // no iteration cap, no sleep
    read_pipe(pipe_stdout[0], stdout_output, stdout_eof);
    read_pipe(pipe_stderr[0], stderr_output, stderr_eof);
}
```

`kill_group()` uses `killpg()`, so a grandchild that escaped the process
group (`setsid`, `daemon()` double-fork) survives and keeps the pipe write
ends open → EOF never arrives → this busy loop spins forever. The normal
exit path already has the 2026-07-18 §fix (100-iteration bounded drain);
the timeout path never got the same treatment. (Also note the condition
here is `&&` vs the normal path's `||`, so even when fixable it drains
less thoroughly.)

**Confirmed repro** (timeout_ms=5000; still blocked >15 s; also leaves an
orphaned `sleep 600`):

    setsid sleep 600 & sleep 100

This one is the realistic agent trigger: starting a dev server with
`setsid … &` and then having the foreground part run past the 60 s
default timeout.

### Suggested fixes (for upstream)

1. **Hang A:** replace the blocking `waitpid(pid, &status, 0)` in the EOF
   branch with the same deadline-aware polling loop used in the main read
   loop (WNOHANG + timeout check + kill_group on expiry).
2. **Hang B:** give the timeout-path drain loop the same 100-iteration
   bounded-drain treatment the normal path got on 2026-07-18, and change
   the loop condition to `||` to match.
3. **General:** consider closing the pipe read ends after `kill_group()`
   on timeout — output from a survivor that escaped the group is not
   worth hanging the tool for.
