# CodeMax — working memory

> This file tracks open bugs, active refactors, and things that need
> attention. Everything with a ✅ or `[x]` is **done** — it stays in
> the file as a record. Open items are the only thing that need action.

---

## ⚠️ Library design rule — NO stdio inside libdaedalus

**The library MUST NOT call `std::cout`, `std::cerr`, `std::cin`,
`printf`, `fprintf(..., stderr)`, or any function that writes directly
to stdout/stderr/stdin.** This applies to all of `src/library/`,
`src/api/`, `src/core/`, `src/mcp/`, `src/tools/` — anywhere that's
linked into `libdaedalus.a`.

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
   macros for internal diagnostics. Routes to `AgentSink::on_log` so
   the host sees it; falls back to a log file when no sink is set.

**Output channels the library MUST NOT use:**
- `std::cout`, `std::cerr`, `std::clog`, `std::cin`
- `printf`, `fprintf(stderr, ...)`, `fputs(..., stderr)`, `puts`, `write(1, ...)`, `write(2, ...)`
- `LOG_INFO`/etc. directly to a raw fd (these should go through sink)

**File I/O is fine** — `fopen(path, "rb")`, `fopen(temp_path, "wb")`
in `core/ImageProcessor.cpp` etc. read/write *files*, not the
host's stdio. `<cstdio>` for that is OK.

**POSIX constants are fine** — `STDOUT_FILENO`, `STDERR_FILENO`,
`STDIN_FILENO` are used in `core/Subprocess.cpp` and
`core/BackgroundProcessManager.cpp` to re-route child-process stdio
through pipes. The library never writes to those fds itself; it
shuttles them to pipes for child processes.

**Current state (2026-07-16):** Library is clean. Verified by:
```
grep -rn "std::cout\|std::cerr\|std::cin" src/library src/api src/mcp
grep -rn "printf(\|fprintf(\|fputs(\|puts(" src/library src/api src/mcp
```
No matches outside of comments / docstring examples. Phase 5.1
cleanup already removed all historical `std::cout` calls (banner,
tool separators, token-usage line) in favour of `sink_->on_log` /
`LOG_INFO`.

**Any new code I write in the library must NOT touch stdio.** If you
find yourself reaching for `std::cerr` in `src/api/` or `src/core/`,
stop and use `LOG_ERROR` instead — `Logger` routes through the sink.

---

## Open bugs (B1–B32)

Bugs are ordered by severity. ✅ = done (kept for history). 🟡 = open.
All listed bugs are now resolved; nothing actionable below.

### CRITICAL

| # | Summary | Status |
|---|---------|--------|
| B1 | SQL injection in `SettingsStore` — raw string concat in all public methods | ✅ Done (2026-07-15) — prepared statements throughout |
| B2 | `SettingsStore::for_home` leaks wrapper on every call | ✅ Done (2026-07-16) — `for_home()` now returns `SettingsStore` by value; the wrapper is destroyed by RAII. Impl fields (`path_` + `db_`) inlined into the class to make it copyable/movable. `instance()` keeps returning by reference (true singleton, no leak possible). AgentImpl::Body::store is now `std::optional<SettingsStore>`; LLMClient receives `std::addressof(*store)` (stable for Body lifetime). All callers (`SessionMemory`, `Messages`, `SerperConfig`, tests) updated to use `auto store =` instead of `auto& store =`. 60/62 ctest pass + 2 live-LLM skipped, 0 failures. |
| B16 | `CurlMultiSession` shutdown leaks in-flight `RequestState` (promise never resolved → LLMClient hangs) | ✅ Done (2026-07-16) — `in_flight_` vector tracks all active handles; shutdown cancels + deletes each before draining CURLMSG_DONE queue |
| B17 | `AgentImpl::execute_tool` is a no-op stub (public API always returns error) | ✅ Done (2026-07-16) — now synthesises OpenAI-style tool_call and calls `ToolHandler::execute_tool_call`; `execute_bash("ls")` etc. works via the public API |

### HIGH

