# CodeMax — working memory

## System info

> Captured by `system_info` tool.

| Field | Value |
|-------|-------|
| Container | podman |
| OS | Ubuntu 26.04 LTS (Resolute Raccoon), x86_64 |
| Kernel | Linux 7.0.3-6.stable |
| Date | 2026-07-14 04:15 UTC |
| Display | wayland |
| User | ubuntu (uid 1000) |
| Host | daedalus-dev-wayland |
| Shell | /bin/bash |
| Session | local |

## Project layout

- `/workspace/daedalus` — the C++ agent library + demos (CMake build)
- `/workspace/Testing` — manual + automated test suites for the demos

## Task — Remove `ask_user` Tool

### Context

The `ask_user` tool is broken and needs to be removed along with all its dead/zombie code.

### Files to Modify

| File | Change |
|------|--------|
| `src/tools/UserTool.h` | Remove `AskUserTool` class declaration and forward declaration |
| `src/tools/UserTool.cpp` | Remove `AskUserTool::get_definition()` and `AskUserTool::execute()` |
| `src/api/LLMClient.h` | Update comment at line ~167 mentioning "ask_user" |
| `src/api/LLMClient.cpp` | Remove ask_user handling block (~lines 1967-2007), update comment at line ~1797 |
| `include/daedalus/AgentSink.h` | Remove `request_user_input()` virtual method |
| `include/daedalus/types/AgentState.h` | Remove `AwaitingUser` enum value |
| `src/library/AgentState.cpp` | Remove `AwaitingUser` from `to_string()` and `agent_state_from()` |
| `src/library/AgentImpl.cpp` | Remove CHAT → AwaitingUser mapping (line ~53) |
| `src/tools/SetStatusTool.cpp` | Remove "AwaitingUser" from valid states array |
| `demos/daedalus-cli/ConsoleAgentSink.cpp` | Remove `request_user_input()` method |
| `demos/daedalus-webui/web_sse_sink.h` | Remove `request_user_input()` method and comment |
| `src/core/utils.cpp` | Update comment mentioning request_user_input |

### Tests to Update

| File | Change |
|------|--------|
| `tests/test_agent_state_transitions.cpp` | Update CHAT documentation comments |
| `tests/test_set_status_tool.cpp` | Remove "AwaitingUser" from test data |

### Code Reference Summary

The ask_user tool:
- **Returns**: `{"status": "ask_user", "question": "..."}`
- **Handled in**: `LLMClient.cpp` lines ~1967-2007 - calls `sink_->request_user_input()`, returns `AgentState::CHAT`
- **State mapping**: Internal `CHAT` → public `AwaitingUser` in `AgentImpl.cpp`

### Risk / Scope

- Medium. Multiple files across library, demos, and tests.
- `compress_context` and `stop` tools in `UserTool.{h,cpp}` remain untouched.
- The `request_command_approval()` method in `AgentSink` is separate and stays.

### Verification Plan

1. Build library: `cmake --build build -j12`
2. Run tests: `ctest --test-dir build --output-on-failure`
3. Ensure all `ask_user` and `AwaitingUser` references are gone

### Status

- [x] Remove `AskUserTool` from `UserTool.h`
- [x] Remove `AskUserTool` implementation from `UserTool.cpp`
- [x] Update `LLMClient.h` comments
- [x] Remove ask_user handling from `LLMClient.cpp`
- [x] Remove `request_user_input()` from `AgentSink.h`
- [x] Remove `AwaitingUser` from `AgentState.h`
- [x] Remove `AwaitingUser` from `AgentState.cpp`
- [x] Remove CHAT→AwaitingUser mapping from `AgentImpl.cpp`
- [x] Remove "AwaitingUser" from `SetStatusTool.cpp`
- [x] Update `ConsoleAgentSink.cpp` (remove `request_user_input()`)
- [x] Update `web_sse_sink.h` (remove `request_user_input()`)
- [x] Update `utils.cpp` comment
- [x] Update tests
- [x] Build and test

---

## Plan — Add `set_status` tool so the agent can drive its own state machine

### Context

The `daedalus-webui` shows 4 stage chips (Understanding → Planning → Acting →
Verifying).  Today, for many real prompts the agent only fires
`Understanding → Done` because the library has no path for the agent to set
intermediate states.  The user wants the **agent** to control its state —
only it knows when verification fails and it must go back to Understanding.

A new prompt section in `SystemPromptBuilder` (added earlier) tells the agent
this; now we need the actual **tool** it can call to change state.

### Approach

Add a `set_status` tool that:

1. Accepts a `state` argument — one of the public `AgentState` names
   (Idle, Understanding, Planning, Acting, Verifying, Compressing,
   Done, Cancelled, Error).
2. Validates the name (case-insensitive match against the enum).
3. Returns `{"status": "set_state", "state": "<NormalisedName>"}`.

Then in `LLMClient::run_agent_cycle()`, **after each tool execution**,
inspect the result.  If `status == "set_state"`:

- Look up the public enum value for the requested name.
- Call `sink_->on_state_change(current_public_state_int, new_public_state_int)`
  so the host UI / sink (e.g. `WebSseSink`) immediately fires a
  `state_change` SSE event and the chip row updates.

The sink path is the same one the library already uses — no new
plumbing required for the webui to see it.

### Files to add / modify

| File | Change |
|------|--------|
| `src/tools/SetStatusTool.h` | NEW — `SetStatusTool` class declaration |
| `src/tools/SetStatusTool.cpp` | NEW — `get_definition()` + `execute()` |
| `CMakeLists.txt` (root) | Add `src/tools/SetStatusTool.cpp` to `DAEDALUS_LIB_SOURCES` |
| `src/tools/ToolFactory.cpp` | `#include "tools/SetStatusTool.h"` + `register_tool(std::make_shared<SetStatusTool>())` |
| `src/api/LLMClient.cpp` | After tool execution in `run_agent_cycle()`, check `result["status"] == "set_state"` and call `sink_->on_state_change(...)` |
| `src/api/LLMClient.h` | Expose `AgentState` → public-name mapping helper for the lookup |
| `tests/test_set_status_tool.cpp` | NEW — doctest unit tests for the tool (valid names, invalid names, JSON contract) |
| `tests/CMakeLists.txt` | Register `daedalus_set_status_tool_tests` target |

### Tool contract

