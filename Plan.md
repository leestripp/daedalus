# Plan.md — Active Tasks

> Living task list. Completed tasks stay in place with a DONE marker and the
> commit/date that landed them.

---

## TASK-2026-08-31-02: SANITIZE-NOTHROW — malformed chain entries SIGABRT the flush worker

> **Status**: DONE (2026-08-31, uncommitted working tree)
> **Created**: 2026-08-31

### Problem

MonkeyByte coredump (2026-08-30): `Messages::sanitize_message_chain()` used
`entry.value("role", ...)` / `.value("tool_call_id", ...)` / `.value("id", ...)`,
which throw `json::type_error` when a chain entry is not an object, or when
`role`/`tool_call_id`/`id` is present but null or non-string. The exception
escaped `save()` onto the background flush worker thread → `std::terminate`
→ SIGABRT — intermittent, fired only when a malformed/partially-written chain
entry met a flush tick. Follow-up to the `daedalus_bugs.md` crash work
(§CRASH-6, 2026-08-05).

### Scope

1. `src/core/Messages.cpp`:
   - defensive string extraction (`chain_safe_str()`) in
     `sanitize_message_chain()` and `sanitize_assistant_message()`; drop
     non-object chain entries, tool results with missing `tool_call_id`,
     non-object `tool_call` entries, and `tool_call`s with non-object
     `function` instead of throwing
   - `save_impl()`: catch sanitize failures, re-mark `dirty_`, return
   - `flush_worker_main()`: last-resort catch around `flush_if_dirty()` so
     no persistence error can ride the worker into `std::terminate`
2. `tests/test_message_chain_validity.cpp`: SANITIZE-NOTHROW suite
3. `CHANGELOG.md` (Unreleased → Fixed)

### Verification

- `daedalus_message_chain_validity_tests` passes (incl. new suite)
- `ctest --test-dir build --output-on-failure` passes

---

## TASK-2026-08-31-01: Move `read_pdf` (PDFTool) back into core agent tools

> **Status**: DONE (2026-08-31)
> **Created**: 2026-08-31

### Problem

`ReadPDFTool` (`read_pdf`) was moved out of `libdaedalus` into
`demos/example-tools/` during the §TOOL-TRIM work, but poppler-cpp is still a
**REQUIRED** dependency of the library build
(`pkg_check_modules(POPPLER REQUIRED poppler-cpp)` in the root
`CMakeLists.txt`). Requiring the dependency while shipping the tool only as an
opt-in example is inconsistent — `read_pdf` should be a built-in agent tool,
like `read_image` and `serper_search` (which moved back for the same reason).

### Scope

1. `git mv demos/example-tools/PDFTool.{h,cpp}` -> `src/tools/`
2. Register `ReadPDFTool` in `ToolFactory::initialize_tools()` (§PDF-CORE)
3. Root `CMakeLists.txt`:
   - add `src/tools/PDFTool.cpp` to `TOOLS_SOURCES`
   - add `${POPPLER_INCLUDE_DIRS}` / `${POPPLER_LIBRARIES}` to the `daedalus`
     target (PRIVATE); fix the stale "PDF tools moved to demos" comment
4. `demos/example-tools/`: drop `PDFTool.cpp` from the example-tools library,
   remove poppler from its includes/links, drop the include from
   `example_tools.h`
5. `demos/daedalus-cli/main.cpp` + `demos/daedalus-webui/session_manager.h`:
   remove the explicit `ReadPDFTool` registration (now registered by default)
6. `tests/CMakeLists.txt`: point `daedalus_tool_definitions_tests` at the new
   `src/tools/PDFTool.cpp` path; assert `read_pdf` in default tool set
7. Docs: `docs/tools.md` (core tool table + notes), `AGENTS.md`
   (project-structure table), `CHANGELOG.md` (Unreleased)

### Verification

- `cmake --build build -j$(nproc)` succeeds
- `ctest --test-dir build --output-on-failure` passes
- `read_pdf` appears in the default tool definitions

---

## TASK-2026-09-02-01: Fix MonkeyByte crash + hangs from `daedalus_bugs.md`

