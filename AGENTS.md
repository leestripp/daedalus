<!-- monkeybyte:type: cpp -->

# AGENTS.md — daedalus monorepo guide

This repository is a **monorepo** containing the daedalus agent library
and both of its host applications. Read this root file first for every task.
All component-specific guidance is consolidated into the Daedalus, Icarus,
and MonkeyByte sections below; those sections are the authoritative living
documents for their codebases. **This is the only `AGENTS.md` in the repo —
do not recreate component copies.** Likewise, `plan.md` is the only planning
file. Instructions below to update `AGENTS.md` mean this root file; references
to applications scanning a target project's `AGENTS.md` describe runtime
behaviour and are unchanged.

§Monorepo (2026-09-24): the three standalone repos were consolidated
into this one (remote `gitlab.com/leestripp/daedalus`, library history
and tags preserved). Standalone `icarus`/`monkeybyte` histories live in
the archived GitLab repos and local backups.

## Layout

```
/                      ← THE daedalus repo (one git history, one remote)
├── CMakeLists.txt     ← superbuild root (add_subdirectory of all three)
├── daedalus/          ← the static library (libdaedalus.a), demos, docs, tests
├── icarus/            ← CLI host (REPL + A2A server)
├── monkeybyte/        ← GTK4/gtkmm desktop host
├── plan.md            ← consolidated active plans + DONE history
└── AGENTS.md          ← this file
```

**Commit, tag, and push once** — one history covers everything. Bare `x.y.z`
tags version the whole monorepo (see Versioning below).

## The architecture in one picture

```
┌──────────────────────┐   ┌──────────────────────────────┐
│ icarus (CLI)         │   │ monkeybyte (GTK4/gtkmm app)  │
│ terminal REPL + A2A  │   │ IDE-style editor + chat      │
└─────────┬────────────┘   └───────────────┬──────────────┘
          │  both link statically against  │
          ▼                                ▼
┌──────────────────────────────────────────────────────────┐
│ daedalus — libdaedalus.a (C++23 autonomous agent library)│
│ Agent loop · AgentSink events · tools · MCP · providers  │
└──────────────────────────────────────────────────────────┘
```

- **daedalus** is the shared brain: a static C++23 library
  (`libdaedalus.a`) exposing `daedalus::Agent` plus the `AgentSink`
  callback interface. It owns the agent loop, LLM HTTP/SSE clients
  (provider plugins), tool registry (bash, memory, skills, todos, scheduled
  tasks, web, MCP, …), settings store, and crash resilience. **The library never
  touches stdio** — all host-visible output flows through `AgentSink`.
- **icarus** is the terminal host: an interactive REPL with streaming
  rendering, slash commands, linenoise editing, and an A2A (Agent2Agent)
  HTTP server mode. It embeds `daedalus::Agent` and implements
  `ConsoleAgentSink`.
- **monkeybyte** is the desktop host: a native GTK4/gtkmm-4.0 code
  editor (GtkSourceView) with an agent chat panel, WebKit previews, and
  an HTTP `ControlServer` for UI automation. It embeds the same
  `daedalus::Agent` via `ChatController`.

Scheduled work uses the core `schedule_task` tool, persisted at
`agent_dir/scheduled_tasks.json`. A per-agent `std::jthread` queues due prompts
with source `trigger:task-<id>` and calls `on_turn_requested()`; hosts must
only schedule/drain the existing queue, never resubmit the prompt. One-shot
state is removed and recurring state advanced before that callback. If a turn
is active, the occurrence is dropped permanently instead of queued; recurring
tasks wait for their next slot, preventing unbounded stale-prompt backlogs.
See `daedalus/docs/tools.md` and `daedalus/docs/storage.md`.

## How the hosts consume daedalus (dual-mode)

Each host's CMake works in **two modes**, selected automatically:

| Mode | Trigger | Behaviour |
|------|---------|-----------|
| **In-tree** (default here) | root superbuild `add_subdirectory` — `TARGET daedalus` already exists | Hosts link the in-tree `daedalus` target directly; one configure builds everything |
| **Standalone** | `cmake -S icarus …` / `cmake -S monkeybyte …` on the subdir alone | Host FetchContents this repo from GitLab `main`; the superbuild root detects it is not top-level and builds **only the library** (no GTK/CLI deps leak in) |

To point a standalone host build at a local checkout (e.g. uncommitted
library changes), use the standard CMake override:

```bash
cmake -S icarus -B /tmp/icarus-build \
    -DFETCHCONTENT_SOURCE_DIR_DAEDALUS=$PWD
```

**There is no local patching mechanism** for the fetched copy — API
changes must land in the repo. In the monorepo that just means committing
both sides together; the superbuild always builds hosts against the
in-tree library.

**Static-link gotcha:** `libdaedalus.a` does not propagate its PRIVATE
dependencies through plain `.a` linking, so hosts explicitly link
daedalus's transitive system libs (OpenSSL, libpng, libjpeg, poppler-cpp,
sqlite3). When you add a new system dependency inside daedalus, expect
matching `target_link_libraries` updates in **both** hosts.

## Shared runtime state (important)

The two hosts are independent binaries but deliberately share
daedalus's **user-tier store**:

| Path | Shared by | Contents |
|------|-----------|----------|
| `$XDG_DATA_HOME/daedalus/daedalus.db` | both hosts | Providers, models, MCP servers, encrypted vault, brain index (FTS5), tuning limits |
| `$XDG_DATA_HOME/daedalus/brain/` | both hosts | §SECOND-BRAIN: global markdown knowledge base (PARA folders; `brain` tool) — source of truth, safe to edit by hand |
| `$XDG_CONFIG_HOME/daedalus/vault.key` | both hosts | Vault master key |
| `$XDG_DATA_HOME/daedalus/skills/` | both hosts | User-global skills |
| `<project>/.daedalus/daedalus[-<name>]/` | per workspace | Transcript (`messages.json`), crash mirror, per-project memory, logs |
| `$XDG_DATA_HOME/monkeybyte/monkeybyte.db` | monkeybyte only | UI settings, prompts, workspace bindings |

Consequences:

- A provider/model/MCP server configured in one host is immediately
  available in the other. Schema changes in daedalus's `SettingsStore`
  affect both apps — coordinate migrations carefully.
- Two agents (or two monkeybyte instances) on the **same** project
  directory share `.daedalus/` agent state — this is unsupported.
- daedalus enforces a strict **two-tier rule**: user data in XDG paths,
  project data under `<project>/.daedalus/`. Never mix them.

## Common conventions

- **C++23**, CMake ≥ 3.28, Ninja/Make; gcc preferred on Ubuntu.
- **Dated `§Marker` comments** (e.g. `§Monorepo`, `§PDF-CORE`,
  `§BUG-006`, `2026-08-31`) annotate non-obvious decisions and
  cross-component contract points. Keep them accurate; add one for
  subtle fixes.
- **Versioning — ONE stream.** Bare `x.y.z` tags at the repo root are
  the only tags; a release tags the whole monorepo once and the
  library, icarus, and monkeybyte all derive their version from the
  same `git describe --tags` at configure time, baked in as a compile
  definition. No hand-maintained version files, no per-component tag
  namespaces. User-Agents: `daedalus-icarus/<v>`,
  `daedalus-monkeybyte/<v>`.
