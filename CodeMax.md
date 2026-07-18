# Daedalus - CodeMax Project Analysis

## System Context

| Property | Value |
|----------|-------|
| Kernel | Linux 7.0.3-6.stable x86_64 |
| OS | Ubuntu 26.04 LTS |
| Container | Podman |
| Session | xterm, local, uid=1000 (ubuntu) |
| Display | Wayland |

---

## Project Overview

**Daedalus** is an autonomous agent library for C++23. It ships as `libdaedalus.a` plus four worked example hosts (`demos/`). Embed it in any C++23 host (CLI, web, IDE plugin, CI bot) and drive one or more `Agent` instances via the public API.

- **Language**: C++23
- **Build System**: CMake 3.28+
- **Version**: 0.5.3 (current development version)
- **Key Dependencies**: libcurl, OpenSSL, SQLite3, libpng, libpoppler-cpp, nlohmann/json, OmniNet

---

## Data Storage Status (v0.6.0)

The storage system has been fully migrated from JSON-based configuration to SQLite-backed SettingsStore. Key changes:

### Completed Migration Work (v0.5.0)

1. **SettingsStore is the sole source of truth** for:
   - LLM models (`models` table)
   - Key-value settings (`kv_settings` table)
   - MCP server registry (`mcp_servers` table)
   - Serper API key (`serper_config` table)

2. **Files migrated**:
   - `config.json` → `daedalus.db` (models, kv_settings)
   - `mcp/servers.json` → `daedalus.db` (mcp_servers table)
   - `mcp/.env` → Still in XDG config path (env vars for MCP servers)
   - Serper API key → `daedalus.db` (serper_config table)

3. **Legacy `~/.daedalus/` fallback removed** (v0.6.0):
   - XDG-compliant paths are now the only supported locations
   - Removed `~/.daedalus/` probe from `Paths::user_root()`
   - Updated all demos to use XDG paths directly
   - Removed `test_legacy_deprecation_warning.cpp`

### Bug Fixes During Migration

- **Fixed SettingsStore constructor bug**: The constructor was using uninitialized `path_` instead of the passed `db_path` parameter. Fixed in `src/core/SettingsStore.cpp`.
- **Updated tests**: `test_phase15_disabled_switches.cpp` and `test_serper_startup.cpp` updated to use the new SettingsStore API.

### Key Files Changed

| File | Change |
|------|--------|
| `src/core/SettingsStore.cpp` | Fixed constructor to properly initialize `path_` |
| `tests/test_phase15_disabled_switches.cpp` | Rewrote to use SettingsStore API |
| `tests/test_serper_startup.cpp` | Rewrote to test SettingsStore-based storage |
| `src/core/SerperConfig.h` | Removed unused `load_serper_api_key_from_dirs` declaration |
| `src/library/paths.cpp` | Removed legacy `~/.daedalus/` fallback from `user_root()` |
| `src/library/AgentImpl.cpp` | Removed `load_config()` fallback, now uses SQLite only |
| `tests/test_legacy_deprecation_warning.cpp` | Deleted (no longer needed) |
| `docs/storage.md` | Updated to reflect XDG-only storage layout |

---

## Project Structure

```
daedalus/
├── cmake/              # CMake config files
├── demos/              # Example host applications
│   ├── daedalus-cli/   # Console REPL demo (canonical embedding example)
│   ├── daedalus-manager/ # 3-Agent orchestration demo
│   ├── daedalus-subagent/ # Parent-child agent demo
│   └── daedalus-webui/  # HTTP + SSE web interface demo
├── docs/               # Living reference documentation
├── include/daedalus/   # Public API headers
├── resources/          # Banner, icons
├── src/
│   ├── api/            # LLMClient, HttpSession, SSE parser
│   ├── core/            # Core utilities (FileIO, Logger, Settings, etc.)
│   ├── library/         # AgentImpl (PIMPL for public Agent facade)
│   ├── mcp/             # MCP client/server manager, transports
│   └── tools/           # Tool implementations (bash, file, git, etc.)
├── tests/              # Unit tests (Catch2-based)
└── CMakeLists.txt      # Root build configuration
```

---

## Key Components

### Public API (`include/daedalus/`)

| Header | Purpose |
|--------|---------|
| `Agent.h` | Top-level facade - main class hosts instantiate |
| `AgentSink.h` | Callback interface for host to receive events |
| `types/AgentConfig.h` | Configuration structure |
| `types/AgentState.h` | State enum (Idle, Thinking, ToolUse, etc.) |
| `settings.h` | SettingsStore API for SQLite-backed config |

### Core Implementation (`src/library/`)

- **AgentImpl.h/cpp**: PIMPL backing the public Agent facade
- Owns LLMClient instance (state machine + loop)
- Translates AgentConfig ↔ AppConfig
- Routes AgentSink events

### LLM Client (`src/api/`)

- **LLMClient.h/cpp**: Main state machine, HTTP session management, SSE parsing
- **HttpSession.h/cpp**: HTTP request handling with cancellation support
- **MiniMaxSseParser.h**: SSE event parsing

### Tool System (`src/tools/`)

| Tool | File | Description |
|------|------|-------------|
| Bash | `BashTool.cpp` | Command execution with path checking |
| File | `FileTool.cpp` | File read/write/edit/delete operations |
| Git | `GitTool.cpp` | Git operations (status, diff, branch, etc.) |
| Search | `SearchTool.cpp` | Web search via Serper API |
| Network | `NetworkTool.cpp` | HTTP/DNS/TCP/TLS/WebSocket tools |
| MCP | `McpProxiedTool.cpp` | Proxied MCP server tools |
| Memory | `MemoryTool.cpp` | Session memory management |
| Skills | `SkillTool.cpp` | Skill loading/execution |
| System | `SystemInfoTool.cpp` | System information gathering |
| PDF | `PDFTool.cpp` | PDF text extraction |
| Background | `BackgroundTool.cpp` | Background process management |