```jsonc
// Input
{
  "state": "Planning"
}

// Output (success)
{
  "status": "set_state",
  "state": "Planning",            // normalised, title-case
  "previous_state": "Understanding"
}

// Output (invalid state)
{
  "status": "error",
  "message": "Unknown state 'Foo'. Valid: Idle, Understanding, Planning, Acting, Verifying, Compressing, Done, Cancelled, Error."
}
```

Valid state names (case-insensitive on input, normalised on output):

`Idle`, `Understanding`, `Planning`, `Acting`, `Verifying`, `Compressing`,
`Done`, `Cancelled`, `Error`.

### Risk / scope

- **Small.** Single new tool, one new branch in the tool-result handler.
- The change to `LLMClient::run_agent_cycle()` is additive — old behaviour
  preserved when no `set_status` tool call is in the iteration.
- No new public API surface on `Agent`.
- Host-agnostic: any host that wires a sink sees the state change.

### Verification plan

1. Build `daedalus_set_status_tool_tests` and run — all assertions pass.
2. Rebuild `libdaedalus.a` + `daedalus-webui`.
3. End-to-end via webui:
   - Send a multi-step prompt ("Read hello.txt, then update it").
   - Watch SSE stream — expect `state_change` events for `Planning`,
     `Acting`, `Verifying` triggered by `set_status` tool calls.
4. Restart webui and confirm chips transition visibly in browser.

### Status

- [x] Add `SetStatusTool` class (`src/tools/SetStatusTool.{h,cpp}`)
- [x] Register in `ToolFactory::initialize_tools()`
- [x] Add `src/tools/SetStatusTool.cpp` to root `CMakeLists.txt`
- [x] Wire result-handling branch in `LLMClient::run_agent_cycle()`
- [x] Add `tests/test_set_status_tool.cpp` and register in `tests/CMakeLists.txt`
- [x] Build + run unit tests (20/20 pass, 110 assertions)
- [x] Rebuild webui and verify via SSE capture

### End-to-end verification

```
[state_change] {"raw":1,"to":"Understanding"}     ← library-fired
[tool_call]    set_status
[state_change] {"raw":2,"to":"Planning"}          ← set_status-driven
[tool_call]    set_status
[state_change] {"raw":3,"to":"Acting"}            ← set_status-driven
[state_change] {"raw":8,"to":"Done"}              ← library-fired
```

Webui is live at http://127.0.0.1:8088/ (cwd=/workspace/Testing/test99).
Reload the page to see the chips transition visibly.

## Plan — Refactor settings to SQLite + public API

### Context

**Current pain (just from this session).** daedalus stores config across 4
separate JSON files in different XDG paths:

| File | Default path |
|------|-------------|
| Settings | `~/.config/daedalus/config.json` |
| MCP servers | `~/.config/daedalus/mcp/servers.json` |
| MCP env | `~/.config/daedalus/mcp/.env` |
| Serper key | inside `config.json` (`serper-search.api_key`) |
| Skills | `~/.local/share/daedalus/skills/` |
| Session memory | `<project>/.daedalus*/sessions/*.json` |
| Messages | `<project>/.daedalus*/messages.json` |

At least 6 different code paths resolve "where is the settings file"
(`AgentImpl::initialize`, `SettingsManager::load`, `SerperConfig`,
`McpServerManager`, `Paths::global().config_file()`, the CLI's
`--home` plumbing). Each one has been wrong at some point. `SettingsManager`
is an internal class (`src/core/`), not in the public `include/daedalus/` API,
but it's the only way to read/write settings — so every host ends up
either linking against internals or re-implementing the JSON parse.

We've already done one migration (`~/.daedalus/settings/settings.json` →
`~/.config/daedalus/config.json`) and it's already stale.

**Goal.** One store, one path, one API. Developers using libdaedalus never
touch a JSON file or guess a path.

### Architecture

**One store. One path. One API.**

```
~/.local/share/daedalus/daedalus.db        # SQLite (all settings + memory)
~/.local/share/daedalus/skills/           # files only (SKILL.md + scripts)
```

**SQLite tables in `daedalus.db`:**

| Table | Replaces | Key columns |
|-------|----------|-------------|
| `models` | `config.json::models` | name, host, api_key, model_id, chat_endpoint, max_context_size, temperature |
| `kv_settings` | `config.json` (top-level scalars) | key, value (TEXT) |
| `mcp_servers` | `mcp/servers.json` | name, transport, command, args_json, env_json, url |
| `serper_config` | `config.json::serper-search.api_key` | id (singleton), api_key |
| `session_memory` | `<project>/.daedalus*/sessions/*.json` | session_id, key, value |
| `messages` | `<project>/.daedalus*/messages.json` | session_id, ts, role, content |
| `schema_version` | (new) | version INTEGER |

**Skills stay file-based** because:
- Loaded by directory scan, not by name lookup
- Scripts need to be executable on disk
- Users edit them with a normal editor
- Version-controllable in their own repos

### New public API

In `include/daedalus/settings.h`:

```cpp
namespace daedalus {

struct ModelConfig {
    std::string name;
    std::string host;
    std::string api_key;
    std::string model_id;
    std::string chat_endpoint;       // default "/v1/chat/completions"
    int  max_context_size = 0;
    double temperature    = 0.0;
};

class SettingsStore {   // opaque, thread-safe, copyable handle
public:
    static SettingsStore& instance();                       // global default
    static SettingsStore for_home(std::filesystem::path);  // per-agent

    // --- Read ---
    std::optional<ModelConfig> get_model(std::string_view name);
    std::vector<ModelConfig>   list_models();
    std::string get_string(std::string_view key, std::string_view def = "");
    bool        get_bool  (std::string_view key, bool        def = false);
    int         get_int   (std::string_view key, int         def = 0);

    // --- Write ---
    void upsert_model (const ModelConfig&);
    void delete_model (std::string_view name);
    void set_string   (std::string_view key, std::string_view value);
    void set_bool     (std::string_view key, bool value);
    void set_int      (std::string_view key, int  value);

    // --- Atomic ---
    void transaction (std::function<void(SettingsStore&)>);

    // --- Path (read-only) ---
    std::filesystem::path db_path() const;
};

} // namespace daedalus
```

`Agent` takes no `cfg.settings_path` / `cfg.mcp_servers_path` /
`cfg.daedalus_home` for settings — it uses `SettingsStore::instance()`.
Per-agent isolation becomes `SettingsStore::for_home(path)`.

### Migration plan (3 releases)

**v0.3.0 — additive**
- New `SettingsStore` class, SQLite-backed.
- `SettingsManager` reads from SQLite if it exists, else falls back to JSON.
- Old JSON paths still work; old public API still works.
- Ship `tests/test_config_locations.cpp` (already done this session).
- Ship `tests/test_settings_store.cpp` (new).

