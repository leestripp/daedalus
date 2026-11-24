# icarus CLI AI Agent — Design Document

> **Scope:** design decisions, architecture, data flow, and rationale.
> For usage instructions see `Readme.md`. For developer reference see the
> Icarus section of [`../../AGENTS.md`](../../AGENTS.md).

---

## 1. Design Goals

icarus was built with four non-negotiable constraints:

1. **No new external dependencies.** Everything comes from daedalus (FetchContent) or system packages. Every dependency in `CMakeLists.txt` existed before icarus.
2. **Consume daedalus's public A2A implementation.** The old standalone ACP v1 library (`src/acp/`) has been removed; icarus uses `daedalus::a2a` for both the client tool and the agent HTTP server.
3. **Graceful degradation.** Network failures (no A2A server, SSE not supported, server unreachable) must never crash or hang. Every network call has a fallback.
4. **Clean sink abstraction.** daedalus talks exclusively through `AgentSink` callbacks. All terminal output, formatting, and filtering lives in `ConsoleAgentSink` — the daedalus library never touches `std::cout` directly.

---

## 2. High-Level Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│  Shell                                                        │
│  $ ./build/icarus [options] [directory]                        │
│  $ ./build/icarus --a2a-agent                                  │
└────────────────────────┬────────────────────────────────────────┘
                         │
          ┌──────────────┴──────────────────┐
          │  main.cpp                       │
          │  · argument parsing              │
          │  · --migrate-data detection     │
          │  · --a2a-agent bypass          │
          │  · daedalus::AgentConfig setup │
          │  · REPL loop                   │
          └──────────────┬──────────────────┘
                         │
         ┌────────────────┼────────────────┐
         │                │                │
    ┌────▼─────┐   ┌──────▼──────┐  ┌────▼────────┐
    │ Console  │   │   A2aTool   │  │ A2aAgent   │
    │ Agent    │   │  (a2a tool) │  │ (a2a agent)│
    │ Sink     │   │             │  │            │
    └────┬─────┘   └──────┬──────┘  └────┬────────┘
         │                │                │
  ┌──────▼────────────────▼──────┐  ┌─────▼─────────┐
  │      daedalus::Agent         │  │ A2A HTTP      │
  │  · state machine             │  │ server        │
  │  · tool registry            │  │ (daemon mode) │
  │  · LLM client              │  │               │
  │  · session memory          │  └───────────────┘
  └──────┬──────────────────────┘
         │
  ┌──────▼──────────────────────────────────┐
  │  daedalus::a2a::A2aClient               │
  │    └─► OmniNet HTTP/SSE client          │
  │         (daedalus dependency)            │
  └───────────────────────────────────────────┘
```

---

## 3. Two Operating Modes

### 3.1 Interactive CLI Mode (default)

```
stdin → main.cpp REPL → daedalus::Agent → ConsoleAgentSink → terminal
```

The REPL in `main.cpp` reads lines from stdin using the linenoise-backed `InputReader`. Every non-empty, non-command line is passed to the agent:

```cpp
agent.submit_user_message(line);
agent.run_one_turn();
```

`ConsoleAgentSink` handles all rendering:
- Assistant text deltas → buffered output, flushed at newline or 2048 chars
- Tool calls → terminal save/restore around `execute_bash`
- State transitions → spinner updates
- Log messages → every message to `logs.txt`; Warn/Error/Debug also to
  stderr (Info chatter stays out of the terminal)

User input is read on a dedicated thread (`InputThread`) through the
linenoise-backed `InputReader` and handed to the main loop via an
`InputQueue`. Ghost-text hints and tab-completion for slash commands are
driven by a single canonical table (`detail::kSlashCommands` in
`src/ui/InputReader.h`) — add new commands there and in `main.cpp`.

Background process exits use the same main-thread scheduler without creating
a second agent thread. Daedalus queues the `trigger:background-process`
message and invokes `ConsoleAgentSink::on_turn_requested()` on its process
monitor; the sink only wakes `InputQueue`. Before blocking for terminal input,
the main loop checks `Agent::pending_count()` and drives any queued completion
with `run_one_turn()`. The completion is never submitted again, and the agent
may safely end the preceding turn with `Done` or `stop` instead of polling.

### 3.2 A2A Agent Daemon Mode (`--a2a-agent`)

```
A2A client ──HTTP──► icarus --a2a-agent
        ◀──JSON/SSE responses── stdout / HTTP
