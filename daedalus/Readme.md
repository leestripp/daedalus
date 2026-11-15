![daedalus Banner](resources/banner.png)

# daedalus

**daedalus** is an autonomous-agent library for C++23. It builds the static
library `libdaedalus.a` and includes five worked hosts under `demos/`. A host
constructs one or more `daedalus::Agent` objects, supplies model settings, and
receives all output through `daedalus::AgentSink`.

The current release line is **1.4.x**. `Agent::library_version()` reports the
version compiled into a binary; [`CHANGELOG.md`](CHANGELOG.md) records release
changes. Start with [`docs/index.md`](docs/index.md) and
[`docs/embedding.md`](docs/embedding.md).

## What the library provides

- A synchronous, host-driven agent loop with queued user messages,
  cancellation, context compaction, persistent transcripts, and crash recovery.
- OpenAI-compatible providers plus provider-specific plugins for MiniMax,
  Moonshot/Kimi, and LM Studio, including streaming and thinking controls.
- A focused built-in tool set: bash, user/status controls, skills, session
  memory, todos, scheduled tasks, an encrypted vault, a global Markdown
  knowledge base, web search, image/PDF input, and tool introspection.
- Dynamic MCP tools over stdio and streamable HTTP, with runtime server
  enable/disable controls and diagnostics.
- A2A HTTP+JSON client/server types for agent-to-agent integrations.
- XDG user settings in SQLite and per-project/per-agent runtime state.
- Optional example tools for Git, grep/Wikipedia, system information,
  background processes, HTTP/DNS/TCP/TLS, and WebSockets.

The library does not own a terminal, install signal handlers, or print routine
output to stdout/stderr. Those belong to the host. `AgentSink::on_log()` has a
stderr fallback; hosts should override it when they need another destination.

## Build

### Requirements

- CMake 3.28+, Git, and a C++23 compiler (Clang 17+ or GCC 13+).
- OpenSSL, SQLite3, libpng, libjpeg, poppler-cpp, libuuid, and zlib development
  packages.
- Network access on the first configure when dependencies must be fetched.

On Ubuntu:

```bash
sudo apt update
sudo apt install -y build-essential cmake git clang pkg-config \
  nlohmann-json3-dev libssl-dev libsqlite3-dev libpng-dev \
  libjpeg-turbo8-dev libpoppler-cpp-dev uuid-dev zlib1g-dev
```

CMake fetches OmniNet and, when no suitable system package is found,
nlohmann_json. The test build fetches doctest. `OMNINET_LOCAL` can point at a
local OmniNet checkout.

### Monorepo build (recommended)

From the repository root:

```bash
cmake -S . -B build
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

This builds daedalus, Icarus, MonkeyByte, their tests, and the enabled demos in
one build tree.

### Standalone library build

From `daedalus/`:

```bash
cmake -S . -B build
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Demos and tests default to ON for a standalone build and OFF when daedalus is
embedded with `add_subdirectory` or `FetchContent`. Useful options are:

| Option | Default (standalone) | Purpose |
|---|---:|---|
| `DAEDALUS_BUILD_DEMOS` | ON | Build demo hosts |
| `DAEDALUS_BUILD_TESTS` | ON | Build the doctest suite |
| `DAEDALUS_DEMO_SUBAGENT` | ON | Build the sub-agent example |
| `DAEDALUS_DEMO_MANAGER` | ON | Build the manager example |
| `DAEDALUS_DEMO_SETTINGS_TUI` | ON | Build the FTXUI settings editor |
| `DAEDALUS_DEMO_WEBUI` | OFF | Build the HTTP/SSE web example |

Install to the default user prefix (or choose another prefix explicitly):

```bash
cmake --install build
# or: cmake --install build --prefix /desired/prefix
```

The install exports `daedalus::daedalus`. Because this is a static library and
OmniNet is build-interface-only in the export, downstream consumers must also
provide the dependencies required by the features they use.

## Minimal host

```cpp
#include <daedalus.h>
#include <daedalus/paths.h>

#include <iostream>
#include <memory>
#include <string>

class StdoutSink final : public daedalus::AgentSink {
public:
    void on_assistant_delta(std::string_view text) noexcept override {
        std::cout << text << std::flush;
    }

    void on_tool_call(std::string_view name,
                      std::string_view /*call_id*/,
                      const nlohmann::json&) noexcept override {
        std::cerr << "\n[tool] " << name << " ...\n";
    }

    void on_tool_result(std::string_view name,
                        std::string_view /*call_id*/,
                        const nlohmann::json& result) noexcept override {
        std::cerr << "[tool] " << name << " -> " << result.dump() << '\n';
    }

    void on_state_change(daedalus::AgentState,
                         daedalus::AgentState to) noexcept override {
        std::cerr << "[state] " << daedalus::to_string(to) << '\n';
    }

    void on_log(daedalus::LogLevel level,
                std::string_view message) noexcept override {
        std::cerr << '[' << daedalus::to_string(level) << "] " << message << '\n';
    }

    bool request_command_approval(std::string_view command) override {
        std::cerr << "Approve: " << command << " [y/N] " << std::flush;
        std::string answer;
        std::getline(std::cin, answer);
        return answer == "y" || answer == "Y";
    }
};

int main() {
    daedalus::AgentConfig cfg;
    cfg.project_root = ".";
    // This is the data root containing daedalus.db, not the XDG config root.
    cfg.daedalus_home = daedalus::Paths::global().user_data_root();
    cfg.session_id = "example-session";
    // Skill discovery is opt-in for embedded hosts.
    cfg.skills_dirs = {daedalus::Paths::global().global_skills_dir()};

    daedalus::Agent agent(std::move(cfg), std::make_shared<StdoutSink>());
    agent.initialize();

    std::string input;
    while (std::getline(std::cin, input) && input != "/quit") {
        agent.submit_user_message(std::move(input));
        agent.run_one_turn();
        std::cout << '\n';
    }
    // shutdown() is idempotent and is also called by the destructor.
}
```

