# demos/

> **There is no "mode" flag on the `Agent`. The library has one loop.
> Each demo shows a different way to compose `Agent`s.**

Every demo here is a standalone executable that exercises `libdaedalus.a`
through the **public** `<daedalus.h>` API only — none include internal
headers (`LLMClient`, `SettingsManager`, `SkillManager`, etc.).
They are exemplars: a host that wants to embed CodeMax in their own
application copies this layout.

---

## Available demos

| Demo | Default | What it shows | Code |
|------|---------|---------------|------|
| `daedalus-cli` | **ON** | Console REPL: read user input, drive one `run_one_turn()`, render streamed tokens. The canonical "embed example". | [`demos/daedalus-cli/`](daedalus-cli/) |
| `daedalus-manager` | **ON** | 3-Agent orchestration (planner → actor → validator) from a host loop. The pattern that replaced `--manager-mode`. | [`demos/daedalus-manager/`](daedalus-manager/) |
| `daedalus-subagent` | **ON** | Parent agent spawns a child agent via a host-registered tool. The pattern that replaced `--sub-agent`. | [`demos/daedalus-subagent/`](daedalus-subagent/) |
| `daedalus-webui` | OFF | HTTP + Server-Sent Events host: one agent per browser session, streamed live. | [`demos/daedalus-webui/`](daedalus-webui/) |

---

## Build flags

```cmake
option(DAEDALUS_BUILD_DEMOS       "Build demo executables"  ON)
option(DAEDALUS_DEMO_SUBAGENT     "Build sub-agent demo"    ON)
option(DAEDALUS_DEMO_MANAGER      "Build manager demo"      ON)
option(DAEDALUS_DEMO_WEBUI        "Build webui demo"        OFF)
```

Set the ones you don't want on the cmake command line:

```bash
# Build everything, including the webui
cmake -S . -B build -DDAEDALUS_DEMO_WEBUI=ON

# Library-only build (no demos at all)
cmake -S . -B build -DDAEDALUS_BUILD_DEMOS=OFF
```

---

## Run

```bash
# CLI — point at any project directory (defaults to .)
./build/demos/daedalus-cli/daedalus-cli /path/to/project

# Manager — provide a goal (default goal is the demo goal)
./build/demos/daedalus-manager/daedalus-manager /path/to/project

# Subagent — type a question; the parent delegates to a child
./build/demos/daedalus-subagent/daedalus-subagent /path/to/project

# Webui — open in a browser
./build/demos/daedalus-webui/daedalus-webui 8088
# then open http://127.0.0.1:8088/
```

---

## Code layout (all 4 demos)

Each demo follows the same pattern:

```
demos/daedalus-<name>/
├── CMakeLists.txt     # declares the executable, links daedalus
├── main.cpp           # the host loop + arg parsing (~60-180 lines)
└── <helper>.h         # extracted sinks, parsers, HTML, etc.
```

The helpers exist so `main.cpp` is mostly the orchestration loop. All
non-trivial logic lives in sibling headers and is `#include`'d from
`main.cpp`.

---

## Extending the demo set

To add a new demo (e.g. `daedalus-tui`, `daedalus-gtk4`, `daedalus-discord`):

1. `mkdir -p demos/daedalus-<your-host>/`
2. Write `main.cpp` (under 300 lines) and any helper headers you need.
3. Add `demos/daedalus-<your-host>/CMakeLists.txt` that links against
   `daedalus` and includes only `<daedalus.h>`.
4. Add `add_subdirectory(daedalus-<your-host>)` to `demos/CMakeLists.txt`,
   behind an `option(DAEDALUS_DEMO_<YOUR-HOST> ...)` gate.
5. Add a row to the table above.

Use the existing demos as templates — they all share the same shape.
