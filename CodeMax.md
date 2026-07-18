# Daedalus - Autonomous Coding Agent System

## System Information

| Property | Value |
|----------|-------|
| OS | Ubuntu 26.04 LTS (x86_64) |
| Kernel | Linux 7.0.3-6.stable |
| Container | Podman |
| Display | Wayland |
| User | ubuntu |
| Date | 2026-07-18 07:41:42 UTC |

## Project Overview

**Daedalus** is an autonomous coding agent written in modern C++23. It provides:
- A static library (`libdaedalus.a`) with a clean public API
- Multiple demo applications (CLI, Manager, Subagent, WebUI)
- Comprehensive tool system for file operations, bash execution, Git, networking, and MCP integration
- Session memory and context compression
- Skill management system

**Current Version:** 0.5.4-dirty

## Project Structure

```
daedalus/
├── CMakeLists.txt           # Main build configuration
├── src/
│   ├── api/                 # LLM client and HTTP session handling
│   ├── core/                # Core utilities (Messages, Logger, SessionMemory, etc.)
│   ├── library/             # Public API implementation (Agent, paths)
│   ├── mcp/                 # MCP (Model Context Protocol) client/transport
│   └── tools/               # Tool implementations (File, Bash, Git, Network, etc.)
├── include/daedalus/        # Public headers
├── tests/                   # Unit tests using doctest
├── demos/                   # Demo executables
│   ├── daedalus-cli/        # Console REPL interface
│   ├── daedalus-manager/    # Multi-agent orchestration
│   ├── daedalus-subagent/   # Parent-child agent pattern
│   └── daedalus-webui/      # HTTP+SSE web interface
├── docs/                    # Architecture documentation
└── build/                   # Build output directory
```

## Key Dependencies

| Dependency | Purpose |
|------------|---------|
| OmniNet | HTTP/WebSocket networking (fetched from GitLab) |
| nlohmann_json | JSON parsing |
| OpenSSL | TLS/cryptography |
| libpng | PNG image processing |
| poppler-cpp | PDF processing |
| doctest | Unit testing framework |

## Build System

### CMake Configuration

- **Minimum CMake Version:** 3.28
- **C++ Standard:** C++23
- **Default Compiler:** clang/clang++ (auto-detected, GCC overrideable via `CC=gcc CXX=g++`)
- **Build Type:** Release (default)

### Build Commands

```bash
# Full build with all demos
cmake -S . -B build
cmake --build build -j$(nproc)

# Library only (no demos)
cmake -S . -B build -DDAEDALUS_BUILD_DEMOS=OFF

# With webui demo
cmake -S . -B build -DDAEDALUS_DEMO_WEBUI=ON

# Force GCC instead of clang
CC=gcc CXX=g++ cmake -S . -B build
```

### Build Targets

| Target | Description |
|--------|-------------|
| `daedalus` | Static library (libdaedalus.a) |
| `daedalus-cli` | Console REPL demo |
| `daedalus-manager` | Manager mode demo |
| `daedalus-subagent` | Subagent demo |
| `daedalus-webui` | WebUI demo (optional) |
| `daedalus_clang_tidy` | Full static analysis |
| `daedalus_clang_tidy_lib` | Static analysis for src/ |
| `daedalus_clang_tidy_tests` | Static analysis for tests/ |

### Test Commands

```bash
# Run all tests
ctest --test-dir build

# Run specific test
./build/tests/test_messages

# Run with verbose output
ctest --test-dir build -V
```

## Public API

The main public API is exposed through `<daedalus.h>`:

```cpp
#include <daedalus.h>

// Key types (in include/daedalus/)
#include <daedalus/Agent.h>        // Main agent class
#include <daedalus/AgentSink.h>   // Event callbacks
#include <daedalus/settings.h>    // Configuration
#include <daedalus/types/AgentConfig.h>
#include <daedalus/types/AgentState.h>
#include <daedalus/types/HostTool.h>
```

### Agent Usage Pattern

```cpp
#include <daedalus.h>

daedalus::AgentConfig config;
config.project_root = "/path/to/project";
config.daedalus_home = daedalus::Paths::global().user_root();
config.session_id = "my-session";

auto sink = std::make_shared<MySink>();
daedalus::Agent agent(std::move(config), sink);
agent.initialize();

agent.submit_user_message("Hello, write me a hello world in Python");
agent.run_one_turn();
```

### AgentSink Callbacks (Host Interface)

