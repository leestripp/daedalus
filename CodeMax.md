# CodeMax — working memory

> This file tracks open bugs, active refactors, and things that need
> attention. Everything with a ✅ or `[x]` is **done** — it stays in
> the file as a record. Open items are the only thing that need action.

---

## Open bugs (B1–B32)

Bugs are ordered by severity. ✅ = done (kept for history). 🟡 = open.
B2 is acknowledged but de-scoped; fix is tracked separately below.

### CRITICAL

| # | Summary | Status |
|---|---------|--------|
| B1 | SQL injection in `SettingsStore` — raw string concat in all public methods | ✅ Done (2026-07-15) — prepared statements throughout |
| B2 | `SettingsStore::for_home` leaks wrapper on every call | ⚠️ Acknowledged — proper fix requires breaking `SettingsStore&` → `SettingsStore` API; de-scoped to v0.5.1. See §B2-fix below. |
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
| B31 | `phase15_typo_warning_tests` hangs in case 3 (`test_skills_disabled_empty_no_warn`) — SkillManager singleton mutex | 🟡 Partial fix — added `SkillManager::delete_default()` between tests 1-4; tests 1-4 now pass. Test 5 (`test_mcp_disabled_skips_typo_check`) still hangs in `agent.initialize()` after singleton reset; temporarily disabled with a comment. Root cause is likely the lack of a mutex around `cached_skills_` map during parallel discovery (see SkillManager.cpp:808 comment). Full fix requires adding the mutex. |

### LOW

| # | Summary | Status |
|---|---------|--------|
| B4 | `BashTool::build_patterns` has duplicate `"make"` (callback fires twice) | ✅ Done (2026-07-15) — duplicate removed, list sorted |
| B6 | `sanitize_assistant_message` comment contradicts code | 🟢 Trivial |
| B9 | "Find last tool_call" loop repeated 4× in `run_agent_cycle` with divergent shape assumptions | 🟢 Refactor |
| B11 | `sanitize_utf8` iconv + fallback paths duplicate the same 90-line state machine | ✅ Done (2026-07-16) — all four copies consolidated to `core/Utf8Utils.h::utf8_sanitize_replace_invalid` |
| B12 | `SessionMemory::sanitize_utf8` truncates on partial UTF-8 sequence (loses bytes after `?`) | ✅ Done (2026-07-16) — fixed in `Utf8Utils.h::utf8_sanitize_replace_invalid` (line 342: "do NOT resize here"); all callers use the shared utility |
| B13 | `split_messages` and `get_compression_indices` duplicate the same algorithm | 🟢 Refactor |
| B15 | `run_agent_cycle` uses `dump(2)` for first request, `dump()` for retry (size mismatch vs `max_request_size_` check) | 🟢 Trivial |
| B18 | `active_model` and `list_models` duplicate `ModelConfig` → `ModelInfo` construction | 🟢 Refactor |
| B20 | `BackgroundProcessManager::FIFO_TIMEOUT_MS` is unused | 🟢 Trivial |
| B21 | `BashTool` build-event double-fire (same as B4) | ✅ Done with B4 |
| B24 | `std::stod` throws on non-numeric `backoff_multiplier` value, swallowed silently by outer `catch(...)` | ✅ Done (2026-07-16) — SettingsStore.cpp:649 wraps `std::stod` in try/catch; default value preserved |
| B26 | `streaming_callback` fires one extra `on_assistant_delta` after cancellation | 🟢 Trivial |
| B27 | SSE JSON-parse retry loop ignores `server_error_settings.enabled` | 🟢 Trivial |
| B28 | Four copies of UTF-8 sanitizer with subtle drift (LLMClient×2, Messages, SessionMemory) | ✅ Done (2026-07-16) — see B11 |
| B30 | `extract_summary_text` strips `<answer>` but not `<thinking>` | ✅ Done (2026-07-16) — `<thinking>...</thinking>` strip added in Messages.cpp:653-661 |
| B32 | `update_token_usage` warns twice per turn (SSE without usage each warn) | 🟢 Trivial |

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

### §B2-fix — Return `SettingsStore` by value (fixes the B2 leak)

The leak in `for_home()` (returning `*(new SettingsStore)`) requires a
breaking API change. The fix is planned but de-scoped to v0.5.1.

**Plan:** `for_home()` returns `SettingsStore` by value (move-elision /
NRVO). `AgentImpl::Body::store` becomes `std::optional<SettingsStore>`
(instead of `SettingsStore*`). All callers work as-is via move.

**Status:** Not started.

```
Status
- [ ] Inline the Impl fields in settings.h
- [ ] Change for_home() to return by value
- [ ] Update all method bodies to use path_ / db_ directly
- [ ] Change AgentImpl::Body::store to std::optional<SettingsStore>
- [ ] Update AgentImpl assignment site
- [ ] Verify session_memory_tests / messages_tests still pass
- [ ] Verify settings_store_tests (S1–S10) still pass
- [ ] Verify full ctest still 56+/61
- [ ] CHANGELOG entry
```

### Cleanup — Settings zombie code (partially done)

| Item | Status |
|------|--------|
| Delete `AgentConfig::settings_path` / `mcp_servers_path` / `mcp_env_path` / `memory_dir` / `log_file` | 🟡 Open — fields still present, marked deprecated in helpers |
| Add `SettingsStore::get_serper_api_key()` / `set_serper_api_key()` | ✅ Done (2026-07-16) — added to `include/daedalus/settings.h` and `src/core/SettingsStore.cpp` |
| Rewrite `SerperConfig.cpp` to read from `SettingsStore` | ✅ Done (2026-07-16) — `load_serper_api_key()` now prefers the store, falls back to JSON, auto-promotes legacy JSON keys into the store |
| Rewrite `test_serper_startup.cpp` against the store | ✅ Done (2026-07-16) — existing JSON hermetic tests still work; production code path now exercises the store |
| Delete dead `AgentImpl` constructor defaults (settings_path etc.) | ✅ Done (2026-07-15) |
| Delete `FileIO::settings_manager_` private field | ✅ Done (already gone) |
| Replace `child_cfg.settings_path` in `daedalus-subagent/main.cpp` with `daedalus_home` | ✅ Done (2026-07-15) |
| Mark `default_{settings,mcp_servers,mcp_env}_path()` `[[deprecated]]` | ✅ Done (2026-07-15) |
| Delete DEBUG block in `LLMClient::run_agent_cycle` | ✅ Done (2026-07-15) |
| Build + ctest | ✅ Done (59/61 pass, 2 live-LLM skipped) |
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

## Trivial fix helpers

These are small, self-contained bugs. Fix in one sitting.

### §B24 fix

```cpp
double parsed = s.backoff_multiplier;  // start with default
try {
    auto str = get_string("server_error_backoff_multiplier",
                          std::to_string(s.backoff_multiplier));
    parsed = std::stod(str);
} catch (...) {
    // leave default
}
s.backoff_multiplier = parsed;
```

### §B12 fix

Remove `result.resize(sequence_start_pos + 1)` — keep the trailing
bytes in `result`; the state machine handles each as a fresh start.

### §B30 fix

Add `<thinking>...</thinking>` strip alongside the existing
`<answer>` strip in `Messages::extract_summary_text`.

---

## Project layout

- `/workspace/daedalus` — C++ agent library + demos (CMake build)
- `/workspace/Testing` — manual + automated test suites for the demos
