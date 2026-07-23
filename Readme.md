![daedalus Banner](resources/banner.png)

# daedalus

An autonomous agent library for C++23. It ships as `libdaedalus.a`
plus four worked example hosts (`demos/`). Embed it in any C++23 host
(CLI, web, IDE plugin, CI bot) and drive one or more `Agent` instances
via the public API. See `docs/embedding.md` to get started.

**Current tag: `0.4.1`** — stable library. See `CHANGELOG.md` for what's
in each release. See `docs/storage.md` for the storage architecture,
`docs/settings.md` for the SQLite-backed settings model (v0.4.0+).

## File Locations

| Path | Contents |
|------|----------|
| `~/.local/share/daedalus/daedalus.db` | Settings store (models, key/value, MCP servers) — v0.4.0+ |
| `~/.local/share/daedalus/skills/installed/` | Agent skills |
| `~/.local/share/daedalus/memory/` | Session memory |
| `<project>/.daedalus[-<agent_name>]/messages.json` | Conversation transcript (atomic-rename save; auto-flush every ~100 ms) |
| `<project>/.daedalus[-<agent_name>]/messages.json.crash` | Crash mirror (written by SIGSEGV/SIGABRT/SIGFPE/SIGILL handler; recovered on next launch) |
| `<project>/.daedalus[-<agent_name>]/` | Other per-project state for each `Agent` |
| `/etc/xdg/daedalus/` (system) | System-wide defaults |

See [docs/crash_resilience.md](docs/crash_resilience.md) for the full
crash-recovery story (atomic save, write-through auto-flush, async-signal-safe
crash handler, drop-last-message recovery).

## Quick Start (Ubuntu)

### 1. Install Dependencies

```bash
sudo apt update
sudo apt install -y \
    cmake \
    clang \
    libcurl4-gnutls-dev \
    libssl-dev \
    libsqlite3-dev \
    libpng-dev \
    libpoppler-cpp-dev
```

