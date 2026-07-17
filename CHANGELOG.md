# Changelog

All notable changes to CodeMax are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [v0.5.0] — Unreleased

Crash-resilient message persistence + libcurl removal + Agent
Communication Protocol (ACP) sub-library.

### Added

- **Crash-resilient message persistence** (`Messages`):
  - **Atomic save** (CRASH-1): every `save()` writes to `messages.json.tmp`,
    `fsync()`s, then `rename()`s over `messages.json`. POSIX `rename()`
    on the same filesystem is atomic — readers never see a half-written
    file. `kill -9` mid-write now leaves the previous fully-written
    `messages.json` intact.
  - **Write-through auto-flush** (CRASH-2): every `add_*()` marks the
    message chain as dirty; a background `std::thread` flushes within
    ~100 ms (configurable via `auto_flush_interval_ms`). Destructor
    stops the worker cleanly and does one final flush.
  - **Async-signal-safe crash handler** (CRASH-4): `Messages::install_crash_handlers()`
    installs `SIGSEGV`/`SIGABRT`/`SIGFPE`/`SIGILL` handlers that write
    `messages.json.crash` from a pre-opened fd using only
    `open`/`write`/`close`/`fsync`, then re-raise so the user still
    gets a core dump / stack trace. Handler iterates a process-global
    registry of `Messages*` instances under a tiny `std::atomic<int>`
    spinlock (no `pthread_mutex` — not async-signal-safe).
  - **Crash recovery** (CRASH-5): `load()` falls back to
    `messages.json.crash` when the main file is missing or corrupt;
    defensively drops the last message if it looks like the cause of
    the crash (via `validate_message_for_recovery()` — checks role,
    tool_call_id, assistant tool_calls arguments JSON), then re-saves
    the recovered chain to `messages.json` atomically.
  - **Production wiring**: `Agent::install_crash_handlers()` /
    `uninstall_crash_handlers()` forward through `AgentImpl` and
    `LLMClient` to `Messages`. `daedalus-cli/main.cpp` calls
    `agent.install_crash_handlers()` after `agent.initialize()`.
  - Full architecture + trade-offs + end-to-end behaviour per crash
    scenario: **`docs/crash_resilience.md`**.

- **Agent Communication Protocol (ACP) sub-library** at
  `demos/daedalus-cli/acp/`. Sub-library, demos-only (uses `std::cerr`
  for diagnostics which would corrupt a TUI host). Enabled via
  `-DDAEDALUS_CLI_ACP=ON` (default OFF).
  - `AcpClient` with structured `AcpResult<T>` error propagation
    (replaces silent default-constructed return values).
  - SSE structured dispatch via `IRunEventHandler` callbacks
    (`on_run_created`, `on_message_part`, `on_run_completed`,
    `on_run_failed`, `on_run_cancelled`, `on_error`, …).
  - `OmniNet`-backed transport: `create_omni_transport()` (was
    `create_curl_transport` — the old name was a lie).
  - `stream_run_events()` was a leftover stub returning
    `AcpError{501,"not implemented"}`. Now correctly delegates to
    `transport_->stream_events()` (send_streaming + SseParser +
    dispatch_sse_event).
  - End-to-end test (`test_acp_end_to_end`, 4 cases / 42 assertions,
    ~2 s) spins up an in-process OmniNet HTTP server with a
    chunked-SSE endpoint, verifies all 11 spec event types route to
    the correct callbacks.

- **Documentation:**
  - `docs/crash_resilience.md` (new) — the conversation-history
    lifecycle story.
  - `docs/library_design.md` (new) — the no-stdio invariant for
    `libdaedalus.a` (formerly in `CodeMax.md`; promoted to a
    permanent spec).
  - `daedalus_cli` body shape for webui POST
    (`{"message":"..."}`, not `{"content":"..."}`) explicitly
    documented in `Readme.md`.

### Changed

- **`CurlMultiSession` → `HttpSession`** rename (12 files, ~172
  references). The class still wraps an HTTP client; the new name
  reflects the actual implementation (OmniNet HTTP, not curl). The
  public API is unchanged.
- **`make_curl_request_with_retry` → `make_http_request_with_retry`**
  in `LLMClient` (sync path). Same behaviour; libcurl no longer in
  the build.
- **`AcpClient::create_curl_transport` → `create_omni_transport`**.
- **`daedalus-webui` CORS**: default `cfg.corsAllowedOrigins`
  tightened from `"*"` to loopback-only
  (`http://localhost`, `http://localhost:8080`, `http://127.0.0.1`,
  `http://127.0.0.1:8080`); new `--cors-origins` CLI flag for
  override.
- **Phase 15 typo-warning tests**: case 5 re-enabled (was disabled
  pending `SkillManager::delete_default()` lifecycle fix B31).
  All 5 cases now pass in 0.03 s.
- **ACP error message handling**: `err.message` and `err.cause` in
  SSE error events truncated to 1024 chars + `…[truncated]` to
  prevent unbounded payloads from a malicious or buggy server
  consuming receiver memory.

### Removed

- **libcurl dependency completely removed** from the build system.
  `find_package(CURL)` and `CURL::libcurl` links gone from
  `CMakeLists.txt`. Zero `curl_easy_*`, `curl_multi_*` calls
  anywhere in `src/`, `demos/`, `tests/`.