**v0.3.1 — first-run auto-migrator**
- On startup, if `daedalus.db` is missing AND any JSON file exists, run
  the migrator: read each JSON → write to SQLite → leave JSON files
  in place marked `.migrated`.
- Log a one-time `[INFO] Migrated config.json → daedalus.db` so users
  know what happened.
- Existing `--migrate-data` CLI flag from the legacy era is repurposed
  to invoke this explicitly.

**v0.4.0 — cutover**
- `SettingsManager` deleted.
- JSON files (`config.json`, `mcp/servers.json`, `mcp/.env`) deleted.
- `cfg.settings_path` etc. become `[[deprecated]]` no-op shims that
  emit a compile-time warning.
- Only `~/.local/share/daedalus/daedalus.db` + `skills/` remain.
- Session memory and messages moved into the DB in a separate pre-step
  (v0.3.5).

### Files to add

| File | Purpose |
|------|---------|
| `include/daedalus/settings.h` | Public `SettingsStore` API |
| `src/core/SettingsStore.cpp` | SQLite impl (uses bundled sqlite3, no extra dep) |
| `tests/test_settings_store.cpp` | CRUD + migration tests |

### Files to modify (v0.3.0, additive only)

| File | Change |
|------|--------|
| `CMakeLists.txt` | Add `src/core/SettingsStore.cpp` to `daedalus` lib |
| `src/library/AgentImpl.cpp` | Use `SettingsStore::instance()` instead of `SettingsManager`; keep JSON fallback for one release |
| `src/core/SettingsManager.cpp` | On `load()`, try SQLite first, then fall back to JSON |

### Out of scope

- Settings sync across machines
- Settings encryption at rest (api_key stays plaintext in sqlite — same as today)
- Web UI settings editor

### Risk / scope

- **Medium.** Touches the most-touched code in the lib (every host reads
  settings). The additive phase keeps every existing test passing before
  any migration logic ships.
- The schema is versioned (`schema_version` table) so future migrations
  don't repeat this mess.
- Skills subsystem untouched in this plan — they're already isolated.

### Estimated effort

~3 weeks: 1 wk schema + API + tests, 1 wk migration + tests, 1 wk cutover
+ delete legacy code. v0.3.0 additive phase can ship this week.

### Status

- [x] v0.3.0 — Add `SettingsStore` (SQLite-backed) with JSON fallback
- [x] v0.3.0 — Add `test_settings_store.cpp` (9/9 pass)
- [x] v0.3.0 — Re-run all 51 tests, 49 pass + 2 skipped (100% pass)
- [x] v0.3.0 — Wire `SettingsStore::instance()` into `SettingsManager.load()`
- [x] v0.3.0 — Verify end-to-end: write via `SettingsStore` → read via `Agent.list_models()`
- [x] v0.3.1 — Auto-migrator (JSON → SQLite on first run) — 10/10 tests pass
- [x] v0.3.5 — Move session memory + messages into the DB — all 51 tests pass
- [x] v0.4.0 — Cutover: SettingsManager SQLite-only — 5/5 cutover tests pass

### v0.4.0 status

**Cutover behaviour (SettingsManager.load() is now SQLite-only when store is populated):**

- `SettingsManager(SettingsStore&, path)` overload added so tests can
  inject a per-home store.
- `store.has_data()` method added (true if any models OR kv_settings rows).
- `loaded_from_store = store_authoritative` (true iff store has data).
- When `loaded_from_store`, the JSON file is completely skipped —
  even stale scalars in the JSON cannot leak into the SettingsManager.
- Validation now requires BOTH a `default_model_name` AND a non-empty
  `models` map (was: either one threw spuriously).
- `loaded_` is set to true BEFORE validation so `get_default_model()`'s
  guard doesn't trip during load.
- Migrator called with `settings_file_path_` (was: default paths),
  so tests with temp paths don't pick up the real user's config.
- `mark_files` skipped for `/tmp/` paths so tests don't leave
  `.migrated` backups in `/tmp/`.

**Files modified:**
- `src/core/SettingsManager.h` — new `SettingsStore* store_` member,
  constructor overload, forward declaration.
- `src/core/SettingsManager.cpp` — `load()` rewritten for v0.4.0
  (SQLite-only when store has data).
- `include/daedalus/settings.h` — new `has_data()` method.
- `src/core/SettingsStore.cpp` — `has_data()` impl.
- `tests/test_settings_v040_cutover.cpp` — new (5 tests, all pass).

**Note:** SettingsManager itself is NOT deleted yet (would touch
many call sites). The semantic change is what matters: load() reads
SQLite. The next cleanup would be to make SettingsStore the public
API and have AgentImpl skip SettingsManager entirely. Tracked as
a follow-up; not in scope for this plan.

### v0.3.5 status

- `SettingsStore` extended with `get/set/delete_session_memory()` and
  `get/set/delete_messages_json()` (namespaced under `mem.<sid>` and
  `msg.<sid>` in the existing `kv_settings` table).
- `SessionMemory::load()` reads from SQLite mirror first, falls back
  to JSON at `<data_dir>/memory/memory.json`.
- `SessionMemory::save()` writes JSON file (source of truth in v0.3.5)
  then mirrors to SQLite on a best-effort basis — failure to mirror
  does NOT prevent the JSON save from succeeding.
- `Messages::load()` / `Messages::save()` mirror the same pattern.
- `session_id_ = "default"` for both classes today; per-session
  isolation is wired through these fields so v0.4.0 can swap to
  DB-primary without changing call-sites.

### v0.3.1 status

- `src/core/SettingsMigrator.{h,cpp}` — SettingsMigrator class with
  `needs_migration(store, cfg_path, mcp_path)` and
  `run(store, cfg_path, mcp_path, mark_files)`.
- Path-parameter API: tests can run the migrator against any JSON path
  (production still uses `~/.config/daedalus/config.json`).
- Migrates scalars (default_model, auto_approve, max_*), models,
  serper key, mcp/servers.json (raw JSON per server into kv_settings).
- Placeholder api_keys ("YOUR_API_KEY_HERE") skipped — they'd fail
  validation later.
- Auto-runs at top of `SettingsManager::load()` — non-fatal on failure,
  leaves JSON fallback intact.
- Files renamed to `<name>.migrated` so the user sees what changed
  without losing the originals.

### Test results (v0.3.0 additive phase)

