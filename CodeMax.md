# CodeMax — working memory

> This file tracks open bugs, active refactors, and things that need
> attention. Everything with a ✅ or `[x]` is **done** — it stays in
> the file as a record (one line per item). Open items are the only
> thing that need action.

---

## Current state (2026-07-16)

- **Build:** clean (only OmniNet-internal warnings).
- **Tests:** `ctest` → **64/64 pass**, 2 live-LLM skipped, 0 failures.
- **libcurl:** fully gone from the build system. HTTP/SSE stack is `OmniNet::HTTP::Client`.
- **Open items:** none.

Recent work: §NOMORE-CURL complete (12/12 NC tasks), ACP code review complete (16/16 ACP tasks), demos cleanup (subagent/manager/webui fixed hardcoded paths), NC-9 cosmetic rename `CurlMultiSession` → `HttpSession`. ACP-16: fixed empty `session_id` serialization in `RunCreateRequest::to_json`. **§CRASH-RESILIENCE plan complete** (5/5 phases done; 14 new tests).

---

## Library design rule — NO stdio inside libdaedalus

**The library MUST NOT call `std::cout`, `std::cerr`, `std::cin`,
`printf`, `fprintf(..., stderr)`, or any function that writes directly
to stdout/stderr/stdin.** Applies to all of `src/library/`, `src/api/`,
`src/core/`, `src/mcp/`, `src/tools/` — anywhere linked into
`libdaedalus.a`.

**Why:** A host that builds a TUI (ncurses, blessed, ratatui, raw
terminal mode, vim-style status line, …) needs total control of
stdio. Library code that reaches for `std::cout` or `std::cerr`
corrupts the terminal, blocks the host's input-handling thread, eats
escape sequences, and breaks mouse/keystroke routing. Once any
library call leaks a single `\n` into a raw-mode terminal, the host
is dead in the water until it `stty sane`s the user.

**Output channels the library MAY use:**

1. **`AgentSink`** (host-controlled callback) — primary I/O path for
   user-visible text. Hosts wire this to their own stdout, a log
   file, an in-memory buffer, whatever fits the host's I/O model.
2. **`core/Logger.h`** — `LOG_INFO` / `LOG_WARNING` / `LOG_ERROR`
   macros for internal diagnostics. Writes to a per-project log
   file under `<project>/.daedalus/logs/`. Does NOT touch stdio.

**Output channels the library MUST NOT use:** `std::cout`, `std::cerr`,
`std::clog`, `std::cin`, `printf`, `fprintf(stderr, ...)`, `fputs(...,
stderr)`, `puts`, `write(1, ...)`, `write(2, ...)`.

**File I/O is fine** — `fopen(path, "rb")` reads/writes *files*, not
the host's stdio. `<cstdio>` for that is OK.

**POSIX constants are fine** — `STDOUT_FILENO`, `STDERR_FILENO`,
`STDIN_FILENO` are used in `Subprocess.cpp` and
`BackgroundProcessManager.cpp` to re-route child-process stdio
through pipes. The library never writes to those fds itself.

**Verified (2026-07-16):** `grep -rn "std::cout\|std::cerr\|std::cin"
src/library src/api src/mcp` → no matches outside comments. Runtime
test: override `std::cout` / `std::cerr` rdbuf, run full agent
lifecycle → **0 bytes written**.

---

## Open bugs — all resolved

Bugs are tagged by severity. ✅ = done (one-liner; details in
`Completed (kept for history)` below).

### CRITICAL

- ✅ B1 SQL injection in SettingsStore (2026-07-15, prepared statements)
- ✅ B2 SettingsStore::for_home wrapper leak (2026-07-16, returns by value)
- ✅ B16 CurlMultiSession shutdown leaks in-flight RequestState (2026-07-16)
- ✅ B17 AgentImpl::execute_tool was a no-op stub (2026-07-16)

### HIGH