- **Deleted** `include/daedalus/net/CurlHandle.h`. File no longer
  referenced after the libcurl removal.
- **Deleted** `tests/test_curl_handle.cpp`. The curl handle unit
  tests are no longer applicable.
- **Deleted** `tests/test_curl_multi_session.cpp`; replaced by
  `tests/test_http_session.cpp` (covers the renamed HttpSession).
- **Legacy `.codemax[-<name>]/` runtime-state paths** completely
  removed. `.codemax` was a leftover from a previous tool with the
  same name as a user; never a previous name for `daedalus`. User
  explicitly banned `.codemax` references in the library. Fallback
  branch and constants removed from `src/library/project_paths.cpp`;
  test cases removed from `tests/test_project_paths.cpp`; doc
  sections removed from `docs/storage.md` and `docs/migration.md`.
  `grep -rn codemax src/ include/ tests/` → 0 hits
  (`demos/daedalus-cli/system_prompt.txt` retains one teaching
  example for the LLM about `pkill -f codemax`).

### Fixed

- **Crash-safe message persistence**: every crash scenario
  (SIGSEGV, SIGABRT, SIGFPE, SIGILL, `kill -9`, power loss, OOM kill)
  now recovers up to the last successfully-`fsync()`'d save().
  Worst-case data-loss window is bounded by the 100 ms auto-flush
  interval.
- **ACP-16**: `RunCreateRequest::to_json()` had an empty `if (req.session_id)`
  block — the `session_id` field was silently dropped from every
  `POST /v1/runs` request body. Now correctly writes
  `j["session_id"] = *req.session_id;`.
- **daedalus-subagent / daedalus-manager `--help` crash** (F2,
  was open in v0.4.6-prep notes). Both demos now short-circuit
  `--help`/`-h` BEFORE positional argument parsing, so `--help`
  prints help text and exits 0 instead of passing `"--help"` as a
  non-existent `project_root` to `Agent::initialize()`.
- **Webui body shape documented** (F3, was open in v0.4.6-prep
  notes). `Readme.md` line 525 now explicitly states
  `/sessions/<id>/messages` expects `{"message": "..."}`.
- **Messages destructor no longer auto-saves** when background flush
  is disabled (per-phase test race fix: tests that wiped the data
  directory between Messages construction and destruction no longer
  get phantom `messages.json` files re-created by the destructor).
- **ACO transport / Message / Run `from_json` / `to_json`
  completeness**: previously `Run.created_at` / `Message.created_at`
  were parsed but not written out, and `from_json(Run&)` discarded
  most fields. Both now round-trip cleanly.
- **`is_connected` check**: previously returned true for any status
  `<500`; now requires `2xx` (the original semantics were a typo).
- **SSE streaming drops the usage-only trailer on slow providers**
  (e.g. MiniMax/M2.7). The streaming loop in
  `HttpSession::submit_cancellable` treated any `nullopt` from
  `OmniNet::HTTP::ClientResponseStream::read_some()` as end-of-stream.
  `read_some()` also returns `nullopt` when `socket->waitForRead(100ms)`
  times out without data — providers whose chunks are spaced
  >100 ms apart hit this and the loop exited after only the first
  2 reads, dropping the `{"choices":[],"usage":{...}}` trailer that
  carries real token counts. Fixed by checking `stream.eof()` before
  breaking on `nullopt`; on transient timeout, sleep 5 ms and
  continue. Symptom was `'usage' is null in SSE stream — token counts
  unavailable` and a zero-token-count UI bar on every turn. Full
  write-up of the SSE consumption pipeline and its gotchas:
  **[`docs/sse_streaming.md`](docs/sse_streaming.md)**.

### Tests

- `messages_tests` 73/73 + 1 skipped (203/203 assertions).
- `test_acp_basic` 40/40 (131/131 assertions).
- `test_acp_end_to_end` 4/4 (42/42 assertions).
- Full `ctest` → 65/65 pass, 2 live-LLM skipped, 0 failures
  (was 64/64 with `phase15_typo_warning_tests` excluded; that
  test is now passing again and excluded flag is no longer needed).

### Migration notes (v0.4.x → v0.5.0)

- **libcurl users**: no API change. `LLMClient`'s sync HTTP path now
  uses OmniNet under the hood. Behaviour is identical.
- **`CurlMultiSession` users**: rename to `HttpSession`. Source
  drops & include paths are unchanged; only the class name moved.
- **`cfg.corsAllowedOrigins = "*"` (webui)**: now defaults to
  loopback-only. Hosts running on a non-loopback origin must pass
  `--cors-origins https://myhost.example` (comma-separated).
- **`.codemax[-<name>]/` users**: those paths no longer work.
  Migrate to `.daedalus[-<name>]/` (the existing canonical namespacing)
  via `daedalus-cli --migrate-data`. If data still exists in the
  legacy path the migrator handles it.
- **Crash recovery**: hosts that previously saw empty sessions after
  a crash now get the recovered conversation back automatically on
  next launch. No host code change required; `Messages::load()` is
  backward-compatible.

