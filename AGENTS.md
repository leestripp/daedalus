<!-- monkeybyte:type: cpp -->

# AGENTS.md — daedalus monorepo guide

This repository is a **monorepo** containing the daedalus agent library
and both of its host applications. Read this file first when a task may
span more than one component. For deep, component-specific guidance
always defer to each component's own `AGENTS.md` — those are the
authoritative, living documents for their codebases.

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
├── Plan.md            ← monorepo consolidation plan
└── AGENTS.md          ← this file
```

**Commit, tag, and push once** — one history covers everything. Tags are
namespaced per component (see Versioning below).

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

Per-component test notes: daedalus is a doctest suite (~105 tests;
live-LLM tests skipped unless `DAEDALUS_RUN_LIVE_LLM_TESTS=1`); icarus
has doctest tests (`cli_args`, `system_prompt`, `migrate`); monkeybyte
has a hand-rolled assert runner (`unit_tests`) plus the ControlServer
HTTP API for UI automation (`./monkeybyte/run.sh` runs the app with its
local GSettings schema).

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
| daedalus | `daedalus/AGENTS.md`, `daedalus/Readme.md` | `daedalus/docs/` (library_design, storage, settings, tools, mcp_server, crash_resilience, …) + `daedalus/CHANGELOG.md` |
| icarus | `icarus/AGENTS.md`, `icarus/Readme.md` | `icarus/docs/` (cli, design, ui-design, a2a-protocol) |
| monkeybyte | `monkeybyte/AGENTS.md`, `monkeybyte/docs/design.md` (authoritative spec) | `monkeybyte/README.md`, `monkeybyte/Plan.md` |

## Reserved directories

`.daedalus/` directories (at the repo root and in any project an agent
runs on) are **agent runtime state** — memory, transcripts, todos, logs.
Never delete, rename, or hand-edit them, and never include them in
cleanup tasks.