- `std::expected<T, std::string>`-style error handling, no exceptions
  for control flow; in monkeybyte, GTK callbacks must be wrapped in
  `ui_guard(...)` (a thrown exception in a GTK callback aborts).
- Error handling on network paths is non-fatal (JSON/expected errors,
  no asserts).

## Build & test (superbuild)

```bash
cmake -S . -B build                 # hosts default ON for top-level builds
                                    # (monkeybyte only if pkg-config finds
                                    # gtkmm-4.0 + gtksourceview-5 +
                                    # webkitgtk-6.0 + md4c; override with
                                    # -DDAEDALUS_BUILD_MONKEYBYTE=ON/OFF)
cmake --build build -j$(nproc)      # lib + demos + tests + icarus + monkeybyte
ctest --test-dir build --output-on-failure   # all suites, one run
ctest --test-dir build -L lib       # or cli / app slices
cmake --build build --target static_analysis  # cppcheck umbrella
```

Per-component test notes: daedalus is a doctest suite (100+ test binaries;
live-LLM tests skipped unless `DAEDALUS_RUN_LIVE_LLM_TESTS=1`); icarus
live-LLM tests skipped unless `DAEDALUS_RUN_LIVE_LLM_TESTS=1`); icarus
has doctest tests (`cli_args`, `system_prompt`, `migrate`); monkeybyte
has a hand-rolled assert runner (`unit_tests`) plus the three-route
loopback ControlServer for UI automation (`./monkeybyte/run.sh` runs the app
with its local GSettings schema).

Verification bar before submitting anything: clean superbuild + full
ctest green, no new static-analysis warnings.

## Where to change what

| You want to… | Component(s) |
|--------------|--------------|
| Change agent-loop behavior, tools, providers, MCP, settings schema, compression | `daedalus/` (then adopt in hosts) |
| Change terminal UX, slash commands, A2A server | `icarus/` |
| Change editor UI, panels, chat rendering, browser tabs, desktop packaging | `monkeybyte/` |
| Add a provider/model/MCP capability | `daedalus/` core — both hosts pick it up through the shared `daedalus.db` |
| Expose a host-specific capability (UI control, web tools) as an agent tool | the host, via `ToolFactory::register_tool` (replace-by-name since daedalus 0.9.7) |

## Key documentation

| Component | Start here | Deep dives |
|-----------|------------|------------|
| daedalus | `AGENTS.md` (Daedalus section), `daedalus/Readme.md` | `daedalus/docs/` (library_design, storage, settings, tools, mcp_server, crash_resilience, …) + `daedalus/CHANGELOG.md` |
| icarus | `AGENTS.md` (Icarus section), `icarus/Readme.md` | `icarus/docs/` (cli, design, ui-design, a2a-protocol) |
| monkeybyte | `AGENTS.md` (MonkeyByte section), `monkeybyte/docs/design.md` (authoritative spec) | `monkeybyte/README.md`; all plans: root `plan.md` |

## Reserved directories

`.daedalus/` directories (at the repo root and in any project an agent
runs on) are **agent runtime state** — memory, transcripts, todos, logs.
Never delete, rename, or hand-edit them, and never include them in
cleanup tasks.

---

## daedalus — Agent Guide

> Component root: `daedalus/`. Relative source, test, and
> documentation paths in this section are relative to that component
> unless they are explicitly root-qualified. Build commands labelled
> standalone are run from the component directory; prefer the root
> superbuild commands in the general guide above.

This file is a living reference for AI coding agents working on the daedalus
project. It summarises the project's architecture, build system, conventions,
tests, and security model. Treat it as the first stop before making changes.

### Project overview

daedalus is a **C++23 autonomous agent library** that ships as a static library
(`libdaedalus.a`) plus five demo hosts under `demos/`. Hosts embed the
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

### Technology stack

| Layer | Technology |
|-------|------------|
| Language | C++23 (std::expected, std::flat_map, std::span, std::jthread, std::stop_token, std::print, deducing this) |
| Build system | CMake 3.28+ |
| Compiler | System default; GCC 13+ or Clang 17+ (override with `CC`/`CXX`) |
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

### Project structure

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

#### Source module responsibilities

| Module | Files | Responsibility |
|--------|-------|----------------|
| `src/library` | `Agent*.cpp`, `paths.cpp`, `project_paths.cpp`, ... | Public facade and PIMPL internals |
| `src/library/a2a` | `A2aClient.cpp`, `A2aServer.cpp` | A2A (Agent2Agent Protocol) HTTP+JSON client + server |
| `src/core` | `SettingsStore.cpp`, `SecondBrain.cpp`, `InterprocessLock.cpp`, `Messages.cpp`, `Logger.cpp`, ... | Settings, global brain + process locking, persistence, logging, compression, metrics |
| `src/api` | `LLMClient.cpp`, `HttpSession.cpp`, `providers/*.cpp` | Provider-agnostic HTTP/SSE chat client + provider plugins (`ProviderPlugin` hook implementations: OpenAI-compatible default, MiniMax, Moonshot, LM Studio; see `docs/provider-plugins.md`) + prompt-cache helpers (`ContextStatus.h`, §ContextCache) |
| `src/tools` | `ToolFactory.cpp`, `BashTool.cpp`, `SkillTool.cpp`, `MemoryTool.cpp`, `WebSearchTool.cpp`, `VaultTool.cpp`, `BrainTool.cpp`, `PDFTool.cpp` (read_pdf, §PDF-CORE), ... | Core tool implementations registered by default |
| `demos/example-tools` | `GitTool.cpp`, `NetworkTool.cpp`, `SearchTool.cpp` (grep/wiki), `SystemInfoTool.cpp`, `BackgroundTool.cpp`, ... | Optional example / reference tool implementations |
| `src/mcp` | `McpClient.cpp`, `McpServerManager.cpp`, `StdioMcpTransport.cpp`, `HttpMcpTransport.cpp` | MCP server integration |

### Build and test commands

#### Configure and build

```bash
# Default: Release build, system compiler (GCC on Ubuntu); override with CC/CXX

# §Monorepo (2026-09-24): PREFERRED — build everything from the repo root:
#   (cd .. && cmake -S . -B build && cmake --build build -j$(nproc))
# One ./build for library + hosts. The standalone build below still works
# (dual-mode) but creates a per-component build/ you must clean up yourself.
cmake -S . -B build
cmake --build build -j$(nproc)
```

#### Debug build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

#### Install

```bash
cmake --install build
```

Installs `libdaedalus.a`, public headers, and CMake config files to
`$HOME/.local` by default. The CLI demo installs to `~/.local/bin/daedalus-cli`.

#### Run tests

```bash
ctest --test-dir build --output-on-failure
```

Expected: all hermetic tests pass; live-LLM tests are skipped unless
`DAEDALUS_RUN_LIVE_LLM_TESTS=1`.

#### Static analysis

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

#### Demo build options