---

## [Unreleased]

### Fixed (2026-07-16, v0.4.2 prep — physical test pass)

- **`print_banner` crash on long paths** (HIGH — F0).
  `demos/daedalus-cli/banner.h` padded each field with
  `std::string(WIDTH - s.size(), ' ')`. Whenever `s.size() > WIDTH`
  (e.g. a 59-char `data_dir` on a long project path), the
  subtraction underflowed and the constructor threw
  `std::length_error`, `std::terminate`'d the demo, and aborted
  with `[FATAL] basic_string::_M_create` before the LLM was
  even contacted. Replaced the inline pad with a `pad_to(s, width)`
  helper that clamps to 0 and truncates with ` …` for long values.
  Found during the PT-1 physical test pass; all 4 demos now run
  end-to-end with the live `minimax-m2.7` model.

- **`daedalus-subagent --help` / `daedalus-manager --help` crash**
  (HIGH — F2). Both demos treat `argv[1]` as `project_root` and
  pass it to `Agent::initialize()` without a `--help` short-circuit;
  the demo throws `std::invalid_argument("Project path does not
  exist: --help")` and aborts. Same root cause in both; same
  fix pattern as `daedalus-cli/main.cpp:52-60`. **Fixed in v0.5.0**
  (see above). Tracked in CodeMax.md F2.

### Fixed (2026-07-15, v0.4.2 prep)

- **Test fix:** `legacy_deprecation_warning_tests` case 2
  (`$DAEDALUS_HOME set → no warning`) used `/opt/daedalus` as the
  override path. That fails with `Permission denied` on systems
  where the test runner is unprivileged (CI, podman container,
  any non-root user). Replaced with a temp-dir path so the test
  exercises the override-acknowledgement logic without depending
  on `/opt` being writable. The behaviour assertion — "when
  `$DAEDALUS_HOME` is set, no deprecation warning fires" — is
  unchanged.

- **Dead-code removal:** `matches_pattern()` in `src/core/FileIO.cpp`
  (the simple-glob helper used by the old SettingsManager
  load-ignore path) was never called — `FileIO` no longer takes
  a `SettingsManager*`, the settings pipeline went through SQLite
  in v0.4.0, and no SettingsManager/JSON-driven code reads the
  result. Compiler `-Wunneeded-internal-declaration` flagged it
  on every rebuild. Deleted.

- **DEBUG block:** `LLMClient::run_agent_cycle` had a 35-line
  "DEBUG: Log tool_calls" block gated on a specific `tc_id` and
  `has_invalid_chars` check. Both branches were empty — only
  local-variable assignments, no `LOG_*` call. Deleted.

- **Constructor dead code:** `AgentImpl::AgentImpl` had a 17-line
  block that defaulted `cfg_.settings_path`, `cfg_.mcp_servers_path`,
  `cfg_.mcp_env_path`, `cfg_.memory_dir` from `cfg_.daedalus_home`
  (or `default_*_path()`). Since v0.5.0 the SettingsStore reads
  directly from `cfg_.daedalus_home`; those four `AgentConfig`
  fields are now `[[deprecated]]` and only used as explicit
  per-field overrides (the override read at line 320–324 still
  works). The default-population block was pure write-only. Deleted.

- **Deprecation warnings:** `default_settings_path()`,
  `default_mcp_servers_path()`, `default_mcp_env_path()` now
  carry `[[deprecated("…")]]` annotations pointing at
  `SettingsStore::instance()` and `list_mcp_servers()`. Three
  call sites inside the library still use them (the `migrate.cpp`
  one-shot CLI, plus the daedalus-subagent / daedalus-manager
  demos) — the migration is left for a future release. CMake
  emits the warning cleanly; no errors.

- **Documentation sweep (Readme.md, docs/embedding.md,
  docs/mcp_server.md, docs/session_memory.md, docs/storage.md,
  docs/tools.md, docs/how_compress_works.md,
  docs/migration.md):**
  - Readme.md `StdoutSink` example no longer references the
    removed `AgentSink::request_user_input(std::string_view)`.
  - Readme.md "Configuration" section replaced the hand-edited
    `config.json` walkthrough with a programmatic example using
    `SettingsStore::upsert_model()`, plus a `sqlite3` one-liner
    for packagers.
  - Readme.md "File Locations" table now shows the v0.4.0+ DB
    path (`~/.local/share/daedalus/daedalus.db`).
  - Readme.md "Current tag" bumped from 0.2.5 → 0.4.1.
  - docs/embedding.md `Settings, memory, MCP paths` section
    rewritten to describe the v0.4.0 SQLite model and explicitly
    call out the removed `cfg.settings_path` / `cfg.mcp_*_path` /
    `cfg.memory_dir` fields.
  - docs/embedding.md state-machine table no longer lists
    `AwaitingUser` (removed in v0.4.0 with the `ask_user` tool).
  - docs/embedding.md `Wiring your own input source` section
    no longer claims the agent calls `request_user_input` mid-turn.
  - docs/mcp_server.md: added a "Configuration (v0.5.0+,
    recommended)" section using `SettingsStore::upsert_mcp_server`,
    kept the legacy JSON section as a fallback.
  - docs/session_memory.md "Storage location" tree no longer
    shows `settings.json` — settings live in `daedalus.db` now.
  - docs/storage.md §2.2: removed the `agent_dir/config.json`
    row (config moved to the user-tier SQLite DB).
  - docs/tools.md §14: replaced the removed `ask_user` reference
    with `set_status` (the actual current tool).
  - docs/how_compress_works.md "Per-Model Context Size" now uses
    `SettingsStore::upsert_model()` instead of a JSON snippet.
  - docs/migration.md §5: notes that `settings.json` was moved
    to SQLite in v0.4.0 (the migration guide previously claimed
    "unchanged").