```

The main loop is entirely bypassed. `a2a::run_a2a_agent()` runs an HTTP server using `daedalus::a2a::A2aServer` and a custom `IA2aTaskBackend`. Each incoming task spawns a dedicated `daedalus::Agent` and streams events back to the caller.

The per-task agents are configured from a template built from the CLI args,
so `--model`, `--home`, `--skills`, `--system-prompt`, `--no-memory`, and
`--agent-name` behave the same as in interactive mode; only the project root
(current directory) and `auto_approve = true` are forced. Once the server
has bound, the backend's agent card is updated to advertise the real bound
URL. A bind failure exits with code 1.

---

## 4. The daedalus Agent as the Core Engine

The `daedalus::Agent` class is the black box that drives all AI interactions. icarus does not call any LLM directly — it configures `AgentConfig` and hands it to daedalus.

### 4.1 AgentConfig fields used

```cpp
cfg.project_root         = target_path;         // Working directory for the agent
cfg.model_name          = args.model_override;  // Selected model name
cfg.auto_approve        = args.auto_mode;       // Skip tool-approval prompts
cfg.memory_enabled      = !args.no_memory;      // Persistent session memory
cfg.clear_memory        = args.clear_memory_flag;
cfg.worker_threads      = args.thread_count_override;
cfg.max_agent_iterations = 600;
cfg.verification_enabled = true;                // Experimental worker → verifier loop
cfg.stream              = args.stream_enabled;   // SSE token streaming
cfg.daedalus_home       = resolved_path;         // SQLite DB location
cfg.agent_name          = args.agent_name;       // Per-agent namespace
cfg.skills_dirs         = {...};                // Skill discovery paths
cfg.system_prompt       = resolve_system_prompt(...);
```

The experimental verification supervisor keeps worker and verifier roles on
the same model, tools, and transcript. Background completions can wake either
the normal input queue or the supervisor's suspended steer queue.

### 4.2 What daedalus owns

| Concern | Owner |
|---|---|
| LLM API calls (HTTP to OpenAI-compatible endpoint) | `daedalus::LLMClient` |
| Message history + compaction | `daedalus::Messages` |
| Tool execution + approval flow | `daedalus::ToolRegistry` |
| MCP server registry | `daedalus::MCPHub` |
| Settings persistence (SQLite) | `daedalus::SettingsStore` |
| SSE parsing for streaming responses | `daedalus` (MiniMaxSseParser) |
| State machine (Understanding→Planning→Acting→Verifying) | `daedalus::AgentImpl` |
| Crash-signal handlers | `daedalus::AgentImpl::install_crash_handlers()` |
| A2A client/server implementation | `daedalus::a2a` |

### 4.3 What icarus owns

| Concern | File |
|---|---|
| CLI argument parsing | `src/util.h` |
| REPL loop + slash commands | `src/main.cpp` |
| Terminal rendering | `src/ConsoleAgentSink.cpp`, `src/ui/AnsiRenderer.cpp` |
| Line editing / input pipeline | `src/ui/InputReader.h`, `InputThread.h`, `InputQueue.h` |
| A2A client tool registration | `src/a2a_tools.cpp` |
| A2A agent HTTP server | `src/a2a_agent.cpp` |
| Legacy settings migration | `src/migrate.cpp` |
| Settings TUI embed + post-TUI runtime sync | `src/main.cpp` (tab sources from daedalus's `demos/daedalus-settings-tui/`) |
| Signal handling (SIGINT/SIGTERM/crash chaining) | `src/util.h` |

---

## 5. A2A Client Tool Design

### 5.1 Why a BaseTool?

daedalus has a built-in mechanism for extending agent capabilities: `daedalus::BaseTool`. Registering `A2aTool` in the agent's tool registry means the agent can invoke A2A actions by name (e.g. `"get_agent_card"`) just like any other tool. The agent decides *when* to call A2A — not the other way around.

This is the right model because the agent has the full conversation context. It can decide to delegate a sub-task to a specialized A2A agent rather than handling it directly.

### 5.2 Tool call flow

```
daedalus agent decides to call a2a.send_message
    │
    ▼
A2aTool::execute(args, ctx)
    │
    ├─► create_client(url)   → daedalus::a2a::A2aClient → A2A server
    │
    ├─► REST/SSE call
    │
    └─► returns structured JSON result to agent
