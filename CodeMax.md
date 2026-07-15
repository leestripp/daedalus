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
- **Counter widget.** The `toolCounter` accumulator
  (pending/succeeded/failed) is updated but never displayed.
  Add a small "3 ok / 1 error" badge to the section header.
- **Collapse/expand args + result.** They're shown in full
  right now. For tools with huge outputs (e.g.
  `execute_bash` running `cat large.txt`) this blows out the
  scroll. Add a "show full" toggle.