| Option | Default | Demo |
|--------|---------|------|
| `DAEDALUS_BUILD_DEMOS` | ON | All demos |
| `DAEDALUS_DEMO_SUBAGENT` | ON | `demos/daedalus-subagent/` |
| `DAEDALUS_DEMO_MANAGER` | ON | `demos/daedalus-manager/` |
| `DAEDALUS_DEMO_WEBUI` | OFF | `demos/daedalus-webui/` |
| `DAEDALUS_DEMO_SETTINGS_TUI` | ON | `demos/daedalus-settings-tui/` |

```bash
cmake -S daedalus -B build -DDAEDALUS_DEMO_WEBUI=ON
```

### Code style guidelines

#### Language standard

- C++23 is required. Favour standard-library features over external deps.
- Use `std::expected`, `std::flat_map`, `std::span`, `std::jthread`,
  `std::stop_token`, `std::print`, and deducing-this where appropriate.

#### No stdio in the library

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

#### Naming and formatting

- Match the existing style in each file.
- Prefer clear, descriptive names. Phases and bug fixes are tagged in comments
  with markers like `§Phase 9.9`, `§BUG-001`, `§CRASH-4`.
- Keep edits scoped; avoid opportunistic refactors unrelated to the task.

#### Includes

- Public headers must not leak internal implementation details.
- Internal code may include from `src/` and `include/` as needed.

### Testing instructions

#### Test architecture

Two complementary layers:

| Layer | Location | Speed | External deps |
|-------|----------|-------|---------------|
| Unit tests | `tests/test_*.cpp` | Fast | None (doctest fetched) |
| Integration tests | `/workspace/Testing/test_*/` | Slow | Real MiniMax API |

#### Running unit tests

```bash
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

Live-LLM tests (label `live-llm`) return skip code `77` by default. To run
them:

```bash
DAEDALUS_RUN_LIVE_LLM_TESTS=1 ctest --test-dir build --output-on-failure -L live-llm
```

#### Running integration tests

```bash
bash /workspace/Testing/test02_hello_world/run.sh
bash /workspace/Testing/run_all.sh
```

These require a configured MiniMax model in `~/.local/share/daedalus/daedalus.db`.

#### Adding a unit test

1. Create `tests/test_<component>.cpp` with doctest
   (`#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`).
2. Add `add_executable` + `add_test` in `tests/CMakeLists.txt`.
3. Reconfigure CMake and build the new target.
4. Run the test directly or via ctest.

### Storage and paths

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
| `daedalus.db` | `$XDG_DATA_HOME/daedalus/daedalus.db` | SQLite settings and brain index; WAL mode required |
| `brain/` | `$XDG_DATA_HOME/daedalus/brain/` | Global Second Brain Markdown source of truth |
| `brain/.lock` | Same brain root | Bounded shared/exclusive inter-process lock for all brain operations |
| `messages.json` | `<project>/.daedalus/daedalus[-<name>]/messages.json` | Conversation transcript |
| `messages.json.crash` | Same dir | Crash mirror written by signal handlers |
| `memory/memory.json` | Same dir | Long-term memory (JSON source of truth + SQLite recovery mirror) |

Env-var resolution is documented in `docs/storage.md`. For CI/sandboxing you
can collapse everything with `DAEDALUS_HOME=/tmp/foo`.

Second Brain mutations hold the brain-root `flock` exclusively and readers hold
it shared; never narrow that coverage to only the file write. Note replacement
must remain temp-file + `fsync` + rename + directory `fsync`, and brain
metadata/FTS updates must remain transactional. `SettingsStore` opens in WAL
mode and serializes schema initialization with `BEGIN IMMEDIATE`. Multi-process
and `SIGKILL` recovery coverage lives in
`tests/test_second_brain_concurrency.cpp`; always run it after changing these
invariants.

### Public API at a glance

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

### Security considerations

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

### Useful documentation pointers

| Topic | Doc |
|-------|-----|
| Embedding and sink contract | `docs/embedding.md` |
| Storage layout and env vars | `docs/storage.md` |
| Settings API | `docs/settings.md` |
| Provider plugins | `docs/provider-plugins.md` |
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

### Quick checklist before submitting changes

1. `cmake --build build -j$(nproc)` succeeds.
2. `ctest --test-dir build --output-on-failure` passes (or any new failures are
   explained).
3. No `std::cout`/`std::cerr`/`std::cin` added under `src/`.
4. New code follows the surrounding style and naming conventions.
5. Relevant docs are updated if behaviour changed.
6. If changing the public API, update `include/daedalus/` headers and consider
   `cmake/DaedalusConfig.cmake.in` impact.


---

## AGENTS.md — icarus

> Component root: `icarus/`. Relative source, test, and
> documentation paths in this section are relative to that component
> unless they are explicitly root-qualified. Build commands labelled
> standalone are run from the component directory; prefer the root
> superbuild commands in the general guide above.

> This file is written for AI coding agents that need to understand and work on
> the icarus project. It assumes no prior knowledge of the codebase.

---

### Project overview

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

### Technology stack

- **Language**: C++23
- **Build system**: CMake ≥ 3.28
- **Compiler**: gcc/g++ (preferred when available; clang also works)
- **License**: MIT — see `LICENSE` file. Individual source files carry
  `SPDX-License-Identifier: BSL-1.0` headers.

#### System dependencies

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

#### Fetched dependencies

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

### Build commands

#### Standard build

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

#### Tests

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

#### Static analysis

```bash
cmake --build build --target icarus_cppcheck
```

This target is created automatically when `cppcheck` is found. It checks only
the `src/` tree; warnings from fetched dependencies (daedalus, OmniNet) are
suppressed.

#### Install

```bash
cmake --build build --target install
```

Installs `icarus` to `$HOME/.local/bin/` by default (default prefix skipped
when `HOME` is unset or `/`). Honour/override with `-DCMAKE_INSTALL_PREFIX=...`
or `cmake --install build --prefix ...`.

