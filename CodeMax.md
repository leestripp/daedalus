# CodeMax — working memory

> This file tracks open bugs, active refactors, and things that need
> attention. Everything with a ✅ or `[x]` is **done** — it stays in
> the file as a record (one line per item). Open items are the only
> thing that need action.

---

## Current state (2026-07-16)

- **Build:** clean (only OmniNet-internal warnings).
- **Tests:** `ctest -E phase15_typo_warning` → **62/62 pass**, 2 live-LLM skipped, 0 failures.
- **libcurl:** fully gone from the build system. HTTP/SSE stack is `OmniNet::HTTP::Client`.
- **Open items:** none.

Recent work: §NOMORE-CURL complete (12/12 NC tasks), ACP code review complete (15/15 ACP tasks), demos cleanup (subagent/manager/webui fixed hardcoded paths), NC-9 cosmetic rename `CurlMultiSession` → `HttpSession`.

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
- ✅ B31 phase15_typo_warning_tests hangs in case 3 (SkillManager singleton)

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
- ✅ F2 daedalus-subagent/manager crash on `--help` (2026-07-16, argv short-circuit)
- ✅ F3 Readme webui section doesn't document `message` field (2026-07-16)

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

## ACP (Agent Communication Protocol) — 15/15 ✅

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
| ACP-15 | `CurlTransport` get/post + is_connected split (now single `make_request` path) | ✅ |

---

## Demos — all clean (2026-07-16)

- **daedalus-subagent:** hardcoded `/home/ubuntu/.daedalus/skills/` → `Paths::global()`; explicit `daedalus_home`; child `session_id` collision fix (monotonic counter); `> ` prompt; expanded `--help`.
- **daedalus-manager:** same hardcoded path fix; explicit `base.daedalus_home`; expanded `--help`.
- **daedalus-webui:** same path fix; `daedalus_home = user_root()` (NOT `user_data_root`) so `$DAEDALUS_HOME`/`--home` both route to user's chosen model store; `--home` flag actually honoured via `setenv(DAEDALUS_HOME)`; unknown options exit 2; full `--help` rewrite with endpoint table.

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

## Project layout

- `/workspace/daedalus` — C++ agent library + demos (CMake build)
- `/workspace/Testing` — manual + automated test suites for the demos