`submit_user_message()` queues input; `run_one_turn()` drains the pending queue
and drives the agent until it stops, fails, or is cancelled. An idle host that
must react to background-process completions should implement
`AgentSink::on_turn_requested()` and schedule `run_one_turn()` on its normal
driving thread. The message is already queued—do not submit it again.

See [`docs/embedding.md`](docs/embedding.md) for the complete configuration,
callback, threading, and cancellation contracts.

## Model and provider settings

Settings live in `$XDG_DATA_HOME/daedalus/daedalus.db` (normally
`~/.local/share/daedalus/daedalus.db`). A model references a provider; hosts,
credentials, endpoints, timeout, and plugin selection belong to the provider.
Use `SettingsStore`, the CLI, the settings TUI, or the WebUI settings routes to
configure them.

```cpp
#include <daedalus/settings.h>

auto& store = daedalus::SettingsStore::instance();
store.upsert_provider(daedalus::ProviderConfig{
    .name = "minimax",
    .host = "https://api.minimax.io",
    .api_key = "YOUR_API_KEY",
    .chat_endpoint = "/v1/chat/completions",
    .discovery_endpoint = "/v1/models",
    .timeout_seconds = 30,
    .plugin = "minimax",
});
store.upsert_model(daedalus::ModelConfig{
    .name = "minimax-m3",
    .provider = "minimax",
    .model_id = "MiniMax-M3",
});
store.set_default_model_name("minimax-m3");
```

`SettingsStore::for_home(path)` selects an isolated database at
`path/daedalus.db`. JSON settings are not automatically imported. See
[`docs/settings.md`](docs/settings.md) and
[`docs/storage.md`](docs/storage.md).

For a local LM Studio server, add an auth-less provider on
`http://localhost:1234`, select the `lmstudio` plugin (or let loopback
auto-detection select it), discover models, and choose one. No API key is
required.

## Tools

The built-in tool surface is intentionally focused:

| Area | Built-in tools |
|---|---|
| Execution | `execute_bash` |
| Turn control | `tell_user`, `compress_context`, `set_status`, `stop` |
| Introspection | `list_tools`, `mcp_status` when MCP is enabled |
| Skills | `discover_skills`, `load_skill`, `get_active_skills`, `search_skills`, `deactivate_skill`, `get_skill_context` |
| Session memory | `remember`, `forget`, `memories`, `memory_stats` |
| Knowledge/secrets | `brain`, `vault` |
| Todos/scheduling | `todo_add`, `todo_edit`, `todo_delete`, `todo_list`, `todo_count`, `set_max_todos`, `todo_clear`, `schedule_task` |
| Search/input | `web_search`, `read_image`, `read_pdf` |
| MCP | `mcp_control`, plus dynamic `mcp__<server>__<tool>` proxies |

`web_search` uses enabled rows in `web_search_providers`, in priority order,
with failover. Providers are Brave, Tavily, Exa, Firecrawl, Serper, SerpApi,
SearXNG, and key-less DuckDuckGo. `provider` can force one provider. The legacy
`serper_search` and `searxng_search` tool names no longer exist.

Git, grep/Wikipedia, system-info, background-process, HTTP, DNS, TCP, TLS, and
WebSocket tools are optional reference implementations in
`demos/example-tools/`; they are not all registered by the core library. Hosts
can also implement `daedalus::BaseTool` and call `Agent::register_tool()`.
Registration replaces an existing tool with the same name. `daedalus::HostTool`
remains a deprecated alias for `BaseTool`.

`execute_bash` asks for approval unless auto-approval or an applicable approval
cache allows the command. It is not an OS sandbox: it runs as the invoking
user, with path/privilege/credential guardrails described in the tool source
and [`docs/tools.md`](docs/tools.md). Enable auto-approval only in an
environment whose access you accept.

## MCP

MCP is enabled by default in `AgentConfig` and can be disabled with
`cfg.mcp_enabled = false`. The authoritative server registry is the
`mcp_servers` table in `daedalus.db`. A legacy `servers.json` file is merged as
a fallback, with SQLite winning name conflicts.

