# AGENTS.md — icarus

> This file is written for AI coding agents that need to understand and work on
> the icarus project. It assumes no prior knowledge of the codebase.

---

## Project overview

**icarus** is a C++23 command-line AI coding agent built on the
[daedalus](https://gitlab.com/leestripp/daedalus.git) framework. It can
run in two modes:

1. **Interactive CLI mode** — a terminal REPL that explores repositories,
   reads/writes files, runs shell commands, and calls tools.
2. **A2A Agent mode** (`--a2a-agent`) — an HTTP server that speaks the
   Agent2Agent Protocol (A2A) for IDE integration.

icarus consumes the A2A client/server implementation built into daedalus
(`daedalus::a2a`). The old standalone ACP v1 library (`src/acp/`) has been
removed; `--acp-agent` remains only as a deprecated alias for `--a2a-agent`.

Other notable features:

- 31 in-session slash commands (models, providers, skills, tools, todos,
  settings, history, MCP servers, usage stats — see `docs/cli.md`).
- MCP support — connects to MCP servers for additional tool access.
- Provider plugins (daedalus §ProviderPlugins) — per-provider behavior
  (payload shaping, temperature clamps, auth-less local servers, SSE
  dialects) via built-in `openai-compatible`/`minimax`/`moonshot`/
  `lmstudio` plugins, auto-detected from the provider host or pinned with
  `/provider-add ... [plugin]` / the settings TUI plugin field.
- Context caching (daedalus §ContextCache) — server-side prompt cache keys
  (e.g. Moonshot `prompt_cache_key`) derived from `agent_name`, falling
  back to a stable per-project `AgentConfig::session_id` that icarus sets
  in both modes; cache hits surface as `cached=` in `/stats` and a
  `Cached:` line in `/usage`.
- Persistent conversation memory across sessions (SQLite-backed).
- Streaming output with delta rendering and a thinking spinner.
- Resize-aware terminal UI (boxed tables, tool-call cards) with
  `--plain` / `NO_COLOR` fallbacks.
- linenoise-backed line editing with history, ghost-text hints, and
  tab-completion for slash commands.

---

## Technology stack

- **Language**: C++23
- **Build system**: CMake ≥ 3.28
- **Compiler**: gcc/g++ (preferred when available; clang also works)
- **License**: MIT — see `LICENSE` file. Individual source files carry
  `SPDX-License-Identifier: BSL-1.0` headers.

### System dependencies

| Package | Purpose |
|---------|---------|
| `nlohmann_json` | JSON parsing |
| `OpenSSL` | TLS/crypto |
| `libpng` | PNG support (daedalus dependency) |
| `libjpeg` | JPEG support (daedalus JPEG-SCALE) |
| `poppler-cpp` | `read_pdf` tool (daedalus 1.0.2+ PDF-CORE) |
| `uuid` | UUID generation |
| `sqlite3` | Settings database |
| `pthread`, `dl`, `z` | Standard system libs |

daedalus 1.0.1+ requires poppler-cpp at configure time
(`pkg_check_modules(POPPLER REQUIRED poppler-cpp)` in the daedalus build fails
without the system package, `libpoppler-cpp-dev` on Ubuntu). Since daedalus
1.0.2 (§PDF-CORE) `ReadPDFTool` lives in the library itself, so libdaedalus.a
carries poppler_* references and icarus **links poppler-cpp too** (static libs
don't propagate their PRIVATE link deps).

### Fetched dependencies

`CMakeLists.txt` uses `FetchContent` + `ExternalProject_Add` to download and
build **daedalus** from its `main` branch. daedalus itself fetches **OmniNet**,
which provides the HTTP and SSE client used by daedalus's A2A implementation
(and pulls in fmt, nlohmann_json, sqlite-vec). **linenoise** (a single-file
line editing library, pinned to commit `a473823`) is fetched and built as a
static C library. **FTXUI** (pinned to `v7.0.1`) is fetched for the embedded
settings TUI: the tab sources from daedalus's
`demos/daedalus-settings-tui/` (all except its `main.cpp`) are compiled
straight into the icarus binary so bare `/settings` can open the TUI
in-process; the standalone demo is disabled in the daedalus build via
`-DDAEDALUS_DEMO_SETTINGS_TUI=OFF`. The Agent tab's `thinking_level` row
persists a generic `thinking_level` kv row — a host-side convention
daedalus never reads itself; icarus honors it by re-applying the level at
startup, via `/level`, and when the TUI closes. When the TUI closes, icarus syncs
changes into the running agent: `auto_approve` is re-applied (unless
`--auto` is authoritative), `thinking_level` is re-applied via
`set_thinking_level()`, a changed `current_provider` triggers
`switch_provider()`, edits to the active model/provider are re-applied by
re-selecting the model (§MODEL-SWITCH resets history), and MCP
enable/disable toggles go through `set_mcp_server_enabled()`. MCP
add/edit/remove and the other Agent-tab kv values have no runtime API —
icarus prints a "next launch" note when they change. An empty
`current_provider` row is seeded from the active model before the TUI
opens (daedalus only writes that row on `switch_provider()`). `<daedalus/settings.h>` includes
`<db/Database.h>`, so the OmniNet source dir inside the daedalus build tree
is on the icarus include path.

> **Note**: There used to be a `patch_daedalus.py` post-fetch patch step.
> It has been removed — the `delete_model` Agent API (and the earlier
> switch-model fixes) are now upstream in daedalus. If you need a daedalus
> **library** API change, it must land upstream; there is no local patching
> mechanism anymore.
>
> The daedalus **demos** are different: they are example code, not API.
> icarus may copy demo sources (e.g. the settings-TUI tabs) into `src/ui/`
> and treat the copies as its own — improve them freely instead of waiting
> for upstream. When you vendor a demo file, say so in a dated `§` comment
> at the top (what was copied, when, and why) and point `CMakeLists.txt`
> at the icarus copy.

---

## Build commands

### Standard build

```bash
# §Monorepo (2026-09-24): PREFERRED — build everything from the repo root:
#   (cd .. && cmake -S . -B build && cmake --build build -j$(nproc))
# One ./build for library + hosts. The standalone build below still works
# (dual-mode) but creates a per-component build/ you must clean up yourself.
cmake -S . -B build
cmake --build build -j$(nproc)
```

The resulting binary is `build/icarus` (standalone) or `build/icarus/icarus`
(superbuild at the repo root). Default build type is `Release`
(`-O3`); use `-DCMAKE_BUILD_TYPE=Debug` for `-g`.

### Tests

icarus has a committed unit-test suite in `tests/` (doctest + CTest,
added 2026-08-20 — an earlier untracked `tests/` folder was lost in a
git-restore cleanup, so the suite is part of the repo now):

```bash
cmake -S . -B build                       # ICARUS_BUILD_TESTS defaults to ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

Three CTest registrations, 28 doctest cases / 94 assertions:

| ctest name     | Binary               | Covers |
|----------------|----------------------|--------|
| `cli_args`     | `icarus_test_util`   | `parse_cli_args()` — every flag, `--plain` expansion, stream-toggle ordering, deprecated ACP aliases, positional `target_path` |
| `system_prompt`| `icarus_test_util`   | `resolve_system_prompt()` — override file vs `AGENTS.md` priority, missing-override short-circuit quirk |
| `migrate`      | `icarus_test_migrate`| `detect_migrate_data_args()`, `compute_migration_plan()`, `apply_migration_plan()` — mappings, size probing, recursive copies |

Notes for adding tests:

- doctest is fetched at `v2.5.3` (same tag daedalus pins) via FetchContent
  in `tests/CMakeLists.txt`.
- Filesystem tests use PID-unique temp dirs only; migrate tests redirect
  `XDG_CONFIG_HOME`/`XDG_DATA_HOME` with an RAII guard — **never** touch
  the real `~/.daedalus`.
- Flags that `std::exit(1)` on malformed input are deliberately untested
  (they would kill the test runner).
- UI (`AnsiRenderer`, `StatusHud`, spinner) and A2A code are still
  untested — the obvious next coverage targets.
- Dependencies (daedalus, OmniNet) ship and run their own tests upstream;
  they are neither run nor duplicated here.

### Static analysis

```bash
cmake --build build --target icarus_cppcheck
```

This target is created automatically when `cppcheck` is found. It checks only
the `src/` tree; warnings from fetched dependencies (daedalus, OmniNet) are
suppressed.

### Install

```bash
cmake --build build --target install
```

Installs `icarus` to `$HOME/.local/bin/` by default (default prefix skipped
when `HOME` is unset or `/`). Honour/override with `-DCMAKE_INSTALL_PREFIX=...`
or `cmake --install build --prefix ...`.

### Quick verification after changes

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
./build/icarus --version
```

### Versioning

`ICARUS_VERSION` comes from the latest git tag
(`git describe --tags --abbrev=0`) and is baked in as a compile definition
(fallback: `"dev"`). It is sent to providers as the
`daedalus-icarus/<version>` User-Agent; `--version` prints
`icarus <version> (daedalus <lib version>)` — the icarus version from the
`ICARUS_VERSION` compile definition, the lib version from
`daedalus::Agent::library_version()`.
Do not edit `src/core/version.h` — its macros are unused legacy stubs.

---

## Project structure

```
src/
├── main.cpp                 # CLI entry point, slash-command handlers, REPL loop (~1700 lines)
├── util.h                   # CLI args struct, signal handling, print_help(),
│                            #   system prompt resolution
├── ConsoleAgentSink.cpp/h   # daedalus sink: renders callbacks to terminal
├── ThinkingSpinner.h        # Header-only spinner during agent thinking states
├── banner.h                 # Startup banner printer
│
├── a2a_tools.cpp/h          # A2A client exposed as a daedalus BaseTool
├── a2a_agent.cpp/h          # A2A agent HTTP server backend
│
├── migrate.cpp/h            # Legacy ~/.daedalus → XDG migration helper
├── core/
│   └── version.h            # Legacy auto-generated macros (unused; version comes
│                            #   from the ICARUS_VERSION compile definition)
│
└── ui/
    ├── AnsiRenderer.cpp/h    # ANSI colors, boxes, DrawTable, width helpers
    ├── StatusHud.cpp/h       # Status formatting (sticky footer deferred)
    ├── InputReader.h        # linenoise input, hints/completion (kSlashCommands)
    ├── InputQueue.h         # Queue of InputItems for REPL processing
    └── InputThread.h        # Background thread for async input

tests/
├── CMakeLists.txt           # doctest v2.5.3 FetchContent, ICARUS_BUILD_TESTS option,
│                            #   ctest registrations (cli_args, system_prompt, migrate)
├── test_cli_args.cpp        # parse_cli_args() flag-parser tests
├── test_system_prompt.cpp   # resolve_system_prompt() priority-chain tests
└── test_migrate.cpp         # --migrate-data argv/plan/copy tests (temp-dir sandboxed)

CMakeLists.txt               # Build configuration (FetchContent daedalus + linenoise + ftxui)
Readme.md                    # User-facing documentation
prompt.txt                   # Design note for prompt formatting (referenced in code comments)
resources/                   # Banner/icon images
docs/
├── cli.md                   # Complete CLI and in-session command reference
├── design.md                # Architecture and design rationale
├── ui-design.md             # Terminal UI design
├── a2a-protocol.md          # A2A protocol notes
└── acp-openapi.yaml         # Legacy ACP OpenAPI spec (historical reference)
```

---

## Code style and conventions

- **Language**: C++23.
- **Warnings**: `-Wall -Wextra` are enabled for the main target.
- **Headers**: Use `#pragma once`.
- **License header**: New source files should include
  `// SPDX-License-Identifier: BSL-1.0` as the first line.
- **Namespaces**:
  - `daedalus` for sink implementations.
  - `a2a` for the A2A tool/agent wrappers.
  - `daedalus_cli` for migration utilities.
- **Formatting**: Match surrounding style — 4-space indentation, braces on the
  same line. No `.clang-format` file is checked in.
- **Header-only utilities**: `util.h`, `ThinkingSpinner.h`, `banner.h`, and
  the `ui/Input*.h` stack are header-only; use `inline` for process-wide
  globals and free functions in headers to avoid ODR/internal-linkage issues
  (see comments in `util.h`).
- **Change annotations**: The codebase uses dated `§...` comment markers
  (e.g. `// §Demo fix 2026-07-16:`) to explain non-obvious decisions.
  Follow this convention for subtle fixes.
- **Thread safety**: Protect shared state with `std::mutex`. Delta buffering
  in `ConsoleAgentSink` is mutex-protected because callbacks and flush calls
  come from different threads.
- **Error handling**: Network errors are non-fatal — they return expected
  error JSON, not exceptions. Production code does not use `assert()` on
  network paths.

---

## Key source components

### `src/main.cpp` — Entry point and REPL

- Parses CLI arguments via `parse_cli_args()` in `util.h`.
- Handles `--migrate-data` and `--clear-memory` short-circuits before agent
  init.
- Runs either the interactive REPL or the A2A agent daemon based on flags.
- In REPL mode: reads lines from `InputReader`, submits to `daedalus::Agent`,
  and renders output via `ConsoleAgentSink`. Contains all slash-command
  handlers. Before blocking for input it drains library-originated pending
  messages (background completions); the sink's `on_turn_requested()` wakes
  `InputQueue`, while `run_one_turn()` stays on the main thread.
- Identifies itself to LLM providers via the User-Agent
  `daedalus-icarus/<version>` (`AgentConfig::user_agent`, §UA 2026-07-29).
  `<version>` is the `ICARUS_VERSION` compile definition, derived by
  CMakeLists.txt from the latest git tag (`git describe --tags --abbrev=0`,
  `"dev"` when the tree is untagged or git is unavailable) — there is no
  manually maintained version file.

### `src/util.h` — CLI and infrastructure

- `CliArgs` struct holds all parsed flags; `parse_cli_args()` is the parser.
- `print_help()` prints the `--help` text including the slash-command summary.
- `resolve_system_prompt()` loads the system prompt in priority order:
  1. `--system-prompt <file>` override
  2. `<project_root>/AGENTS.md`
- Signal handlers restore the terminal (cursor/SGR) and cancel the in-flight
  turn via `g_agent_for_signal`; a second SIGINT/SIGTERM force-exits.

### `src/ConsoleAgentSink.cpp` — Terminal sink

- Implements `daedalus::AgentSink` callbacks, including the structured
  `on_error` channel and `on_turn_requested()` background-completion wake-up.
  The latter only wakes the input queue; it never resubmits message text.
- Buffers assistant text deltas; flushes at newline or 2048-char threshold.
- Prints `output_chunk` tool-progress events (live bash stdout/stderr,
  pre-sanitised by daedalus) verbatim to stdout; `on_tool_result` then
  elides the duplicated `output` field as `<streamed live above>`.
- `tell_user` results: the tool call header truncates the `content` arg to
  50 chars, so `on_tool_result` also prints the result's full `content`
  field verbatim to stdout after the result card (via
  `print_user_facing_text()`, also used for `stop`'s `summary`).
- Hidden result fields surfaced in the result card (§Hidden fields
  2026-08-02): bash `denied` shows its message like an error; bash
  poll/background add `process_id`/`pid`/`exited`/`signal` lines and render
  `stdout`/`stderr` through the same 30-line head/tail elision as `output`;
  `web_search` shows up to 5 `title — url` lines plus which provider served them (§WEB-SEARCH-CORE unified provider chain);
  MCP tools (`mcp__*`) render their string `result` payload; `remember`
  shows the saved `memory.category`/`memory.content`.
- Handles terminal save/restore around bash command execution.
- Filters daedalus's startup banner via `on_log`; every log message goes to
  `logs.txt`, but only Warn/Error/Debug also reach the terminal (Info
  chatter is file-only).
- Command approval prompt: `y` approves once, `n` refuses, `a` flips
  daedalus's authoritative runtime auto-approve via an injected
  `Agent::set_auto_approve(true)` handle (`set_auto_approve_setter()`;
  daedalus ≥ 0.8.7). Commands that bypass the prompt (auto-mode, session
  or persisted approval caches) are reported by the
  `on_command_auto_approved()` override. Without an injected setter the
  sink falls back to a local session-scoped always-approve flag.
  `AgentConfig::auto_approve` is `std::optional<bool>` (§BUG-006): main()
  only sets it when `--auto` is passed, leaving the SettingsStore row in
  effect otherwise.
- `ThinkingSpinner` runs for the whole agent turn: `main()` brackets
  `submit_user_message`/`run_one_turn` with `start_agent_turn()`/
  `stop_agent_turn()`; the spinner stops on any output and a watchdog
  restarts it after 3 s of quiet or on a state change. The label is the
  agent state only (`Thinking...` before the first state change), set via
  the thread-safe `ThinkingSpinner::set_message()`. The label's source of
  truth is the `to` value of `on_state_change` — `Agent::state()` only
  ever reports `Understanding`/terminal states, because the LLM drives
  intermediate states via its `set_status` tool and those reach the host
  only through the sink callback. Before (re)starting the spinner the
  sink commits any mid-line streamed text with a `\n`
  (`stream_mid_line_` guard in `start_status_spinner()`) — the spinner's
  `\r` + erase-to-EOL frames would otherwise wipe the partial line.
  While a tool call executes, the label switches to
  `Running <tool>... (Ns)` (§ToolSpinner 2026-08-07): `on_tool_call` arms
  it via `begin_tool_spinner()`, the watchdog refreshes the elapsed
  seconds at ~1 Hz, and `on_tool_result` disarms it. Do NOT reintroduce a
  `stop_agent_turn()` in `save_terminal_for_subprocess()` — that killed
  the spinner for the entire duration of every `execute_bash` run (the
  UI looked hung). The old TTY-takeover premise is false: daedalus's
  `Subprocess` pipes child stdout/stderr, so the child never paints the
  terminal directly; its output returns as sanitised `output_chunk`
  events printed by the sink, and printing already stops the spinner.

### `src/a2a_tools.cpp` — A2A client tool

- Registers `A2aTool` (a `daedalus::BaseTool`) with the agent.
- Exposes 7 actions: `get_agent_card`, `send_message`,
  `send_streaming_message`, `get_task`, `list_tasks`, `cancel_task`,
  `subscribe_to_task`.
- Uses `daedalus::a2a::A2aClient` internally.

### `src/a2a_agent.cpp` — A2A agent server

- Runs an HTTP server via `daedalus::a2a::A2aServer`.
- Implements `IA2aTaskBackend` to drive a fresh `daedalus::Agent` per task.
- Auto-approves all commands (no interactive terminal in daemon mode).
- Per-task agents honour `--model`, `--home`, `--skills`,
  `--system-prompt`, `--no-memory`, and `--agent-name` (built into an
  `AgentConfig` template from the parsed `CliArgs`).
- The agent card is served at
  `http://127.0.0.1:<port>/.well-known/agent-card.json` and advertises the
  real bound URL (set after the server binds); a bind failure exits with
  code 1.

### `src/ui/AnsiRenderer.cpp` — ANSI rendering

- `RenderConfig::detect()` probes terminal capabilities (color, Unicode).
- `effective_terminal_width()` re-queries the width per render
  (TIOCGWINSZ → `$COLUMNS` → fallback) — all boxes/tables/rules are
  resize-aware; there is no SIGWINCH handler.
- `Style` class emits SGR escape codes (or no-ops when color is disabled).
- `make_box` is always left-justified (`Alignment::Auto` → `Left`).
- **`DrawTable` is the canonical table builder** — every table in the app
  goes through it (auto-size, shrink-to-fit, `…` truncation). Do not
  hand-roll tables.

### `src/ui/InputReader.h` — line editing

- linenoise-backed input with history, ESC detection (bare ESC cancels the
  agent turn), and a stop-aware async read used by `InputThread`.
- **`detail::kSlashCommands` is the single source of truth** for ghost-text
  hints and tab-completion. When adding/removing a slash command, update
  three places: the handler in `main.cpp`, `kSlashCommands`, and
  `print_help()` in `util.h`.

---

## CLI reference

### Key options

| Option | Description |
|--------|-------------|
| `--model <name>` | Select model (e.g. `minimax-m3`) |
| `--model-list` | List configured models and exit |
| `--discover-models <name>` | Query provider's `/v1/models` endpoint and exit |
| `--auto` | Auto-approve all tool calls without prompting |
| `--no-memory` | Disable persistent conversation memory |
| `--clear-memory` | Clear all session memory and exit |
| `--threads <n>` | Number of worker threads |
| `--stream` / `--no-stream` | Enable/disable SSE streaming (default: on) |
| `--plain` | Alias for `--no-color --no-hud --no-unicode` |
| `--skills <dir>` | Additional skills directory (can be passed multiple times) |
| `--a2a-url <URL>` | Default A2A server URL (default: `http://localhost:8000`) |
| `--a2a-agent` | Run as an A2A agent HTTP server (`--acp-agent` is a deprecated alias) |
| `--a2a-port <n>` | Port for `--a2a-agent` (0 = ephemeral, default: 0) |
| `--home <path>` | Override daedalus home directory |
| `--system-prompt <file>` | Override system prompt with a file |
| `--agent-name <name>` | Per-agent namespace for session files |
| `--migrate-data[-apply]` | Migrate legacy `~/.daedalus` data to XDG locations |
| `--version` | Print version and exit |

### In-session commands

Type at the `> ` prompt (full reference in `docs/cli.md`):

| Command | Action |
|---------|--------|
| `/quit`, `/exit` | Exit cleanly |
| `/help` | Print help summary |
| `/init` | Re-generate `AGENTS.md` from scratch |
| `/clear` | Clear session memory |
| `/compress` | Compress conversation context (`Agent::compress_context()`) |
| `/models` | Discover models from the current provider (falls back to the current provider's configured models when it has no `/v1/models` endpoint) |
| `/models --configured [provider]` | List models configured in `daedalus.db`, optionally filtered to one provider |
| `/model <name\|#>` | Switch active model by name or index |
| `/model-add`, `/model-delete` | Add / delete a configured model |
| `/providers` | List configured providers (table) |
| `/provider <name\|#>` | Switch provider by name or index |
| `/provider-add`, `/provider-delete` | Add / delete a provider |
| `/mcp-servers` | List configured MCP servers |
| `/skills` | List loaded skills |
| `/skills enable/disable <name>` | Enable or disable a skill |
| `/tools` | List registered tools |
| `/tools enable/disable <name>` | Enable or disable a tool |
| `/todo` | Manage todos (`add`, `done/undone`, `edit`, `delete`, `clear`, `max`) |
| `/settings` | Interactive settings TUI (embedded daedalus settings editor; read-only table summary when not on a terminal) |
| `/memory` | Show session memory stats |
| `/history` | Show recent messages |
| `/export` | Export conversation |
| `/tool <name> [json]` | Call a tool directly |
| `/last` | Show last assistant response |
| `/continue` | Re-submit the last user message |
| `/stats` | Show token/iteration stats |
| `/usage` | Show token/context usage summary |
| `/state` | Show current agent state |
| `/levels` | List thinking levels for the current model |
| `/level <name\|#>` | Set thinking level (persisted; re-applied at startup) |
| `/paths` | Show all config/data/project paths (table) |

---

## Configuration and data layout

### daedalus home

```
$XDG_DATA_HOME/daedalus/        (default: $HOME/.local/share/daedalus/)
```

Override with `--home <dir>` or `DAEDALUS_HOME`.

**Important**: `daedalus.db` lives in the **data** hierarchy, not config.
Using `--home $HOME/.config/daedalus` will cause `--model-list` to report
`(none configured)` because the database is not there.

### Runtime data files

```
$DAEDALUS_HOME/
├── daedalus.db              # SQLite: models, providers, MCP servers, settings

$XDG_DATA_HOME/icarus/
└── input_history.txt        # linenoise line-edit history

<project_root>/.daedalus/daedalus/
├── messages.json            # Current session history
├── messages.json.tmp        # Auto-save temp file
├── messages.json.crash      # Crash-recovery copy
├── logs.txt                 # Timestamped on_log output (all levels)
├── memory/memory.sqlite     # Persistent session memory
├── sessions/                # Saved sessions
└── system_prompt.txt        # Legacy per-project overlay (unused)

<project_root>/.daedalus/daedalus-<name>/
    # Named-agent variant of the above
```

Run `/paths` in a session for the fully-resolved list.

### System prompt resolution

```
1. --system-prompt <file>          (CLI override, highest priority)
2. <project_root>/AGENTS.md        (open standard for agent instructions)
```

The legacy `<project_root>/.daedalus/daedalus/system_prompt.txt` overlay and
the `<exe-dir>/system_prompt.txt` sidecar are **no longer read**. Migrate any
instructions from those files into `AGENTS.md`.

---

## Security considerations

- **`--auto` skips confirmation** for shell commands. Only use in trusted
  environments.
- **A2A agent daemon auto-approves** all commands unconditionally.
- **Network errors are non-fatal** — A2A calls return error JSON, never crash.
- **Crash handlers** write `messages.json.crash` before re-raising, enabling
  recovery on next launch.
- **Terminal state** is restored via `std::atexit` and signal handlers — the
  signal handler itself restores cursor visibility and SGR attributes using
  async-signal-safe `write()` (the `ThinkingSpinner` hides the cursor), so
  there is no cursor-left-behind on crash or kill.

---

## Things agents should know before editing

1. **Do not assume a local daedalus patch step exists** — `patch_daedalus.py`
   was removed; daedalus **library** API changes must go upstream. daedalus is fetched
   from its `main` branch at configure time, so upstream drift can break the
   build; pin via `GIT_TAG` in `CMakeLists.txt` if needed. The daedalus
   **demos**, however, are example code: icarus may vendor them into
   `src/ui/` and improve the copies freely (see the note under
   §Fetched dependencies).
2. **All terminal output goes through `AgentSink`**. Do not write directly to
   `std::cout`/`std::cerr` from daedalus-facing code.
3. **Every network call must degrade gracefully** — never let an A2A or HTTP
   failure crash the process.
4. **Run the build after changes**. The minimum verification is:
   ```bash
   cmake -S . -B build
   cmake --build build -j$(nproc)
   ./build/icarus --version
   ```
5. **Run cppcheck** if available:
   ```bash
   cmake --build build --target icarus_cppcheck
   ```
6. **New slash commands need three edits**: the handler in `main.cpp`,
   `detail::kSlashCommands` in `src/ui/InputReader.h` (hints + completion),
   and `print_help()` in `src/util.h`. Also update `docs/cli.md` for
   user-facing docs.
7. **New tables go through `icarus::ui::DrawTable`** (`src/ui/AnsiRenderer.h`)
   — never hand-roll table rendering.
8. **`.daedalus*` directories are agent runtime state** — never delete,
   rename, or edit them (they hold memory and session data).

---

## Notes for agents

- Prefer dedicated tools (`Read`, `Grep`, `Glob`) over `cat`/`grep` from
  `execute_bash` when available.
- Run the build and tests after non-trivial changes; do not assume the code
  compiles.
- Make minimal, targeted changes — review the diff before committing.
- Leave unrelated code alone unless the user explicitly asks.
- When requirements are ambiguous, ask the user rather than guessing.
- User-facing behaviour changes should be reflected in `Readme.md` and/or
  `docs/cli.md`.
