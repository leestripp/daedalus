# daedalus

A C++23 autonomous AI-agent library and its two host applications, in one
monorepo.

| Component | What it is |
|-----------|------------|
| [`daedalus/`](daedalus/) | The shared brain — `libdaedalus.a`: agent loop, LLM providers, tool registry (bash, memory, skills, todos, web, MCP…), settings store. Never touches stdio; hosts implement `AgentSink`. |
| [`icarus/`](icarus/) | Terminal host — streaming REPL with slash commands and an A2A (Agent2Agent) HTTP server mode. |
| [`monkeybyte/`](monkeybyte/) | Desktop host — GTK4/gtkmm code editor with agent chat panel, WebKit previews, and an HTTP UI-automation server. |

## Build everything

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure    # or: -L lib | -L cli | -L app
```

`monkeybyte` builds by default only when pkg-config finds the GTK stack
(`gtkmm-4.0`, `gtksourceview-5`, `webkitgtk-6.0`, `md4c`) — override with
`-DDAEDALUS_BUILD_MONKEYBYTE=ON/OFF`; `-DDAEDALUS_BUILD_ICARUS=OFF`
skips the CLI. Install prefix defaults to `~/.local`.

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

One repo, three tag streams: bare `x.y.z` = library, `icarus/x.y.z` and
`monkeybyte/x.y.z` = hosts. Versions are derived from `git describe` at
configure time — no hand-maintained version files.

## Documentation

Start with [AGENTS.md](AGENTS.md) (monorepo guide), then each component's
own `AGENTS.md` / `Readme.md` / `docs/`.