```cpp
store.upsert_mcp_server(daedalus::SettingsStore::McpServer{
    .name = "example",
    .transport = "stdio",
    .command = "uvx",
    .args = {"example-mcp-server"},
    .env = {{"EXAMPLE_API_KEY", "${EXAMPLE_API_KEY}"}},
    .description = "Example MCP server",
    .enabled = true,
});
```

Every discovered server tool is exposed directly as
`mcp__<server>__<tool>`; there is no generic `mcp` routing tool. Use
`mcp_status` to diagnose configuration and connections, and `mcp_control` or
`Agent::set_mcp_server_enabled()` for session-local enable/disable changes.
See [`docs/mcp_server.md`](docs/mcp_server.md).

## Demos

| Demo | Option | What it demonstrates |
|---|---|---|
| `daedalus-cli` | Always built when demos are enabled | Console REPL, A2A client commands, A2A server, and example-tool registration |
| `daedalus-settings-tui` | `DAEDALUS_DEMO_SETTINGS_TUI` | FTXUI editor for providers, models, MCP, web search, and agent settings |
| `daedalus-subagent` | `DAEDALUS_DEMO_SUBAGENT` | One agent exposed to another as a custom tool |
| `daedalus-manager` | `DAEDALUS_DEMO_MANAGER` | Host-side planning/action/verification orchestration |
| `daedalus-webui` | `DAEDALUS_DEMO_WEBUI` (OFF) | Multiple agents behind HTTP/SSE with browser settings UI |

Standalone build-tree paths are:

```bash
./build/demos/daedalus-cli/daedalus-cli [options] [project-directory]
./build/demos/daedalus-settings-tui/daedalus-settings-tui
./build/demos/daedalus-subagent/daedalus-subagent /path/to/project
./build/demos/daedalus-manager/daedalus-manager /path/to/project

cmake -S . -B build -DDAEDALUS_DEMO_WEBUI=ON
cmake --build build --target daedalus-webui
./build/demos/daedalus-webui/daedalus-webui 8080 /path/to/project
```

In the monorepo build tree, demo paths are under `build/daedalus/demos/`.
Manager and sub-agent behavior is composition in the host, not a mode flag in
the library.

The CLI demo can serve A2A:

```bash
./build/demos/daedalus-cli/daedalus-cli --a2a-server --a2a-port 8080
```

## Storage

[`docs/storage.md`](docs/storage.md) is the authoritative storage reference.
The common locations are:

| Location | Contents |
|---|---|
| `$XDG_DATA_HOME/daedalus/daedalus.db` | Providers, models, MCP servers, web-search providers, key/value settings, vault entries |
| `$XDG_CONFIG_HOME/daedalus/vault.key` | Vault master key (private permissions) |
| `$XDG_DATA_HOME/daedalus/brain/` | Global Markdown knowledge base |
| `$XDG_DATA_HOME/daedalus/skills/` | User-level skills |
| `$XDG_CONFIG_HOME/daedalus/mcp/` | Legacy `servers.json` and `.env` fallback |
| `<project>/.daedalus/daedalus[-<agent>]/` | Transcript, session memory, todos, schedules, logs, approvals, and other per-agent state |

Do not move project runtime files into the XDG user tier, and do not store user
credentials in a project transcript. Session memory and the Second Brain are
separate stores.

## Context and persistence

Transcripts are saved atomically and mirrored for crash recovery. Automatic
compaction triggers near the configured context limit; hosts can call
`Agent::compress_context()`, and the model can call `compress_context`. The
compactor summarizes the oldest safe prefix while retaining the newest suffix
without splitting tool-call/result chains. Requests deferred during a turn run
at the cycle boundary.

Session memory (`remember` and related tools) is distinct from the full
transcript. See [`docs/session_memory.md`](docs/session_memory.md),
[`docs/how_compress_works.md`](docs/how_compress_works.md), and
[`docs/crash_resilience.md`](docs/crash_resilience.md).

## Tests and static analysis

```bash
ctest --test-dir build --output-on-failure
cmake --build build --target daedalus_static_analysis
```

The tests use doctest. Live-LLM tests return CTest skip code 77 unless
`DAEDALUS_RUN_LIVE_LLM_TESTS=1` is set; ordinary test runs require no provider
credentials. The integration-test guide describes separate live-provider
checks. Do not claim those checks ran when they were skipped.

## Documentation map

- [`docs/index.md`](docs/index.md) — complete documentation index
- [`docs/embedding.md`](docs/embedding.md) — public API and sink guide
- [`docs/settings.md`](docs/settings.md) — SettingsStore reference
- [`docs/storage.md`](docs/storage.md) — storage layout and migration
- [`docs/tools.md`](docs/tools.md) — built-in and example tools
- [`docs/custom-tools.md`](docs/custom-tools.md) — host tool registration
- [`docs/provider-plugins.md`](docs/provider-plugins.md) — provider plugins
- [`docs/mcp_server.md`](docs/mcp_server.md) — MCP integration
- [`docs/a2a.md`](docs/a2a.md) — A2A client/server
- [`docs/webui.md`](docs/webui.md) — WebUI demo
- [`docs/testing.md`](docs/testing.md) — test strategy