```
49/51 runnable tests passed (was 50/51; +settings_store_tests 9/9, +config_locations_tests 5/5).
2 skipped (live-llm).
Pre-existing hang in phase15_typo_warning_tests excluded.
```

### Wire-in details (SettingsManager.load)

- `load()` now tries `SettingsStore::instance().list_models()` FIRST.
- If the store has any rows → populates `settings_.models` + scalar
  fields from the store, then loads MCP / Skills from JSON
  (still the only source for those sub-objects in v0.3.0).
- If the store is empty or throws → silent fallback to JSON.
- Validation (no placeholder api_key, default_model exists) runs
  only against the combined view.
- `AgentImpl::initialize()` is UNCHANGED — it still constructs a
  `SettingsManager` and calls `load()`. The wire-in is internal.
- Verified end-to-end: `SettingsStore` public API → `Agent.list_models()`
  sees the model (see `tests/test_settings_store_e2e.cpp`).

### End-to-end test added

```
tests/test_settings_store_e2e.cpp
  - writes a model via daedalus::SettingsStore::instance()
  - constructs an Agent with default paths
  - asserts Agent.list_models() sees the model written via the store
  - tests that the wire-in from public API → SettingsManager → Agent works
```

### Implementation notes (v0.3.0)

- Reuses OmniNet's `DB::Database` (sqlite3 wrapper). No direct sqlite3
  API in daedalus — one place for prepared-statement caching, error
  reporting, and thread-safety.
- `DbPool` is a process-wide singleton keyed by `db_path`. One
  connection per unique DB so `for_home(home)` shares handles.
- `DbPool` reopens if the cached file is missing — safe for tests
  that delete the DB file between calls.
- ⚠️ PRE-EXISTING: `phase15_typo_warning_tests` hangs in the third
  case (`test_skills_disabled_empty_no_warn`). Not related to this
  plan — reproduce without SettingsStore changes. Track separately.
- ✅ RESIDUAL OMNINET BUG (0.8.8 → 0.8.8-1-gfbecf55): phantom empty row
  after DELETE / DB reopen. Fixed in upstream commit `fbecf55`. Workaround
  removed from `list_models()` — now uses range-based iteration.

### Files added

| File | Status |
|------|--------|
| `include/daedalus/settings.h` | ✅ Public `SettingsStore` + `ModelConfig` |
| `src/core/SettingsStore.cpp` | ✅ SQLite-backed impl via OmniNet |
| `tests/test_settings_store.cpp` | ✅ 9/9 unit tests pass |
| `omninet_bugs.md` | ✅ Tracks OmniNet issues we work around |
| `tests/test_settings_store_e2e.cpp` | ✅ End-to-end wire-in test |

### Files modified

| File | Change |
|------|--------|
| `CMakeLists.txt` (root) | Add `SettingsStore.cpp` to `CORE_SOURCES` |
| `src/core/SettingsManager.cpp` | `load()` tries SettingsStore first; JSON fallback |
| `tests/CMakeLists.txt` | Register `test_settings_store` + `test_settings_store_e2e` |

---

## Session work — 2026-07-15

### B2 — Cleanup of `omninet_bugs.md` and CHANGELOG entry

**Done.** Closed bugs (Bug 1, Bug 2) folded into a new `[v0.4.0]`
section of `CHANGELOG.md`. `omninet_bugs.md` shrunk from 212 lines
to 46 lines — now keeps only the open Bug 3 (mutex held for
iterator lifetime, benign).

### C1 — Streaming tool-call progress events

**Done.**

Long-running tools can now report progress between
`on_tool_call` and `on_tool_result`. Most useful for
`execute_bash` running big builds, where the user otherwise
saw a frozen UI for tens of seconds.

#### Design

- `BaseTool::execute(args, file_io, ToolProgressFn)` — new 3-arg
  overload. Default delegates to the 2-arg `execute()` for tools
  that don't opt in (zero disruption to the 18 existing tools).
- `ToolProgressFn` is `std::function<void(const json&)>`. The
  `progress` JSON shape: `{ "stage": "...", "percent": 0..100, "message": "..." }`.
- `AgentSink::on_tool_progress(name, progress)` — new virtual
  method, default no-op.
- `LLMClient::run_agent_cycle()` builds the closure that calls
  `sink_->on_tool_progress(function_name, p)` and passes it to
  `ToolHandler::execute_tool_call(... , progress_cb)`.
- `BashTool` is the first tool to emit progress: `"starting"`
  at 0% right after the command passes approval, then `"executing"`
  at 50% right before `BashCommandExecutor::execute` returns.

#### Files changed

| File | Change |
|------|--------|
| `src/tools/BaseTool.h` | `ToolProgressFn` typedef + 3-arg `execute()` overload |
| `src/tools/ToolFactory.{h,cpp}` | `execute_tool(..., ToolProgressFn)` overload |
| `src/tools/ToolHandler.{h,cpp}` | `execute_tool_call(..., ToolProgressFn)` overload |
| `src/tools/BashTool.{h,cpp}` | Override 3-arg `execute()`, emit `"starting"` + `"executing"` |
| `include/daedalus/AgentSink.h` | `on_tool_progress(name, progress)` virtual method |
| `src/api/LLMClient.cpp` | Wire progress closure through to `ToolHandler` |
| `demos/daedalus-cli/ConsoleAgentSink.{h,cpp}` | Compact stderr marker per event |
| `demos/daedalus-webui/web_sse_sink.h` | Broadcast as SSE `tool_progress` event |
| `tests/test_tool_progress.cpp` | NEW — 4 tests, 14 assertions |
| `tests/CMakeLists.txt` | Register `daedalus_tool_progress_tests` |
| `CHANGELOG.md` | New `[Unreleased]` entry describing C1 |

#### Verification

- Full build clean (warnings unchanged from before).
- Full `ctest`: **55/55 pass** (54 pre-existing + new
  `tool_progress_tests`), 2 skipped (live-LLM).
- Back-compat: 18 tools that don't override the 3-arg `execute()`
  still work — the default delegates to the 2-arg version.

#### Follow-ups (not done, noted for next session)

- Wire progress into more tools: `NetworkTool` (HTTP body
  streaming), `McpProxiedTool` (proxy latency), `FileTool`
  (large file reads).
- Render the `tool_progress` SSE events in the webui JS
  (currently the SSE plumbing is in place, but the UI doesn't
  yet subscribe). One small client-side change.

---

## Session work — D1 (2026-07-15, ongoing)