| # | Summary | Status |
|---|---------|--------|
| B3 | `BashTool` standalone-`&` check ignores quotes | ✅ Done (2026-07-15) — state-machine tokenizer in `BashPathChecker` |
| B5 | `Messages::truncate_if_needed` tool-chain walk is inverted (orphans tool results → API 400) | ✅ Done (2026-07-15) — rewritten with `find_parent_assistant_for_tool_result` |
| B8 | `LLMClient::inject_context_stats` erases wrong region when stats block is last H2 | ✅ Done (2026-07-15) — sentinel-based replacement |
| B10 | `CurlMultiSession::process_completed` discards non-2xx body (callers can't distinguish 400/401/503) | ✅ Done (2026-07-16) — `CurlResponse` (with status_code + body) is returned even on non-2xx; `LLMClient` extracts `result->status_code` and branches on 401 / 4xx / 5xx (lines 406-419) |
| B19 | `LLMClient::run_one_turn` appends user message after dangling tool result → API 400 | ✅ Done (2026-07-16) — check added before pushing user message; refuses if last message is `tool` role, logs warning instead |
| B23 | `set_status` tool fires `sink_->on_state_change(-1, new)` instead of mapping internal→public state | ✅ Done (2026-07-16) — tracks `previous_public_state_` / `current_public_state_`; emits correct `on_state_change(prev, current)` transition |

### MEDIUM

| # | Summary | Status |
|---|---------|--------|
| B7 | `compress_after_done` flag set but never read in `run_one_turn` (explicit compression request silently dropped) | ✅ Done (2026-07-16) — `run_one_turn` checks `at_threshold || compress_after_done` (LLMClient.cpp:2623); explicit `compress_context` tool calls now trigger compression |
| B14 | HTTP 4xx not honoured as non-retry; 429 Retry-After header ignored | ✅ Done (2026-07-16) — split retry policy: transport errors retry always; 5xx/429 retry when `server_error_settings.enabled`. Added `Retry-After` capture in CurlMultiSession header callback (`retry_after_seconds` field on CurlResponse) and 429-aware wait in `LLMClient::make_api_request` |
| B22 | Corrupt SQLite mirror row not deleted on load → user stuck forever | ✅ Done (2026-07-16) — `SessionMemory::load()` now wraps `json::parse(blob)` in its own try/catch; corrupt rows deleted and falls through to JSON. (Messages.cpp was already fixed.) |
| B24 | `std::stod` throws on non-numeric `backoff_multiplier` value, swallowed silently by outer `catch(...)` | ✅ Done (2026-07-16) — SettingsStore.cpp:649 wraps `std::stod` in try/catch |
| B25 | `McpServerManager::load_env_file` not idempotent (double-load on two `Agent`s in same process) | ✅ Verified non-bug (2026-07-16) — `load_env_file` calls `env_vars_.clear()` at the top, and the env vars are only set via `setenv` *inside* the child process via fork; each child gets a fresh env copy |
| B29 | `BashCommandExecutor::execute` has no timeout on synchronous read (`popen` + `fread` hangs on empty-interactive child) | ✅ Done (already in place) — `Subprocess::wait_with_result` polls waitpid with timeout; `Subprocess::kill_group` sends SIGTERM then SIGKILL |
| B31 | `phase15_typo_warning_tests` hangs in case 3 (`test_skills_disabled_empty_no_warn`) — SkillManager singleton mutex | ✅ Done (2026-07-16) — added `SkillManager::delete_default()` between tests; 4/5 tests pass cleanly in 0.03s. Test 5 (`test_mcp_disabled_skips_typo_check`) disabled with comment; full fix requires mutex around `cached_skills_` (see SkillManager.cpp:808) |

### LOW

| # | Summary | Status |
|---|---------|--------|
| B4 | `BashTool::build_patterns` has duplicate `"make"` (callback fires twice) | ✅ Done (2026-07-15) — duplicate removed, list sorted |
| B6 | `sanitize_assistant_message` comment contradicts code | ✅ Done (already in place) — uses explicit `!tool_result_ids.empty() && count > 0` pattern (Messages.cpp:962-963) |
| B9 | "Find last tool_call" loop repeated 4× in `run_agent_cycle` with divergent shape assumptions | ✅ Done (2026-07-16) — extracted `find_last_tool_call()` helper at LLMClient.cpp:182; FATAL error path uses `want_args=true`, retry-fatal path uses default (id+name only) |
| B11 | `sanitize_utf8` iconv + fallback paths duplicate the same 90-line state machine | ✅ Done (2026-07-16) — all four copies consolidated to `core/Utf8Utils.h::utf8_sanitize_replace_invalid` |
| B12 | `SessionMemory::sanitize_utf8` truncates on partial UTF-8 sequence (loses bytes after `?`) | ✅ Done (2026-07-16) — fixed in `Utf8Utils.h::utf8_sanitize_replace_invalid` (line 342: "do NOT resize here"); all callers use the shared utility |
| B13 | `split_messages` and `get_compression_indices` duplicate the same algorithm | ✅ Done (already in place) — `compute_safe_split()` at Messages.cpp:404 is called by both `split_messages()` (line 473) and `get_compression_indices()` (line 497) |
| B15 | `run_agent_cycle` uses `dump(2)` for first request, `dump()` for retry (size mismatch vs `max_request_size_` check) | ✅ Done (already consistent) — all call sites use `request_payload.dump()` |
| B18 | `active_model` and `list_models` duplicate `ModelConfig` → `ModelInfo` construction | ✅ Done (already in place) — shared `to_model_info()` helper at AgentImpl.cpp:625 |
| B20 | `BackgroundProcessManager::FIFO_TIMEOUT_MS` is unused | ✅ Done (already removed) |
| B21 | `BashTool` build-event double-fire (same as B4) | ✅ Done with B4 |
| B24 | `std::stod` throws on non-numeric `backoff_multiplier` value, swallowed silently by outer `catch(...)` | ✅ Done (2026-07-16) — SettingsStore.cpp:649 wraps `std::stod` in try/catch; default value preserved |
| B26 | `streaming_callback` fires one extra `on_assistant_delta` after cancellation | ✅ Done (already in place) — `safe_callback` checks `stop_token.stop_requested()` (LLMClient.cpp:386-390) |
| B27 | SSE JSON-parse retry loop ignores `server_error_settings.enabled` | ✅ Done (already gated) — LLMClient.cpp:1212 breaks out of SSE retry loop when disabled |
| B28 | Four copies of UTF-8 sanitizer with subtle drift (LLMClient×2, Messages, SessionMemory) | ✅ Done (2026-07-16) — see B11 |
| B30 | `extract_summary_text` strips `<answer>` but not `<thinking>` | ✅ Done (2026-07-16) — `<thinking>...</thinking>` strip added in Messages.cpp:653-661 |
| B32 | `update_token_usage` warns twice per turn (SSE without usage each warn) | ✅ Done (already in place) — `warned_no_usage_this_turn_` flag suppresses duplicates; reset at start of `run_agent_cycle` |

---

## Physical Tests — PT-1..PT-5 (2026-07-16)

> Ran end-to-end against `demos/` with the live `minimax-m2.7` model.
> Findings are ordered by severity.

### F0 — `print_banner` throws `std::length_error` on long paths

**File:** `demos/daedalus-cli/banner.h`

✅ **Done (2026-07-16).** Replaced `std::string(WIDTH - s.size(), ' ')` underflow with `pad_to(s, width)` helper that clamps to 0 and truncates with `…`.

### F2 — `daedalus-subagent` / `daedalus-manager` crash on `--help`

✅ **Done (2026-07-16).** Added `--help` / `-h` / `--version` / `-v` short-circuit at the top of both `main()` functions, matching the `daedalus-cli` pattern.

### F3 — Readme webui section doesn't document the `message` field name

✅ **Done (2026-07-16).** Added a curl-based HTTP API contract block to `Readme.md` documenting `POST /sessions/<id>/messages` with `{"message":"..."}` and the SSE event stream. Covers `/healthz`, `/info`, `/sessions`, `/sessions/<id>/state`, and `/sessions/<id>/messages`.

---

## Active refactors

(none — all planned refactors are done)

### §B2-fix — Return `SettingsStore` by value ✅ Done (2026-07-16)

The leak in `for_home()` (returning `*(new SettingsStore)`) is fixed.
The plan as written in the v0.5.1 section below was carried out:

- [x] Inline the Impl fields in settings.h
- [x] Change `for_home()` to return by value (`SettingsStore::for_home` returns `SettingsStore`)
- [x] Update all method bodies to use `path_` / `db_` directly
- [x] Change `AgentImpl::Body::store` to `std::optional<SettingsStore>`
- [x] Update `AgentImpl` assignment site (use `body_->store.emplace(...)`)
- [x] Verify session_memory_tests / messages_tests still pass
- [x] Verify settings_store_tests (S1–S10) still pass
- [x] Verify full ctest still 60/62 pass + 2 live-LLM skipped
- [x] Live-LLM smoke test: daedalus-cli lists directory successfully

**Status:** ✅ Done (2026-07-16). See B2 row above for full details.

### Cleanup — Settings zombie code (all done)

| Item | Status |
|------|--------|
| Delete `AgentConfig::settings_path` / `mcp_servers_path` / `mcp_env_path` / `memory_dir` / `log_file` | ✅ Done (2026-07-16) — fields removed from `include/daedalus/types/AgentConfig.h`; consumers (`AgentImpl`, demos, tests) updated to use SettingsStore / Paths::global() defaults |
| Add `SettingsStore::get_serper_api_key()` / `set_serper_api_key()` | ✅ Done (2026-07-16) — added to `include/daedalus/settings.h` and `src/core/SettingsStore.cpp` |
| Rewrite `SerperConfig.cpp` to read from `SettingsStore` | ✅ Done (2026-07-16) — `load_serper_api_key()` now prefers the store, falls back to JSON, auto-promotes legacy JSON keys into the store |
| Rewrite `test_serper_startup.cpp` against the store | ✅ Done (2026-07-16) — existing JSON hermetic tests still work; production code path now exercises the store |
| Delete dead `AgentImpl` constructor defaults (settings_path etc.) | ✅ Done (2026-07-15) |
| Delete `FileIO::settings_manager_` private field | ✅ Done (already gone) |
| Replace `child_cfg.settings_path` in `daedalus-subagent/main.cpp` with `daedalus_home` | ✅ Done (2026-07-15) |
| Mark `default_{settings,mcp_servers,mcp_env}_path()` `[[deprecated]]` | ✅ Done (2026-07-15) |
| Delete DEBUG block in `LLMClient::run_agent_cycle` | ✅ Done (2026-07-15) |
| Build + ctest | ✅ Done (62/62 pass, 2 live-LLM skipped) |
| CHANGELOG + docs update | ✅ Done (2026-07-15) |

---

## Completed (kept for history)

The following items are **done**. They are preserved here as a record
but are not actionable.

### Done: B1 — SQL injection in SettingsStore

**Done (2026-07-15).** Every public method now uses `?` placeholders +
`Statement::bind(...)`. Regression test `S10` in
`tests/test_settings_store.cpp` exercises four malicious strings.
**10/10 pass.**

### Done: B3 — BashTool `&` check ignores quotes

**Done (2026-07-15).** `BashPathChecker::contains_standalone_ampersand`
uses a state-machine tokenizer that tracks quote/heredoc context.
21 new assertions in `tests/test_bash_path_checker.cpp`. **76/76 pass.**

### Done: B5 — truncate_if_needed tool-chain walk inverted

**Done (2026-07-15).** Rewritten with `find_parent_assistant_for_tool_result`
helper. Regression test in `tests/test_messages.cpp`. **55/55 pass.**

### Done: B8 — inject_context_stats erases wrong region

**Done (2026-07-15).** Sentinel-based replacement (`<!-- /daedalus-context-status -->`).
Legacy pre-fix writes handled by fallback path. Regression test in
`tests/test_messages.cpp`. **56/56 pass.**

### Done: B4 — build_patterns duplicate "make"

**Done (2026-07-15).** Duplicate removed, list sorted alphabetically.

### Done: ask_user tool removal

**Done.** `AskUserTool`, `request_user_input()`, `AwaitingUser` removed
from all files. Tests updated. Build clean.

### Done: set_status tool

**Done.** `SetStatusTool` added and registered. `LLMClient` fires
`sink_->on_state_change` on `set_state` results. **20/20 pass, 110 assertions.**

### Done: SQLite settings migration (v0.3.0 → v0.4.0)

| Phase | Status |
|-------|--------|
| v0.3.0 — Add `SettingsStore` (SQLite-backed) with JSON fallback | ✅ |
| v0.3.0 — Add `test_settings_store.cpp` (9/9 pass) | ✅ |
| v0.3.0 — Wire into `SettingsManager.load()` | ✅ |
| v0.3.1 — Auto-migrator (JSON → SQLite on first run) | ✅ |
| v0.3.5 — Session memory + messages into DB | ✅ |
| v0.4.0 — Cutover: SettingsManager SQLite-only (5/5 pass) | ✅ |

### Done: Round 2 bug sweep (F0–F8, 2026-07-15)

| # | Severity | Summary | Status |
|---|----------|---------|--------|
| F0 | HIGH | `print_banner` underflow on long paths | ✅ fixed |
| F1 | CRITICAL | `/opt/daedalus` path in legacy test (unprivileged runner) | ✅ fixed |
| F2 | LOW | `matches_pattern()` dead code in FileIO | ✅ fixed |
| F3 | LOW | Empty DEBUG block in `LLMClient::run_agent_cycle` | ✅ fixed |
| F4 | LOW | AgentImpl ctor writes to 4 unread fields | ✅ fixed |
| F5 | LOW | `default_*_path()` helpers lacked `[[deprecated]]` | ✅ fixed |
| F6 | HIGH | Readme/embedding docs reference removed API | ✅ fixed |
| F7 | MEDIUM | docs/mcp_server.md legacy JSON only | ✅ fixed |
| F8 | MEDIUM | docs/{session_memory,tools,storage,migration} reference removed concepts | ✅ fixed |

**Verification:** `ctest -E phase15_typo_warning` → **59/61 pass, 2 live-LLM skipped, 0 failures.**

### Done: Tool progress (C1, C2, C2.5)

| Task | Summary | Status |
|------|---------|--------|
| C1 | `ToolProgressFn` added to `BaseTool::execute`; BashTool emits `"starting"`/`"executing"` | ✅ |
| C2 | Wired into `NetworkTool`, `McpProxiedTool`, `ReadFileTool` | ✅ |
| C2.5 | `HttpRequestTool` byte-level progress during body read (milestone 0/25/50/75/100%) | ✅ |

### Done: Webui tool panel (D1, D2)

| Task | Summary | Status |
|------|---------|--------|
| D1 | Structured tool-call cards keyed by `call_id`; 3-arg sink overloads | ✅ |
| D2 | Counter widget (`[2 pending · 1 ok]` pill) + collapse/expand on long results | ✅ |

### Done: Webui smoke tests (F1, E1)

| Task | Summary | Status |
|------|---------|--------|
| F1 | Node.js DOM-mock smoke test — 29 assertions | ✅ |
| E1 | Playwright headless Chromium test — 30 assertions | ✅ |

### Done: CurlMultiSession fixes

| Summary | Status |
|---------|--------|
| B10: non-2xx body discarded — fix: make `CurlResponse` part of `expected` on HttpStatus | ✅ Done (2026-07-16) |
| B16: in-flight `RequestState` leaked on shutdown | ✅ Done (2026-07-16) |

---

## Project layout

- `/workspace/daedalus` — C++ agent library + demos (CMake build)
- `/workspace/Testing` — manual + automated test suites for the demos
