# Crash Resilience

> **Added (2026-07-16):** this document describes the §CRASH-RESILIENCE
> feature set. Conversation history now survives abnormal termination
> (SIGSEGV, SIGABRT, kill -9, power loss) and graceful signals (Ctrl-C,
> Ctrl-Z) without losing the in-flight turn.

This doc covers four mechanisms layered on top of `Messages`:

1. **Atomic save** — every write goes through temp + rename + fsync so
   the on-disk file is never half-written.
2. **Write-through auto-flush** — every `add_*()` marks the message
   chain as dirty and a background worker flushes to disk within
   ~100 ms.
3. **Async-signal-safe crash handler** — `SIGSEGV`, `SIGABRT`,
   `SIGFPE`, and `SIGILL` all write `messages.json.crash` before
   re-raising the signal so the user still gets a core dump / stack
   trace.
4. **Crash recovery** — `load()` falls back to `messages.json.crash`
   when the main file is missing or corrupt, with a defensive
   "drop last message" heuristic to discard a malformed message that
   may have caused the crash.

---

## 1. Atomic save via temp + rename

> §MSG-HEADERS (2026-10-04): each message in `messages.json` carries a
> `_meta` origin envelope (`source` + `received_at`, always UTC with an
> explicit ` UTC` indicator). Crash recovery preserves it verbatim —
> replayed messages keep their original timestamps, which is exactly
> what lets the model distinguish a restored old message from a fresh
> one after a crash.


`Messages::save()` writes to `messages.json.tmp` first, `fsync()`s it,
then renames it over `messages.json`. POSIX `rename(2)` on the same
filesystem is atomic — readers always see either the old file or the
new file, never a half-written one.

```cpp
// save() implementation (simplified):
const std::string tmp_path = messages_file_.string() + ".tmp";
{
    std::FILE* wf = std::fopen(tmp_path.c_str(), "wb");
    std::fwrite(contents.data(), 1, contents.size(), wf);
    std::fflush(wf);
#ifndef _WIN32
    ::fsync(::fileno(wf));   // barrier: data is on disk before rename
#endif
    std::fclose(wf);
}
std::filesystem::rename(tmp_path, messages_file_);   // atomic
```

**Failure modes handled:**

| Cause | Old behaviour | New behaviour |
|-------|---------------|---------------|
| `kill -9` mid-write | `messages.json` truncated → next `load()` parses empty | `messages.json` is the previous fully-written version; `.tmp` may exist (overwritten next save) |
| Power loss | Same as kill -9 | Same — `fsync` flushes before `rename` |
| Disk full during write | Exception → `messages.json` partial | Exception → `messages.json` untouched; error logged |

**Stale `.tmp` handling:** the next save() unconditionally overwrites
`.tmp`. There's no recovery needed from a stale `.tmp`.

---

## 2. Write-through auto-flush (background worker)

Every `add_*()` (`add_system`, `add_user`, `add_assistant`,
`add_tool_result`, `add_message`) sets an atomic `dirty_` flag and
notifies a background thread that waits on a `std::condition_variable`.
The worker wakes on the notification (or a 100 ms safety timeout),
calls `save()`, and clears `dirty_`. End result: messages are on
disk within ~100 ms of being added.

```cpp
// Constructor: starts the worker thread
if (config_.auto_flush_enabled) {
    flush_worker_ = std::thread([this]{ flush_worker_main(); });
}

// Destructor: stops the worker cleanly
if (flush_worker_.joinable()) {
    {
        std::lock_guard<std::mutex> lk(flush_mutex_);
        stop_flush_ = true;
    }
    flush_cv_.notify_all();
    flush_worker_.join();   // worker does a final flush before returning
}
```

**Configuration:**

| `MessageCompressionConfig` field | Default | Purpose |
|---|---|---|
| `auto_flush_enabled` | `true` | Set to `false` to disable the worker (tests do this). Caller must then call `save()` explicitly. |
| `auto_flush_interval_ms` | `100` | Worker wake-up timeout when no notification arrives. Lower = more I/O; higher = longer worst-case data-loss window. |

**Synchronous force-flush:** `Messages::flush_now()` blocks until any
pending dirty state is on disk. Use this immediately before dispatching
an LLM call so the user's prompt is preserved even if the LLM call
hangs or crashes the process.

**Why tests disable auto-flush:** tests that wipe the data directory
between Messages construction and destruction would otherwise race the
background worker (the worker could write into the wiped dir).
`create_test_messages()` in `tests/test_messages.cpp` sets
`auto_flush_enabled = false` by default; tests that exercise the
worker explicitly enable it.

---

## 3. Async-signal-safe crash handler

`Messages::install_crash_handlers()` registers `SIGSEGV`, `SIGABRT`,
`SIGFPE`, and `SIGILL` handlers that write the current messages to
`messages.json.crash` and then re-raise the signal. The handlers use
**only async-signal-safe primitives** (write, lseek, ftruncate, fsync,
signal) — no allocations, no locks, no stdio, no SQLite.