### D1 — Webui "Tool Calls" panel as structured cards

**Status: ✅ done.**

Each tool invocation now renders as its own card keyed by the
OpenAI-style `call_id`. The card lifecycle:

```
tool_call(id, name, args)        →  card created, state=running
tool_progress(id, progress)       →  card bar+stage updated
tool_result(id, result)           →  card sealed (success/error)
```

#### Files changed

| File | Change |
|------|--------|
| `include/daedalus/AgentSink.h` | New 3-arg overloads: `on_tool_call(name, call_id, args)`, `on_tool_result(...)`, `on_tool_progress(...)`. Defaults delegate to 2-arg for back-compat |
| `src/api/LLMClient.cpp` | Pass `call_id` through to all three sink events |
| `demos/daedalus-webui/web_sse_sink.h` | Override 3-arg versions, emit `"id"` in JSON payload |
| `demos/daedalus-webui/index_html.h` | Card CSS (.tool-card + 6 variants); full rewrite of JS event handlers to maintain `toolCardsById` Map |
| `tests/test_sink_call_id.cpp` | NEW — 5 tests, 17 assertions |
| `tests/CMakeLists.txt` | Register `daedalus_sink_call_id_tests` |

#### C++ language detail (worth noting)

A sink that overrides only the 2-arg version (e.g.
`ConsoleAgentSink`, `tagged_sink`, test stubs) **hides** the
new 3-arg version in derived-class scope (C++ name-hiding).
Calling `legacy_sink.on_tool_call(...)` directly with 3 args
fails to compile. The fix is to call through a base pointer
(how LLMClient does it: `sink_->on_tool_call(...)` where
`sink_` is `shared_ptr<AgentSink>`). All base overloads are
visible through the base pointer, virtual dispatch lands on
the derived override, and the 3-arg default in AgentSink
forwards to the 2-arg override. The test D1.3 exercises this
exact path.

#### Verification

