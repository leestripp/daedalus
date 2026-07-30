# daedalus — Agent Guide

This file is a living reference for AI coding agents working on the daedalus
project. It summarises the project's architecture, build system, conventions,
tests, and security model. Treat it as the first stop before making changes.

## Project overview

daedalus is a **C++23 autonomous agent library** that ships as a static library
(`libdaedalus.a`) plus four worked example hosts under `demos/`. Hosts embed the
library and drive one or more `daedalus::Agent` instances via a narrow public
API. The library owns the agent loop; hosts receive all events through an
`AgentSink` callback interface.

- **Repository root:** `/workspace/daedalus`
- **Primary artifact:** `libdaedalus.a`
- **Public API:** `include/daedalus.h` and headers under `include/daedalus/`
- **Current version:** derived from git tags via `git describe --tags --always --dirty`
- **License:** see `LICENSE`

Key design invariant: **the library never touches stdio**. All output flows
through `AgentSink` or the internal file logger. See `docs/library_design.md`
for the full rationale and verification steps.

## Technology stack

| Layer | Technology |
|-------|------------|
| Language | C++23 (std::expected, std::flat_map, std::span, std::jthread, std::stop_token, std::print, deducing this) |
| Build system | CMake 3.28+ |
| Default compiler | Clang 17+ (GCC 13+ also supported) |
| JSON | nlohmann/json 3.11+ (system package OR fetched via CMake) |
| HTTP/WebSocket | OmniNet (fetched from `gitlab.com/leestripp/omninet`) |
| TLS | OpenSSL |
| Image processing | libpng |
| PDF text extraction | poppler-cpp |
| Settings store | SQLite (`libsqlite3-dev`) via `SettingsStore` |
| Unit tests | doctest 2.5+ (fetched by CMake) |
| Static analysis | clang-tidy (optional) |

System dependencies on Ubuntu:

```bash
sudo apt install -y cmake clang libcurl4-openssl-dev libssl-dev \
    libsqlite3-dev libpng-dev libpoppler-cpp-dev
# Optional but recommended: nlohmann_json system package. CMake will
# fetch it from GitHub (v3.11.3) if absent, but installing it locally
# avoids the first-build network round-trip.
sudo apt install -y nlohmann-json3-dev
```

`libcurl4-openssl-dev` and `libsqlite3-dev` are pulled in as transitive OmniNet
requirements. OmniNet, nlohmann_json (optional), sqlite-vec, doctest, and FTXUI
(for the `daedalus-settings-tui` demo) are downloaded automatically by CMake
into `build/_deps/` on first build.

## Project structure

```
.
├── CMakeLists.txt              # Root build configuration
├── cmake/
│   └── DaedalusConfig.cmake.in # find_package(daedalus) template
├── include/daedalus/           # Public API headers
│   ├── daedalus.h              # Convenience umbrella header
│   ├── Agent.h                 # Main facade
│   ├── AgentSink.h             # Host callback interface
│   ├── settings.h              # SettingsStore API
│   ├── paths.h                 # XDG path abstraction
│   ├── types/                  # AgentConfig, AgentState, ModelInfo, ...
│   └── a2a/                    # A2A (Agent2Agent Protocol) client + server
├── src/
│   ├── library/                # Public API implementation (Agent, AgentImpl, paths)
│   ├── core/                   # Core engine (FileIO, Logger, Messages, SettingsStore, ...)
│   ├── api/                    # LLM HTTP client, SSE parser, HttpSession
│   ├── tools/                  Core tools (BashTool, SkillTool, MemoryTool, UserTool, ...) and ToolFactory
│   └── mcp/                    # MCP client/server manager and transports
├── demos/
│   ├── daedalus-cli/           # Reference console REPL
│   ├── daedalus-manager/       # Multi-agent orchestration demo
│   ├── daedalus-subagent/      # Agent-spawns-agent demo
│   ├── daedalus-webui/         # HTTP + SSE web host
│   └── daedalus-settings-tui/  # FTXUI settings editor (SettingsStore TUI)
├── tests/                      # doctest-based unit tests
├── docs/                       # Living documentation
└── resources/                  # Banner, icons
```

### Source module responsibilities

| Module | Files | Responsibility |
|--------|-------|----------------|
| `src/library` | `Agent*.cpp`, `paths.cpp`, `project_paths.cpp`, ... | Public facade and PIMPL internals |
| `src/library/a2a` | `A2aClient.cpp`, `A2aServer.cpp` | A2A (Agent2Agent Protocol) HTTP+JSON client + server |
| `src/core` | `SettingsStore.cpp`, `Messages.cpp`, `Logger.cpp`, `SystemPromptBuilder.cpp`, ... | Settings, persistence, logging, compression, thread pool, metrics |
| `src/api` | `LLMClient.cpp`, `HttpSession.cpp` | Provider-agnostic HTTP/SSE chat client |
| `src/tools` | `ToolFactory.cpp`, `BashTool.cpp`, `SkillTool.cpp`, `MemoryTool.cpp`, ... | Core tool implementations registered by default |
| `demos/example-tools` | `GitTool.cpp`, `NetworkTool.cpp`, `SearchTool.cpp`, `PDFTool.cpp`, `SystemInfoTool.cpp`, `BackgroundTool.cpp`, ... | Optional example / reference tool implementations |
| `src/mcp` | `McpClient.cpp`, `McpServerManager.cpp`, `StdioMcpTransport.cpp`, `HttpMcpTransport.cpp` | MCP server integration |