#### Quick verification after changes

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
./build/icarus --version
```

#### Versioning

`ICARUS_VERSION` comes from the latest git tag
(`git describe --tags --abbrev=0`) and is baked in as a compile definition
(fallback: `"dev"`). It is sent to providers as the
`daedalus-icarus/<version>` User-Agent; `--version` prints
`icarus <version> (daedalus <lib version>)` — the icarus version from the
`ICARUS_VERSION` compile definition, the lib version from
`daedalus::Agent::library_version()`.
Do not edit `src/core/version.h` — its macros are unused legacy stubs.

---

### Project structure

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

### Code style and conventions

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

### Key source components

#### `src/main.cpp` — Entry point and REPL

- Parses CLI arguments via `parse_cli_args()` in `util.h`.
- Handles `--migrate-data` and `--clear-memory` short-circuits before agent
  init.
- Runs either the interactive REPL or the A2A agent daemon based on flags.
- In REPL mode: reads lines from `InputReader`, submits to `daedalus::Agent`,
  and renders output via `ConsoleAgentSink`. Contains all slash-command
  handlers. Before blocking for input it drains library-originated pending
  messages (background completions and scheduled tasks); the sink's
  `on_turn_requested()` wakes `InputQueue`, while `run_one_turn()` stays on
  the main thread.
- Identifies itself to LLM providers via the User-Agent
  `daedalus-icarus/<version>` (`AgentConfig::user_agent`, §UA 2026-07-29).
  `<version>` is the `ICARUS_VERSION` compile definition, derived by
  CMakeLists.txt from the latest git tag (`git describe --tags --abbrev=0`,
  `"dev"` when the tree is untagged or git is unavailable) — there is no
  manually maintained version file.

#### `src/util.h` — CLI and infrastructure

- `CliArgs` struct holds all parsed flags; `parse_cli_args()` is the parser.
- `print_help()` prints the `--help` text including the slash-command summary.
- `resolve_system_prompt()` loads the system prompt in priority order:
  1. `--system-prompt <file>` override
  2. `<project_root>/AGENTS.md`
- Signal handlers restore the terminal (cursor/SGR) and cancel the in-flight
  turn via `g_agent_for_signal`; a second SIGINT/SIGTERM force-exits.

#### `src/ConsoleAgentSink.cpp` — Terminal sink

- Implements `daedalus::AgentSink` callbacks, including the structured
  `on_error` channel and `on_turn_requested()` background/scheduled-task
  wake-up. The latter only wakes the input queue; it never resubmits message
  text.
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

#### `src/a2a_tools.cpp` — A2A client tool

- Registers `A2aTool` (a `daedalus::BaseTool`) with the agent.
- Exposes 7 actions: `get_agent_card`, `send_message`,
  `send_streaming_message`, `get_task`, `list_tasks`, `cancel_task`,
  `subscribe_to_task`.
- Uses `daedalus::a2a::A2aClient` internally.

#### `src/a2a_agent.cpp` — A2A agent server

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

#### `src/ui/AnsiRenderer.cpp` — ANSI rendering

- `RenderConfig::detect()` probes terminal capabilities (color, Unicode).
- `effective_terminal_width()` re-queries the width per render
  (TIOCGWINSZ → `$COLUMNS` → fallback) — all boxes/tables/rules are
  resize-aware; there is no SIGWINCH handler.
- `Style` class emits SGR escape codes (or no-ops when color is disabled).
- `make_box` is always left-justified (`Alignment::Auto` → `Left`).
- **`DrawTable` is the canonical table builder** — every table in the app
  goes through it (auto-size, shrink-to-fit, `…` truncation). Do not
  hand-roll tables.

#### `src/ui/InputReader.h` — line editing

- linenoise-backed input with history, ESC detection (bare ESC cancels the
  agent turn), and a stop-aware async read used by `InputThread`.
- **`detail::kSlashCommands` is the single source of truth** for ghost-text
  hints and tab-completion. When adding/removing a slash command, update
  three places: the handler in `main.cpp`, `kSlashCommands`, and
  `print_help()` in `util.h`.

---

### CLI reference

#### Key options

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

#### In-session commands

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

### Configuration and data layout

#### daedalus home

```
$XDG_DATA_HOME/daedalus/        (default: $HOME/.local/share/daedalus/)
```

Override with `--home <dir>` or `DAEDALUS_HOME`.

**Important**: `daedalus.db` lives in the **data** hierarchy, not config.
Using `--home $HOME/.config/daedalus` will cause `--model-list` to report
`(none configured)` because the database is not there.

#### Runtime data files

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
├── memory/memory.json       # Persistent session memory
├── todos.json               # Persistent todo list
├── scheduled_tasks.json     # Persistent scheduled tasks
├── sessions/                # Saved sessions
└── system_prompt.txt        # Optional additional Daedalus project overlay

<project_root>/.daedalus/daedalus-<name>/
    # Named-agent variant of the above
```

Run `/paths` in a session for the fully-resolved list.

#### System prompt resolution

```
1. --system-prompt <file>          (CLI override, highest priority)
2. <project_root>/AGENTS.md        (open standard for agent instructions)
```

The executable-sidecar prompt is no longer read. Daedalus additionally appends
`<project_root>/.daedalus/daedalus[-<name>]/system_prompt.txt` during agent
initialization when that optional overlay exists.

---

### Security considerations

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

### Things agents should know before editing

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

### Notes for agents

- Prefer dedicated tools (`Read`, `Grep`, `Glob`) over `cat`/`grep` from
  `execute_bash` when available.
- Run the build and tests after non-trivial changes; do not assume the code
  compiles.
- Make minimal, targeted changes — review the diff before committing.
- Leave unrelated code alone unless the user explicitly asks.
- When requirements are ambiguous, ask the user rather than guessing.
- User-facing behaviour changes should be reflected in `Readme.md` and/or
  `docs/cli.md`.


---

## AGENTS.md — MonkeyByte

> Component root: `monkeybyte/`. Relative source, test, and
> documentation paths in this section are relative to that component
> unless they are explicitly root-qualified. Build commands labelled
> standalone are run from the component directory; prefer the root
> superbuild commands in the general guide above.

Guidance for AI coding agents working in this repository. Read this before
making changes.

### Project overview

**MonkeyByte** is a native desktop **AI code editor** written in **C++23**
with **GTK4 / gtkmm**. It is a GTK4 port of a previously browser-based UI;
the full behavioral specification lives in **`docs/design.md`** — treat it
as the source of truth. Section references like `(design §4.2)` in code
comments point into that document; keep them accurate when behavior changes.

Core loop: the user picks a workspace folder, edits files in a
GtkSourceView editor, and talks to an LLM agent in a side chat panel. The
agent backend is the **daedalus** agent library (fetched from GitLab via
CMake FetchContent; daedalus in turn fetches **OmniNet**, an HTTP library).
LLM providers are configured through an OpenAI-compatible chat API
(base URL + endpoint + model name + API key per provider).
LLM API requests identify the client via `AgentConfig::user_agent`, set to
`daedalus-monkeybyte/<version>` in `ChatController::ensure_agent()`. The
version comes from the latest git tag at configure time (`MONKEYBYTE_VERSION`
compile definition; `project()` version is only a fallback).

The UI is IDE-shaped: Activity Bar (left, 48 px) → Sidebar with 4 panels
(Workspace / Search / Source Control / Settings) → tabbed Editor in the
center → Chat panel docked right → Status Bar at the bottom.

### Technology stack

- **Language:** C++23 (`CMAKE_CXX_STANDARD 23`, extensions off), built with
  CMake ≥ 3.28 + Ninja.
- **UI:** GTK4 via **gtkmm-4.0** (C++ bindings, `Gtk::` / `Glib::` / `sigc::`),
  plus **gtksourceview-5** for the code editor and **webkitgtk-6.0** for the
  read-only preview tabs (`ui/HtmlView` — markdown preview is converted with
  **md4c**, `backend/MarkdownToHtml`; system packages
  `libwebkitgtk-6.0-dev` + `libmd4c-dev`).
- **HTTP:** **libcurl** (`ApiClient`, `EventSourceClient`) for the
  `/api/*` contract; **OmniNet** (bundled via daedalus) for the local
  `ControlServer` (127.0.0.1-only UI control endpoint, used for automated
  UI testing; `MONKEYBYTE_CONTROL_PORT` env override, 0 = off; default
  port 17878).