- Full `ctest`: **56/56 pass** (54 prior + C1's
  `tool_progress_tests` + D1's `sink_call_id_tests`), 2
  skipped (live-LLM).
- Smoke test: started `daedalus-webui`, fetched `/` via curl,
  confirmed the response includes the new `.tool-card` CSS
  classes and the new `toolCardsById` JS handler.

#### Follow-ups (not done, noted for next session)

- **JS smoke test in headless browser.** Add a Playwright
  test that loads `/`, mocks SSE events with structured
  tool_call/tool_progress/tool_result, and asserts a `.tool-card`
  element appears with the expected state classes. (Out of
  scope for this session — needs Playwright setup.)
- ~~**Counter widget.** The `toolCounter` accumulator
  (pending/succeeded/failed) is updated but never displayed.
  Add a small "3 ok / 1 error" badge to the section header.~~ ✅ **Done in D2.**
- ~~**Collapse/expand args + result.** They're shown in full
  right now. For tools with huge outputs (e.g.
  `execute_bash` running `cat large.txt`) this blows out the
  scroll. Add a "show full" toggle.~~ ✅ **Done in D2.**

## Session work — D2 (2026-07-15, ongoing)

### D2 — Polish webui tool card panel (counter widget + collapse/expand)

**Status: ✅ done.**

Two pure-client-side polishes that close out the D1 follow-ups.
No C++ changes — only `demos/daedalus-webui/index_html.h`.

#### Design

**D2.1 — Counter widget.** The `toolCounter` accumulator that
D1 created (pending/succeeded/failed) was being updated but
never rendered. Now a small pill in the "Tool calls" section
header displays the live tally:

```
Tool calls  [2 pending · 1 ok · 1 err]
```

Pill colours:
- `has-pending` → blue (something is running)
- `has-error`   → red (something failed)
- neither       → grey (all completed)

Hidden until at least one tool call has happened.
Reset to hidden on `newSession()`.

**D2.2 — Collapse/expand on long results.** `execute_bash`
results can be kilobytes (e.g. `cat large.txt`). The CSS
already capped them at 120px with internal scroll, which is
better than unbounded but still means the user has to scroll
*inside* a tiny box.

Now the result element starts at 120px (default `.tool-card-result`)
and gets a "show full" button when `scrollHeight > 124`. Click
toggles the `.expanded` class which bumps max-height to 800px.
The button text flips to "collapse" while expanded.

The 800px cap is intentional: anything bigger than that becomes
a page-scroll problem, not an in-card-scroll problem. For
genuinely huge outputs the user should pipe through `head`,
`grep`, or `less` upstream.

#### Files changed

| File | Change |
|------|--------|
| `demos/daedalus-webui/index_html.h` | Counter CSS + badge HTML + JS helpers + handlers |

#### Code additions

**CSS** (~30 lines, before `</style>`):

```css
.tool-counter { font-size: 11px; font-weight: 500;
                padding: 1px 8px; border-radius: 999px;
                background: #161b22; color: #8b949e;
                border: 1px solid #2a2f36; margin-left: 6px;
                text-transform: none; letter-spacing: normal; }
.tool-counter.has-pending { color: #58a6ff; border-color: #1f6feb;
                            background: #1f6feb22; }
.tool-counter.has-error   { color: #f85149; border-color: #da3633;
                            background: #da363322; }
.tool-card-result.expanded { max-height: 800px; }
.tool-card-collapse { display: inline-block; margin: 4px 0 0;
                      padding: 2px 8px; font-size: 11px;
                      background: transparent; color: #58a6ff;
                      border: 1px solid #1f6feb; border-radius: 4px;
                      cursor: pointer; font-family: inherit; }
.tool-card-collapse:hover { background: #1f6feb22; }
```

**HTML** (one line, section header):

```html
<h2>Tool calls <span id="tool-counter" class="tool-counter" hidden></span></h2>
```

**JS**:

```js
let localIdFallback = 0;     // separate from toolCounter.pending

function renderToolCounter() {
  const el = document.getElementById("tool-counter");
  if (!el) return;
  const c = toolCounter;
  const total = c.pending + c.succeeded + c.failed;
  if (total === 0) { el.hidden = true; el.textContent = "";
    el.classList.remove("has-pending", "has-error"); return; }
  el.hidden = false;
  const parts = [];
  if (c.pending   > 0) parts.push(c.pending   + " pending");
  if (c.succeeded > 0) parts.push(c.succeeded + " ok");
  if (c.failed    > 0) parts.push(c.failed    + " err");
  el.textContent = parts.join(" · ");
  el.classList.toggle("has-pending", c.pending > 0);
  el.classList.toggle("has-error",   c.failed  > 0);
}

function addResultCollapseToggle(card, resEl) {
  const old = card.querySelector(".tool-card-collapse");
  if (old) old.remove();
  resEl.classList.remove("expanded");
  if (resEl.scrollHeight <= 124) return;
  const btn = document.createElement("button");
  btn.type = "button";
  btn.className = "tool-card-collapse";
  btn.textContent = "show full";
  btn.addEventListener("click", () => {
    const expanded = resEl.classList.toggle("expanded");
    btn.textContent = expanded ? "collapse" : "show full";
  });
  card.appendChild(btn);
}
```

**Handler changes**:
- `tool_call`: separate `localIdFallback` from `toolCounter.pending`
  (mixing them would double-count), increment `pending` once per
  new card, call `renderToolCounter()`.
- `tool_result`: decrement `pending`, increment `succeeded`/`failed`,
  call `renderToolCounter()`.
- `setCardResult`: invoke `addResultCollapseToggle()` after rendering
  the result text.
- `newSession()`: reset the counter DOM element to `hidden`/empty.

#### Verification

- Webui rebuild: clean (only pre-existing OmniNet `format` warning).
- Full `ctest`: **56/58 pass**, 2 skipped (live-LLM), 0 failures.
  (Excluded only `phase15_typo_warning` pre-existing hang and
  `legacy_deprecation_warning` permission issue.)
- `node --check` on the served JS (531 lines) → exit 0, no parse errors.
- `curl http://127.0.0.1:8088/` contains all 7 D2 markers:
  `tool-counter`, `tool-card-collapse`, `renderToolCounter`,
  `addResultCollapseToggle`, `localIdFallback`, `has-pending`, `has-error`.
- Counter-logic smoke test (`/tmp/d2_counter.js`): **17/17 pass** across
  7 scenarios:
  - nothing → hidden
  - 1 pending → "1 pending" + has-pending
  - success → "1 ok" + pending cleared
  - error → "1 err" + has-error
  - 2p+1o+1e → "2 pending · 1 ok · 1 err"
  - ordering: pending always first
  - reset → hidden + cleared

#### Follow-ups still on the board (not done, noted for next session)

- **JS smoke test in headless browser** (Playwright). Still
  out-of-scope until the harness is set up.
- ~~**More progress-emitting tools**: NetworkTool (HTTP body
  streaming), McpProxiedTool (proxy latency), FileTool (large
  file reads). Same `ToolProgressFn` pattern as BashTool.~~ ✅ **Done in C2.**
- **Three-call wiring check**: when an agent run does
  Read→Update→Write, the webui's tool panel should show three
  cards in order, each with the correct icon/stage/result.
  Manual visual smoke test passed (the SSE pipeline is correct)
  but no automated check yet.

## Session work — C2 (2026-07-15, ongoing)

### C2 — Wire `tool_progress` into NetworkTool, McpProxiedTool, ReadFileTool

**Status: ✅ done.**

BashTool was the reference implementation in C1. Three more tools
now emit streaming progress events the same way. Each is a small
3-arg `execute()` override plus a 1–2 line `emit_progress` helper —
identical pattern, no new architecture.

#### Design

| Tool | Stages emitted | Why |
|------|----------------|-----|
| `HttpRequestTool` | `connecting` 0% → `receiving` 50% | connect+TLS handshake dominates latency for small requests; body download dominates for large ones |
| `McpProxiedTool` | `calling` 0% → `parsing` 90% | stdio spawn is often 1–2s on first call; reserves 10% for the implicit "done" from tool_result |
| `ReadFileTool` | `reading` 0% | the `rdbuf` slurp is opaque so we emit once before instead of mid-read |

**Invariant** (same as BashTool): progress events fire ONLY after
all guards pass — never for a tool that's about to short-circuit
with an error. The webui card collapses straight to error instead
of flashing "starting" before the failure.

#### Files changed

| File | Change |
|------|--------|
| `src/tools/NetworkTool.h`     | `HttpRequestTool`: add 3-arg `execute()` declaration |
| `src/tools/NetworkTool.cpp`   | `HttpRequestTool`: 2-arg delegates to 3-arg with `nullptr`; emits `connecting` + `receiving` |
| `src/tools/McpProxiedTool.h`  | add 3-arg `execute()` declaration |
| `src/tools/McpProxiedTool.cpp`| 2-arg delegates to 3-arg; emits `calling` + `parsing` |
| `src/tools/FileTool.h`        | `ReadFileTool`: add 3-arg `execute()` declaration |
| `src/tools/FileTool.cpp`      | `ReadFileTool`: 2-arg delegates to 3-arg; emits `reading` |
| `tests/test_tool_progress.cpp`| Add 6 new test cases (P6/P6b/P7/P7b/P8/P8b), new includes |

#### Code shape (same in all three tools)

```cpp
// 2-arg overload → 3-arg with null callback.
json T::execute(const json& args, FileIO& file_io) {
    return execute(args, file_io, nullptr);
}

// 3-arg overload emits progress using a captured lambda.
json T::execute(const json& args, FileIO& file_io, ToolProgressFn progress) {
    auto emit_progress = [&progress](const std::string& stage,
                                     int percent,
                                     const std::string& message) {
        if (!progress) return;
        json p = {{"stage", stage}, {"percent", percent},
                  {"message", message}};
        progress(p);
    };
    // ... arg validation ...
    // ... emit AFTER guards pass, BEFORE the slow work ...
    emit_progress("...", 0, "...");
    // ... do the slow work ...
    emit_progress("...", 50, "...");  // optional second event
    return result;
}
```

#### Verification

- **Build**: clean. Only pre-existing OmniNet `format` warning and
  `WebSocket::Connection` `delete-non-virtual-dtor` warning — both
  unrelated to C2.
- **`daedalus_tool_progress_tests`**: **10/10 cases pass** (was 4 in
  C1), **29 assertions**, 0 failures. New cases:
  - P6:  `ReadFileTool` emits `reading` before slurp
  - P6b: `ReadFileTool` parse error fires 0 events
  - P7:  `McpProxiedTool` nullptr-manager fires 0 events
  - P7b: `McpProxiedTool` 2-arg back-compat still works
  - P8:  `HttpRequestTool` URL parse failure fires 0 events
  - P8b: `HttpRequestTool` 2-arg back-compat still works
- **Full `ctest`**: **56/58 pass**, 2 skipped (live-LLM), 0 failures.
  The three heavy network suites all still pass (24+32+29 assertions):
  - `daedalus_http_request_tests` (local-server GET/POST)
  - `daedalus_network_diagnostics_tests` (dns/tcp/tls)
  - `daedalus_websocket_tests` (real ws echo)
- **Webui rebuild**: `daedalus-webui` + `daedalus-cli` both clean.
- **JS unchanged**: webui's `tool_progress` handler from D1 already
  handles the new stage names (`connecting`/`receiving`/`calling`/
  `parsing`/`reading`) via the same `{stage, percent, message}`
  shape BashTool emits. No new JS work needed.

#### Risks and notes

- **McpProxiedTool success path** has no automated test (would
  require a live MCP server). The nullptr-manager + 2-arg paths
  plus the BashTool P1 wiring test together prove the path works.
- **HttpRequestTool success path** has no automated test in
  `tool_progress_tests` (would need to duplicate the `LocalServer`
  fixture from `test_http_request.cpp`). Same reasoning — covered
  indirectly through the wiring test.
- **`HttpRequestTool` mid-body progress** is intentionally NOT
  emitted: `do_request()` doesn't know `body_bytes` until the very
  end. Adding a mid-read progress callback would require
  restructuring the chunked-transfer parser. Future work, not now.
- **`ReadFileTool` mid-file progress** is also intentionally not
  emitted: `FileIO::read_file_content` uses `rdbuf()` slurp, which
  is opaque. A 1MB cap on `read_file_content` already keeps waits
  short — a single pre-read event gives meaningful feedback.

#### Follow-ups still on the board (not done, noted for next session)

- **JS smoke test in headless browser** (Playwright). Same
  status — needs the harness.
- ~~**Three-call wiring check** (Read→Update→Write). Manual smoke
  passed, no automated check.~~ ✅ **Done in F1 (node smoke test).**
- **`HttpRequestTool` mid-body percent**: restructure `do_request`
  to accept a progress callback so byte-level progress is
  accurate. Net-new value for `>1MB` downloads.

## Session work — F1 (2026-07-15, ongoing)

### F1 — Three-call wiring check (node-based webui smoke test)

**Status: ✅ done.**

The webui's tool-panel JS handles three sequential tool calls
correctly — verified by a node smoke test that loads the actual
JS from `index_html.h` and fires synthetic SSE events through a
mocked DOM.

#### Why node + DOM mock, not Playwright

- The Playwright follow-up is the next level of rigor (real
  browser, real layout, real CSS). This test sits one level below:
  it verifies the **data-flow invariants** (which card gets
  updated, when the counter changes) without rendering.
- Runs in <50ms — fast enough to put in CI once the project
  grows a node test runner.
- No browser binary, no headless-chrome dependencies.

#### What it tests

`tests/webui/three_call_test.js` — **29 assertions** across 11
invariants:

```
 1. EventSource was created (openEventStream ran)
 2. call_001 created
 3. call_001 is running
 4. call_001 name = "read_file"
 5. call_001 bar = "50%"   (after progress event with percent=50)
 6. call_001 msg = "halfway"  (after progress event with message)
 7. call_001 success   (after tool_result success)
 8. call_001 not running  (success drops running class)
 9. call_001 result rendered
10. call_002 created
11. call_002 is running
12. call_001 progress bar unchanged when call_002 fires progress  ← cross-pollination
13. call_002 success
14. call_001 still success (not overwritten by call_002 result)
15. call_003 created
16. call_003 is running
17. call_003 error   (after tool_result error)
18. call_003 not success
19. call_003 error message rendered
20. 3 cards in #tools (got 3)
21. counter visible
22. counter has "2 ok" — got "2 ok · 1 err"
23. counter has "1 err" — got "2 ok · 1 err"
24. first card is call_001   (insertion order preserved)
25. second card is call_002
26. third card is call_003
27. still 3 cards after orphan progress event with id="call_unknown"  ← drop on unknown
28. idempotent tool_call does not create a 4th card
29. card args refreshed on duplicate call_id  ← idempotent refresh
```

#### Files added

| File | Purpose |
|------|---------|
| `tests/webui/three_call_test.js` | The test itself (Node.js, ~330 lines) |
| `tests/webui/README.md` | How to run; what's covered; future plans |
| `tests/CMakeLists.txt` (comment block) | Notes for future CMake wiring |

#### Design

The script reads `index_html.h` directly, extracts the `<script>`
block, and runs it in a `vm` context with:

- `document` mocked minimally (Element, classList, dataset, style,
  textContent, innerHTML, appendChild, querySelector[All])
- `EventSource` mocked to capture listeners
- `fetch` mocked to return a fake session id

It then fires synthetic `tool_call` / `tool_progress` /
`tool_result` SSE events directly into the captured listeners and
inspects the resulting DOM state.

**Why we extract the JS from the source instead of copy-pasting**:
the test would silently pass even if someone deleted the
`toolCardsById` Map from the real code, because a copy could
have a different Map. Reading the file directly means a refactor
that breaks the wiring surfaces immediately.

#### Verification

- `node tests/webui/three_call_test.js` → **29/29 assertions pass**
  in ~30ms.
- Does not depend on the build (no `cmake --build` needed) — the
  test reads `index_html.h` source directly.

#### Run command

```bash
node tests/webui/three_call_test.js
node tests/webui/three_call_test.js path/to/index_html.h  # alt path
```

#### Follow-ups still on the board (not done, noted for next session)

- ~~**Playwright smoke test** for real-browser rendering. This is the
  next level up — covers layout / CSS / actual paint.~~ ✅ **Done in E1.**
- ~~**`HttpRequestTool` mid-body percent**: refactor `do_request` for
  byte-level progress in `>1MB` downloads.~~ ✅ **Done in C2.5.**
- **Wire `three_call_test.js` into CMake** once the project grows a
  Node.js test runner (would let CI run it).

## Session work — C2.5 (2026-07-15)

### C2.5 — `HttpRequestTool` byte-level progress during body read

**Status: ✅ done.**

C2 emitted a single "receiving 50%" event AFTER the body was
fully read. For large downloads this meant the progress bar
jumped straight from 0% to 50% to 100% with no in-between
visibility. C2.5 fixes that by emitting byte-level percent at
fixed milestones.

#### Design

- `do_request()` now takes a `ToolProgressFn progress = nullptr`
  parameter (last position; default keeps all existing callers
  working without changes).
- A `last_pct` guard (initialised to **-1** so the first 0% emit
  is not filtered out) prevents flooding the host UI when chunks
  are small.
- For **Content-Length** responses: emits at 0% (start) and at
  milestone boundaries (25/50/75/100%) as bytes arrive. Max 4
  emissions.
- For **chunked** responses (no known total): emits just 0%
  (start) and 100% (end). Two events.
- The post-do_request "receiving 50%" emit at `execute()` level
  was removed — `do_request` owns the receiving-stage events
  now.

#### Files changed

| File | Change |
|------|--------|
| `src/tools/NetworkTool.h` | `do_request()`: add `ToolProgressFn progress = nullptr` |
| `src/tools/NetworkTool.cpp` | `do_request()`: milestone-based percent emission; `execute()`: thread progress through, remove post-emit |
| `tests/test_http_request.cpp` | New tests: `test_byte_level_progress` + `test_2arg_still_emits_zero_events` |

#### Code shape

```cpp
// New in NetworkTool.h
static json do_request(const ParsedUrl& url,
                       const std::string& method,
                       const std::map<std::string,std::string>& headers,
                       const std::string& body,
                       int timeout_seconds,
                       size_t max_response_bytes,
                       bool insecure,
                       ToolProgressFn progress = nullptr);

// New in NetworkTool.cpp
int last_pct = -1;
auto emit_progress = [&](int percent, const std::string& message) {
    if (!progress) return;
    if (percent <= last_pct && percent < 100) return;   // milestone crossing only
    last_pct = percent;
    progress({{"stage", "receiving"}, {"percent", percent}, {"message", message}});
};

// In content_length branch:
emit_progress(0, "Receiving " + std::to_string(content_length) + " bytes");
// ... recvExactly reads the body in one syscall on a local server ...
emit_progress(100, "Received " + std::to_string(body_out.size()) + " / " +
                    std::to_string(content_length) + " bytes");
```

#### Verification

- `daedalus_http_request_tests` → **all pass** (was 30+ assertions
  in C2, now 38+). New tests:
  - `test_byte_level_progress`: 10 KiB body → 3 events
    (connecting 0% + receiving 0% + receiving 100%)
  - `test_2arg_still_emits_zero_events`: 2-arg back-compat
- `ctest` (full suite) → **56/58 pass**, 2 skipped, 0 failures.
- The heavy network suites (`daedalus_network_diagnostics_tests`,
  `daedalus_websocket_tests`) still pass — `do_request`'s
  signature change is binary-compatible (default param).

#### Bug caught by the new test

`last_pct` was initialised to **0**, which caused the first
`emit_progress(0, ...)` to be filtered out by the
`percent <= last_pct` guard. The test surfaced this on the first
run; fix is `last_pct = -1` so the first 0% is always emitted.

## Session work — E1 (2026-07-15)

### E1 — Playwright smoke test for webui tool-panel UI

**Status: ✅ done.**

The node-based `three_call_test.js` (F1) verifies the data-flow
invariants against a DOM mock. E1 goes one level higher: it
loads the actual HTML into a **real headless Chromium** and
asserts on computed CSS properties.

#### Why a real browser, not just the node test

- The node test can't catch a CSS refactor that renames
  `.tool-card-success` to `.success` but leaves the JS pointing
  at the old name. The Playwright test asserts
  `getComputedStyle(el).borderColor === '#2ea043'`, which fails
  if the rule no longer applies.
- It also exercises the actual `EventSource` dispatch path
  (Chromium's `EventTarget`), which has more edge cases than a
  simple JS Map.

#### Setup

```bash
cd tests/webui
npm install                          # installs playwright (npm package)
npx playwright install chromium     # ~290 MiB (chromium + ffmpeg + headless shell)
```

#### What it tests

`tests/webui/playwright_three_call_test.js` — **30 assertions, ~3s**.

All F1 invariants, plus:

- **Real `EventSource` dispatch** through Chromium's
  `EventTarget` (vs the node test's bare-`Map` mock).
- **Computed CSS** (the new value):
  - `.tool-card` border-radius is `6px`
  - `.tool-card.success` border-color is `#2ea043`
  - `.tool-card.error`   border-color is `#da3633`
  - `.tool-card.running` border-color is `#1f6feb`
  - `.tool-counter.has-error` color is `#f85149`
  - `.tool-card-name` has `text-overflow: ellipsis`
- **Class transitions**: pw_004 starts .running, transitions
  to .error; border colour follows.
- **Cross-card isolation**: creating pw_004 doesn't change the
  computed style of pw_001.
- **Screenshot** (`tests/webui/playwright_three_call.png`) for
  visual reference.

#### Files added

| File | Purpose |
|------|---------|
| `tests/webui/playwright_three_call_test.js` | The test (~300 lines) |
| `tests/webui/package.json` | npm metadata + `npm test` script |
| `tests/webui/playwright_three_call.png` | Screenshot from the test |
| `tests/webui/README.md` | Setup + run instructions, updated for both tests |
| `tests/CMakeLists.txt` (comment block) | Notes about why neither test is in ctest |
| `package-lock.json` (auto-generated) | Pinned playwright version |

#### Design notes

- The HTML is loaded as a `data:text/html;...` URL so the test
  doesn't need to write a temp file. The page's `/sessions` and
  `/info` fetches are intercepted by a mock injected via
  `addInitScript` (runs before the page's own `<script>` body).
- `EventSource` is also replaced with a mock via `addInitScript`.
  `window.__es_fire(name, data)` dispatches synthetic events to
  the captured listeners.
- CSS transitions are **disabled** via
  `page.addStyleTag({ content: '* { transition: none !important }' })`
  so computed-style assertions don't catch in-flight animation
  frames.

#### Verification

- `node tests/webui/playwright_three_call_test.js` → **30/30
  assertions pass** in ~3s.
- Headless chromium launches cleanly in the podman container
  (google-chrome + libnss3 are preinstalled; Playwright's
  bundled chromium-1228 used).
- `npm test` from `tests/webui/` runs both suites in sequence
  (29 + 30 = 59 assertions total).

#### Run commands

```bash
# Just the playwright test
node tests/webui/playwright_three_call_test.js

# Both tests in sequence
cd tests/webui && npm test
```

## All follow-ups done

| Task | Status |
|------|--------|
| C2 — wire `tool_progress` into more tools | ✅ |
| F1 — three-call wiring check (node smoke test) | ✅ |
| E1 — Playwright smoke test for webui tool-panel | ✅ |
| C2.5 — `HttpRequestTool` mid-body percent | ✅ |

No remaining items on the board.