## Build and test commands

### Configure and build

```bash
# Default: Release build, clang++ preferred

cmake -S . -B build
cmake --build build -j$(nproc)
```

### Debug build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

### Install

```bash
cmake --install build
```

Installs `libdaedalus.a`, public headers, and CMake config files to
`$HOME/.local` by default. The CLI demo installs to `~/.local/bin/daedalus-cli`.

### Run tests

```bash
ctest --test-dir build --output-on-failure
```

Expected: all hermetic tests pass; live-LLM tests are skipped unless
`DAEDALUS_RUN_LIVE_LLM_TESTS=1`.

### Static analysis

```bash
# Run clang-tidy over src, tests, demos, and tools
cmake --build build --target daedalus_static_analysis

# Or per-tree targets
cmake --build build --target daedalus_clang_tidy_lib
cmake --build build --target daedalus_clang_tidy_tests
cmake --build build --target daedalus_clang_tidy_demos
cmake --build build --target daedalus_clang_tidy_tools
```

`compile_commands.json` is emitted automatically. clang-tidy is optional;
if missing the targets are omitted with a status message.

### Demo build options

| Option | Default | Demo |
|--------|---------|------|
| `DAEDALUS_BUILD_DEMOS` | ON | All demos |
| `DAEDALUS_DEMO_SUBAGENT` | ON | `demos/daedalus-subagent/` |
| `DAEDALUS_DEMO_MANAGER` | ON | `demos/daedalus-manager/` |
| `DAEDALUS_DEMO_WEBUI` | OFF | `demos/daedalus-webui/` |
| `DAEDALUS_DEMO_SETTINGS_TUI` | ON | `demos/daedalus-settings-tui/` |

```bash
cmake -S . -B build -DCODEMAX_DEMO_WEBUI=ON
```

## Code style guidelines

### Language standard

- C++23 is required. Favour standard-library features over external deps.
- Use `std::expected`, `std::flat_map`, `std::span`, `std::jthread`,
  `std::stop_token`, `std::print`, and deducing-this where appropriate.

### No stdio in the library

**Rule:** code under `src/library/`, `src/api/`, `src/core/`, `src/mcp/`, and
`src/tools/` must not call `std::cout`, `std::cerr`, `std::cin`, `printf`,
`fprintf(stderr, ...)`, or write to stdout/stderr fds.

- Use `AgentSink` for host-visible output.
- Use `core/Logger.h` macros (`LOG_INFO`, `LOG_WARNING`, `LOG_ERROR`) for
  diagnostics; these write to per-project log files, not stdio.
- Demos under `demos/` are exempt.

Verification:

```bash
grep -rn "std::cout\|std::cerr\|std::cin" src/library src/api src/mcp src/tools
```

should return no matches outside comments.

### Naming and formatting

- Match the existing style in each file.
- Prefer clear, descriptive names. Phases and bug fixes are tagged in comments
  with markers like `§Phase 9.9`, `§BUG-001`, `§CRASH-4`.
- Keep edits scoped; avoid opportunistic refactors unrelated to the task.

### Includes

- Public headers must not leak internal implementation details.
- Internal code may include from `src/` and `include/` as needed.

## Testing instructions

### Test architecture

Two complementary layers:

| Layer | Location | Speed | External deps |
|-------|----------|-------|---------------|
| Unit tests | `tests/test_*.cpp` | Fast | None (doctest fetched) |
| Integration tests | `/workspace/Testing/test_*/` | Slow | Real MiniMax API |

### Running unit tests

```bash
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

Live-LLM tests (label `live-llm`) return skip code `77` by default. To run
them:

```bash
DAEDALUS_RUN_LIVE_LLM_TESTS=1 ctest --test-dir build --output-on-failure -L live-llm
```

### Running integration tests

```bash
bash /workspace/Testing/test02_hello_world/run.sh
bash /workspace/Testing/run_all.sh
```

These require a configured MiniMax model in `~/.local/share/daedalus/daedalus.db`.

### Adding a unit test

1. Create `tests/test_<component>.cpp` with doctest
   (`#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`).
2. Add `add_executable` + `add_test` in `tests/CMakeLists.txt`.
3. Reconfigure CMake and build the new target.
4. Run the test directly or via ctest.

## Storage and paths

daedalus uses a strict **two-tier** layout:

- **User-tier data** → XDG-compliant paths under
  `$XDG_DATA_HOME/daedalus/`, `$XDG_CONFIG_HOME/daedalus/`, etc.
- **Project-tier data** → `<project_root>/.daedalus/daedalus[-<agent_name>]/`

All project-specific state lives under a single hidden `.daedalus/`
base, with non-hidden `daedalus/` (default) and `daedalus-<name>/`
(namespaced) sub-folders. Never place project-specific memory or
transcripts in the XDG user tier. `tests/test_two_tier_rule.cpp`
enforces this.

Key files:

| File | Location | Purpose |
|------|----------|---------|
| `daedalus.db` | `$XDG_DATA_HOME/daedalus/daedalus.db` | SQLite settings (models, MCP servers, Serper key) |
| `messages.json` | `<project>/.daedalus/daedalus[-<name>]/messages.json` | Conversation transcript |
| `messages.json.crash` | Same dir | Crash mirror written by signal handlers |
| `memory/memory.sqlite` | Same dir | Long-term memory |

Env-var resolution is documented in `docs/storage.md`. For CI/sandboxing you
can collapse everything with `DAEDALUS_HOME=/tmp/foo`.

## Public API at a glance

```cpp
#include <daedalus.h>

class MySink : public daedalus::AgentSink {
public:
    void on_assistant_delta(std::string_view text) noexcept override;
    void on_tool_call(std::string_view name, std::string_view call_id,
                      const nlohmann::json& args) noexcept override;
    void on_tool_result(std::string_view name, std::string_view call_id,
                        const nlohmann::json& result) noexcept override;
    void on_state_change(int from, int to) noexcept override;
    void on_error(daedalus::ErrorKind kind, std::string_view msg,
                  bool will_retry) noexcept override;
    void on_log(daedalus::LogLevel lvl, std::string_view msg) noexcept override;
    bool request_command_approval(std::string_view cmd) override;
};

int main() {
    daedalus::AgentConfig cfg;
    cfg.project_root = ".";
    cfg.daedalus_home = daedalus::Paths::global().user_data_root();
    cfg.session_id = "my-session";

    daedalus::Agent agent(std::move(cfg), std::make_shared<MySink>());
    agent.initialize();

    agent.submit_user_message("hello");
    agent.run_one_turn();
}
```

See `docs/embedding.md` for the complete sink contract and lifecycle.

## Security considerations

- The agent runs as an unprivileged user; OS permissions are the primary
  security boundary.
- Bash commands require user approval unless `auto_approve` is enabled.
  `AgentConfig::auto_approve` is `std::optional<bool>`: when set it is
  authoritative (the SettingsStore row is ignored); when unset the store
  value applies as a legacy fallback. Hosts can toggle at runtime
  (`Agent::set_auto_approve()`), manage the approval caches
  (`clear_session_approvals()`, `clear_persisted_approvals()`,
  `set_remember_approvals()`), and observe cache/auto-mode bypasses via
  `AgentSink::on_command_auto_approved()`.
- Privilege escalation commands (`sudo`, `su -`, `pkexec`, `doas`, `dzdo`,
  `run0`) are blocked.
- Credential files (`*.pem`, `*.key`, `*.crt`, `*.p12`, `*.pfx`, `*id_rsa*`,
  `*id_ed25519*`) and sensitive paths (`.ssh/id_`, `.aws/`, `.gcp/`, `.azure/`,
  `.kube/`) are protected even if OS permissions would allow access.
- Force-push requires explicit approval.
- Sensitive data is filtered from Session Memory.

See `Readme.md` §Security for the full access-by-operation matrix.

## Useful documentation pointers

| Topic | Doc |
|-------|-----|
| Embedding and sink contract | `docs/embedding.md` |
| Storage layout and env vars | `docs/storage.md` |
| Settings API | `docs/settings.md` |
| Settings TUI editor | `docs/settings-tui.md` |
| Tool specifications | `docs/tools.md` |
| Context compression | `docs/how_compress_works.md` |
| Crash resilience | `docs/crash_resilience.md` |
| Library invariants | `docs/library_design.md` |
| MCP integration | `docs/mcp_server.md` |
| Network tools | `docs/network-tools.md` |
| Skills | `docs/skills.md` |
| Testing strategy | `docs/testing.md` |
| State machine | `docs/agent_state_machine.md` (redirects to `include/daedalus/Agent.h`) |
| A2A protocol | `docs/a2a.md` |

## Quick checklist before submitting changes

1. `cmake --build build -j$(nproc)` succeeds.
2. `ctest --test-dir build --output-on-failure` passes (or any new failures are
   explained).
3. No `std::cout`/`std::cerr`/`std::cin` added under `src/`.
4. New code follows the surrounding style and naming conventions.
5. Relevant docs are updated if behaviour changed.
6. If changing the public API, update `include/daedalus/` headers and consider
   `cmake/DaedalusConfig.cmake.in` impact.