- **Audio:** **PipeWire** (`libpipewire-0.3`) for local microphone capture
  and drained playback. `backend/VoiceService` coordinates the loopback-only
  ChimpFlow STT/TTS gateway; no cloud audio service is used.
- **Storage:** **SQLite3** — `monkeybyte.db` at
  `$XDG_DATA_HOME/monkeybyte/` (interface settings, system prompts,
  workspace, per-model prompt bindings). Providers and models live in
  **daedalus's** store (`$XDG_DATA_HOME/daedalus/daedalus.db`): shared
  `ProviderConfig` (host/key/endpoints + a `plugin` column selecting a
  daedalus provider plugin — empty = auto-detect from name/host, daedalus
  §ProviderPlugins; built-ins `openai-compatible`, `minimax`, `moonshot`,
  `lmstudio` — configured once) + `ModelConfig`
  referencing a provider by name — the registry the agent resolves models
  from. MCP servers live there too, as do agent tuning limits (max
  iterations, tool-result/request/response sizes); the Settings →
  Providers / Models / MCP Servers / Agent tabs read/write it via
  `backend/ModelBridge` / `backend/McpBridge`
  (`daedalus::SettingsStore::instance()`). Skill/tool enablement is
  host policy daedalus does NOT persist — monkeybyte stores the disabled
  lists in monkeybyte.db kv (`agent.disabled_skills`,
  `agent.disabled_tools`) and seeds `AgentConfig` on every agent build.
  **GSettings** (schema
  `io.github.monkeybyte.*`) as UI cache only (window/sidebar/chat widths,
  theme name, selected model, chat history blob). Window/sidebar/chat
  geometry is authoritative in monkeybyte.db kv (`ui.window_width`,
  `ui.window_height`, `ui.sidebar_width`, `ui.chat_width` — see
  `MainWindow::load_px`/`save_px`); GSettings copies are best-effort cache
  because dconf does not persist in every environment (e.g. containers
  sharing a foreign session bus).
- **JSON:** nlohmann_json (≥ 3.11).
- **Agent:** `daedalus` static library (FetchContent from
  `https://gitlab.com/leestripp/daedalus.git`, tag `main`, demos/tests off,
  `EXCLUDE_FROM_ALL`). First configure needs network access.
- **Static analysis:** optional `cppcheck` target (`monkeybyte_cppcheck`).

### Build, run, test

```bash
# §Monorepo (2026-09-24): PREFERRED — build everything from the repo root:
#   (cd .. && cmake -S . -B build && cmake --build build -j$(nproc))
# One ./build for library + hosts. The standalone build below still works
# (dual-mode) but creates a per-component build/ you must clean up yourself.
# (run.sh auto-detects both build trees; superbuild binaries land in
# build/icarus/ and build/monkeybyte/).
cmake -B build -G Ninja        # configure (standalone: fetches daedalus monorepo)
cmake --build build            # build app + unit_tests
./run.sh                       # rebuild + run with local GSettings schema
cd build && ctest --output-on-failure   # run the test suite
./build/unit_tests             # run the test binary directly
cmake --build build --target monkeybyte_cppcheck   # static analysis (if cppcheck installed)
```

Notes:
- Default build type is `Debug`.
- The GSettings schema is compiled into `build/gschemas` (no system
  install); `run.sh` exports `GSETTINGS_SCHEMA_DIR` so the app finds it.
  Running `./build/monkeybyte` directly without that env var aborts with
  `Settings schema 'io.github.monkeybyte.*' is not installed`.
- `data/` (gschema XML + `theme/dark.css`, `theme/light.css`) is copied
  next to the binary at build time.