**See [`docs/embedding.md`](docs/embedding.md) → [Callback Data Reference](docs/embedding.md#callback-data-reference) for what data each callback receives.**

The host implements `daedalus::AgentSink` to receive all events. **Only `on_log` is required** (has a stderr default). All other methods have empty defaults.

```cpp
#include <daedalus/AgentSink.h>

struct daedalus::AgentSink {
    virtual ~AgentSink() = default;

    // Streaming tokens (one per SSE chunk, typically 1-20 chars)
    virtual void on_assistant_delta(std::string_view text) noexcept;

    // Tool lifecycle (structured events - use these instead of parsing tokens)
    virtual void on_tool_call(std::string_view name, const nlohmann::json& args) noexcept;
    virtual void on_tool_result(std::string_view name, const nlohmann::json& result) noexcept;

    // State machine transitions
    virtual void on_state_change(int from, int to) noexcept;

    // Logging (route to log window/file)
    virtual void on_log(daedalus::LogLevel level, std::string_view message) noexcept;

    // Bash command approval (blocking - return true to allow)
    virtual bool request_command_approval(std::string_view command);

    // Optional notifications
    virtual void on_bell() noexcept;
    virtual void on_compaction(std::size_t before, std::size_t after) noexcept;
};
```

**Typical event sequence:**
```
submit_user_message("fix the bug")     [host]
run_one_turn()                         [host]
on_state_change(Idle, Understanding)   [sink]
on_log(Info, "[PERSIST] ...")
on_assistant_delta(...)                [sink, many times]
on_tool_call("grep_search", {...})    [sink]
on_tool_result("grep_search", {...})   [sink]
request_command_approval("cmake ...")  [sink - blocking]
on_tool_result("execute_bash", {...})  [sink]
on_state_change(Acting, Done)         [sink]
run_one_turn() returns                 [host]
```

**Threading:** Sink methods are called on the agent's thread. Marshal to your UI thread as needed (e.g., `g_idle_add()` for GTK4).

> **See [`docs/embedding.md`](docs/embedding.md) for the complete embedding guide with threading examples.**

## Architecture Highlights

### Tool System

- **BaseTool** - Abstract base for all tools
- **ToolFactory** - Creates tools from definitions
- **ToolHandler** - Routes tool calls to implementations
- Tools live in `src/tools/`

### MCP Integration

- **McpClient** - Manages MCP server connections
- **McpServerManager** - Handles server lifecycle
- **HttpMcpTransport** / **StdioMcpTransport** - Transport backends

### Message Handling

- **Messages** - Unified message storage, persistence, and compression
- Context compression triggered at 90% of token limit (200K)
- Middle messages compressed via LLM

### Session Memory

- Persistent memory with per-project and global tiers
- Compression support for long sessions
- XDG Base Directory Specification compliant

## Code Quality

### Static Analysis

```bash
# Run clang-tidy on all targets
cmake --build build --target daedalus_clang_tidy

# Individual targets
cmake --build build --target daedalus_clang_tidy_lib
cmake --build build --target daedalus_clang_tidy_tests
cmake --build build --target daedalus_clang_tidy_tools
cmake --build build --target daedalus_clang_tidy_demos
```

### Code Formatting

- clang-format configuration should be applied
- Header guards and `#pragma once` mixed usage (note: some files use both)

## Development Workflow

1. **Edit code** in `src/`, `include/`, `tests/`, or `demos/`
2. **Build:** `cmake --build build -j$(nproc)`
3. **Test:** `ctest --test-dir build`
4. **Analyze:** `cmake --build build --target daedalus_clang_tidy`
5. **Install:** `cmake --install build`

## Installation

```bash
cmake --install build
# Installs to ~/.local/ by default
# - libdaedalus.a → ~/.local/lib/
# - headers → ~/.local/include/
# - settings template → ~/.daedalus/settings/
```

## Important Files

| File | Purpose |
|------|---------|
| `docs/embedding.md` | **Host developer guide** - Complete AgentSink contract, config, lifecycle |
| `src/core/Messages.h` | Message handling and compression |
| `src/core/SessionMemory.h` | Session memory management |
| `src/core/SkillManager.h` | Skill discovery and loading |
| `src/tools/FileTool.h` | File operations |
| `src/tools/BashTool.h` | Bash command execution |
| `src/api/LLMClient.h` | LLM API communication |
| `src/mcp/McpClient.h` | MCP protocol client |
| `include/daedalus/Agent.h` | Main public API |
| `include/daedalus/AgentSink.h` | Sink callback interface |

## Documentation

Detailed docs in `docs/`:
| Document | Audience | Contents |
|----------|----------|----------|
| [`embedding.md`](docs/embedding.md) | **Host developers** | Complete embedding guide: minimum viable host, **AgentSink contract** (all methods + threading), config, lifecycle, cancellation, composition patterns |
| [`agent_state_machine.md`](docs/agent_state_machine.md) | All | State machine documentation |
| [`agent_system_prompts.md`](docs/agent_system_prompts.md) | Developers | System prompt composition |
| [`mcp_server.md`](docs/mcp_server.md) | Developers | MCP server integration |
| [`skills.md`](docs/skills.md) | Developers | Skill system documentation |
| [`session_memory.md`](docs/session_memory.md) | Developers | Memory architecture |
| [`tools.md`](docs/tools.md) | Developers | Tool definitions and behavior |
| [`storage.md`](docs/storage.md) | Developers | Data storage and paths |
| [`testing.md`](docs/testing.md) | Developers | Test strategy |
| [`token_counts.md`](docs/token_counts.md) | Developers | Token management |
| [`xdg_compliance.md`](docs/xdg_compliance.md) | Developers | XDG compliance |