```

The result is a structured `nlohmann::json` object, not raw text. The agent decides how to interpret it. Streaming actions collect events synchronously and return them as a JSON array.

---

## 6. A2A Agent Daemon Design

### 6.1 Why HTTP?

A2A uses HTTP as its transport. icarus runs an HTTP server bound to `127.0.0.1` (by default on an ephemeral port). This lets any A2A client — an IDE plugin, another agent, or `curl` — communicate with icarus without managing a subprocess stdio pipe.

### 6.2 Task lifecycle

Each incoming message creates a fresh `daedalus::Agent` configured to auto-approve commands. The backend stores tasks in a process-local in-memory map (there is no task database and restart does not recover them) and supports:

- `send_message` / `send_streaming_message`
- `get_task`, `list_tasks`, `cancel_task`
- `subscribe_to_task` (not implemented — returns 501)

HTTP routes use `/v1/message:send`, `/v1/message:stream`, `/v1/tasks`,
`/v1/tasks/{id}`, `/v1/tasks/{id}:cancel`, and
`/v1/tasks/{id}:subscribe`; the agent card remains at
`/.well-known/agent-card.json`.

### 6.3 A2aAgentSink — callback to event bridge

```
daedalus callback              A2A event
─────────────────────────      ─────────────────────────────
on_assistant_delta(text)   →   collected assistant text
on_state_change(from,to)   →   TaskStatusUpdateEvent
```

The sink is intentionally minimal: it collects assistant text and forwards state changes as status events. Bash commands are auto-approved because the daemon has no interactive terminal.

---

## 7. ConsoleAgentSink Design

### 7.1 The sink contract

`daedalus::AgentSink` is the only surface area through which daedalus communicates with the host. Every string daedalus emits — log messages, assistant deltas, tool results, state transitions — arrives as a callback. **daedalus never writes to `std::cout` or `std::cerr` directly.**

This contract is intentional. It means:
- A TUI host can plug in ratatui/ncurses instead of printing to stdout
- A web host can send deltas over WebSocket
- A test host can capture all output in memory

### 7.2 Text delta buffering

`on_assistant_delta` fires with individual tokens or small fragments. Terminals are slow; writing every fragment as a separate `std::cout <<` call causes visible flickering and PIPEstatus issues with large outputs.

`ConsoleAgentSink` buffers deltas in `delta_buffer_`. Flush is triggered when:
- The buffer reaches `DELTA_FLUSH_THRESHOLD` (2048 chars), OR
- A newline character appears anywhere in the buffered text

The buffer is protected by `delta_mutex_` because delta callbacks and flush calls can come from different threads (agent worker vs. state-change flush).

### 7.3 Banner suppression

daedalus emits its startup banner via `on_log(Info, "╔═══ ... Autonomous Coding Agent ... ╚═══")`. The demo `ConsoleAgentSink` that ships with daedalus forwards all Info logs to stdout — which caused the duplicate banner problem.

The fix is a single string filter at the top of `on_log`, before both the
file write and the terminal write, so the library banner is dropped
entirely:

```cpp
if (message.find("Autonomous Coding Agent") != std::string_view::npos) {
    return;  // drop the library banner
}
```

This is the right place for the filter because:
- `on_log` is the correct abstraction boundary for log routing
- The library cannot know whether its banner should appear in the host's UI
- Other Info messages are preserved (they are written to `logs.txt`, just
  not to the terminal)

### 7.4 Thinking spinner

`ThinkingSpinner` renders a terminal spinner with the current daedalus state name. The spinner starts when daedalus enters a thinking state and stops when output arrives or the state becomes terminal. A watchdog thread restarts the spinner during long quiet gaps.

### 7.5 Rendering layer (`src/ui/AnsiRenderer.*`)

All visual output goes through one renderer:

- `RenderConfig::detect()` probes the terminal once at startup (color,
  Unicode, HUD); `--plain` / `--no-color` / `NO_COLOR` degrade it.
- `effective_terminal_width()` re-queries the width at render time
  (`TIOCGWINSZ`, then `$COLUMNS`), so boxes, tables, rules, and the prompt
  header follow terminal resizes and zooms mid-session. There is no
  SIGWINCH handler — the live value is read on every draw.
- `make_box()` renders left-justified boxes clamped to the live width
  (`Alignment::Auto` resolves to `Left` — all agent output is
  left-justified).
- `DrawTable` is the single fluent front end for every table in the app
  (`/models`, `/skills`, `/tools`, `/paths`, …). It auto-sizes columns,
  shrinks the widest columns down to their `min_width` when the table would
  exceed the terminal, and truncates over-long cells with `…`.
- Width math is codepoint-aware (UTF-8 decode + wcwidth-style display
  widths), so CJK and emoji count correctly.

### 7.6 Command approval

`request_command_approval()` draws the command in a box and reads a line
through the injected linenoise reader. Answers: `y` approves once, `n` (or
anything else / EOF) refuses, and `a` sets a session-scoped flag on the
sink that auto-approves all subsequent prompts. `--auto` bypasses the
prompt entirely inside daedalus; the A2A daemon sink always approves.

---

## 8. Signal Handling Design

### 8.1 SIGINT / SIGTERM — graceful cancellation

```
SIGINT (Ctrl+C)
    │
    ▼
