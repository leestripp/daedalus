# Changelog

All notable changes to CodeMax are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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

## [0.x] — Pre-refactor

Single-executable CLI agent. See git history. Features retained in
the refactor: multi-provider LLM support, agent skills (open standard),
40+ tools, MCP integration, session memory, context compression
("70/30 split"), background processes, manager mode, sub-agent
looping, sub-agent mode.

[Unreleased]: https://gitlab.example/codemax/-/compare/v0.x...main