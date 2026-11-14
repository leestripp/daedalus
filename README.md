# daedalus

A C++23 autonomous AI-agent library and its host applications, in one
monorepo.

| Component | What it is |
|-----------|------------|
| [`daedalus/`](daedalus/) | The shared brain — `libdaedalus.a`: agent loop, LLM providers, tool registry (bash, memory, skills, todos, web, MCP…), settings store. Never touches stdio; hosts implement `AgentSink`. |
| [`icarus/`](icarus/) | Terminal host — streaming REPL with slash commands and an A2A (Agent2Agent) HTTP server mode. |
| [`monkeybyte/`](monkeybyte/) | Standalone Electron host — complete IDE, persistent Daedalus chat, browser tools, shared settings, project types, and ChimpFlow voice. |

## Build the C++ superbuild

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure    # or: -L lib | -L cli | -L app
```

The superbuild compiles the library, Icarus, tests, and Electron's standalone
backend. `-DDAEDALUS_BUILD_ICARUS=OFF` skips the CLI;
`-DDAEDALUS_BUILD_ELECTRON_BACKEND=OFF` skips the backend. Install prefix
defaults to `~/.local`.

## Run the Electron app

```bash
cd monkeybyte
npm ci
npm run check
npm test
npm start
```

The sandbox-aware launcher fails closed if Chromium has no valid setuid or
namespace sandbox. On this host's `nosuid` checkout, use the copied-runtime and
root-owned-helper procedure in [`monkeybyte/README.md`](monkeybyte/README.md);
never use `--no-sandbox`.

## Install the applications

```bash
./install.sh            # builds + installs Icarus and Electron MonkeyByte
./install.sh /opt/foo   # or a custom prefix
```

One pass installs Icarus, the shared library, and standalone Electron
MonkeyByte with its compiled backend. The old GTK executable and source tree
are retired. The single **MonkeyByte** desktop launcher starts
`bin/monkeybyte-electron` and keeps the original icon; user settings and
sessions are preserved.

## Use just the library

Each subdir also builds standalone — and other CMake projects can embed
the library without any host dependencies:

```cmake
include(FetchContent)
FetchContent_Declare(daedalus
    GIT_REPOSITORY https://gitlab.com/leestripp/daedalus.git
    GIT_TAG main)
FetchContent_MakeAvailable(daedalus)   # hosts default OFF when not top-level
target_link_libraries(your_app PRIVATE daedalus)
```

## Versioning

One repo, one version stream: a release tags the whole monorepo with a
bare `x.y.z` tag, and the library, icarus, and monkeybyte all derive
their version from the same `git describe` at configure time. No
hand-maintained version files, no per-component tags.

## Documentation

Start with [AGENTS.md](AGENTS.md) (the consolidated monorepo and component
guides), then each component's `Readme.md` / `docs/`. There is only this one
root `AGENTS.md`; all active planning is consolidated in root `plan.md`.