### Added (2026-07-15)

- **Webui "Tool Calls" panel — structured cards** (§Phase D1).
  Each tool invocation now renders as its own card with a header
  (tool name + status icon + stage pill), a collapsed args block,
  a progress bar that updates in real time from `tool_progress`
  events, and a final result block (or error message). Replaces
  the previous flat "→ name ← result" line stream that mixed
  unrelated tool calls together.
  - **Correlation key:** OpenAI-style `call_id` (the same id the
    LLM uses to match tool calls to tool results). Threaded
    through to all three SSE event types via new 3-arg
    `AgentSink` overloads (`on_tool_call(name, call_id, args)`,
    `on_tool_result(name, call_id, result)`,
    `on_tool_progress(name, call_id, progress)`). The 2-arg
    overloads are kept as back-compat; sinks that don't care
    about correlation (ConsoleAgentSink, test stubs) keep
    overriding only those — the 3-arg default delegates.
  - **Card states:** `running` (blue border, indeterminate
    shimmer bar until percent arrives, spinner icon),
    `success` (green border, ✓ icon, result block),
    `error` / `denied` (red border, ✗ icon, error result).
  - **Idempotency:** if a `tool_call` arrives for an id that
    already has a card (e.g. a retried invocation), the args
    are refreshed in place — no duplicate card is created.
  - **Out-of-order tolerance:** `tool_progress` for an unknown
    id is silently dropped (LLMClient emits `tool_call` before
    the tool runs, so this should never happen, but defensive).
  - **SSE event payload changes:**
    `tool_call`, `tool_progress`, `tool_result` now include
    `"id": "<call_id>"` at the top level. Hosts that ignore the
    id (or use a different correlation scheme) are unaffected.

- **Unit test** (`daedalus_sink_call_id_tests`, 5 cases, 17
  assertions, all pass) for the new `AgentSink` 3-arg
  overloads:
  - D1.1 — 3-arg sink receives `call_id` for all 3 events
  - D1.2 — empty `call_id` still produces an event
  - D1.3 — back-compat: 2-arg-only sink still fires when called
    through a base pointer (which is how LLMClient does it)
  - D1.4 — default no-op sink swallows everything
  - D1.5 — same id across the 3 events enables correlation

- **CSS for the new panel:** `.tool-card` and its variants
  (`running` / `success` / `error` / `indeterminate`),
  `.tool-card-head`, `.tool-card-name`, `.tool-card-icon`,
  `.tool-card-stage`, `.tool-card-args`, `.tool-card-progress`,
  `.tool-card-progress-bar` (with `shimmer` keyframes for
  indeterminate state), `.tool-card-message`, `.tool-card-result`.
  Dark theme, monospace, ~80 lines.

### Added (2026-07-15)

- **Streaming tool-call progress events** (§Phase C1).
  Long-running tools (`execute_bash` with big builds, network ops,
  MCP calls) now report progress between `on_tool_call` and
  `on_tool_result` so hosts can show live feedback instead of
  freezing until the result lands.
  - **Public API:** `AgentSink::on_tool_progress(name, progress_json)`
    — virtual method on the sink interface. Default: no-op.
  - **Tool opt-in:** `BaseTool::execute(args, file_io, ToolProgressFn)`
    — new 3-arg overload. Default 2-arg `execute()` still works
    unchanged for the 18 tools that don't need progress.
  - **JSON shape:** `{ "stage": "...", "percent": 0..100, "message": "..." }`
    with `stage ∈ { "starting" | "executing" | "finalizing" | "done" }`.
  - **BashTool** is the first tool to emit progress (two events:
    `"starting"` at 0% and `"executing"` at 50% before the
    `BashCommandExecutor::execute` call returns).
  - **Demo sinks wired:**
    - `ConsoleAgentSink` (CLI) — compact one-liner per event:
      `  ↳ execute_bash: executing (50%)`
    - `WebSseSink` (webui) — broadcasts as a new SSE event type
      `tool_progress` with `{ "name", "progress" }` payload. The
      webui JS can render a live status bar from it.
  - **Test added:** `daedalus_tool_progress_tests` (4 cases, 14
    assertions, all pass):
    - `P1+P2`: BashTool emits `starting` + `executing` with the
      right JSON shape
    - `P3`: pre-execution rejection (privilege escalation) fires
      **zero** progress events
    - `P4`: legacy 2-arg `execute()` still works (back-compat)
    - `P5`: `ToolHandler::execute_tool_call` wires the closure
      through to the sink the same way `LLMClient` does

### Changed (2026-07-13)

