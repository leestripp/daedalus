# icarus

![banner](resources/banner.png)

**icarus** is a CLI AI coding agent built on the [daedalus](https://gitlab.com/leestripp/daedalus.git) framework. It is an autonomous coding agent that can explore repositories, read and write files, run shell commands, and call tools — either interactively from the terminal or as an A2A agent HTTP server.

## Features

- **Interactive CLI mode** — conversational coding agent with persistent context
- **Rich in-session commands** — 31 slash commands for models, providers, skills, tools, todos, settings, history, and more (see `docs/cli.md`)
- **A2A Agent Mode** — run as an HTTP server spoken to by an IDE or another A2A client
- **A2A Client Tool** — the running agent can call remote A2A agents as tools
- **MCP Support** — connects to MCP servers for tool access
- **Provider plugins** — daedalus's provider-plugin system (OpenAI-compatible,
  MiniMax, Moonshot/Kimi, LM Studio) with host auto-detection; payload quirks,
  temperature clamps, and auth-less local servers handled per provider
- **Context caching** — server-side prompt caching (e.g. Moonshot
  `prompt_cache_key`) with cache-hit visibility in `/stats` and `/usage`
- **Memory** — persistent conversation history across sessions
- **Streaming output** — real-time token streaming with delta rendering
- **Adaptive terminal UI** — boxed tables, tool-call cards, thinking spinner; all rendering follows the live terminal width (resize-aware), with `--plain` / `NO_COLOR` fallbacks
- **Line editing** — linenoise-backed input with history, ghost-text hints, and tab-completion for slash commands

## Building (Ubuntu)

### 1. Install dependencies

```bash
sudo apt update
sudo apt install -y \
    build-essential \
    cmake \
    git \
    pkg-config \
    nlohmann-json3-dev \
    libssl-dev \
    libpng-dev \
    libjpeg-turbo8-dev \
    libpoppler-cpp-dev \
    uuid-dev \
    libsqlite3-dev \
    zlib1g-dev
```