daedalus_signal_handler(int sig)
    │
    ├─► daedalus_cli_restore_terminal_state()   (show cursor, reset raw mode)
    ├─► g_agent_for_signal.load()->cancel()     (tell daedalus to stop)
    └─► g_shutting_down.store(true)              (tell main.cpp REPL to exit)
```

`g_agent_for_signal` is an atomic pointer to the current `daedalus::Agent`. It is set after the agent is constructed and cleared after `shutdown()`. This allows the signal handler to call `cancel()` on the agent from any context.

`cancel()` causes daedalus to stop the current turn at the next await point. The REPL exits cleanly with a message rather than a raw process termination.

### 8.2 Crash signals — last-resort terminal restore

`agent.install_crash_handlers()` installs async-signal-safe handlers for `SIGSEGV`, `SIGABRT`, `SIGFPE`, `SIGILL`. icarus chains its own terminal-restoring handler in front of them so a crash does not leave the TTY in raw mode or with a hidden cursor.

---

## 9. Settings and Data Layout

### 9.1 XDG-compliant paths

```
$XDG_DATA_HOME/daedalus/              default: $HOME/.local/share/daedalus/
$XDG_DATA_HOME/daedalus/daedalus.db   SettingsStore (models, MCP servers, settings/mirrors)
$XDG_DATA_HOME/daedalus/skills/       installed skills
$XDG_CONFIG_HOME/daedalus/            default: $HOME/.config/daedalus/
$XDG_CONFIG_HOME/daedalus/config.json  legacy path; use `icarus --migrate-data[-apply]` explicitly
```

The distinction between data (`daedalus.db`) and config (`config.json` fallback) follows the XDG standard: data is for programs to read/write, config is for user-editable preferences.

### 9.2 Why `user_data_root()` not `user_config_root()`?

The `SettingsStore` SQLite database lives at `<daedalus_home>/daedalus.db`. This is stored in the **data** hierarchy, not config. Using `user_config_root()` would point `daedalus_home` at `$HOME/.config/daedalus/` — a directory that exists and is writable, but never contains the actual DB. The result was `--model-list` reporting `(none configured)` against an empty DB at the wrong path.

The fix: `cfg.daedalus_home = daedalus::Paths::global().user_data_root()`.

### 9.3 Settings TUI and runtime sync

Bare `/settings` opens the daedalus settings-demo TUI (FTXUI tabs:
Providers / Models / MCP Servers / Agent) **in-process** — the tab sources
from `demos/daedalus-settings-tui/` are compiled into the icarus binary
and driven via `settings_tui::RunApp(SettingsStore::for_home(home))`, so
the TUI edits the same `daedalus.db` the agent uses. Non-TTY (piped)
`/settings` prints a read-only table summary instead, and `/settings`
takes no arguments (the old subcommands were removed; `/providers`,
`/models --configured`, `/provider`, `/model` cover the read/switch
cases, and `/provider-add`/`/model-add` etc. cover scripting).

The TUI writes through to SQLite, but a running daedalus agent snapshots
most settings at `initialize()` / switch time. The `/settings` handler
therefore snapshots the store before opening the TUI and, on close,
pushes changes into the live agent through every runtime path daedalus
exposes:

| TUI change | Runtime application |
|---|---|
| `auto_approve` | `Agent::set_auto_approve()` immediately (skipped when `--auto` is authoritative, §BUG-006) |
| `current_provider` | `Agent::switch_provider()` (also seeds the row from the active model when empty — daedalus only writes it on `switch_provider()`) |
| Active model/provider edits (host, key, endpoints, temperature) | `Agent::switch_model()` on the same model — rebuilds the client config from the store; resets conversation history (§MODEL-SWITCH) and says so |
| Active model deleted | Warning to pick another with `/model` |
| MCP server enable/disable | `Agent::set_mcp_server_enabled()` (live connect/disconnect) |
| MCP add/edit/remove; iteration/size limits; retry; Serper key | No runtime API — icarus prints a "fully apply on next launch" note |

Two caveats shaped this design: re-selecting a model clears conversation
history, so the re-apply only fires on a real before/after diff; and the
sync is host-side — daedalus exposes no "reload settings" API, so
anything without a runtime setter can only be reported, not applied.

---

## 10. Build Dependency Graph

```
CMakeLists.txt (icarus)
    │
    ├─► daedalus (FetchContent main)
    │       ├─► OmniNet
    │       │       ├─► fmt
    │       │       ├─► nlohmann_json
    │       │       └─► sqlite_vec
    │       └─► sqlite-vec
    │
    ├─► linenoise (FetchContent pinned commit)
    └─► FTXUI (FetchContent pinned v7.0.1, for the embedded settings TUI)