- ✅ B3 BashTool standalone-`&` check ignores quotes
- ✅ B5 truncate_if_needed tool-chain walk inverted
- ✅ B8 inject_context_stats erases wrong region when stats is last H2
- ✅ B10 CurlMultiSession discards non-2xx body (callers can't distinguish 400/401/503)
- ✅ B19 run_one_turn appends user message after dangling tool result
- ✅ B23 set_status fires on_state_change(-1, new) instead of mapping transition

### MEDIUM

- ✅ B7 compress_after_done flag set but never read
- ✅ B14 HTTP 4xx not honoured as non-retry; 429 Retry-After header ignored
- ✅ B22 Corrupt SQLite mirror row not deleted on load
- ✅ B24 std::stod throws on non-numeric backoff_multiplier
- ✅ B25 McpServerManager::load_env_file idempotency (verified non-bug)
- ✅ B29 BashCommandExecutor::execute has no timeout (already in place)
- ✅ B31 phase15_typo_warning_tests hangs in case 3 (SkillManager singleton, 2026-07-16; re-enabled case 5 in 2026-07-16 sweep — all 5 tests pass in 0.03s)

### LOW

- ✅ B4 BashTool::build_patterns has duplicate "make"
- ✅ B6 sanitize_assistant_message comment contradicts code (already correct)
- ✅ B9 "Find last tool_call" loop repeated 4× (extracted helper)
- ✅ B11 sanitize_utf8 iconv + fallback paths duplicate state machine
- ✅ B12 SessionMemory::sanitize_utf8 truncates on partial UTF-8
- ✅ B13 split_messages and get_compression_indices duplicate algorithm
- ✅ B15 run_agent_cycle uses dump(2) for first request, dump() for retry
- ✅ B18 active_model and list_models duplicate ModelConfig → ModelInfo
- ✅ B20 BackgroundProcessManager::FIFO_TIMEOUT_MS unused
- ✅ B21 BashTool build-event double-fire (same as B4)
- ✅ B26 streaming_callback fires one extra delta after cancellation
- ✅ B27 SSE JSON-parse retry loop ignores server_error_settings.enabled
- ✅ B28 Four copies of UTF-8 sanitizer with subtle drift (same as B11)
- ✅ B30 extract_summary_text strips `<answer>` but not `<thinking>`
- ✅ B32 update_token_usage warns twice per turn

---

## Physical tests — PT-1..PT-5

Live demos against `minimax-m2.7`:

- ✅ F0 `print_banner` throws `length_error` on long paths (2026-07-16, `pad_to` helper)
- ✅ F2 daedalus-subagent/manager crash on `--help` (2026-07-16, argv short-circuit — both demos now check for `--help`/`-h` before positional parsing)
- ✅ F3 Readme webui section already documents `{"message":"..."}` body shape (Readme.md:525 — confirmed present)

---

## §NOMORE-CURL — 12/12 ✅

Migrate every direct libcurl usage to `OmniNet::HTTP::Client`. Audit
triggered by ACP transport being written with `curl/curl.h` directly.
**Final state:** zero `curl_easy_*`, `curl_multi_*`, `find_package(CURL)`,
`CURL::libcurl` anywhere in src/, demos/, tests/, CMakeLists.txt.

| # | Task | Status |
|---|------|--------|
| NC-1 | ACP transport → OmniNet | ✅ |
| NC-2 | Client-side SSE parser (`<daedalus/net/SseParser.h>`, 17 tests / 47 assertions) | ✅ |
| NC-3 | ACP SSE structured dispatch via JSON `type` discriminator (15 new dispatch tests) | ✅ |
| NC-4 | SearchTool (Serper + Wikipedia) → OmniNet | ✅ |
| NC-5 | utils.cpp `download_image_as_base64` → OmniNet | ✅ |
| NC-6 | CurlMultiSession rewritten on OmniNet (public API preserved) | ✅ |
| NC-7 | LLMClient async path (transparent via NC-6) | ✅ |
| NC-8 | LLMClient sync path (`make_curl_request_with_retry` + `run_agent_cycle`) → OmniNet | ✅ |
| NC-9 | `CurlHandle.h` deleted; **CurlMultiSession → HttpSession** rename (12 files, ~172 references) | ✅ |
| NC-10 | `test_curl_handle.cpp` deleted; `test_curl_multi_session.cpp` → `test_http_session.cpp` | ✅ |
| NC-11 | `find_package(CURL)` + `CURL::libcurl` links removed from CMakeLists | ✅ |
| NC-12 | ThreadPool.h comment "curl_multi" → "OmniNet HTTP/MCP" | ✅ |

---

## ACP (Agent Communication Protocol) — 16/16 ✅

Sub-library at `demos/daedalus-cli/acp/` (demos-only — uses `std::cerr`
for diagnostics, which would corrupt a TUI host). Enabled via
`-DDAEDALUS_CLI_ACP=ON` (default OFF).

| # | Task | Status |
|---|------|--------|
| ACP-1 | Fix standalone CMakeLists (`find_package(PkgConfig)` order) | ✅ |
| ACP-2 | Wire into demo build via `DAEDALUS_CLI_ACP` option | ✅ |
| ACP-3 | Fix status enum string mismatch (kebab-case wire format) | ✅ |
| ACP-4 | Complete `from_json(Run&)` — `run_id`/`status`/`created_at` parsed | ✅ |
| ACP-5 | Complete `from_json(Message&)` + `to_json(Message&)` for `created_at` | ✅ |
| ACP-6 | `AcpResult<T>` error propagation (was default-constructed return values) | ✅ |
| ACP-7 | SSE structured dispatch via IRunEventHandler (was a stub) | ✅ |
| ACP-8 | libuuid dependency decision (use for client-side run_id) | ✅ |
| ACP-9 | stdio policy (user accepted `std::cerr` for demos-only) | ✅ |
| ACP-10 | Convert test to doctest (13 cases / 50 assertions) | ✅ |
| ACP-11 | `create_run` hardcoded `RunStatus::Queued` (now reads server response) | ✅ |
| ACP-12 | `get_session` parses `messages` field | ✅ |
| ACP-13 | `is_connected` checks 2xx (not `<500`) | ✅ |
| ACP-14 | `get_agent` 404 returns `AcpError` (not silent default manifest) | ✅ |
| ACP-15 | Long error messages in SSE: truncate to 1024 chars + `…[truncated]` | ✅ |
| ACP-16 | `session_id` in `RunCreateRequest::to_json` was no-op (was empty `if` body) | ✅ |

---

## Demos — all clean (2026-07-16)

- **daedalus-subagent:** hardcoded `/home/ubuntu/.daedalus/skills/` → `Paths::global()`; explicit `daedalus_home`; child `session_id` collision fix (monotonic counter); `> ` prompt; expanded `--help`.
- **daedalus-manager:** same hardcoded path fix; explicit `base.daedalus_home`; expanded `--help`.
- **daedalus-webui:** same path fix; `daedalus_home = user_root()` (NOT `user_data_root`) so `$DAEDALUS_HOME`/`--home` both route to user's chosen model store; `--home` flag actually honoured via `setenv(DAEDALUS_HOME)`; unknown options exit 2; full `--help` rewrite with endpoint table; CORS tightened to loopback-only with `--cors-origins` override.

**Open (not yet fixed):** none.

## ACP end-to-end test (2026-07-16)

`test_acp_end_to_end` (4 cases / 42 assertions, all pass in ~2s):
spins up an in-process OmniNet HTTP server, registers a chunked-SSE
endpoint at `/v1/runs/<id>/events`, emits the 11 spec ACP event types
in order, then connects with `AcpClient` and verifies the right
`IRunEventHandler` callbacks fire. Covers the full lifecycle
(run.created → message.created → message.part ×2 → message.completed
→ generic → run.completed), error events (run.created → error),
and run.failed / run.cancelled routing.

Key gotcha: the mock server must emit `Transfer-Encoding:
chunked`-framed data (`<hex size>\r\n<data>\r\n`), not raw SSE
frames. Without the chunked wrapper the client decoder reports
EOF immediately and the test sees `calls.size() == 0`. The fix
matches `SSEHandler::writeChunked()`'s on-the-wire format.

---

## ACP transport rename + stub fix (2026-07-16)

- `create_curl_transport` → `create_omni_transport` (the old name was a lie — it returned `OmninetTransport`).
- `AcpClient::stream_run_events` was returning `AcpError{501, "not implemented"}` (a leftover stub). Now correctly delegates to `transport_->stream_events()` (send_streaming + SseParser + dispatch_sse_event).
- Test updated: "stream_run_events returns AcpError 501" → "stream_run_events returns success on unreachable server".

---

## §B2-fix — Remove legacy `.codemax[-<name>]/` paths (2026-07-16) ✅

`.codemax[-<name>]/` was leftover from a previous tool with the same
name as the user — never a previous name for daedalus. User
explicitly banned any `.codemax` references in the daedalus library.

- [x] Strip legacy fallback in `src/library/project_paths.cpp` (constants + branch removed)
- [x] Update doc comments in `include/daedalus/{project_paths.h,Agent.h,types/AgentConfig.h}` and `src/library/default_paths.cpp`
- [x] Delete legacy-fallback tests in `tests/test_project_paths.cpp`
- [x] Update `tests/CMakeLists.txt` and `tests/test_no_content_duplicate.cpp` comments
- [x] Delete codemax sections in `docs/storage.md` + `docs/migration.md`
- [x] Update `docs/network-tools.md` (3× GitHub URLs `leestripp/codemax` → `leestripp/daedalus`)
- [x] `grep -rn codemax src/ include/ tests/` → 0 hits (only `demos/daedalus-cli/system_prompt.txt` example remains — false positive, "pkill -f codemax" is a teaching example for the LLM)

---

## Completed (kept for history)

One-liner each. Test count at end of each block shows when work
completed.

### Bug sweep round 2 (F0–F8, 2026-07-15)

- F0 banner underflow; F1 `/opt/daedalus` unprivileged test; F2 `matches_pattern()` dead; F3 empty DEBUG block; F4 AgentImpl unread ctor fields; F5 missing `[[deprecated]]`; F6 Readme/embedding docs; F7 mcp_server.md; F8 storage/migration docs. → 59/61 pass + 2 live-LLM skipped.

### set_status tool (2026-07-15)

`SetStatusTool` added; LLMClient fires `on_state_change(prev, current)`. 20/20 pass, 110 assertions.

### ask_user tool removal (2026-07-15)

`AskUserTool`, `request_user_input()`, `AwaitingUser` removed from all files; tests updated.

### SQLite settings migration (v0.3.0 → v0.4.0)

- v0.3.0: SettingsStore (SQLite) + JSON fallback + 9/9 tests
- v0.3.0: Wire into SettingsManager.load()
- v0.3.1: Auto-migrator (JSON → SQLite on first run)
- v0.3.5: Session memory + messages into DB
- v0.4.0: Cutover — SettingsManager SQLite-only (5/5 tests)

### Tool progress (C1, C2, C2.5)

`ToolProgressFn` added to `BaseTool::execute`; BashTool emits starting/executing; wired into NetworkTool, McpProxiedTool, ReadFileTool; HttpRequestTool byte-level progress (0/25/50/75/100%).

### Webui tool panel (D1, D2)

Structured tool-call cards keyed by `call_id` (3-arg sink overloads); counter widget `[2 pending · 1 ok]`; collapse/expand on long results.

### Webui smoke tests (F1, E1)

Node.js DOM-mock smoke (29 assertions); Playwright headless Chromium (30 assertions).

### CurlMultiSession fixes

B10: non-2xx body discarded → `CurlResponse` part of `expected` on HttpStatus. B16: in-flight `RequestState` leaked on shutdown → `in_flight_` vector tracks active handles, shutdown cancels each before draining CURLMSG_DONE queue.

---

## §CRASH-RESILIENCE — Crash-safe message persistence (2026-07-16)

**Problem.** When the Agent process crashes (SIGSEGV, SIGABRT, Ctrl-C
during a turn, power loss, OOM kill), we lose the current session's
messages — including the user's last input, the assistant's partial
reply, and any tool results from the in-flight turn. The user has to
re-paste their prompt and lose context.

**Goals.**
1. Survive abnormal termination with messages preserved up to the last
   well-formed write boundary.
2. Survive graceful termination (SIGINT, SIGTERM) cleanly with all
   messages flushed before exit.
3. Detect and discard a final message that may have caused the crash
   (heuristic: if the file fails to load after a crash, try without the
   last message).
4. Never leave a half-written `messages.json` that corrupts the next
   session — write must be atomic.
5. Zero regression to existing test suite (64/64 must stay green).

**Non-goals.**
- Full state checkpointing (session memory, MCP state, etc.). Only
  `messages.json` is in scope for v1. Session memory has its own
  per-write persistence (already in place).
- Performance optimisation. The current save() is fine for our scale
  (≤ 1000 messages per session); we don't need incremental saves.

---

### Phase 1 — Atomic save via temp + rename (foundation)

Today `Messages::save()` writes directly to `messages_file_` via
`std::ofstream`. A kill -9 mid-write produces a truncated file; next
`load()` reads it, fails to parse, and falls back to an empty session.
Fix: write to `<file>.tmp`, `fsync`, then `rename()` (atomic on POSIX).

**Changes:**
- `src/core/Messages.cpp` `save()`: write to `messages_file_ + ".tmp"`,
  flush + `fsync(fileno(f))`, `f.close()`, `fs::rename(tmp, final)`.
  The existing empty-file-removal path stays.
- Mirror to SQLite is best-effort as before; on SQLite failure we still
  have the JSON file (which is the new source of truth post-rename).
- Idempotent on partial rename: if `messages.json.tmp` already exists
  from a previous crashed save, overwrite it.

**Tests (added to `tests/test_messages.cpp`):**
- `Messages::save produces a valid file even when previous file was truncated`
- `Messages::save replaces existing file atomically (no partial writes visible)`
- `Messages::save never leaves a .tmp file behind on success`

**Status:** ✅ Done (2026-07-16). Atomic temp+rename + .crash mirror wired in; 4 new CRASH-1 tests pass (`messages_tests` 60/60).

### Phase 2 — Auto-save on every mutation (write-through)

Today `save()` is called explicitly at end-of-turn. Between turns, a
crash loses everything. Fix: every `add_*()` call marks a dirty flag
and a debounced flush writes within ~100 ms (or synchronously when
the agent enters a blocking wait / readline prompt).

**Changes:**
- `Messages` class gains `dirty_` flag (`std::atomic<bool>`).
- Each `add_*` sets `dirty_ = true`.
- New `Messages::flush_if_dirty()` writes via the atomic Phase 1 path.
- Background thread (`std::jthread` or our existing `ThreadPool`)
  wakes every 100 ms, calls `flush_if_dirty()`. Lifetime tied to the
  `Messages` object (thread is a member, joined in destructor).
- `add_user()` triggers an *immediate* synchronous flush BEFORE
  dispatching to the LLM. Rationale: if the LLM call hangs or crashes,
  at least the user's prompt is preserved.
- `add_assistant()` and `add_tool_result()` use the background flush.

**Tests:**
- `Messages::flush_if_dirty writes when dirty; no-op when clean`
- `Messages::add_user flushes synchronously before returning`
- `Background flush runs within 100 ms of marking dirty`
- `Background flush thread joins cleanly on destruction`

**Status:** ✅ Done (2026-07-16). Background flush worker wired in; 4 new CRASH-2 tests pass (`messages_tests` 64/64 total, 185/185 assertions).

### Phase 3 — Graceful shutdown hooks (SIGINT, SIGTERM, SIGTSTP)

Today `demos/daedalus-cli/util.h` already installs SIGINT/SIGTERM
handlers that set `g_shutting_down = true` and call `Agent::cancel()`.
The agent loop sees `g_shutting_down` and exits. **But it does not
flush messages first.**

**Changes:**
- Extend the existing `daedalus_signal_handler` to also call
  `Messages::flush_now()` on the active messages object before the
  loop exits.
- Register a SIGTSTP (Ctrl-Z) handler that does the same flush, then
  re-raises SIGTSTP with the original handler so the process actually
  stops (user can `fg` to resume). This means a Ctrl-Z in the middle
  of typing a prompt saves the prompt + any in-progress tool calls.
- `Agent::cancel()` already triggers the agent loop to return; add a
  `Messages*` to the cancel context so the flush happens deterministically
  (instead of relying on the signal handler racing the loop).

**Tests:**
- `Agent::cancel flushes messages before returning` (synchronous path)
- Signal handler integration is covered by the existing
  `cli_system_prompt_tests` smoke test; add a new one that raises
  SIGINT mid-turn and asserts messages.json was written.

**Status:** ✅ Done (2026-07-16). `validate_message_for_recovery()` + `load_from_crash_file()` helpers; 5 new CRASH-5 tests pass (73/73 + 1 skipped).

### Phase 4 — Crash handler (SIGSEGV, SIGABRT, SIGFPE, SIGILL)

This is the hard one. Signal handlers can run with arbitrary locks
held; only `write(2)`, `_exit(2)`, `signal(2)`, and a handful of others
are async-signal-safe per POSIX. We can't safely call `Messages::save()`
(which allocates, calls into SQLite, etc.) from inside the handler.

**Design: pre-opened fd + signal-safe write of pre-serialised bytes.**

1. `Messages::save_async_safe()` builds the JSON string in normal
   process context and `write()`s it to a pre-opened fd
   (`messages_file_.string() + ".crash"`). The fd is opened once in
   the `Messages` ctor and held open for the lifetime of the object.
2. On every dirty flush, also rewrite the `.crash` file. This costs
   one extra disk write per flush but keeps the file always-current.
3. The signal handler does ONLY: `write(fd, contents, len)`, then
   re-raise the signal so the user gets their core dump / stack trace.
   No allocations, no mutexes, no stdlib.

**Crash recovery on next launch:**
- `Messages::load()` checks for `<messages.json.crash>`.
- If present AND `messages.json` is missing/corrupt, rename `.crash`
  → `.json` and use it.
- If both present, prefer `.json` (it was last fully-written via
  atomic rename); log that `.crash` was stale and discard.

**Signal handler installation:**
- New `Messages::install_crash_handlers()` static method.
- Installs SIGSEGV/SIGABRT/SIGFPE/SIGILL via `sigaction` with
  `SA_NODEFER | SA_RESETHAND` (one-shot, then re-raise default action).
- Idempotent — calling twice is a no-op.
- Demos call this from `install_signal_handlers()` in util.h.

**Tests:**
- Unit: `Messages::save_async_safe writes the JSON string and syncs`
- Unit: `Messages::load recovers from messages.json.crash when messages.json missing`
- Integration: `fork + kill -11 child; parent checks messages.json.crash contains the expected messages`

**Status:** ✅ Done (2026-07-16). Pre-opened fd + crash payload cache + signal handler for SIGSEGV/SIGABRT/SIGFPE/SIGILL. 5 new CRASH-4 tests pass (`messages_tests` 73/73 + 1 skipped, 203/203 assertions). In-process fork test deferred to standalone binary — libc `abort()` flushes stdio which fights with doctest's SIGABRT handler.

### Phase 5 — "Drop last message" heuristic

The user wants to optionally discard the last message on crash, since
it may have caused the crash (e.g. malformed JSON in a tool result
caused a parser crash on the next add).

**Design:** after crash recovery loads `messages.crash`, validate every
message in the chain (must parse, must have `role` field, must
reference existing `tool_call_id`s). If the last message fails any
validation AND the second-to-last is well-formed, drop the last message
and re-save. This is a defensive heuristic — the only way to truly
know is a debugger — but it covers the common case of a tool-result
parser crash on bad upstream data.

**Tests:**
- `Messages::load with corrupt last message drops it and re-saves`
- `Messages::load with corrupt middle message (not last) leaves as-is` (we don't try to be too clever)

**Status:** ⏳ Planned.

---

### Implementation order

1. **Phase 1** — atomic save. Pure refactor, no API change. Land first
   so subsequent phases build on a solid foundation. ✅
2. **Phase 2** — auto-save. Adds the dirty flag + background thread. ✅
3. **Phase 3** — graceful shutdown hooks. Wires into existing
   `install_signal_handlers` in `demos/daedalus-cli/util.h`. ✅ (covered
   by Phase 2 — auto-flush + destructor's worker-stop-and-flush is
   sufficient; no separate code path needed)
4. **Phase 4** — crash handler. Most complex; depends on Phase 1
   (atomic rename) and Phase 2 (always-fresh `.crash` file). ✅
5. **Phase 5** — drop-last heuristic. Final polish; depends on Phase 4. ✅

### Final diff (vs base)

- `Messages.{h,cpp}`: ~250 lines added, ~30 modified.
- `tests/test_messages.cpp`: ~280 lines (14 new test cases).
- `Agent.{h,cpp}` + `AgentImpl.{h,cpp}` + `LLMClient.{h,cpp}`:
  forward crash-handler install/uninstall through the stack.
- `demos/daedalus-cli/main.cpp`: ~5 lines (call `install_crash_handlers`
  after `initialize`).
- `CodeMax.md`: this plan + per-phase status updates.

### Test counts

- Before: 64/64 ctest pass.
- After: 64/64 ctest pass + 14 new messages_tests (73/73 + 1 skipped in
  messages_tests; 203/203 assertions).

### What was deferred

- **In-process fork test of the SIGABRT signal handler** — libc's
  `std::abort()` flushes stdio streams (not async-signal-safe), which
  fights with doctest's own SIGABRT handler. The unit tests cover the
  components; end-to-end verification needs a standalone test binary
  outside the doctest framework. Placeholder test marked
  `doctest::skip(true)`.
- **Ctrl-Z (SIGTSTP) handler** — Phase 3 was simplified: instead of a
  SIGTSTP handler that flushes + re-raises, we rely on the auto-flush
  worker in Messages. A SIGTSTP stops the process; on resume, the
  worker resumes too. No separate handler needed.

---

## Project layout

- `/workspace/daedalus` — C++ agent library + demos (CMake build)
- `/workspace/Testing` — manual + automated test suites for the demos