- **XDG-compliant storage layout implemented in full.**
  Per-user state (`settings.json`, `mcp/servers.json`, `mcp/.env`,
  `skills/`) now resolves under XDG-compliant locations
  (`$XDG_CONFIG_HOME/daedalus/`, `$XDG_DATA_HOME/daedalus/`) instead
  of the flat `~/.daedalus/` layout. The legacy layout is honoured
  as a READ fallback for one release cycle with a one-shot
  `[DEPRECATION]` warning via `AgentSink`.

  **Env-var changes:**
  - **`$DAEDALUS_HOME`** is now the single-root override (was a
    no-op; the legacy `$DAEDALUSLIB_HOME` / `$CODEMAXLIB_HOME` vars
    are no longer consulted at the library layer).
  - **`$DAEDALUS_CONFIG_HOME` / `$DAEDALUS_DATA_HOME` /
    `$DAEDALUS_STATE_HOME` / `$DAEDALUS_CACHE_HOME`** override
    individual XDG roots.
  - All 4 standard `$XDG_*_HOME` vars honoured as before.

  **Public API additions:**
  - `daedalus::Paths` abstract interface + `Paths::global()` singleton
    + `Paths::set_override()` for testing.
  - `daedalus::ProjectPaths` per-project resolver (namespaced by
    `agent_name`).
  - `Paths::ensure_directories()` — auto-creates the four XDG roots
    with `0750` (config/data) / `0700` (state/cache) — secure
    default permissions for the four XDG dirs.
    Called from `AgentImpl::initialize()` so subsystems downstream
    don't have to think about mkdir + chmod.

  **CLI additions:**
  - `daedalus-cli --migrate-data`            — dry-run (prints plan)
  - `daedalus-cli --migrate-data-apply`      — actually copy files
  - `daedalus-cli --migrate-data-from DIR`   — non-default legacy root
  - `daedalus-cli --migrate-data-verbose`    — print every file copy

  The legacy `~/.daedalus/` directory is **never** removed by the
  library — the user does that manually after verifying the new layout.

  **New docs:** `docs/storage.md` (379 lines, source of truth for
  storage questions). `containers/Dockerfile` +
  `containers/docker-compose.yml` + `containers/README.md` (XDG-clean
  reference deployment).

  **Migration recipe:** `daedalus-cli --migrate-data-apply` → verify →
  `rm -rf ~/.daedalus`. See `docs/storage.md` §3 for details.

### Removed (effective 2026-07-13)

- **`default_memory_dir()` from `<daedalus/default_paths.h>`**.
  This used to return `~/.daedalus/memory/` and was the API that
  *enabled* the bug the XDG migration fixes — global memory shared
  every agent and every project. It was first deprecated in Phase 1
  (shim returning the legacy path) and is now **REMOVED**.
  All callers must use `daedalus::ProjectPaths::memory_dir()` instead,
  which returns the per-project, per-agent namespaced path.

### Removed (effective next release)

- **`$DAEDALUSLIB_HOME`** env var: silently no longer consulted at
  the library layer. Use `$DAEDALUS_HOME` instead.
- **`$CODEMAXLIB_HOME`** env var: same — replaced by `$DAEDALUS_HOME`.
  These vars were documented in earlier versions; they no longer have
  any effect.

### Changed (2026-07-12)

- **`$DAEDALUS_HOME` env var is no longer consulted by codemax-lib.**
  It belongs to the original codemax agent (different project,
  different conventions). The new env var is **`$CODEMAXLIB_HOME`**,
  with identical semantics (tier-3 fallback for the config home).
  Hosts that explicitly set `AgentConfig::settings_path` or
  `::daedalus_home` are unaffected. If `$DAEDALUS_HOME` is set in the
  shell, codemax-lib deliberately ignores it and falls through to
  `$HOME/.codemax`.

### Added (Phase 8.7, 2026-07-11)

- **Per-model `temperature` config.** Each model entry in
  `settings.json` may declare `"temperature": 0.7` (or any value in
  `0.0–2.0`). Hosts can override per-session via
  `cfg.temperature = 0.7`. The library sends `{"temperature": X}` on
  both the main chat request (`run_agent_cycle`) and the
  context-compression summary request (`call_llm_for_summary`).
  Resolution priority (highest first):
    1. `cfg.temperature >= 0.0` — explicit session override
    2. Model's value from `settings.json::models.<name>.temperature`
    3. `0.0` — library default (deterministic)
  Default `cfg.temperature = -1.0` is the sentinel meaning "use the
  model's value". See `docs/embedding.md` for the priority chain and
  code examples.

### Changed (Phase 9.9, 2026-07-11)

- **Default compiler switched from gcc → clang++.** CMake now pins
  `clang++` as the default C++ compiler. The pin runs before
  `project()` so the choice gets into the `CMAKE_CXX_COMPILER` cache
  before CMake latches onto system gcc. Users who prefer gcc can
  still override with `CC=gcc CXX=g++ cmake -S . -B build`. Verified
  clean build under clang 21.1.8.
