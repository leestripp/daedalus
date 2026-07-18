# Daedalus CodeMax Agent Context

> Status: **✅ COMPLETE — all 4 demos + hello-world C++ test green; 100% pass
> rate serially (97% under `-j12` due to two pre-existing DB-lock flake
> unrelated to this work; see §6).**
> Last updated: 2026-07-18 (build with `-j12`)

---

## 1. Project Snapshot

- **Name**: Daedalus - Autonomous Agent Library for C++
- **Version**: 0.5.3-dirty
- **Language**: C++23
- **Build**: CMake 3.28+, Clang 17+ / GCC 14+
- **Type**: Static library `libdaedalus.a` + 4 demo hosts
- **Core value**: XDG-compliant storage, per-agent isolation, native MCP support

### 4 demo apps (per prompt.txt)

| Demo | Role | CMake Option | Default |
|---|---|---|---|
| `daedalus-cli` | REPL with `/models /skills /tools /stats /quit` | always on | ON |
| `daedalus-subagent` | Spawns sub-agents from a parent | `DAEDALUS_DEMO_SUBAGENT` | ON |
| `daedalus-manager` | Orchestrates planner/actor/verifier/coder | `DAEDALUS_DEMO_MANAGER` | ON |
| `daedalus-webui` | HTTP + SSE browser host | `DAEDALUS_DEMO_WEBUI` | OFF (enabled in this build) |

All 4 are built with `cmake -S . -B build -DDAEDALUS_DEMO_WEBUI=ON`.

---

## 2. System

- **Kernel**: Linux 7.0.3-6.stable x86_64
- **OS**: Ubuntu 26.04 LTS (Podman container)
- **Cores**: 12 (use `cmake --build build -j12`)
- **Display**: Wayland (no GPU; SSH-friendly)
- **User**: ubuntu at `/workspace/daedalus`

---

## 3. Plan & Results

### Step A — Bug fix surfaced by `test_mcp_load_from_agent`

`mcp::McpServerConfig::is_valid()` required `args` to be non-empty for
stdio servers. Reasonable CLI servers (e.g., `/bin/true`, `node`,
`cat`) accept zero args, so this was too strict — both the JSON and
SQLite load paths silently dropped legitimate entries.

**Fix applied:** `src/mcp/McpServerConfig.h` — stdio now requires only
non-empty `name` and `command`; `args` is optional.

Regression test: `tests/test_mcp_load_from_agent.cpp::alpha` (no args)
now loads; previously was dropped.

### Step B — All 4 demos built with `-j12`

```bash
cmake -S . -B build -DDAEDALUS_DEMO_WEBUI=ON
cmake --build build -j12
```

```
[ 91%] Built target daedalus-subagent
[ 91%] Built target daedalus-manager
[ 92%] Built target daedalus-cli
[ 93%] Built target daedalus-webui
[100%] Built target daedalus_tool_definitions_tests
```

### Step C — Hello-world C++ test, one per demo

New smoke test: `tests/test_demos_hello_world.cpp` (registered as
`demos_hello_world_tests`). It does five things:

1. Launches `daedalus-cli --version` → exit 0, banner in stdout.
2. Launches `daedalus-subagent --version` → exit 0, banner.
3. Launches `daedalus-manager --version` → exit 0, banner.
4. Launches `daedalus-webui --version` → exit 0, banner.
5. Spawns `daedalus-webui` on `:18888`, polls `/healthz` until
   `{"ok":true}` returns.
6. Compiles a one-liner C++ `Hello, World!` program and runs the
   binary.

Per-demo gating honours `DAEDALUS_DEMO_<NAME>_OPT` env vars set by
ctest from the CMake option cache — a default `webui=OFF` build skips
the webui step cleanly.