### MCP Integration (`src/mcp/`)

- **McpClient.h/cpp**: MCP protocol client
- **McpServerManager.h/cpp**: Manages multiple MCP server connections
- **StdioMcpTransport.cpp**: Stdio transport for MCP
- **HttpMcpTransport.cpp**: HTTP/SSE transport for MCP

### Core Utilities (`src/core/`)

| Module | Purpose |
|--------|---------|
| `FileIO.cpp` | Safe file operations with atomic saves |
| `Logger.cpp` | Structured logging |
| `SettingsStore.cpp` | SQLite-backed settings |
| `SessionMemory.cpp` | Session context management |
| `SkillManager.cpp` | Skill discovery and loading |
| `Messages.cpp` | Message chain management, compression |
| `ThreadPool.cpp` | Thread pool for async operations |
| `Metrics.cpp` | Token usage and metrics tracking |
| `SerperConfig.cpp` | Serper API key validation (SettingsStore-backed) |

---

## Build System

### Configuration

```bash
cmake -S . -B build -DDAEDALUS_DEMO_WEBUI=ON
cmake --build build -j$(nproc)
```

### Key Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `DAEDALUS_BUILD_DEMOS` | ON | Build demo executables |
| `DAEDALUS_DEMO_SUBAGENT` | ON | Build subagent demo |
| `DAEDALUS_DEMO_MANAGER` | ON | Build manager demo |
| `DAEDALUS_DEMO_WEBUI` | OFF | Build webui demo |

### Compiler Preference

- Prefers clang/clang++ when available (better diagnostics)
- GCC can be forced: `CC=gcc CXX=g++ cmake ...`

---

## File Locations (XDG-Compliant)

| Path | Contents |
|------|----------|
| `~/.local/share/daedalus/daedalus.db` | Settings store (models, MCP servers, Serper key) |
| `~/.local/share/daedalus/skills/installed/` | Agent skills |
| `~/.local/share/daedalus/memory/` | Session memory |
| `<project>/.daedalus[-<agent_name>]/messages.json` | Conversation transcript |
| `<project>/.daedalus[-<agent_name>]/messages.json.crash` | Crash recovery mirror |
| `~/.config/daedalus/mcp/.env` | MCP server environment variables |
| `/etc/xdg/daedalus/` | System-wide defaults |

---

## Testing

- **Framework**: Catch2
- **Location**: `tests/` directory
- **Build**: Part of standard build (tests/CMakeLists.txt included automatically)
- **Run**: `./build/tests/<test_name>` or `ctest --test-dir build`
- **Web UI tests**: Playwright-based (`tests/webui/`)

### Key Test Files

| Test | Purpose |
|------|---------|
| `test_settings_store.cpp` | SettingsStore API tests |
| `test_mcp_load_from_store.cpp` | MCP server loading from SettingsStore |
| `test_phase15_disabled_switches.cpp` | MCP/Skill disable filtering |
| `test_serper_startup.cpp` | Serper API key storage tests |
| `test_migrate_data.cpp` | CLI migration tool tests |

---

## Key Design Patterns

### 1. PIMPL Pattern
- Public `Agent` facade hides implementation in `detail::AgentImpl`
- Clean ABI boundary for library users

### 2. Sink Pattern
- `AgentSink` callback interface for all host communication
- Hosts only override what they need (defaults are no-ops)

### 3. Tool Factory
- Central `ToolFactory` creates/manages all tools
- Supports MCP-proxied tools dynamically

### 4. Crash Resilience
- Atomic file saves with crash mirrors
- Async-signal-safe crash handler
- Drop-last-message recovery

### 5. SQLite-Backed Settings
- `SettingsStore` provides type-safe access to all settings
- `DbPool` manages one connection per unique path
- Schema migrations handled automatically

---

## Important Guidelines

### Threading
- `Agent` instance is NOT thread-safe to share
- Construct one `Agent` per session
- `AgentSink` callbacks called on agent's worker thread
- Marshal calls to UI thread for GTK/Qt

### Async Signal Safety
- Library must not use stdio in async-signal handlers
- All output goes through `AgentSink::on_log`
- File operations use `FileIO` (not raw `std::fstream`)

### Compression
- 70/30 context compression algorithm
- Tool-call-pair integrity maintained
- Safe split-point rules in `docs/how_compress_works.md`

---

## Key Documentation Files

| File | Description |
|------|-------------|
| `docs/embedding.md` | Complete embedding guide |
| `docs/agent_system_prompts.md` | System prompt composition |
| `docs/how_compress_works.md` | Context compression algorithm |
| `docs/crash_resilience.md` | Crash recovery mechanism |
| `docs/tools.md` | All tools reference |
| `docs/mcp_server.md` | MCP integration guide |
| `docs/settings.md` | SQLite settings API |
| `docs/storage.md` | XDG path layout |

---

## Version History

- **v0.6.0**: Removed legacy `~/.daedalus/` fallback, XDG-only storage
- **v0.5.3**: Development version
- **v0.5.0**: MCP server registry in SettingsStore, Serper key in SettingsStore
- **v0.4.0**: SQLite-backed settings model
- See `CHANGELOG.md` for full history