Requirements: a C++23 compiler (g++ 13+, clang also works) and CMake 3.28 or
newer. Both are satisfied by stock **Ubuntu 24.04 (noble)** packages. On
Ubuntu 22.04 the bundled CMake is too old; install a newer one via
`sudo snap install cmake --classic` or the
[Kitware APT repository](https://apt.kitware.com/).

Note: `libpoppler-cpp-dev` is required at configure time
(`pkg_check_modules(POPPLER REQUIRED poppler-cpp)`). The built-in daedalus
`read_pdf` tool lives inside the library, and icarus also links poppler-cpp
directly.

The daedalus library itself (plus linenoise and FTXUI) is fetched
automatically by CMake FetchContent from its `main` branch - no manual step
needed.

### 2. Build

```bash
# §Monorepo (2026-09-24): PREFERRED — build everything from the repo root:
#   (cd .. && cmake -S . -B build && cmake --build build -j$(nproc))
# One ./build for library + hosts. The standalone build below still works
# (dual-mode) but creates a per-component build/ you must clean up yourself.
cmake -S . -B build
cmake --build build -j$(nproc)
```

### 3. Install to `~/.local/bin/`

```bash
cmake --build build --target install
```

### 4. Verify

```bash
~/.local/bin/icarus --version
# -> icarus <git-derived-version>   # e.g. icarus 1.4.0
```

Make sure `~/.local/bin` is on your `PATH` (add
`export PATH="$HOME/.local/bin:$PATH"` to `~/.bashrc` if `icarus` is not found).

For containers, run the install as the runtime user so it lands in that
user's `~/.local/bin`, or override the prefix with
`-DCMAKE_INSTALL_PREFIX=/usr/local` / `cmake --install build --prefix ...`.

## Usage

### Interactive mode

```bash
icarus [options] [target_path]
```

**Key options:**

| Option | Description |
|---|---|
| `--model <name>` | Select a configured model (see `--model-list`) |
| `--model-list` | List configured models and exit |
| `--discover-models <name>` | Query the provider's `/v1/models` endpoint and exit |
| `--auto` | Auto-approve all tool calls without asking |
| `--no-memory` | Disable persistent conversation memory |
| `--clear-memory` | Clear all session memory and exit |
| `--home <path>` | Override the daedalus home directory |
| `--agent-name <name>` | Per-agent namespace for session files |
| `--threads <n>` | Number of worker threads |
| `--stream` / `--no-stream` | Enable/disable SSE streaming (default: on) |
| `--plain` | Alias for `--no-color --no-hud --no-unicode` |
| `--skills <dir>` | Additional skills directory (repeatable) |
| `--system-prompt <file>` | Override the project `AGENTS.md` system prompt |
| `--a2a-url <URL>` | A2A server URL for the `a2a` tool (default: `http://localhost:8000`) |
| `--a2a-agent` | Run as an A2A agent HTTP server |
| `--a2a-port <n>` | Port for `--a2a-agent` (0 = ephemeral, default) |
| `--migrate-data[-apply]` | Migrate legacy `~/.daedalus` data to XDG locations |
| `--version` | Print version and exit |

Run without arguments for an interactive REPL using the current directory as the target:

```bash
icarus
icarus --model minimax-m3 /path/to/project
icarus --auto /path/to/project  # no confirmations
```

Inside the REPL, type `/help` for the full command list. Highlights:

| Command | Description |
|---|---|
| `/models` | Discover models from the current provider |
| `/model`, `/provider` | Switch the active model / provider |
| `/settings` | Interactive settings TUI (providers, models, MCP, agent options) |
| `/tools`, `/skills` | List and enable/disable tools and skills |
| `/todo` | Manage the session todo list |
| `/history`, `/export` | Inspect or export the conversation |
| `/paths` | Show every config/data/project path in use |
| `/usage`, `/stats`, `/state` | Token, iteration, and agent-state info |

### A2A Agent Mode

In this mode, icarus runs as an HTTP server speaking the [Agent2Agent Protocol](https://a2a-protocol.org/latest/specification/) (A2A).

```bash
icarus --a2a-agent
icarus --a2a-agent --a2a-port 8080
```

`--model`, `--home`, `--skills`, `--system-prompt`, and `--no-memory` are honoured for
each task's agent. The agent card is served at
`http://127.0.0.1:<port>/.well-known/agent-card.json` and advertises the real bound URL.

The legacy `--acp-agent` flag is still accepted as a deprecated alias, but the protocol is now A2A over HTTP rather than ACP v1 over stdio JSON-RPC.

See [`docs/a2a-protocol.md`](docs/a2a-protocol.md) for protocol notes.

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                        icarus                           │
│                                                         │
│  ┌─────────────┐    ┌──────────────────────────────┐  │
│  │ daedalus    │    │ A2A Client Tool              │  │
│  │ Agent       │───▶│ get_agent_card / send_message│  │
│  │             │    │ get_task / list_tasks / ...  │  │
│  │             │    └──────────────┬───────────────┘  │
│  │  AgentSink  │                   │                  │
│  │ (console/   │    ┌──────────────▼───────────────┐  │
│  │  a2a_agent) │    │ daedalus::a2a::A2aClient     │  │
│  │             │    │ (OmniNet HTTP/SSE client)    │  │
│  └─────────────┘    └──────────────────────────────┘  │
│                                                         │
│  ┌─────────────────┐    ┌───────────────────────────┐  │
│  │ A2A Agent Mode   │    │ OmniNet                   │  │
│  │ --a2a-agent      │    │ (HTTP + SSE client)       │  │
│  └─────────────────┘    └───────────────────────────┘  │
└─────────────────────────────────────────────────────────┘
```

**Key components:**

| Component | File | Description |
|---|---|---|
| CLI entry / REPL | `src/main.cpp` | Argument parsing, slash commands, interactive loop |
| CLI args / signals | `src/util.h` | Flag parser, signal handling, system prompt resolution |
| Console sink | `src/ConsoleAgentSink.cpp` | Terminal output, spinner, approval prompts |
| ANSI renderer | `src/ui/AnsiRenderer.cpp` | Colors, boxes, `DrawTable`, width-aware text utils |
| Input stack | `src/ui/InputReader.h` etc. | linenoise line editing, hints/completion, input thread/queue |
| A2A Client Tool | `src/a2a_tools.cpp` | daedalus `BaseTool` wrapping the A2A client |
| A2A Agent Daemon | `src/a2a_agent.cpp` | A2A agent HTTP server backend |
| Migration helper | `src/migrate.cpp` | Legacy `~/.daedalus` → XDG migration |

## Configuration

daedalus home (where `daedalus.db` and model configs live):

```bash
icarus --home ~/.local/share/daedalus
# or via environment
export DAEDALUS_HOME=~/.local/share/daedalus
```

See available models:

```bash
icarus --model-list
```

The `/paths` in-session command shows every path icarus, daedalus, and OmniNet use —
config, database, data, cache, logs, skills, and the per-project session files.

## Documentation

- [`docs/cli.md`](docs/cli.md) — complete CLI and in-session command reference
- [`docs/design.md`](docs/design.md) — architecture and design rationale
- [`docs/ui-design.md`](docs/ui-design.md) — terminal UI design
- [`docs/a2a-protocol.md`](docs/a2a-protocol.md) — A2A protocol notes
- [`../AGENTS.md`](../AGENTS.md) — root contributor/agent guide (Icarus section)

## License

MIT License — see LICENSE file.