- **clang-tidy infrastructure added.** New targets
  `codemax_clang_tidy_lib`, `codemax_clang_tidy_tests`,
  `codemax_clang_tidy_demos`, plus master aliases `codemax_clang_tidy`
  and `codemax_static_analysis`. Conservative rule set
  (`clang-analyzer-*`, `cppcoreguidelines-*` with noise disabled,
  `modernize-*`, `performance-*`, `readability-*`) — see `.clang-tidy`.
  HeaderFilterRegex restricts analysis to the project's own sources so
  OmniNet warnings are not flagged.
- **`.clang-tidy` and `.clang-format` are not committed.** Each
  developer generates them at configure time. Both are listed in
  `.gitignore`.

### Added (Phase 9.8, 2026-07-11)

- **SSE streaming wired end-to-end.** `LLMClient::run_agent_cycle`
  now sends `{"stream": true}` and forwards each `data: {...}`
  chunk to a `streaming_callback`. The `MiniMaxSseParser` (file-scope
  helper in `src/api/LLMClient.cpp`) parses chunks line-by-line and
  fires `sink_->on_assistant_delta(text)` per token. Replaces the
  previous "all at once at end of turn" delivery, giving chat hosts
  (webui, CLI REPL) real-time progressive text.
- **`AgentConfig::stream`** knob (default `true`). Set to `false`
  for batch / deterministic-replay hosts (test fixtures, embedded
  batching). Both modes produce identical final text.
- **Two critical streaming bugs fixed** (uncovered by the lava-lamp
  smoke test in `/workspace/Testing/test01/`):
    1. SSE parser assumed array-shaped chunks; MiniMax sends single
       objects per line. Fixed by normalising parsed→list-of-objects.
    2. Tool calls dropped on streaming path (chunks carry `delta`,
       but `run_agent_cycle` reads `message.tool_calls`). Fixed by
       `MiniMaxSseParser` accumulating `role`, `tool_calls[]`, and
       `finish_reason` alongside text content, and exposing
       `synthesized_message()` which returns the reconstructed
       message in the standard non-streaming shape.

### Added (Phase 9.7, 2026-07-11)

- **`AgentConfig::skills_dirs` + `AgentConfig::skills`.** Developer-
  controlled Skill loading. Both default to empty (no skills
  loaded). Hosts opt in by listing directories to scan and/or
  explicit skill names to activate. Pattern copied from
  `demos/codemax-webui/main.cpp`.

### Added (Phase 9.6, 2026-07-11)

- **Sink observability gaps closed.** `AgentSink` now fires
  `on_state_change` on every state transition so hosts can render
  real-time status (busy indicator, progress bar, cancel button).
  Host-side wiring demonstrated in `demos/codemax-webui/main.cpp`.

### Added (Phase 9.5, 2026-07-11)

- **codemax-webui polish.** Brought up to date with Phase 9.6
  observability hooks and Phase 9.7 skill loading. Demonstrates the
  full Sink pattern (BashTool ask_user, file logger, SSE delivery).

### Changed (2026-07-10)

- **Phase 7 demo set reduced from 5 to 4.** The `codemax-tui` (FTXUI)
  demo is cancelled. The remaining demo set is `codemax-cli`,
  `codemax-manager`, `codemax-subagent`, `codemax-webui`. TUI was
  redundant with `webui` (both stream `on_assistant_delta` events) and
  the web version is what developers actually want to reference. Hosts
  that want a TUI write their own ~200-line embed using the patterns
  from `demos/codemax-webui/` as the template. The CMake option
  `CODEMAX_DEMO_TUI` has been removed. Plan updated in `Agent.md`.