- ctest registers `unit_tests` (the project's own suite) plus ~22 OmniNet
  dependency tests. The OmniNet test binaries are not built by the default
  target; build them explicitly (`cmake --build build --target omnet_test
  test_buffer_protection ...`) or ctest reports them as "Not Run".

### Code organization

```
src/monkeybyte/
  main.cpp                 Gtk::Application entry point; creates AppServices + MainWindow
  AppServices.h            Shared services struct (SettingsStore, FileService,
                           ThemeManager, GSettings handles) passed by reference everywhere
  FileOpenMode.h           enum class FileOpenMode { Text, MarkdownPreview,
                           HtmlPreview, Browser } — how a file opens in the
                           editor area (§6); Browser tabs hold a URL, not a
                           workspace path (EditorArea::open_url)
  backend/                 GTK-free core services (unit-testable):
    FileService            workspace-rooted file ops with path-traversal sandboxing (§10)
    FileTemplates          built-in starter-file templates for "Add File from
                           Template…" (§4.2); include_guard_name() derives C++
                           include guards from the chosen filename
    SearchService          case-insensitive content search; skips hidden dirs,
                           binaries, >5 MB files, and the glob exclude list
                           from Settings → Search (§4.3)
    MarkdownToHtml         markdown → HTML (md4c, GFM tables/strikethrough/
                           task lists) + themed document wrapper for the
                           WebKit preview tabs (§6)
    SettingsStore          SQLite persistence: interface, prompts, search
                           excludes, model→prompt bindings (§10.2)
    ModelBridge            converts the flat UI provider/model structs to/from
                           daedalus's SettingsStore (providers configured once,
                           models reference them by name, §4.5/§10.2); owns the
                           one-time legacy `models` table migration;
                           supported_thinking_levels() queries a model's
                           thinking-level capabilities via daedalus's
                           ProviderRegistry without building an agent
                           (§ThinkingLevel, chat header dropdown)
    SkillDiscovery         scans the daedalus global skills dir; parses
                           SKILL.md front matter (§4.5 Agent tab)
    McpBridge              converts the flat UI MCP model to/from daedalus's
                           SettingsStore (the registry the agent loads, §10.2);
                           parse_json() imports raw provider JSON
                           ("mcpServers" wrapper / keyed / bare, §5.4)
    GitService             branch + porcelain status (read-only panel, §4.4)
    WebFetch               synchronous GET for the web_fetch agent tool,
                           built on OmniNet's HttpClient (redirects, 10 s
                           timeout, 2 MB cap, §6)
    HtmlToText             static HTML → plain text for agent consumption (§6)
    WebSearch              key-less DuckDuckGo HTML-endpoint search for the
                           web_search agent tool; parse() is a pure function
    ChimpFlowClient        synchronous, worker-called curl client for ChimpFlow
                           discovery, WAV synthesis and multipart transcription;
                           injectable transport, cancellation, no error retries
    Wav                    bounded PCM16/24/32 WAV parsing, 16 kHz PCM encoding,
                           binary-safe OpenAI multipart body construction
    AudioEngine            AudioBackend abstraction + native PipeWire capture/
                           playback; streams open only on demand
    VoiceService           shared AppServices owner for Settings + Chat; worker
                           threads, Glib::Dispatcher completions, FIFO speech,
                           preview/dictation cancellation and generation guards
    SpeechStream           incremental assistant sentence/paragraph segmentation;
                           UTF-8-safe boundaries, code-fence holding, remainder-
                           only final flush, no duplicate streamed narration
                           for unit tests (§6)
    Logger                 append-only error log at
                           $XDG_DATA_HOME/monkeybyte/monkeybyte.log (§7.2);
                           daedalus's own per-session diagnostics live at
                           <workspace>/.daedalus/daedalus/logs/ (gitignored)
  http/
    ApiClient              legacy synchronous libcurl client helpers; the old
                           workspace/chat/settings HTTP backend is not served
    EventSourceClient      legacy SSE client helper
    ControlServer          OmniNet loopback-only UI automation endpoint with
                           /api/ui/geometry, /api/ui/commands, /api/ui/control
  tree/
    WorkspaceTree          lazy-loaded file tree logic; expansion-state memory (§4.2)
  ui/
    MainWindow             single top-level window; owns all panels (§2)
    ActivityBar, Sidebar, EditorArea, StatusBar, UiControl, UiGuard.h, FileIcons.h
    HtmlView               WebKit read-only viewer for EditorArea's preview
                           tabs (markdown via backend/MarkdownToHtml, raw
                           HTML files, and browser tabs with a nav/address
                           bar — agent-drivable via the browser.open /
                           browser.navigate / browser.read / browser.eval /
                           browser.links / browser.console / browser.upload /
                           browser.list / browser.close UiControl commands,
                           §6; captures console.* + page errors via an
                           injected document-start user script for
                           browser.console; browser.upload queues local
                           files so a run-file-chooser request completes
                           without the default GTK dialog; device presets
                           (Phone/Tablet/Laptop/Desktop + Rotate + Fit to
                           Window) constrain the real WebKit viewport so
                           width media queries respond as on the device —
                           phone presets also spoof a mobile User-Agent;
                           oversized presets get scrollbars, undersized ones
                           sit centered (§6))
    panels/                WorkspacePanel, SearchPanel, GitPanel, SettingsPanel
    chat/                  ChatPanel, ChatController (agent state machine §7.6),
                           ChatMessage, ToolCard, Markdown (md → Pango §7.4),
                           UserCommands (slash-command registry §7.8), AppUiTool,
                           WebFetchTool, WebSearchTool (tab-less research, §6)
    dialogs/               WorkspacePickerDialog, NewFolderDialog, ModelEditDialog,
                           ProviderEditDialog, McpServerDialog, McpJsonImportDialog,
                           SystemPromptDialog, NewFileDialog, RenameDialog,
                           NewFromTemplateDialog ("Add File from Template…", §4.2),
                           FolderPicker (shared breadcrumb+list folder selector
                           used by the New File / New Folder dialogs, confined
                           to its root, §4.2/§5.1)
    theme/                 ThemeManager (swaps dark/light CSS providers §11)
tests/
  test_main.cpp            single assert-based test runner (no framework)
data/                      gschema XML + dark.css / light.css (Catppuccin-style themes)
                           + icons/ (mb-*-symbolic SVG set: activity bar, file
                           tree/tabs via ui/FileIcons.h, tool cards, buttons —
                           icon search path registered in main.cpp)
resources/                 monkeybyte.desktop launcher entry + monkeybyte.svg icon
                           + banner.png / icon.png / icon_192x192.jpg (README
                           and packaging artwork)
docs/design.md             full design specification (authoritative)
```

Everything except `main.cpp` compiles into the `monkeybyte_core` static
library (globbed: `src/monkeybyte/*.cpp` recursive), which is shared by the
app and the test binary — new `.cpp` files are picked up automatically on
re-configure (`CONFIGURE_DEPENDS`).

### Code style guidelines

- Match the existing style; it is consistent throughout:
  - `snake_case` for functions, variables, and members (members get a
    trailing underscore, e.g. `workspace_`); `PascalCase` for types.
  - Everything in `namespace monkeybyte`; headers use `#pragma once`.
  - Includes ordered: project headers, then third-party, then std.
- Error handling: `std::expected<T, std::string>` for fallible operations
  (see `FileService`, `ApiClient`); `std::optional` for lookups. No
  exceptions for control flow. An exception escaping a GTK signal
  handler / timeout / idle source / C callback aborts the process, so
  callback bodies that touch throwing code (SQLite via `SettingsStore`,
  `nlohmann::json` conversions, daedalus calls, `std::filesystem`
  throwing overloads, `set_markup`) are wrapped in `ui_guard("ctx", …)`
  (`ui/UiGuard.h` — catches, logs to `Logger`, degrades to a no-op).
  Use the `error_code` `std::filesystem` overloads in callback code.
- Widgets are GTK-managed: created with `Gtk::make_managed`, held as raw
  pointers by the parent; cross-component events go through
  `sigc::signal`.
- Keep `backend/`, `tree/`, and `http/` (except `ControlServer`'s OmniNet
  dependency) free of GTK includes so they stay unit-testable without a
  main loop — that separation is what `tests/test_main.cpp` relies on.
- Reference the design document in comments for non-obvious behavior
  (`// ... (design §4.2)`), and update `docs/design.md` when you
  intentionally change specified behavior.
- Warnings: compiled with `-Wall -Wextra -Wpedantic`; cppcheck runs with
  `--enable=warning,style,performance --error-exitcode=1 --inline-suppr`
  (inline `// cppcheck-suppress <id>` comments are honored; the target
  must exit 0 — keep it green).

### Testing instructions

- Tests are a hand-rolled, assert-based runner in `tests/test_main.cpp` —
  **no test framework**. Add cases with the `check(cond, "name")` helper
  and register a new `test_*()` function in `main()`. Current suites:
  `test_file_service`, `test_settings_store`, `test_mcp_bridge`,
  `test_mcp_json_import`,
  `test_model_bridge` (+ `test_model_bridge_migration`,
  `test_model_bridge_thinking_levels`),
  `test_skill_discovery`, `test_workspace_tree_logic`,
  `test_search_service`, `test_markdown`, `test_markdown_to_html`,
  `test_file_icons`, `test_file_templates`,
  `test_user_commands`, `test_html_to_text`, `test_web_search_parse`.
- Tests must not require GTK widgets or a display: only the GTK-free backend
  services are covered. Asynchronous VoiceService dispatcher tests may
  iterate the default **Glib** main context with fake client/audio
  dependencies; they must not open real audio devices. Test classes with
  complex state should expose a synchronous hook for tests (design §14).
- Tests use temp dirs under `std::filesystem::temp_directory_path()` and
  clean up after themselves; a throwaway SQLite DB path is passed to
  `SettingsStore` explicitly.
- Run `cmake --build build && (cd build && ctest --output-on-failure)`
  before considering work done. `unit_tests` must exit 0.
- UI behavior is exercised through the `ControlServer` /
  `UiControl` command surface (HTTP-driven geometry and sizer commands)
  rather than unit tests.

### Deployment / runtime

- Development runs from the build tree via `./run.sh`.
- User-local install (no sudo): `./install.sh [prefix]` (default `~/.local`) —
  builds, then `cmake --install` puts the binary in `bin/`, themes + icons in
  `share/monkeybyte/data/`, the compiled GSettings schema in
  `share/glib-2.0/schemas/` (found via XDG data dirs under `~/.local`),
  and the freedesktop launcher entry (`resources/monkeybyte.desktop`) in
  `share/applications/`, plus the app icon (`resources/monkeybyte.svg`)
  in `share/icons/hicolor/scalable/apps/`. The script then rewrites the
  installed `.desktop` `Exec=` to the absolute `<prefix>/bin/monkeybyte`
  path (desktop sessions don't reliably have `<prefix>/bin` in PATH) and
  refreshes the desktop/icon caches (best effort).
  A bare `cmake --install build` / `ninja install` with no `--prefix`
  defaults to the current user's `~/.local` (non-root only; root keeps
  `/usr/local`), so containers running as an arbitrary user (e.g. `ubuntu`)
  install unprivileged. An explicit `--prefix` always wins.
  `ThemeManager::data_dir()` resolves `data/` relative to the executable
  (build tree or `<prefix>/share/monkeybyte/data`), not the CWD.
- **Multiple instances from the launcher:** MonkeyByte runs
  `G_APPLICATION_NON_UNIQUE` (no D-Bus uniqueness - the name is never
  owned), so N processes coexist fine from a terminal. GNOME Shell,
  however, *focuses* the running window on a plain left-click of the
  launcher icon instead of re-running `Exec=` (shell policy, unfixable
  from the app side). To start another instance: middle-click the icon,
  or right-click -> **New Window** (the `new-window` desktop action in
  `resources/monkeybyte.desktop`). Keep `install.sh`'s `Exec=` rewrite
  in sync for both `Exec=` lines (main entry + action).
- Runtime state: SQLite DBs under `$XDG_DATA_HOME` (`monkeybyte/` and
  `daedalus/`); GSettings UI cache under the `io.github.monkeybyte.*`
  schemas. GSettings is **cache-only** — authoritative state lives in
  SQLite (design §10.2). Do not make GSettings the source of truth for
  anything the backend owns.

- **.deb packaging:** `packages/deb/build-deb.sh` (run from the repo root,
  after a Release build in `build-release/`) stages the CMake install tree,
  derives `Depends:` via `dpkg-shlibdeps` (this is what catches indirect
  linkages like daedalus's **poppler-cpp** — never hand-write the list),
  and writes `packages/deb/monkeybyte_<ver>_<arch>.deb`. Build the package
  ON an Ubuntu 26.04 host — sonames must match the target distro (the host
  is Ubuntu 26.04 since 2026-08; the old Aerynos build would have produced
  a broken package). `dpkg-shlibdeps` needs a dummy `debian/control` in a
  temp dir (handled in the script). Version comes from `git describe --tags`
  (`0.9.3-2-g096727a` -> `0.9.3+2.g096727a`). The CMake install `CODE` step
  that runs `glib-compile-schemas` ignores DESTDIR and targets the real
  prefix — harmless as user (permission denied), but don't run the staging
  step with sudo. Ubuntu's dpkg triggers recompile schemas/desktop DB/icon
  cache on install; the shipped `postinst` is belt-and-braces.

- **Locale safety:** glibmm init (inside `Gtk::Application::create`) calls
  `std::locale::global(std::locale(""))`, so every C++ stream becomes
  locale-aware -- under e.g. en_AU.UTF-8 `os << 37519` yields
  "37,519". OmniNet's HTTP request builder uses std::ostringstream, so
  bodies >= 1000 bytes went out with `Content-Length: 37,519`, which
  Cloudflare's edge rejects with a plain 400 HTML page (small payloads
  slipped through -- no comma below 1000). `main.cpp` restores
  `std::locale::classic()` right after app creation; never remove that
  line, and never format protocol bytes with locale-aware streams.

- **Theme CSS vs Adwaita's paint layers (GTK 4.20+):** Adwaita paints
  widgets with a `background-image` gradient + `box-shadow`, not
  `background-color`/`border`, and can stay in its light variant even when
  `gtk-interface-color-scheme` = DARK (observed: `theme_bg_color` resolves
  light on GTK 4.22 despite the dark setting). A theme rule that only sets
  `background-color` paints UNDER Adwaita's white image -- white-on-white
  buttons. Every rule in `data/theme/*.css` that repaints a widget must
  also clear `background-image`/`box-shadow`/`text-shadow` -- done via one
  shared neutralize rule block in each theme file covering entry,
  textview, spinbutton, switch, headerbar, popover, tooltip, scrollbar,
  listbox/listview rows, modelbutton, plus explicit clears on `button`
  and `dropdown > button`. Diagnose with `app.themeprobe` /
  `app.screenshot` (§14); for a full visual check run a second
  instance with MONKEYBYTE_CONTROL_PORT=17879 on a throwaway workspace,
  POST app.screenshot to /api/ui/control, and toggle
  org.gnome.desktop.interface color-scheme between shots.

- **Guarded load-then-save:** settings handlers use the
  load-modify-save pattern (`load_interface` -> change one field ->
  `save_interface`, which rewrites ALL keys). If the guarded load fails
  (SQLite busy/locked hiccup), the handler MUST bail instead of saving
  the default-constructed struct -- otherwise every other interface
  setting is silently reset to defaults (this wiped `browser_home_url`
  in the field). `load_interface_guarded` therefore returns
  `std::optional`; same pattern in WorkspacePanel's toggle_hidden.

- **WebKitGTK quirks (6.0):** `webkit_web_view_new_with_related_view`
  is gone - popups use the construct-only `related-view` property
  (g_object_new). New-window requests need the `create` signal connected
  (HtmlView popup hook -> EditorArea::open_browser_view). This system's
  webkitgtk-6.0 2.52.3 build has a broken JSC Date numeric path
  (Date.now() = INT32_MIN in web processes) - platform bug, breaks
  YouTube; see browser_bugs.md before suspecting app code.
- **Browser resource bounds (2026-10-27):** long URLs must remain ellipsized
  in EditorArea's header (full URL in the tooltip), not dictate natural
  width. The mbConsole hook must also stay batched and size-bounded: forwarding
  every console call synchronously across WebKit IPC lets each chatty tab
  flood the GTK main thread and allocate unbounded message strings. Keep the
  burst cap/drop warning and native direct-post bounds when changing it.

### Security considerations

- **Path sandboxing:** all workspace file access must go through
  `FileService::resolve()`, which rejects `..` escapes and absolute paths
  outside the workspace. Never build filesystem paths from user/agent
  input by hand. File reads are capped at 5 MB (`kMaxReadSize`).
- **API keys:** model API keys are stored in SQLite and support
  `${ENV_VAR}` placeholders (resolved in `ChatController`) so secrets can
  stay in the environment. Never log or echo API keys; the UI renders them
  masked.
- **ControlServer binds 127.0.0.1 only** and must stay that way — it
  exposes UI control commands with no authentication. Respect the
  `MONKEYBYTE_CONTROL_PORT=0` kill switch.
- **MCP servers** execute user-configured shell commands
  (`McpServer.command`); treat that config surface as trusted-user-only
  and do not auto-populate it from untrusted input.
- **Markdown rendering** escapes text before producing Pango markup
  (`Markdown::to_pango`) — never inject unescaped agent/LLM output into
  Pango markup or the chat UI can be markup-injected.
- **Agent web access:** `browser.eval` runs arbitrary agent-supplied JS in
  an open browser tab — page-sandboxed, but it can act on any site the
  user has logged into in that tab (same trust decision as agent-driven
  navigation). `web_fetch` / `web_search` make outbound HTTP requests to
  arbitrary agent-chosen URLs (2 MB cap, 10 s timeout, http(s) only);
  treat results as untrusted content, never as instructions.
- **Workspace changes** must cancel any in-flight agent session first
  (design §12.1); switching workspaces mid-agent is unsafe.

### Conventions / gotchas

#### Chat scrolling (§SCROLL-KINETIC)

- **A forced bottom pin must cancel GTK deceleration.** GTK 4.22's
  `scrolled_window_deceleration_cb` continues calling
  `gtk_adjustment_set_value()` after idle/timeout pins. Protecting only
  `stick_to_bottom_` leaves the viewport free to return to its old position
  (reproduced: new user bubble pinned at 1363, then deceleration returned it
  to 0). `cancel_scroller_deceleration()` in `ui/chat/ScrollPin.h` toggles
  `set_kinetic_scrolling(false)` to invoke GTK's cancellation, then restores
  the setting. Do not replace this with another timer. Regression coverage
  in `test_scroll_pin_kinetic_cancel()` drives the actual GTK scroll and
  decelerate controllers; live X11 and Wayland reproductions passed too.

#### ChimpFlow / PipeWire invariants

- `pw_stream_new_simple()` retains the `pw_stream_events` pointer. Capture
  and playback event tables must have static lifetime; stack-local tables
  caused a realtime-thread use-after-free/segfault in live voice preview.
- Destroy a PipeWire stream under its thread-loop lock, then unlock **before**
  `pw_thread_loop_stop()` (stop joins the thread). Playback completion means
  the `drained` event, not merely that samples were queued. Re-arm one-shot
  completion latches on every start.
- Normalise every playback utterance to 0.12 RMS, with at most 4x gain and
  a 0.98 peak limit (`normalise_playback_samples`). Do not replace this with
  peak-only normalisation: perceived voice loudness tracks RMS more closely.
- VoiceService public methods and widget callbacks stay on the UI thread;
  curl runs on `std::jthread` workers and returns through `Glib::Dispatcher`.
  Generation counters suppress stale completions. Contain exceptions per
  operation so one failed utterance cannot block later speech.
- ChimpFlow is loopback-only. Use a dedicated `tts` + `stt` key; never log it
  or fall back to an admin/other app's key. Dictation inserts review text
  only—never auto-send. History restoration must not trigger read-aloud.
  Normal deltas use SpeechStream; complete `tell_user` bubbles need their own
  direct `speak()` call (the separate-handler omission caused silent first
  messages). Bubble close flushes only the segmenter's unspoken remainder.


- daedalus's `ToolFactory` is **process-global**; since daedalus 0.9.7
  `register_tool()` is replace-by-name (plus a new `unregister_tool()`),
  so re-registering a host tool on agent rebuild no longer duplicates the
  provider payload. monkeybyte still registers its host tools
  (`AppUiTool`, `WebFetchTool`) on agent construction. Web search is the core
  unified failover `web_search` tool, not a separate MonkeyByte search tool.
- `/clear`, the 🗑 button, and workspace change (`ChatPanel::
  clear_history` → `ChatController::clear(true)`) also wipe daedalus's
  **persisted** message chain via `wipe_agent_session()`
  (`messages.json` + `.tmp`/`.crash` mirrors + the SQLite mirror row
  under `<workspace>/.daedalus/daedalus/`). daedalus restores that chain
  on every agent build (`Messages::load`), so a UI-only clear lets the
  old conversation silently resume. daedalus 0.9.7 (§CRASH-6) fixed
  `Messages::clear()` to persist, but no public
  `Agent::clear_messages()` exists yet — the wipe uses the public
  `resolve_agent_dir` + `SettingsStore::delete_messages_json`. The
  settings-edit rebuild (`reset_agent` → `clear(false)`) deliberately
  keeps the persisted chain so the rebuilt agent resumes the visible
  conversation.
- daedalus's `run_one_turn()` drains the **entire pending queue**, joining
  queued texts FIFO (blank-line separated), concatenating images, and merging
  origins into one dispatch. The host still pairs submissions with driven
  turns: an idle `submit_user_message()` without `run_one_turn()` remains
  pending. Use `pending_count()` and `on_turn_requested()` for scheduling.
- `/compress` is host-triggered context compression: the host only calls
  `Agent::compress_context()` (daedalus §AutoCompress) and updates the UI —
  daedalus summarizes the oldest ~70 % internally (same path as the 90 %
  auto loop; mid-turn it defers to the cycle checkpoint, idle it blocks
  the caller — hence it runs on the turn thread). Never submit a summary
  prompt as a user message. daedalus fires `on_compaction` **only on
  success** — its skip (too few messages) and failure (summary call)
  paths are silent, so `ChatController::compress_context()` pre-checks
  the message count and reports a failed idle compress (no `on_compaction`
  by return, tracked via `compaction_observed_`) through `error_message`.
- **Multiple instances:** the app is a NON_UNIQUE GApplication with
  HANDLES_OPEN (`main.cpp`) — several instances run side by side, one per
  workspace; a folder argument opens that workspace (a file path opens its
  parent). Global settings are shared (SQLite WAL + busy timeouts;
  GSettings/dconf is multi-process safe), window state is per instance.
  Only the **first** instance owns the HTTP control port; later instances
  log a warning and run without it. Two instances on the *same* workspace
  is unsupported (they would share `.daedalus/` agent state).
- **Chat image attachments:** the attach button (§READ-IMAGE) is gated
  on the selected model's vision capability, opens a file dialog, and
  passes the image through so daedalus's LLMClient emits an `image_url`
  content part; thumbnails render inline in the chat bubble.
- The Workspace panel header **+** button is a menu with 5 actions
  (§4.2): New File, New Folder, Add File from Template…, plus refresh and
  reveal-style actions — not a plain "new file" button.
- Intentionally absent (design §16) — do not add: git staging/commit UI,
  diff viewer, tab drag-reorder, multi-window, notifications,
  drag-and-drop file upload, WebSocket support.
- The file tree deliberately does **not** auto-refresh when the agent
  mutates files (only on manual refresh / create / delete / workspace
  change) — preserve this (§4.2).
- The tree hides dotfiles by default (`FileService::show_hidden_`;
  Settings → Interface → Explorer toggles it) and follows the active
  editor tab (`reveal_path` on tab switch). Right-click context menus
  exist on tree rows, editor tabs, the editor text view, and chat
  bubbles (`GtkPopoverMenu` + secondary `GtkGestureClick`).
- File mutations from the tree context menu are coordinated with open
  editor tabs: deletes call `EditorArea::close_tabs_under`, renames call
  `EditorArea::retarget_tabs` (via WorkspacePanel → Sidebar → MainWindow
  signals).
- The settings panel hides editor + chat when active; the chat history is
  per-session and wiped on workspace change; editor tabs survive workspace
  changes.
- `.gitignore` also excludes `.codemax/`, `.daedalus/`, and `prompt.txt`
  (local tooling artifacts).