§CRASH-6 (2026-08-05, monkeybyte crash report `daedalus_bugs.md`
defects #1–#5) reworked the internals:

- **Fixed-size lock-free registry** — a
  `std::array<std::atomic<MessagesCrashState*>, 64>` with CAS
  registration replaces the old `std::vector` + spinlock. The handler
  never allocates and never takes a lock, so a fault inside
  malloc/free (or while the old spinlock was held) cannot deadlock or
  re-corrupt the heap.
- **Leaked per-instance state** — the pre-opened `.crash` fd and the
  payload live in a heap-allocated `MessagesCrashState` that is
  *intentionally never freed* (and the fd never closed). A signal
  handler racing a `Messages` destructor can therefore never touch
  freed memory or write through a recycled fd.
- **Double-buffered payload** — `save()` writes the inactive slot and
  flips an atomic index (release/acquire), so the handler never reads
  a buffer that `save()` is mutating. The old design cached a
  `std::string` the handler read while `save()` reassigned it —
  a dangling-`data()` race.
- **Stale-tail handling** — after writing, both `save()` and the
  handler `ftruncate()` the fd to the exact payload length, and the
  recovery parser tolerates trailing garbage (walks backwards over
  `]` candidates) as a second line of defence.

```cpp
extern "C" void daedalus_crash_signal_handler(int sig) {
    for (const auto& slot : g_crash_registry) {        // atomic loads only
        MessagesCrashState* st = slot.load(std::memory_order_acquire);
        if (!st || !st->active.load(...)) continue;
        const auto& s = st->slots[st->active_slot.load(...) & 1];
        ::lseek(st->fd, 0, SEEK_SET);
        write(st->fd, s.data, s.size);
        ::ftruncate(st->fd, s.size);   // cut any stale tail
        ::fsync(st->fd);
    }
    re_raise(sig);   // restore default handler, re-raise
}
```

**Why pre-opened fd?** Signal handlers can't safely allocate or call
`open()`. We open the `.crash` file once at install time and hold
the fd open for the rest of the process (deliberate leak — see above).

**Why a cached payload?** The handler must not re-enter `save()` (which
calls into the allocator, mutex-guarded SQLite, and `std::ofstream` —
none async-signal-safe). Instead, `save()` publishes the last
successfully-written JSON blob into the double-buffered
`MessagesCrashState`, and the handler writes that blob verbatim.

**Production wiring:**

```cpp
// demos/daedalus-cli/main.cpp
agent.initialize();
agent.install_crash_handlers();   // <-- from here on, SIGSEGV/SIGABRT/...
                                   //     write messages.json.crash before dying

// ... agent.run_one_turn() loop ...

agent.shutdown();                 // uninstalls handlers before teardown
```

**Forwarding chain:**

```
Agent::install_crash_handlers()
  → AgentImpl::install_crash_handlers()
    → LLMClient::install_crash_handlers()
      → Messages::install_crash_handlers()
```

`AgentImpl::shutdown()` calls `uninstall_crash_handlers()` first,
before tearing down LLMClient, so the signal handler never iterates a
stale `messages_` pointer during destruction.

**`SA_NODEFER | SA_RESETHAND` semantics:**

- `SA_NODEFER` — the signal we're handling isn't blocked during its
  own handler, so `re_raise()` can deliver it.
- `SA_RESETHAND` — after one delivery, the handler reverts to
  `SIG_DFL`. A recursive crash still produces a core dump.

---

## 4. Crash recovery on next launch

`Messages::load()` tries three sources in order:

1. **SQLite mirror** in `daedalus.db` (preferred post-v0.4.0; fast
   indexed lookup).
2. **`messages.json`** — the atomic-renamed main file.
3. **`messages.json.crash`** — fallback if `messages.json` is missing
   or has a JSON parse error.

```cpp
if (!std::filesystem::exists(messages_file_)) {
    if (load_from_crash_file(crash_path)) return true;
    return false;
}
try {
    json saved = json::parse(ifstream(messages_file_));
    ...
} catch (const json::parse_error& e) {
    // Main file is corrupt (possibly truncated by a previous crash).
    if (load_from_crash_file(crash_path)) return true;
    return false;
}
```

**Drop-last-message heuristic:** after loading from `.crash`, the last
message is validated. If it fails basic well-formedness (missing role,
tool result missing `tool_call_id`, assistant with malformed JSON
arguments) AND the second-to-last is fine, the last message is
**dropped** and the recovered chain is re-saved to `messages.json`
atomically. This is the most common crash cause — a malformed tool
result that triggered a parser exception during `add_*()`.

```cpp
bool Messages::validate_message_for_recovery(const json& msg) const {
    if (!msg.is_object()) return false;
    if (!msg.contains("role") || !msg["role"].is_string()) return false;
    if (msg["role"] == "tool") {
        // tool_call_id is required
        if (!msg.contains("tool_call_id") || !msg["tool_call_id"].is_string()) return false;
    }
    if (msg["role"] == "assistant" && msg.contains("tool_calls")) {
        // arguments must be valid JSON
        for (const auto& tc : msg["tool_calls"]) {
            try { json::parse(tc["function"]["arguments"]); }
            catch (...) { return false; }
        }
    }
    return true;
}
```

**Load-time validation (§CRASH-6, defect #7):** every message loaded
from the SQLite mirror or `messages.json` is checked with the same
well-formedness validator used for crash recovery
(`validate_message_for_recovery()` — role present and non-empty, tool
results carry `tool_call_id`, assistant tool-call arguments parse).
Structurally invalid entries are **dropped with a warning** instead of
being fed back to the provider, so a single bad write can no longer
poison every subsequent session. If nothing survives, the store is
treated as empty (and a poisoned SQLite mirror is deleted).

**`clear()` persists (§CRASH-6, defect #6):** `Messages::clear()` now
marks the chain dirty and synchronously `save()`s — which on an empty
chain removes `messages.json`, the `.tmp`/`.crash` mirrors, and the
SQLite mirror row, and clears the crash payload. A host-side "clear
chat" can no longer be undone by the next `load()`.

**Trade-off:** we only drop ONE message. If multiple are corrupt, the
user gets them all back so they can inspect / repair / delete by hand.
The recovery is logged at WARN level so the user knows what happened.
(Note: this drop-last trade-off applies to the `.crash` recovery path
only; the normal load path drops every invalid entry — see above.)

---

## 5. End-to-end behaviour summary

| Scenario | Outcome |
|----------|---------|
| Clean exit (Ctrl-D, `/quit`) | All messages on disk via auto-flush worker + destructor's final flush |
| Ctrl-C / SIGTERM | Agent loop sees `g_shutting_down`, returns; destructors flush. Loss: messages added in the last <100 ms before the signal. |
| Ctrl-Z (SIGTSTP) | Process stops; on resume, the worker resumes too. No message loss. |
| SIGSEGV / SIGABRT / SIGFPE / SIGILL | `.crash` file written synchronously before re-raise. Next launch recovers from `.crash`; possibly drops the last message. |
| `kill -9` / power loss | Last successfully-`fsync()`'d save() is the recoverable point. Auto-flush keeps the data-loss window to ~100 ms. |
| OOM kill | Same as `kill -9`. The kernel doesn't run signal handlers before SIGKILL. |

---

## 6. Testing

**Unit tests** (`tests/test_messages.cpp`, 73 cases / 203 assertions):

- **CRASH-1** (4 cases) — atomic save / temp+rename / stale `.tmp` /
  empty-session cleanup.
- **CRASH-2** (4 cases) — background flush worker / `flush_now` /
  destructor-stops-worker / disabled-leaves-save-to-caller.
- **CRASH-4** (5 cases) — `install_crash_handlers` / `uninstall` /
  idempotency / payload cache.
- **CRASH-5** (5 cases) — `load()` falls back to `.crash` / drops
  corrupt last message / re-saves recovered chain.
- **CRASH-6** (4 cases) — `clear()` persists the cleared state
  (file + mirror removed, cleared chain not resurrected) /
  load-time validation drops invalid messages / `.crash` mirror
  tolerates a stale trailing tail.

**Manual verification:** run `daedalus-cli`, type a prompt, hit Ctrl-C.
Inspect `<project>/.daedalus/daedalus/messages.json` — it should contain the
prompt and any partial assistant response.

**End-to-end crash test (deferred):** an in-process fork test would
require `fork()` + `kill(child, SIGABRT)` + `waitpid()` in doctest,
which fights with libc's `abort()` flushing stdio and doctest's own
SIGABRT handler. The crash handler's components are covered by unit
tests; integration verification needs a standalone binary outside the
doctest framework. A placeholder test is marked `doctest::skip(true)`
in `tests/test_messages.cpp` with a pointer to where the standalone
binary should live (`demos/daedalus-cli/crash_recovery_e2e/`).

---

## 7. File layout (this feature)

```
<project_root>/.daedalus/daedalus[-<agent_name>]/
├── messages.json         # main file (atomic-rename target)
├── messages.json.tmp     # temp file mid-write (transient)
├── messages.json.crash   # crash-mirror (written by signal handler)
└── ...
```

- `messages.json.tmp` — transient. May be present after a crash; next
  `save()` overwrites it.
- `messages.json.crash` — kept up-to-date on every save. Read only on
  next launch if `messages.json` is missing/corrupt. Safe to delete
  manually if you want to force a clean start.

The per-agent namespace is nested under a single `.daedalus/` base:

| `agent_name` | Directory |
|---|---|
| `""` (default) | `<project_root>/.daedalus/daedalus/` |
| `"alice"` | `<project_root>/.daedalus/daedalus-alice/` |

**Settings store mirror:** `Messages::save()` also writes to
`<daedalus_home>/daedalus.db` (SQLite, table `messages`) via
`SettingsStore::set_messages_json()`. This is the primary source on
load() if it's present and parseable.