- **`codemax-webui` uses OmniNet, not cpp-httplib.**
  [OmniNet](https://gitlab.com/leestripp/omninet) is already a codemax
  dependency (FetchContent from GitLab, MIT, same author), providing
  HTTP server, SSE streaming, WebSocket, TLS, and static file serving.
  Reusing it means zero new external libraries for the demo, one
  license to review, and built-in SSE thread-safety. Patterns to copy
  in the demo already exist in this repo as
  `demos/http_server_demo` + `tests/test_sse_streaming`. Plan updated
  in `Agent.md`.

### Fixed (Phase 8.6, 2026-07-10)

- **`LLMClient` no longer uses the wrong model's `max_context_size` in
  the in-session token-usage stats.** Previously, running with
  `--model minimax-m2.7` (200K context) showed
  `Token Usage: N/500000` because the code asked settings for
  `default_model`'s context — which is `minimax-m3` (500K). Now
  `LLMClient` resolves the actually-selected model's context via
  `SettingsManager::get_max_context_size_for_model(config_.model_name)`.
  In-session progress bar shows the right number for the running model.
- **codemax-cli banner no longer hides the `auto_approve` setting from
  settings.json.** Previously the banner showed only `cfg.auto_approve`
  (false unless `--auto` was passed), missing the value from
  `settings.json::auto_approve` even when the library's tool
  dispatcher was actually using it. Now the banner calls
  `Agent::auto_approve()` which returns the *effective* value
  (`cfg.auto_approve || settings.json::auto_approve`).

### Added (Phase 8.6, 2026-07-10)

- **`Agent::active_model()`** — returns the `ModelInfo` (name, model_id,
  host, chat_endpoint, max_context_size) for the model that subsequent
  turns will hit. Resolution priority matches `LLMClient`:
  `cfg.model_name` → settings.json `default_model` → first model.
- **`Agent::library_version()`** — static, returns the git hash of the
  linked `libcodemax.a` (compiled in as `CODEMAX_GIT_VERSION` PUBLIC
  definition in `CMakeLists.txt`).
- **`AppConfig::model_name`** — settings.json key for the selected model
  (e.g. `"minimax-m2.7"`), populated by `make_app_config()` from
  `cfg.model_name`. Used by `LLMClient` to resolve per-model context.
- **codemax-cli startup banner** — now shows library version, active
  model + model_id + endpoint + max context, plus the runtime
  `daedalus_home` / `project_root` / `session_id` / `memory_enabled` /
  `auto_approve` / `max_iterations`. Hosts can verify config at a glance.

### Added (Phase 8.5, 2026-07-10)

- **`AgentConfig::daedalus_home` field** — per-Agent config directory.
  When set, empty derived fields (`settings_path`, `mcp_servers_path`,
  `mcp_env_path`, `memory_dir`) are resolved relative to this directory.
  Two `Agent` instances in one process can now have entirely separate
  settings, MCP servers, and memory without env vars or separate
  filesystem paths. See `docs/embedding.md` for the priority table.

### Changed (Phase 2.7, 2026-07-10)

- **`--manager-mode` flag removed from the CLI demo.** The orchestration
  pattern is preserved as a demo composition (`demos/codemax-manager/`,
  Phase 7).
- **`--sub-agent` and `--sub-agent-looping` flags removed from the CLI
  demo.** The sub-agent pattern is preserved as a demo composition
  (`demos/codemax-subagent/`, Phase 7).
- **`LLMClient::run_manager_mode()` deleted.** The Agent has exactly
  one loop (`run_one_turn`).
- **`LLMClient::get_manager_system_prompt()` deleted.** Hosts supply
  their own role prompt via `AgentConfig::system_prompt`.
- **CLI help text updated** to reflect the new flag set.

### Added (Phase 6, 2026-07-10)

- **`daedalus::Agent` public facade** (`include/codemax/Agent.h`). Hosts
  no longer include internal subsystem headers (`LLMClient`,
  `SettingsManager`, ...). The class is PIMPL over
  `std::unique_ptr<detail::AgentImpl>`.
- **`daedalus::AgentSink`** (`include/codemax/AgentSink.h`). The single
  callback interface the library uses to talk to the host.
- **`daedalus::AgentConfig`** (`include/codemax/types/AgentConfig.h`).
  Consolidated config struct passed to the Agent's constructor.
- **`daedalus::AgentState`** (`include/codemax/types/AgentState.h`).
  Public state enum (Idle, Understanding, Planning, Acting, Verifying,
  AwaitingUser, AwaitingApproval, Compressing, Done, Cancelled, Error).
- **`daedalus::AgentStats`** (`include/codemax/types/AgentStats.h`).
  Read-only stats snapshot (token counts, iteration, message counts).
- **`daedalus::default_paths.h`** — helpers that compute
  `~/.codemax/...` defaults honouring `$HOME` (the env var that
  controls the codemax home directory; no other env vars are read).
- **`docs/embedding.md`** — embedding guide for new host authors
  (includes the full `AgentSink` contract; `docs/sink.md` merged here in v0.3.9).

### Changed (Phase 3, 2026-07-09)

- **SSE streaming** replaces the thinking-spinner. `LLMClient` now
  parses `data: {...}` chunks line-by-line and calls
  `sink_->on_assistant_delta(text)` per token.
- **`execute_bash` approval** routes through
  `sink_->request_command_approval(cmd)`.
- **`ask_user` tool** routes through `sink_->request_user_input(prompt)`.
- **HTTP error / retry / warning messages** route through
  `sink_->on_log(level, msg)` instead of `std::cerr`.
- **`ConsoleAgentSink`** in `demos/codemax-cli/` — the CLI demo's
  concrete sink (cout for tokens, cerr for logs, linenoise for
  user input, y/n prompt for command approval).

### Changed (Phase 2.5, 2026-07-08)

- **`LLMClient::run_interaction()` deleted.** The REPL loop is now
  owned by `demos/codemax-cli/main.cpp`. The library exposes
  `run_one_turn(user_message)` instead.

### Changed (Phase 2, 2026-07-08)

- **`SettingsManager`** is no longer a singleton. Constructed with
  `(settings_path)` and owned by `Agent`.
- **`SkillManager`** is no longer a singleton. Same pattern.
- **`McpServerManager`** is no longer a singleton. Same pattern.

### Changed (Phase 1+, 2026-07-07)

- **`src/main.cpp` moved to `demos/codemax-cli/main.cpp`.** The CLI
  is now a demo that links against `libcodemax.a`.
- **`libcodemax.a`** is the primary build artifact. The library is
  built first; demos are built second against it.
- **`codemax.h`** is the umbrella header (one `#include` for the
  entire public API surface).

### Added (Phase 1, 2026-07-07)

- **`include/codemax/`** directory created.
- **`include/codemax/types/`** directory created.
- **`include/codemax/Agent.h`** (stub) created.
- **CMake static-library target** `codemax` (`add_library(... STATIC)`).
- **`install(TARGETS codemax ARCHIVE DESTINATION lib)`** for
  downstream `find_package` / raw link.

## [v0.4.0] — 2026-07-15

Settings refactor cutover + OmniNet bug resolutions.

### Added (v0.4.0)

- **`daedalus::SettingsStore` public API** in `include/daedalus/settings.h`:
  - `SettingsStore::instance()` — global default DB handle
  - `SettingsStore::for_home(path)` — per-agent isolated handle
  - CRUD for models, kv_settings, mcp_servers, serper_config
  - `transaction()` for atomic batches
  - `db_path()`, `has_data()` introspection

  SQLite-backed (`$XDG_DATA_HOME/daedalus/daedalus.db`), replaces the
  scattered JSON files. Hosts that use libdaedalus no longer touch
  `config.json` / `mcp/servers.json` / `mcp/.env` directly.

- **`tests/test_settings_store.cpp`** — 9 unit tests for the public
  API (CRUD, transaction, for_home isolation).
- **`tests/test_settings_store_e2e.cpp`** — end-to-end wire-in test
  (write via `SettingsStore` → read via `Agent.list_models()`).
- **`tests/test_settings_v040_cutover.cpp`** — 5 cutover tests pinning
  the SQLite-only semantics.
- **`tests/test_settings_migrator.cpp`** — 10 tests for the JSON→SQLite
  auto-migrator.
- **`omninet_bugs.md`** — tracks OmniNet bugs found via daedalus.

### Changed (v0.4.0)

- **`src/core/SettingsManager::load()` is now SQLite-only when the
  store has data.** JSON is consulted only when the store is empty.
  Stale scalars in `config.json` can no longer leak into a populated
  store.
- **Validation requires both `default_model_name` AND a non-empty
  `models` map** (was: either one threw spuriously).
- **Per-project state (session memory + messages)** moved into the
  `daedalus.db` store under `mem.<sid>` / `msg.<sid>` keys, with JSON
  files retained as a read fallback (v0.3.5 → v0.4.0 step).
- **`SettingsStore::list_models()` uses range-based iteration** again.
  The OmniNet callback-overload workaround has been removed — see
  "Fixed (v0.4.0)" below.

### Removed (v0.4.0)

- **`AskUserTool`** — broken and unused. Removed from
  `src/tools/UserTool.{h,cpp}`, plus the related plumbing:
  - `request_user_input()` from `AgentSink` (and both demo sinks)
  - `AgentState::AwaitingUser` enum value
  - CHAT → AwaitingUser mapping in `AgentImpl`
  - ask_user handling block in `LLMClient::run_agent_cycle()`
- **Stale "ask_user" / "AwaitingUser" references** throughout the
  library, demos, and tests.

### Fixed (v0.4.0)

- **OMNINET-1 (Bug 1) — `QueryResult` lost the first row's data on
  range-based iteration.** The constructor stepped to the first row
  but did not populate `m_currentRow`, so `for (const auto& row :
  db.query("SELECT ..."))` yielded N rows with the first empty and
  the real first row missing. Fixed upstream in OmniNet **0.8.7**
  (`Database.cpp` lines 335-344: populate `m_currentRow` after
  `step()`). daedalus workaround (callback overload) was retained
  until Bug 2 below was also confirmed fixed.
- **OMNINET-2 (Bug 2) — `QueryResult` returned a phantom empty row
  on the first range-based query after a DELETE or DB-file reopen.**
  The `Iterator` constructor blindly copied `m_currentRow` without
  honouring `m_atEnd`, so `begin() != end()` on empty result sets.
  Fixed upstream in OmniNet **0.8.8-1-gfbecf55**
  (`Database.cpp:392-404`: check `result->m_atEnd` and mirror it on
  the iterator). Upstream added 4 regression tests
  (`database_query_range_empty_after_delete`,
  `database_query_range_empty_after_reopen`,
  `database_query_range_empty_table`,
  `database_query_range_after_delete_then_insert`). OmniNet's own
  suite went 121/121 → 125/125. **Workaround removed** from
  `src/core/SettingsStore.cpp::list_models()`. Closed in daedalus.

### Migration notes (v0.4.0)

- First run on a system with existing JSON config: auto-migrator
  reads `config.json` / `mcp/servers.json` / `mcp/.env`, writes
  rows to `daedalus.db`, and renames the originals to
  `*.migrated`. No user action required.
- Placeholder API keys (`YOUR_API_KEY_HERE`) are skipped by the
  migrator — they'd fail validation later.
- `SettingsManager` itself is still present (would require touching
  many call sites to delete) but is now a thin SQLite shim. Future
  cleanup: route `AgentImpl::initialize()` straight through
  `SettingsStore::instance()` and delete `SettingsManager`.

## [0.x] — Pre-refactor

Single-executable CLI agent. See git history. Features retained in
the refactor: multi-provider LLM support, agent skills (open standard),
40+ tools, MCP integration, session memory, context compression
("70/30 split"), background processes, manager mode, sub-agent
looping, sub-agent mode.

[Unreleased]: https://gitlab.example/codemax/-/compare/v0.x...main