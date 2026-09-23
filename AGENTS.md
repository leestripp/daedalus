<!-- monkeybyte:type: cpp -->

# AGENTS.md — daedalus workspace (monorepo-style multi-repo guide)

This directory is a **workspace containing three independent git
repositories** that together form one product family. Read this file
first when a task may span more than one repo. For deep, repo-specific
guidance always defer to each repository's own `AGENTS.md` — those are
the authoritative, living documents for their codebases.

## Workspace layout

```
/projects/AI/daedalus/          ← workspace root (NOT a git repo itself)
├── daedalus/    → git@gitlab.com:leestripp/daedalus.git    (the library)
├── icarus/      → git@gitlab.com:leestripp/icarus.git      (the CLI host)
└── monkeybyte/  → git@gitlab.com:leestripp/monkeybyte.git  (the desktop host)
```

Each subdirectory is a fully self-contained repo with its own history,
tags, `CMakeLists.txt`, and `AGENTS.md`. **Commit, tag, and push per
repo** — there is no top-level build or meta-repository.

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
  (provider plugins), tool registry (bash, memory, skills, todos, web,
  MCP, …), settings store, and crash resilience. **The library never
  touches stdio** — all host-visible output flows through `AgentSink`.
- **icarus** is the terminal host: an interactive REPL with streaming
  rendering, slash commands, linenoise editing, and an A2A (Agent2Agent)
  HTTP server mode. It embeds `daedalus::Agent` and implements
  `ConsoleAgentSink`.
- **monkeybyte** is the desktop host: a native GTK4/gtkmm-4.0 code
  editor (GtkSourceView) with an agent chat panel, WebKit previews, and
  an HTTP `ControlServer` for UI automation. It embeds the same
  `daedalus::Agent` via `ChatController`.

## How the hosts consume daedalus

Both hosts fetch daedalus from GitLab **`main`** at configure time —
**neither uses the local `daedalus/` checkout by default**:

| Host | Mechanism |
|------|-----------|
| icarus | `FetchContent_Populate` + `ExternalProject_Add` builds `libdaedalus.a`; also compiles the settings-TUI demo tab sources into the icarus binary |
| monkeybyte | `FetchContent_MakeAvailable(daedalus)` with demos/tests off, `EXCLUDE_FROM_ALL` |

**Local cross-repo development:** to test an uncommitted daedalus
change in a host, reconfigure the host build with the standard CMake
override:

```bash
# icarus
cmake -S icarus -B icarus/build -DFETCHCONTENT_SOURCE_DIR_DAEDALUS=$PWD/daedalus
# monkeybyte
cmake -S monkeybyte -B monkeybyte/build -DFETCHCONTENT_SOURCE_DIR_DAEDALUS=$PWD/daedalus
```

To ship a daedalus change: commit + push it to `daedalus` `main` first,
then wipe the host's fetched copy (`rm -rf <host>/build/_deps/daedalus-*`
for monkeybyte; icarus re-fetches via its ExternalProject stamp) and
reconfigure the host. **There is no local patching mechanism** — API
changes must land upstream in daedalus.

**Static-link gotcha:** `libdaedalus.a` does not propagate its PRIVATE
dependencies, so both hosts must link daedalus's transitive system libs
themselves (OpenSSL, libpng, libjpeg, poppler-cpp, sqlite3, curl/
OmniNet pieces). When you add a new system dependency inside daedalus,
expect matching `target_link_libraries` updates in **both** hosts.

## Shared runtime state (important)

The two hosts are independent binaries but deliberately share
daedalus's **user-tier store**:

| Path | Shared by | Contents |
|------|-----------|----------|
| `$XDG_DATA_HOME/daedalus/daedalus.db` | both hosts | Providers, models, MCP servers, encrypted vault, tuning limits |
| `$XDG_CONFIG_HOME/daedalus/vault.key` | both hosts | Vault master key |
| `$XDG_DATA_HOME/daedalus/skills/`, `memory/` | both hosts | Global skills + session memory |
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

## Common conventions across all three repos

- **C++23**, CMake ≥ 3.28, Ninja/Make; clang or gcc.
- **Dated `§Marker` comments** (e.g. `§PDF-CORE`, `§BUG-006`,
  `2026-08-31`) annotate non-obvious decisions and cross-repo contract
  points. Keep them accurate; add one for subtle fixes.
- Versioning: each repo derives its version from `git describe --tags`
  at configure time and bakes it in as a compile definition. No
  hand-maintained version files. User-Agents: `daedalus-icarus/<v>`,
  `daedalus-monkeybyte/<v>`.
- `std::expected<T, std::string>`-style error handling, no exceptions
  for control flow; in monkeybyte, GTK callbacks must be wrapped in
  `ui_guard(...)` (a thrown exception in a GTK callback aborts).
- Error handling on network paths is non-fatal (JSON/expected errors,
  no asserts).

## Build & test cheat sheet

| Repo | Configure + build | Test | Notes |
|------|-------------------|------|-------|
| daedalus | `cmake -S . -B build && cmake --build build -j$(nproc)` | `ctest --test-dir build --output-on-failure` | doctest suite (~100 tests); live-LLM tests skipped unless `DAEDALUS_RUN_LIVE_LLM_TESTS=1` |
| icarus | `cmake -S . -B build && cmake --build build -j$(nproc)` | `ctest --test-dir build --output-on-failure` | doctest (`cli_args`, `system_prompt`, `migrate`); first configure fetches daedalus/linenoise/FTXUI — needs network |
| monkeybyte | `cmake -B build -G Ninja && cmake --build build` | `cd build && ctest --output-on-failure` | hand-rolled assert runner (`unit_tests`); UI exercised via ControlServer HTTP commands; `./run.sh` to run with local GSettings schema |

Verification bar before submitting anything, per repo: clean build +
its test suite green, no new static-analysis warnings
(`daedalus_static_analysis`, `icarus_cppcheck`, `monkeybyte_cppcheck`).

## Where to change what

| You want to… | Repo(s) |
|--------------|---------|
| Change agent-loop behavior, tools, providers, MCP, settings schema, compression | `daedalus` (then adopt in hosts) |
| Change terminal UX, slash commands, A2A server | `icarus` |
| Change editor UI, panels, chat rendering, browser tabs, desktop packaging | `monkeybyte` |
| Add a provider/model/MCP capability | `daedalus` core — both hosts pick it up through the shared `daedalus.db` |
| Expose a host-specific capability (UI control, web tools) as an agent tool | the host repo, via `ToolFactory::register_tool` (replace-by-name since daedalus 0.9.7) |

## Key documentation

| Repo | Start here | Deep dives |
|------|------------|------------|
| daedalus | `daedalus/AGENTS.md`, `daedalus/Readme.md` | `daedalus/docs/` (library_design, storage, settings, tools, mcp_server, crash_resilience, …) + `CHANGELOG.md` |
| icarus | `icarus/AGENTS.md`, `icarus/Readme.md` | `icarus/docs/` (cli, design, ui-design, a2a-protocol) |
| monkeybyte | `monkeybyte/AGENTS.md`, `monkeybyte/docs/design.md` (authoritative spec) | `monkeybyte/README.md`, `monkeybyte/Plan.md` |

## Reserved directories

`.daedalus/` directories (at workspace root, in each repo, and in any
project an agent runs on) are **agent runtime state** — memory,
transcripts, todos, logs. Never delete, rename, or hand-edit them, and
never include them in cleanup tasks.