```

The settings-TUI tab sources compile straight from the fetched daedalus
tree (`${DAEDALUS_SRC_DIR}/demos/daedalus-settings-tui/`, minus its
`main.cpp`); the standalone demo is disabled in the daedalus build via
`-DDAEDALUS_DEMO_SETTINGS_TUI=OFF`. `<daedalus/settings.h>` includes
`<db/Database.h>`, so the OmniNet source dir inside the daedalus build
tree is on the icarus include path.

icarus links OmniNet by path (`${OMNINET_BUILD_DIR}/libOmniNet.a`), not as a CMake imported target. This avoids CMake configuration issues when daedalus changes its internal dependency layout.

---

## 11. Error Handling Philosophy

Three rules govern error handling across icarus:

1. **Network errors are never fatal.** Every A2A call returns an empty/invalid result, not an exception. The A2A tool returns `{"status":"error","message":"..."}` so the agent can decide how to respond.

2. **Assertions are never in production paths.** All `assert()` calls are guarded by `#ifdef DEBUG` or are in test binaries only. Production code uses `if (!result) return {}`.

3. **Exceptions escape only to `main()`.** `agent.run_one_turn()` can throw (daedalus internal errors). The `try/catch` in `main()` catches `std::exception`, restores the terminal, logs the error, and exits with code 1. No exception propagates past `main()`.

---

## 12. Design Decisions Summary

| Decision | Choice | Reason |
|---|---|---|
| HTTP transport | OmniNet (daedalus dep) | Already available; removes raw libcurl from icarus |
| ACP v1 library | Removed | Replaced by daedalus's built-in A2A implementation |
| A2A client as BaseTool | `A2aTool` registered in agent | Agent decides when to delegate; natural tool model |
| A2A agent transport | HTTP server | Standard A2A transport; no subprocess management |
| A2A daemon config | CLI args → per-task `AgentConfig` template | `--model`/`--home`/etc. behave identically in both modes |
| Banner suppression | `on_log` filter | daedalus never touches stdio; sink is the right boundary |
| Log routing | Info → file only; Warn/Error/Debug → stderr + file | Keeps informational chatter out of the conversation |
| daedalus home path | `user_data_root()` | SettingsStore DB is in XDG data hierarchy |
| Delta buffering | 2048-char threshold or newline | Realtime output without terminal flicker |
| Delta buffer thread safety | `std::mutex` | Agent worker and flush can touch buffer concurrently |
| Crash handlers | Signal-async-safe, chained | Survive crashes without losing terminal state |
| Box/table alignment | Always left-justified | Mixed justification looked like a layout bug |
| Terminal width | Live `TIOCGWINSZ` query per render | Resize/zoom mid-session just works; no SIGWINCH handler |
| Tables | Single `DrawTable` builder | Consistent sizing/truncation; one place to fix |
| Settings editing | Embedded FTXUI TUI from daedalus's demo sources | One binary, edits the same store via the public API; no local fork of the TUI code |
| Slash-command hints/completion | One canonical `kSlashCommands` table | Hints and completion can never drift apart |