```
[ok]   daedalus-cli (--version, exit 0)
[ok]   daedalus-subagent (--version, exit 0)
[ok]   daedalus-manager (--version, exit 0)
[ok]   daedalus-webui (--version, exit 0)
[ok]   daedalus-webui (/healthz returns {"ok":true})
[ok]   hello-world C++ program compiled and ran

All 4 demo apps + hello-world C++ test: PASS
```

### Step D — Full regression suite

```
ctest --test-dir build --output-on-failure -j1   # serial
→ 100% tests passed, 0 tests failed out of 66
   (2 live-LLM tests skipped: daedalus_token_counts_sse_tests,
                               daedalus_no_content_duplicate_tests)
```

```
ctest --test-dir build --output-on-failure -j12  # parallel
→ 97% tests passed, 2 tests failed out of 66
   FAILED: settings_active_model_tests, settings_for_home_vs_instance_tests
```

The two parallel failures are pre-existing DB-lock races on the
process-global `SettingsStore::instance()`. **Verified pre-existing
on `main` without this work** — same 2 tests fail under `-j12` from a
clean rebuild with all changes stashed. Out of scope for this change;
fixing it would mean giving each test its own store (touching ~6
tests that share the singleton).

### Step E — Commit

Single changelist (`§FIX-MCP-STORE-LOAD: end-to-end MCP load + 4 demos +
hello-world smoke test`) — see git log.

---

## 4. XDG Storage Layout (verified)

```
$XDG_DATA_HOME/daedalus/                        # ~/.local/share/daedalus/
├── daedalus.db                                 # Settings (models, MCP, kv)
└── skills/installed/                           # §FIX-XDG-STORAGE: was ~/.daedalus/skills/

$XDG_CONFIG_HOME/daedalus/                      # ~/.config/daedalus/
├── mcp/servers.json                            # XDG-compliant path (NEW)
├── mcp/.env                                    # XDG-compliant path (NEW)
└── config.json

$XDG_STATE_HOME/daedalus/                       # ~/.local/state/daedalus/
└── logs/daedalus.log

$XDG_CACHE_HOME/daedalus/                       # ~/.cache/daedalus/
└── (future: response cache)

<project>/.daedalus/                            # project-tier (per-agent default)
├── messages.json + .crash
├── memory/
└── sessions/<id>/scratch.json

<project>/.daedalus-<name>/                     # project-tier (named agent)
└── (same)
```

See `docs/xdg_compliance.md` for the full spec mapping, resolution
priority, and Podman/systemd examples.

---

## 5. Documentation Map

| Document | Purpose |
|---|---|
| `docs/index.md` | Start here |
| `docs/storage.md` | Two-tier layout, env-var resolution |
| `docs/xdg_compliance.md` | XDG Base Directory mapping (NEW this session) |
| `docs/settings.md` | SettingsStore API |
| `docs/mcp_server.md` | MCP integration |
| `docs/tools.md` | Tool specs (40+) |
| `docs/skills.md` | Agent Skills |
| `docs/crash_resilience.md` | Atomic save + crash recovery |
| `docs/how_compress_works.md` | 70/30 split |
| `docs/session_memory.md` | Memory system |
| `docs/agent_state_machine.md` | State transitions |
| `docs/agent_system_prompts.md` | Prompt composition |
| `docs/testing.md` | Test conventions |

---

## 6. Pre-existing issues (not introduced by this session)

- Two tests fail under `-j12` due to SQLite DB-lock races on the
  process-global `SettingsStore::instance()`:
  - `settings_active_model_tests`
  - `settings_for_home_vs_instance_tests`

  Same failures on a clean rebuild of `main` (verified). Both pass
  when run serially (`-j1`). Out of scope for the data-storage fix;
  touching them would expand the surface area without addressing the
  user's request.

- `tests/test_phase15_disabled_switches.cpp` has 3 pre-existing
  `-Wunused-variable` warnings (not errors). Tracked elsewhere.

- `doctest 2.4.12` triggers a `-W#warnings` from `<ciso646>` under
  GCC 15. Tracked in test header comment block.