OmniNet (`build/_deps/omninet-src/`), `nlohmann::json`, and
`sqlite-vec` are pulled in by CMake's `FetchContent` on the first
build — no apt packages needed for those. See [Dependencies](#dependencies)
for the full list.

### 2. Build

```bash
# From project root
cmake -S . -B build

# Use all available CPU cores
cmake --build build -j$(nproc)
```

### 3. Install

```bash
cmake --install build
```

### 4. Run

```bash
# Ensure ~/.local/bin is in your PATH
export PATH="$HOME/.local/bin:$PATH"

# Run on current directory:
./build/demos/daedalus-cli/daedalus-cli

# Or a different project directory (positional arg):
./build/demos/daedalus-cli/daedalus-cli /path/to/project

# Convenience aliases (optional, add to ~/.bashrc):
alias daedalus='./build/demos/daedalus-cli/daedalus-cli'
alias daedalus-webui='./build/demos/daedalus-webui/daedalus-webui 8088'
```

### 5. Embed daedalus in your own host (recommended for new users)

`libdaedalus.a` exposes one class, `daedalus::Agent`, that drives a single
agent loop. Hosts implement `daedalus::AgentSink` to receive streamed
events (tokens, tool calls, state changes, logs).

```cpp
#include <daedalus.h>
#include <daedalus/paths.h>   // §Plan.md: Paths::global() for XDG paths
#include <iostream>

class StdoutSink : public daedalus::AgentSink {
public:
    void on_assistant_delta(std::string_view text) noexcept override {
        std::cout << text;
    }
    void on_tool_call(std::string_view name, const nlohmann::json&) noexcept override {
        std::cerr << "\n[tool] " << name << " ...\n";
    }
    void on_tool_result(std::string_view name, const nlohmann::json& r) noexcept override {
        std::cerr << "[tool] " << name << " -> " << r.dump() << "\n";
    }
    void on_state_change(int /*from*/, int to) noexcept override {
        std::cerr << "\n[state] " << daedalus::to_string(
            static_cast<daedalus::AgentState>(to)) << "\n";
    }
    void on_log(daedalus::LogLevel lvl, std::string_view msg) noexcept override {
        std::cerr << "[" << daedalus::to_string(lvl) << "] " << msg << "\n";
    }
    bool request_command_approval(std::string_view cmd) override {
        std::cerr << "Approve: " << cmd << " [y/N] " << std::flush;
        char c{}; std::cin.get(c);
        std::cin.ignore(1 << 16, '\n');
        return c == 'y' || c == 'Y';
    }
};

int main() {
    daedalus::AgentConfig cfg;
    cfg.project_root  = "/path/to/project";

    // §Plan.md migration (2026-07-13): default to the XDG-compliant
    // locations via Paths::global() so this matches what
    // `daedalus-cli --migrate-data` writes to. Override here only if
    // you have a specific reason (sandboxed CI, custom mount, etc.).
    cfg.daedalus_home  = daedalus::Paths::global().user_config_root();
    cfg.session_id    = "my-host-session";         // for log correlation

    // §Phase 9.7 — opt in to skills (default = no skills).
    // Leave empty if your host doesn't want skill discovery.
    cfg.skills_dirs   = { daedalus::Paths::global().global_skills_dir() };
    cfg.skills        = { };   // or list specific names to activate

    daedalus::Agent agent(std::move(cfg), std::make_shared<StdoutSink>());
    agent.initialize();

    // §Phase 8.6 — show which model this session is bound to.
    auto mi = agent.active_model();
    std::cerr << "[model] " << mi.name
              << " (" << mi.model_id << ") @ " << mi.host
              << " — max ctx " << mi.max_context_size << "\n";

    std::string input;
    while (std::getline(std::cin, input) && input != "/quit") {
        agent.submit_user_message(std::move(input));
        agent.run_one_turn();          // one turn = one user msg → loop → stop/cancel/error
        std::cout << '\n';
    }
    // dtor calls cleanup() automatically; explicit call is also fine.
    return 0;
}
```

Link with `libdaedalus.a`. The library does not call `std::cout`,
`std::cerr`, or `std::cin` itself — every UI bit flows through the sink
or the host's main loop.

See [`docs/embedding.md`](docs/embedding.md) for the full embedding guide
and the complete `AgentSink` event reference.

---

## System Requirements

- **OS**: Ubuntu 24.04+ (tested on Ubuntu 26.04)
- **Compiler**: Clang 17+ (GCC 13+ also fine) (C++23)
- **CMake**: 3.28+
- **Disk Space**: ~100 MB

---

## Dependencies

| Package | Version | Description |
|---------|---------|-------------|
| `cmake` | 3.28+ | Build system |
| `clang` | 17+ | C++23 compiler (gcc 13+ also fine). Pinned as the default in CMake; `CC=gcc CXX=g++ cmake -S . -B build` to override. |
| `libcurl4-gnutls-dev` | any | HTTP client library |
| `libssl-dev` | any | TLS (used by OmniNet) |
| `libsqlite3-dev` | any | Local DB (used by OmniNet) |
| `libpng-dev` | any | PNG image support |
| `libpoppler-cpp-dev` | any | PDF rendering |

The following are pulled in automatically by CMake's `FetchContent`:

- [OmniNet](https://gitlab.com/leestripp/omninet) v0.8.6+
  (`build/_deps/omninet-src/`) — HTTP, SSE, WebSocket, TLS. Provides
  `nlohmann::json` transitively.
- `nlohmann_json` 3.11+ — JSON parsing (via OmniNet FetchContent).
- `sqlite-vec` 0.1.6+ — vector storage (via OmniNet FetchContent).

You do not need a test framework package. Tests use a tiny in-tree
`TEST()` / `EXPECT()` macro pair (`tests/CMakeLists.txt`); no doctest
or gtest dependency.

### Install All Dependencies at Once

```bash
sudo apt install -y cmake clang libcurl4-gnutls-dev libssl-dev libsqlite3-dev libpng-dev libpoppler-cpp-dev
```

OmniNet, nlohmann_json, and sqlite-vec are downloaded by CMake on the
first build into `build/_deps/`.

---

## Build Options

### Debug Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

### Release Build (default)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Run Tests

```bash
ctest --test-dir build --output-on-failure
```

### Static Analysis (optional)

```bash
# Install cppcheck
sudo apt install -y cppcheck

# Run analysis (daedalus library sources only; does not scan fetched dependencies)
cmake --build build --target daedalus_cppcheck
```

---

## Configuration

> **Where state lives (read this if you're new):** daedalus uses
> [XDG-compliant](https://specifications.freedesktop.org/basedir-spec/basedir-spec-latest.html)
> paths by default. LLM keys, model config, and MCP servers live in a
> SQLite database at `$XDG_DATA_HOME/daedalus/daedalus.db` (default
> `~/.local/share/daedalus/daedalus.db`) — see
> [`docs/settings.md`](docs/settings.md) for the `SettingsStore` API.
> Globally-installed skill packs live under `$XDG_DATA_HOME/daedalus/skills/`
> (default `~/.local/share/daedalus/skills/`). Per-project state — **including
> long-term memory** — lives under `<project_root>/.daedalus[-<agent_name>/]`.
> See [`docs/storage.md`](docs/storage.md) for full details, env-var
> resolution order, and a migration recipe (`daedalus-cli --migrate-data`).
>
> The legacy flat `~/.daedalus/` layout is honoured as a one-release-cycle
> read-fallback; daedalus will print a one-time deprecation warning when
> it sees it. Run `daedalus-cli --migrate-data-apply` then `rm -rf ~/.daedalus`
> to silence it.

### Adding Models

As of v0.4.0 there is no hand-edited `config.json`. Models are
configured through the `daedalus::SettingsStore` API (SQLite-backed)
or from the CLI:

```bash
# Interactive REPL picks the active model
daedalus-cli --model-list             # list configured models
daedalus-cli --model NAME             # use a specific model for this session

# Direct DB seeding (for packagers / CI) — see docs/settings.md §3
sqlite3 ~/.local/share/daedalus/daedalus.db \
  "INSERT INTO models (name, host, api_key, model_id, max_context_size) \
   VALUES ('minimax-m3', 'https://api.minimax.io', 'YOUR_API_KEY', 'MiniMax-M3', 500000);"
```

From C++ host code:

```cpp
#include <daedalus/settings.h>
auto& store = daedalus::SettingsStore::instance();
store.upsert_model(daedalus::ModelConfig{
    .name      = "minimax-m3",
    .host      = "https://api.minimax.io",
    .api_key   = std::getenv("MINIMAX_API_KEY"),  // avoid hard-coding
    .model_id  = "MiniMax-M3",
    .max_context_size = 500000,
});
store.set_default_model_name("minimax-m3");
```

Two real-world tips:

- Set the default model to one you've actually inserted (the library
  resolves it at session start). Switch at runtime with
  `Agent::switch_model(name)`.
- The webui demo (`demos/daedalus-webui/`) defaults to `auto_approve`
  on; the CLI flips it with `--auto`. `auto_approve` is a boolean
  that lives in **both** `AgentConfig::auto_approve` (per-session) and
  `SettingsStore::get_auto_approve()` (persistent) — the runtime
  effective value is the OR of the two. See
  [`docs/settings.md`](docs/settings.md) §2.4 for the store API.


## MCP Server Configuration

daedalus supports connecting to MCP (Model Context Protocol) servers for extended capabilities like web search, image analysis, and more. v0.5.0+ stores MCP servers in the SQLite-backed `SettingsStore` (`mcp_servers` table). v0.4.x used a JSON file at `$XDG_CONFIG_HOME/daedalus/mcp/servers.json` — that path is still supported as a one-release-cycle fallback. See [`docs/mcp_server.md`](docs/mcp_server.md) for full details.

### Configuration (programmatic, recommended)

From C++ host code or from the `daedalus::SettingsStore` API:

```cpp
#include <daedalus/settings.h>
auto& store = daedalus::SettingsStore::instance();
daedalus::SettingsStore::McpServer srv{
    .name        = "minimax-web-search",
    .command     = "uvx",
    .args        = {"minimax-coding-plan-mcp", "web-search"},
    .transport   = "stdio",
    .description = "Web search using MiniMax MCP",
};
store.upsert_mcp_server(srv);
```

### Configuration (JSON, legacy — supported)

The legacy file location is `$XDG_CONFIG_HOME/daedalus/mcp/servers.json` (default `~/.config/daedalus/mcp/servers.json`):

```bash
mkdir -p ~/.config/daedalus/mcp
nano ~/.config/daedalus/mcp/servers.json
```

### Example Configuration

```json
{
  "mcpServers": {
    "minimax-web-search": {
      "command": "uvx",
      "args": ["minimax-coding-plan-mcp", "web-search"],
      "env": {
        "MINIMAX_API_HOST": "https://api.minimax.io",
        "MINIMAX_API_KEY": "${MINIMAX_API_KEY}"
      },
      "description": "Web search using MiniMax MCP"
    },
    "minimax-read-image": {
      "command": "uvx",
      "args": ["minimax-coding-plan-mcp", "read-image"],
      "env": {
        "MINIMAX_API_HOST": "https://api.minimax.io",
        "MINIMAX_API_KEY": "${MINIMAX_API_KEY}"
      },
      "description": "Read and analyze images using MiniMax MCP"
    },
    "wikipedia": {
      "command": "uvx",
      "args": ["--from", "mcp-server-wikipedia", "wikipedia-mcp-server"],
      "description": "Search Wikipedia for information"
    }
  }
}
```

### Environment Variables

Store API keys in `$XDG_CONFIG_HOME/daedalus/mcp/.env` (default `~/.config/daedalus/mcp/.env`):

```bash
MINIMAX_API_KEY=your_api_key_here
```

Reference environment variables in `servers.json` using `${VAR_NAME}` syntax.

### Unified MCP Tool

Instead of separate tools, daedalus uses a single `mcp` tool that routes to any configured server:

```
mcp(server, tool, arguments)
  - server: Name of the MCP server (e.g., 'minimax-web-search', 'wikipedia')
  - tool: Tool name on the server (e.g., 'web_search', 'search', 'read_image')
  - arguments: Tool-specific arguments (optional)
```

**Example Usage:**
- Web search: `mcp(server='minimax-web-search', tool='web_search', arguments={'query': 'search term'})`
- Wikipedia: `mcp(server='wikipedia', tool='search', arguments={'query': 'search term'})`
- Image analysis: `mcp(server='minimax-read-image', tool='read_image', arguments={'path': '/path/to/image.png'})`

**List servers:** `mcp(server='servers', tool='servers')`

### Adding More MCP Servers

Add new servers to `$XDG_CONFIG_HOME/daedalus/mcp/servers.json` (default `~/.config/daedalus/mcp/servers.json`). Popular MCP servers include:
- `mcp-github` - GitHub operations
- `@modelcontextprotocol/server-filesystem` - File system access
- `mcp-brave-search` - Web search via Brave

See [docs/mcp_server.md](docs/mcp_server.md) for full documentation.

---

## Running

The CLI binary lives at `build/demos/daedalus-cli/daedalus-cli` after
`cmake --build build`. Examples below assume you've either added it
to `$PATH` (e.g. `cmake --install build` puts it at
`~/.local/bin/daedalus`) or that you're cd'd into the project root.

### Basic Usage

```bash
daedalus                       # project = current directory
daedalus /path/to/project      # project = given directory
```

### With Project Directory (via positional)

```bash
daedalus /path/to/project
```

### Auto Mode (skip confirmations)

```bash
daedalus --auto
daedalus --auto /path/to/project
```

### Command-Line Options

| Option | Description |
|--------|-------------|
| `-h, --help` | Show help message |
| `--version, -v` | Show version information |
| `--auto` | Auto-approve bash commands |
| `--no-memory` | Disable session memory for this session |
| `--clear-memory` | Clear all session memory |
| `--model NAME` | Use the named model from the SQLite settings store |
| `--model-list` | List available models from the SQLite settings store and exit |
| `--threads N` | Number of worker threads (default: hardware concurrency) |

### Running Modes

> **§Phase 2.7 (2026-07-10):** the four old "modes" (Interactive, Sub-Agent,
> Sub-Agent Looping, Manager) have been collapsed. The agent has exactly
> one loop. Patterns that used to be flags are now demos:
>
> | Old flag | Now lives at |
> |----------|--------------|
> | `--manager-mode` | `demos/daedalus-manager/` |
> | `--sub-agent` | `demos/daedalus-subagent/` |
> | `--sub-agent-looping` | `demos/daedalus-subagent/` |
>
> `--sub-agent`, `--sub-agent-looping`, and `--manager-mode` are no
> longer accepted by `daedalus`. These patterns are now implemented
> as separate demos: `demos/daedalus-subagent/` and
> `demos/daedalus-manager/`.

---

## Demos

The library ships as `libdaedalus.a`; concrete hosts live under
`demos/`. **Four demos** are provided as reference implementations of
the patterns `daedalus::Agent` enables:

| Demo | Build flag | Path | What it shows |
|------|------------|------|---------------|
| `daedalus-cli` | always-on | `demos/daedalus-cli/` | Reference host: console REPL with `ConsoleAgentSink` (cout for tokens, cerr for logs, y/n prompt for bash approval). The canonical "embed example" referenced by [`docs/embedding.md`](docs/embedding.md). |
| `daedalus-webui` | `-DCODEMAX_DEMO_WEBUI=ON` (default OFF) | `demos/daedalus-webui/` | **HTTP + SSE multi-pane web UI**. Open in a browser, one Agent per HTTP session, agent events streamed live. Uses OmniNet (already a daedalus dependency) for HTTP/SSE. Tested end-to-end with Firefox BiDi; see [`docs/webui_testing.md`](docs/webui_testing.md). |
| `daedalus-subagent` | `-DCODEMAX_DEMO_SUBAGENT=ON` (default ON) | `demos/daedalus-subagent/` | One Agent spawns another as a tool — the "sub-agent" pattern. Shows `HostTool` registration. |
| `daedalus-manager` | `-DCODEMAX_DEMO_MANAGER=ON` (default ON) | `demos/daedalus-manager/` | Three Agents orchestrated from a host loop (planning → action → verification) — the "manager" pattern. |

### Build all demos (default)

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
```

`daedalus-cli`, `daedalus-manager`, and `daedalus-subagent` are built by
default. `daedalus-webui` is **off** by default to keep the default
build slim:

```bash
cmake -S . -B build -DCODEMAX_DEMO_WEBUI=ON
cmake --build build -j$(nproc) --target daedalus-webui
```

### Run each demo

```bash
# CLI REPL
./build/demos/daedalus-cli/daedalus-cli                  # current directory
./build/demos/daedalus-cli/daedalus-cli /path/to/project

# Web UI — open http://127.0.0.1:8088/ in a browser
./build/demos/daedalus-webui/daedalus-webui 8088

# Sub-agent demo — spawns a planner + a worker as tools
./build/demos/daedalus-subagent/daedalus-subagent /path/to/project

# Manager demo — three Agents in sequence
./build/demos/daedalus-manager/daedalus-manager /path/to/project
```

The **webui demo** is the easiest way to *see* what daedalus is doing —
typed text streams into the transcript pane, tool calls appear in the
middle pane, and the stats pane shows the live state badge plus the
log. The header has two info-pills: the active **model** (read from
the `POST /sessions` response, which embeds `Agent::active_model()`)
and the **cwd** (read from `GET /info`, which returns the project's
working directory). Skills are loaded from the skills directory listed above.

To exercise the webui from a terminal (uses the Firefox BiDi skill):

```bash
~/.local/share/daedalus/skills/installed/firefox/scripts/start-firefox-debug.sh &
./build/demos/daedalus-webui/daedalus-webui 8088 &
# Then open http://127.0.0.1:8088/ in Firefox, type a message.
```

**§F3 — HTTP API contract.** The webui exposes these endpoints. Note
the body shape: `/sessions/<id>/messages` expects `{"message": "..."}`,
NOT `{"content": "..."}`.

```bash
# Health check
curl -s http://127.0.0.1:8088/healthz
# → {"ok":true}

# Server info
curl -s http://127.0.0.1:8088/info
# → {"cwd":"/workspace/daedalus"}

# Create a new session, get the session_id
SID=$(curl -s -X POST http://127.0.0.1:8088/sessions \
        | python3 -c 'import sys,json; print(json.load(sys.stdin)["session_id"])')

# Check the agent's current state (0=Idle, 7=Done)
curl -s http://127.0.0.1:8088/sessions/$SID/state

# Submit a user message. NOTE: the field name is "message", not "content".
curl -s -X POST http://127.0.0.1:8088/sessions/$SID/messages \
        -H 'Content-Type: application/json' \
        -d '{"message":"what is 2 + 2? reply with just the number."}'
# → {"status":"queued"}
```

SSE events stream from `/sessions/<id>/events`. Open
`http://127.0.0.1:8088/` in a browser to see them live in the UI.

---

## Architecture

daedalus is built as a **C++23 static library** (`libdaedalus.a`) with a
narrow public API. Hosts (CLI, webui, manager, sub-agent) embed it by
linking against the library and constructing a `daedalus::Agent`. The
agent loop lives **inside the library**; hosts drive it via
`run_one_turn(std::stop_token)`.

The shipped demos under `demos/` are reference hosts — proof that you
can build any composition pattern on top of `run_one_turn()`. See the
**Demos** section above for the full list and how to build each one.

- **`daedalus::Agent`** (`include/daedalus/Agent.h`) — single public
  facade. Holds the state machine and the one loop. Hosts construct
  one per session; `cleanup()` is idempotent and called by the
  destructor automatically.
- **`daedalus::AgentSink`** (`include/daedalus/AgentSink.h`) — the
  callback interface for streamed tokens, tool calls, state changes,
  log lines, compaction events, bell, user-input requests, and
  command-approval prompts. (`on_tool_call` / `on_tool_result` /
  `on_compaction` / `on_bell` were declared in Phase 6 and started
  firing in Phase 9.6.)
- **`daedalus::AgentConfig`** (`include/daedalus/types/AgentConfig.h`) —
  all knobs (project root, daedalus home, model, files, behaviour
  flags, system prompt, **skills_dirs / skills**). Constructed by the
  host, passed to the Agent.
- **`daedalus::AgentState`** (`include/daedalus/types/AgentState.h`) —
  the state enum (Idle, Understanding, Planning, Acting, Verifying,
  AwaitingApproval, Compressing, Done, Cancelled, Error). Pure data;
  hosts format for display. (`AwaitingUser` was removed in v0.4.0
  when `ask_user` was deleted.)
- **PIMPL internals** (`src/library/AgentImpl.h/.cpp`) — owns the
  `LLMClient`, `SettingsStore`, and the rest of the subsystems.
  Hosts do not include these.

Underlying infrastructure:

- **ThreadPool** (`src/core/ThreadPool.h`) — Singleton thread pool using `std::jthread` and `std::stop_token`. Replaces ad-hoc `std::async` and `std::thread` usage.
- **CurlMultiSession** (`src/api/CurlMultiSession.h`) — Async HTTP client using libcurl's `curl_multi` interface. Non-blocking I/O, HTTP keep-alive, connection pooling, cancellable.
- **CurlHandle** (`src/api/CurlHandle.h`) — RAII wrapper for `CURL*` easy handles (move-only, documents single-threaded constraint).
- **Metrics** (`src/core/Metrics.h`) — Lightweight counters with `ScopedTimer` for duration tracking.
- **SSE streaming** (`src/api/LLMClient.cpp`) — `MiniMaxSseParser` parses `data: {...}` chunks line-by-line, calls `sink_->on_assistant_delta(text)` per token, and accumulates text for the downstream JSON parse. Replaces the old `std::cout` thinking spinner.
- **Skills System** — A catalog of available skills is injected into the system prompt, and the LLM uses the `load_skill` / `deactivate_skill` tools to manage active context. (Earlier auto-activation based on keyword matching was removed: it produced false positives like matching `"bug"` in `"debugging"`.)

  (All 5 sync curl sites migrated to `CurlMultiSession`. Shared CURL handle,
   MCP read_response race, orphan tool_call infinite loop, and compression
   safe-split logic all fixed.)

## Features


- **Multi-Provider Support**: OpenAI, Anthropic, MiniMax, DeepSeek, LM Studio
- **Agent Skills**: Extensible capabilities via Anthropic's Agent Skills standard
- **40+ Tools**: File operations, Git, shell commands, search, images, PDFs, code analysis
- **Network Tools (OmniNet-backed)**: `http_request` (HTTP/HTTPS), `dns_lookup`, `tcp_check`, `tls_inspect`, plus 4 WebSocket tools (`websocket_connect/send/recv/close`). Replaces Bash `curl` + `python3 -c "import urllib"`. See [docs/network-tools.md](docs/network-tools.md).
- **Sub-Agents**: Spawn parallel daedalus processes for independent tasks
- **Session Memory**: Learns from your project across sessions
- **Background Processes**: Run and communicate with long-running servers
- **Context Compression**: "70/30 Split" algorithm - compresses oldest 70%, keeps newest 30% intact for seamless continuation
- **Manager Mode**: Orchestrate multiple sub-agents for complex projects
- **SSE Streaming**: Token-by-token responses via `on_assistant_delta` sink callbacks — turn off with `cfg.stream = false` for buffered batch hosts. (§Phase 9.8)
- **Live Bash Output Streaming**: Long-running `execute_bash` commands stream
  sanitised stdout/stderr chunks to `AgentSink::on_tool_progress()` as
  `{"stage":"output","output_chunk":"..."}` events. TTY control sequences
  are stripped so hosts can safely print the chunks; the full result is still
  returned to the LLM at the end.
- **Developer-controlled Skill loading**: `cfg.skills_dirs` + `cfg.skills` — empty by default, opt in only to what the host needs. (§Phase 9.7)
- **Sink Observability**: Every state transition fires `on_state_change` so hosts can render real-time status (busy indicator, progress bar, cancel button). (§Phase 9.6)
- **Per-Agent Config Home**: `cfg.daedalus_home` lets multiple Agents in one process own separate settings/skills/memory dirs. (§Phase 8.5)
- **Per-model Temperature**: Each model in the `SettingsStore` can
  declare its own `temperature`, with a session-level override via
  `AgentConfig::temperature`. Resolution priority: session override →
  model → 0.0. (§Phase 8.7)
- **clang++ + clang-tidy**: Builds with clang by default. Conservative clang-tidy ruleset wired into CMake targets (`cmake --build build --target daedalus_clang_tidy`). (§Phase 9.9)

---

## Context Compression

daedalus uses **context compression** to keep conversations manageable during long tasks. The algorithm compresses the oldest portion while preserving the newest 30% for seamless agent continuation.

### Algorithm: "70/30 Split"

```
BEFORE: [system, msg1, msg2, ..., msgN*0.7, msgN*0.7+1, ..., msgN]
              ───────────────────────────────────────────────
                    FIRST 70%                LAST 30%
                       ↓                       ↓
                CONVERT TO PLAIN TEXT      KEEP INTACT
                       ↓                       ↓
                  PASS TO LLM             (stays in messages)
                       ↓
                  GET SUMMARY
                       ↓
AFTER:  [system, assistant(compress_context), tool(summary), msgN*0.7+1, ..., msgN]
            ───────   ─────────────────────────────────   ─────────────────
            SYSTEM    FIRST 70% SUMMARIZED (replaced)     LAST 30% KEPT
```

**Key insight:** By keeping the newest 30% intact, the Agent retains recent context including tool call chains. This prevents the "strange behavior" that occurred when context was lost. The split NEVER breaks tool call pairs - whole chains are included in the compressed region.

| Region | Before | After |
|--------|--------|-------|
| **First 70%** | ⚠️ All messages | ⚠️ Compressed (LLM summary) |
| **Last 30%** | ❌ All lost | ✅ Kept intact |
| **Context Preserved** | Low | High |
| **Tool Call Chains** | ❌ Broken | ✅ Intact (safe split) |

### How It Works

- **Automatic**: Triggers when token usage hits 90% of `max_context_size`
- **Manual**: User types `/compress` command for on-demand compression
- **70/30 Split**: Oldest 70% summarized via LLM, newest 30% kept intact
- **Safe Split**: Split NEVER breaks tool call pairs - if a tool result would be orphaned, the whole chain is included in compression
- **Result Structure**: `system + assistant(compress_context tool call) + tool(summary) + suffix(last 30%)`
- **Seamless Continuation**: Agent continues with recent context preserved
- **Delete First**: First 70% deleted BEFORE compression - context reduced even if LLM fails

---

## Available Tools

The core library ships a small, focused tool set. File-system, search, git,
PDF, network, and background-process tools are available as individually
registered example tools under `demos/example-tools/`.

### Shell
- `execute_bash` - run shell commands with chaining (`&&`, `||`, `;`) and
  process isolation. Output is sanitised before being returned (TTY control
  sequences stripped, UTF-8 and ANSI SGR colour/style preserved). Long-running
  commands stream sanitised output chunks to `AgentSink::on_tool_progress()`
  while they execute. This is the **only** core tool that calls
  `AgentSink::request_command_approval()` (unless `auto_approve` is set).

### User Interaction
- `tell_user` - informational message from the agent to the user
- `compress_context` - trigger context compression
- `set_status` - declare the agent's current state
- `stop` - finish the current turn with a summary

### Memory
- `remember`, `forget`, `memories`, `memory_stats`

### Skills
- `discover_skills`, `load_skill`, `get_active_skills`, `search_skills`,
  `deactivate_skill`, `get_skill_context`

### MCP Tools
- `mcp__<server>__<tool>` - call any tool exposed by a configured MCP server

### Manager Mode

Manager Mode is shipped as a demo composition (`demos/daedalus-manager/`)
that orchestrates multiple `daedalus::Agent` instances from a host
loop (planning → action → verification). The library itself is
mode-free; the demo is the proof that the orchestration pattern is
achievable on top of `run_one_turn()`.

```bash
cmake -S . -B build -DCODEMAX_DEMO_MANAGER=ON
cmake --build build -j$(nproc)
./build/demos/daedalus-manager/daedalus-manager /path/to/project
```

See `demos/daedalus-manager/main.cpp` for the implementation.

### Memory
- `remember`, `forget`, `memories`, `memory_stats`

### Skills
- `discover_skills`, `load_skill`, `get_active_skills`, `search_skills`, `deactivate_skill`, `get_skill_context`

### Code Analysis
- `codegraph` - Analyze C/C++ code structure with include graphs, component dependencies, cycle detection, and navigation

### System
- `system_info`

---

## Commands

| Command | Description |
|---------|-------------|
| `/quit`, `/exit` | Exit the program |
| `/clear` | Clear session state |
| `/models` | Select default model |
| `/skills` | Manage Agent Skills |
| `/compress` | Compress conversation context |
| `/help` | Show help message |

---

## Local Models (LM Studio)

1. Download [LM Studio](https://lmstudio.ai/)
2. Download a model (e.g., Llama 3, Mistral)
3. Click "Start Server" (default port: 1234)
4. Configure API key in LM Studio (any string works)
5. Use `/models` in daedalus to switch to LM Studio

---

## Documentation

- [docs/index.md](docs/index.md) — start here: map of all docs
- [docs/embedding.md](docs/embedding.md) — embedding guide + AgentSink contract
- [docs/crash_resilience.md](docs/crash_resilience.md) — atomic save, auto-flush, SIGSEGV/SIGABRT handler, recovery
- [docs/library_design.md](docs/library_design.md) — permanent invariants for `libdaedalus.a` (no-stdio, output channels)
- [docs/storage.md](docs/storage.md) — XDG-compliant storage layout
- [docs/tools.md](docs/tools.md) — tool specifications
- [docs/skills.md](docs/skills.md) — Agent Skills
- [docs/mcp_server.md](docs/mcp_server.md) — MCP server integration
- [docs/network-tools.md](docs/network-tools.md) — OmniNet-backed network tools
- [docs/how_compress_works.md](docs/how_compress_works.md) — context compression
- [docs/session_memory.md](docs/session_memory.md) — Session Memory
- [docs/agent_state_machine.md](docs/agent_state_machine.md) — state machine
- [docs/agent_system_prompts.md](docs/agent_system_prompts.md) — system prompts
- [docs/webui_testing.md](docs/webui_testing.md) — Firefox BiDi testing recipe

---

## Security

### Philosophy: Trust Linux Permissions

The Agent runs as a normal unprivileged user. The OS permissions handle most security - the app only blocks truly critical items.

### What the OS Handles
| Scenario | OS Behavior |
|----------|-------------|
| Read `/etc/passwd` | ✅ Allowed (world-readable) |
| Write `/etc/shadow` | ❌ Denied (root only) |
| Read `/root/.ssh/id_rsa` | ❌ Denied (root only) |
| Write to `/home/user/file` | ✅ Allowed (owner) |

### What the App Blocks (Defense in Depth)
```
BLOCK ALL OPERATIONS:
- Credential files: *.pem, *.key, *.crt, *.p12, *.pfx, *id_rsa*, *id_ed25519*
- Paths containing: .ssh/id_, .aws/, .gcp/, .azure/, .kube/
- Privilege escalation: sudo, su -, pkexec, doas, dzdo, run0
```

### Access Summary by Operation
| Operation | Project | /tmp | Home | System |
|-----------|---------|------|------|--------|
| Read | ✅ | ✅ | ✅ | ✅ |
| Write | ✅ | ✅ | ✅ (config) | ❌ |
| Delete | ✅ | ✅ | ❌ | ❌ |

### Security Features
- Shell commands require user approval (y/s/n/q)
- Privilege escalation commands (`sudo`, `su`, `pkexec`) are blocked
- Credential files are protected even if OS permissions allow access
- Force push requires explicit approval
- Sensitive data is filtered from Session Memory