> **Status**: DONE (2026-09-02) — 95/95 ctest green; race reproduced
>   5/8 SIGSEGV pre-fix, 10/10 clean post-fix, TSAN-clean; both Subprocess
>   hangs reproduced pre-fix (external timeout kill) and fixed.
> **Created**: 2026-09-02
> **Source**: `daedalus_bugs.md` (MonkeyByte GTK4 host, daedalus main post-1.0.2)

### BUG 1 — SIGSEGV: flush worker crashes deep-copying `messages_`

**Root cause (confirmed by backtrace `_M_root(this=0x0)` = torn read of a
json object header):** the LLMClient turn path mutates `messages_` through
the raw `get_messages()` reference WITHOUT holding `messages_mutex_`, while
the flush worker deep-copies `messages_` in `save_impl()` under the mutex.
The mutex is useless when one side doesn't hold it. §PERSIST-DIRTY
(5d7aa77) activated the race: `mark_dirty()` is now called mid-turn, waking
the worker to snapshot while the main thread keeps mutating. The 1.0.2
try/catch (SANITIZE-NOTHROW) can't help — SIGSEGV is not a C++ exception.

**Fix (§PERSIST-RACE): published-snapshot design** — the worker never
reads `messages_` again:

1. New `persist_snapshot_` member (guarded by `messages_mutex_`) + private
   `mark_dirty_locked()`: snapshots the chain and sets `dirty_` in one
   locked step, always called on the mutating thread at a quiescent point.
2. `mark_dirty()` locks + calls it; all internal mutators (`add_*`,
   `set_messages`, `clear`, `cleanup_dangling_tool_results`, `compress`,
   `truncate_if_needed`) call `mark_dirty_locked()` inside their existing
   critical sections.
3. `save_impl()` copies `persist_snapshot_` — never `messages_`.
4. Public `save()` refreshes `persist_snapshot_` from `messages_` first
   (preserves legacy semantics: raw push + save() still persists; the
   worker path uses `flush_if_dirty` which does NOT refresh).
5. Hardening (completes SANITIZE-NOTHROW): `chain_safe_str` for the
   remaining `.value()` throw paths — `sanitize_assistant_message`,
   `cleanup_dangling_tool_results`, `truncate_if_needed`, plus
   LLMClient's `messages.back()` role check. `compress()` now marks dirty
   (its chain rewrite was never persisted). `truncate_if_needed` takes
   `messages_mutex_` (was fully unlocked).

**Deliberately skipped from the report's suggestions:** #3 element-wise
defensive copy — a torn json read is SIGSEGV, not catchable; the root
fix eliminates the torn read. #4 public auto-flush kill switch —
deferred; root fix + regression tests are the answer, not a workaround.

### BUG 2 — `execute_bash` infinite hangs in `Subprocess::execute_combined`

1. **Hang A (§SUBPROC-EOF-HANG):** EOF branch's blocking
   `waitpid(pid, &status, 0)` ignores the timeout — replace with
   WNOHANG polling against the remaining deadline; on expiry `kill_group()`
   + `timed_out = true`.
2. **Hang B (§SUBPROC-DRAIN-HANG):** timeout-path drain loop spins
   forever when a `setsid` grandchild keeps the pipe write ends open —
   apply the normal path's bounded drain (100 iterations x 10ms) and fix
   the loop condition from `&&` to `||`.

### Tests

- New `tests/test_subprocess_timeout.cpp`: Hang A repro
  (`exec sleep 300 >&- 2>&-`, timeout 2s, must return promptly with
  `timed_out`), Hang B repro (`setsid sleep 613 & sleep 100`), plus
  normal-output and plain-timeout sanity cases.
- `tests/test_messages.cpp`: §PERSIST-RACE stress (auto_flush 1ms,
  raw-ref push_back + mark_dirty loop, then flush_now + reload verify);
  malformed-chain save() no-throw (SANITIZE-NOTHROW regression);
  save()-after-raw-push semantics; cleanup_dangling with non-object tail.

### Verification

- New tests run pre-fix to confirm failure/hang, pass post-fix
- One-off `-fsanitize=thread` build of the messages stress test:
  race report pre-fix, clean post-fix
- Full suite: `ctest --test-dir build --output-on-failure`
