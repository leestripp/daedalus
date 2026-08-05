# Daedalus crash report — monkeybyte host app

Report for the daedalus maintainers. Observed while running **monkeybyte**
(GTK4 AI code editor) with the daedalus agent library linked as a static
FetchContent dependency.

- **daedalus version:** `0.9.6`, commit `590e98e`
  (`feat(core): prompt-cache prefix stability — memory section last, memory_in_prompt flag`)
- **Host:** monkeybyte @ `/workspace/AI/monkeybyte/monkeybyte` (C++23, GCC 15,
  Linux x86_64, Debug build)
- **Repro:** launch the app under gdb with an agent session active
  (LLM turn in flight, `app_ui_control` browser tool in use). The crash is
  **very hard to reproduce**: it occurred once within ~4 minutes of uptime;
  two subsequent capture runs (4 min and 15 min of an active agent session)
  exited cleanly with no fault. Total capture effort was stopped after
  ~19 minutes of non-crashing runs.

## What we know

Under `gdb -batch` (`run`, then `bt full` + `thread apply all bt` on stop)
the process died mid-session in the one crashing run. Unfortunately the
capture pipeline truncated gdb's output to its tail, so the crash banner
(signal number, fault address) and the faulting frames (#0–#12) of the
crashing thread were lost; only the outer frames of that thread's stack
survive:

```
#13 Messages::flush_worker_main (this=0x7f55e410a1d0)
    at daedalus-src/src/core/Messages.cpp:933
#14 Messages::Messages(...)::<lambda()> at Messages.cpp:286
#15..#19 std::__invoke_impl / std::thread trampoline (std_thread.h:255-310)
#20..#22 libstdc++ / start_thread / clone3
```

Two facts can still be extracted from this fragment:

1. **The thread that gdb had selected when it stopped (i.e. almost
   certainly the faulting thread) was the daedalus `Messages` background
   flush worker**, spawned at `Messages.cpp:286`.
2. Although frame #13 is *attributed* to line 933 (`flush_cv_.wait_for`),
   an idle wait would need only ~7 frames above it. A stack 13+ frames
   deep above `flush_worker_main` means the thread was executing inside
   the flush path when the process died:
   `flush_if_dirty()` → `save()` → `save_impl()` — which runs
   `sanitize_message_chain()`, `json::dump()`, the atomic temp-file
   write + `rename()`, the `.crash` mirror rewrite, **and a SQLite
   mirror write via `SettingsStore::for_home()`**
   (`Messages.cpp:906-947`, `684-827`).

No `messages.json.crash` file was produced in the workspace's
`.daedalus/daedalus/` directory despite daedalus installing
SIGSEGV/SIGABRT/SIGFPE/SIGILL handlers precisely to write that file —
which is itself evidence consistent with the crash-handler defects listed
below (a secondary fault or deadlock inside the handler would suppress
both the `.crash` file and a clean core dump).

## Bugs found by code inspection (independent of the exact crash frame)

We audited the crash-handling machinery in `src/core/Messages.cpp`
(§CRASH-1/2/4). There are several real defects in
`daedalus_crash_signal_handler` and the registry — each can turn a first,
recoverable crash into a hang, heap corruption, or a lost crash file:

### 1. The signal handler allocates — it is not async-signal-safe

`Messages.cpp:205-236`:

```cpp
std::vector<Messages*> snapshot;
snapshot.reserve(g_crash_registry.size());   // malloc inside a SIGSEGV handler
for (auto* p : g_crash_registry) {
    snapshot.push_back(p);
}
```

The header comment claims "The crash handler must avoid allocations, locks,
and any non-trivial stdlib calls" — but `reserve()`/`push_back()` call the
allocator. If the original fault happened anywhere inside
malloc/free/new/delete (heap lock held or free-list half-updated — the most
common place for a SIGSEGV to originate), the handler **deadlocks or
corrupts the heap a second time**. Fix: iterate `g_crash_registry`
in place under the spinlock, or use a fixed-size `std::array<Messages*, N>`
registry with no dynamic storage anywhere on the signal path.

### 2. Data race on `crash_payload_` between `save()` and the handler

`save_impl()` updates the payload cache **without any lock**
(`Messages.cpp:812`: `crash_payload_ = contents;`) on the flush worker
thread, while the signal handler reads the same `std::string` via
`crash_payload_for_signal()` (`Messages.cpp:228`) on the faulting thread.
Assigning a `std::string` frees/reallocates its buffer — the handler can
read a dangling `data()` pointer or a half-updated length. This is a
genuine candidate for a **secondary crash inside the crash handler**
(and with `SA_RESETHAND`, that means a bare core dump with no `.crash`
file written at all). Fix: double-buffer two pre-allocated slots with an
atomic index swap, never mutate the buffer the handler may be reading.

### 3. Spinlock self-deadlock if the faulting thread holds the registry lock

The registry spinlock (`g_crash_registry_lock`,
`Messages.cpp:170/207-215/857-863/868-880`) is acquired by
`install_crash_handlers()` / `uninstall_crash_handlers()` on ordinary
threads. If a thread faults (or is SIGABRT'd) *while holding that lock*,
the handler spins forever — the process hangs instead of dumping core.
Given the registry is only mutated at Messages construction/destruction,
a single global atomic flag guarding registration ("registry frozen after
install") or a fixed array with atomic slots removes the lock entirely.

### 4. Registry lifetime race → use-after-free in the handler

`~Messages()` calls `uninstall_crash_handlers()` (`Messages.cpp:313`),
but the handler snapshots pointers and then uses them *after releasing*
the spinlock (`Messages.cpp:223-234`). Between the snapshot and the
`m->crash_fd_for_signal()` call the object can be destroyed →
use-after-free in the signal handler. Fix: pin instances for the whole
process lifetime (leak intentionally — it's a crash path), or refcount.

### 5. `ftruncate` result explicitly ignored (minor)

`Messages.cpp:817-822`: on `ftruncate` failure the code proceeds to
overwrite from offset 0 and deliberately leaves a stale tail, which
produces a **corrupt `.crash` JSON file** (trailing garbage after the
closing bracket) exactly in the scenario the mirror exists for. At
minimum the recovery-side parser should tolerate trailing bytes; better,
write to a fresh `.crash.tmp` and rename.

## Suspected root cause

The surviving stack fragment places the crashing thread deep inside the
`Messages` persistence path, so the primary fault is most likely in
`save_impl()` — plausibly the SQLite mirror
(`SettingsStore::for_home(...).set_messages_json(...)`,
`Messages.cpp:795-803`) racing with another thread's use of the same
SQLite connection/store, or JSON serialization of a concurrently mutated
message snapshot. The crash-handler defects above then explain why the
process died without leaving a `.crash` file or other forensic artifacts.

Because the crash has not reproduced since (two clean runs totalling
~19 minutes of active agent use), this remains a hypothesis — but the
five defects in the previous section are verifiable directly from the
source and are worth fixing regardless.

## Additional issue: persisted corrupt message chain survives "clear chat"

Separately from the crash, the host hit a session-breaking error where an
assistant message was rejected for not having its `role` set. The only
way to recover was to **delete the whole `./.daedalus/` folder and
restart** — clearing the chat from the UI did NOT fix it. Source
inspection explains why, and points at two daedalus defects:

### 6. `Messages::clear()` never persists the cleared state

`Messages::clear()` (`Messages.cpp:949-953`) only resets the in-memory
chain:

```cpp
void Messages::clear() {
    std::lock_guard<std::mutex> lk(messages_mutex_);
    messages_ = json::array();
    executing_tools_.clear();
}
```

Unlike every `add_*` method (which all end in
`dirty_.store(true, std::memory_order_release)` — `Messages.cpp:356, 381,
393, 414, 426`), `clear()` sets neither `dirty_` nor calls `save()`, and
it never touches the SQLite mirror. So after a host-side "clear chat",
the corrupt `messages.json` **and** the SQLite mirror still hold the bad
chain on disk; on the next launch `load()` restores it and the error
returns. Deleting `.daedalus/` works precisely because it removes both
stores. Fix: `clear()` should mark dirty (or better, persist
synchronously) and delete/overwrite the SQLite mirror row.

### 7. `load()` restores structurally invalid messages without validation

`load()` (`Messages.cpp:441-469`) prefers the SQLite mirror, then falls
back to `messages.json`. Both paths run only
`sanitize_message_chain()` (which fixes orphaned tool results) — there is
**no per-message well-formedness check** on the normal load path. The
`role`-presence validation (`validate_message_for_recovery()`,
`Messages.cpp:610-613`) is only applied on the `.crash` recovery path,
and even there only to the *last* message (`Messages.cpp:561`). So a
message with a missing/empty `role` that lands in the store once (e.g.
written by an interrupted save, or by the crash described above) is
loaded forever after, and every LLM request built from the chain fails.
Fix: validate every message on load (reuse
`validate_message_for_recovery()` for the whole array) and drop/quarantine
invalid entries instead of feeding them back to the provider.

## Reproduction command

```bash
GSETTINGS_SCHEMA_DIR=$PWD/build/gschemas timeout --signal=SIGINT 900 gdb -batch \
  -ex "set pagination off" -ex "run" \
  -ex "bt full" -ex "info threads" -ex "thread apply all bt" -ex "quit" \
  --args ./build/monkeybyte
```

Note: expect to need many attempts — the crash occurred in roughly 1 of 3
runs of 4–15 minutes each, with an agent session active (LLM turn in
flight, browser-driven `app_ui_control` tool in use). Capture the
**complete** gdb output to a file; do not truncate it.

## Environment

- OS: Linux x86_64 (container; bubblewrap sandboxing disabled)
- Compiler: GCC 15, libstdc++ (`/usr/include/c++/15`)
- Build type: Debug, `-Wall -Wextra -Wpedantic`
- daedalus fetched via CMake FetchContent from
  `https://gitlab.com/leestripp/daedalus.git` (tag `main`, pinned at
  `590e98e` in this build tree)
