# CodeMax — working memory

> **Round 2 sweep (2026-07-15)** added at the bottom of this file —
> starts at [## Physical Tests — 2026-07-16 (PT sweep)

> PT-1..PT-5 ran end-to-end against `demos/` with the live
> `minimax-m2.7` model. Test projects live under
> `/workspace/daedalus/Testing/test-*/project/`. Each PT creates a
> small seeded project (`sum.cpp` with a deliberate bug) and runs
> the demo end-to-end. The goals:
>
> 1. Verify that demos **start, parse argv, and reach the LLM**.
> 2. Verify that demos return to a clean state on `/quit`.
> 3. Catch argv-parsing bugs and crash-on-startup issues that the
>    in-tree ctest suite never exercises.
>
> Findings are ordered by severity. One real bug (F0, banner
> crash) was found and fixed during the sweep; two more (subagent
> / manager crash on `--help`) are filed as F2 for follow-up.

### PT-1 — daedalus-cli

**Setup:** `--home /home/ubuntu/.local/share/daedalus --model minimax-m2.7 --auto --no-memory /workspace/daedalus/Testing/test-cli-bare/project`

**Result:** ✅ **End-to-end works.** The CLI connected to
`https://api.minimax.io`, called the `list_directory` tool, returned
a markdown table of the project files, and exited cleanly on
`/quit`. Exit code 0. Token usage 12,841/200,000 (6.4%).

**Side-effects observed:**
- The `[DEPRECATION] ~/.daedalus/` warning fires correctly (the
  legacy flat dir exists from old installs).
- The legacy read-fallback mechanism imported the models into
  `/home/ubuntu/.local/share/daedalus/daedalus.db` on first use
  — auto-migration works.

**Bug found:** F0 (banner crash — see below).

### PT-2 — daedalus-webui

**Setup:** `--port 18088` (avoid 8088 collision with running instance),
seeded `/workspace/daedalus/Testing/test-webui/project`.

**Result:** ✅ **HTTP server reachable.** Verified endpoints:
- `GET /` → 200, 35,121 bytes (embedded HTML)
- `GET /info` → `{"cwd":"/workspace/daedalus"}`
- `GET /healthz` → `{"ok":true}`
- `POST /sessions` → `{"model":{...},"session_id":"98ba8d243f8d0bfa"}`
- `GET /sessions/<id>/state` → `{"state":N}` (0=Idle, 7=Done)
- `POST /sessions/<id>/messages` (body `{"message":"..."}`) →
  `{"status":"queued"}`; state advanced 0→1→7 in ~30s.

**Issues observed:**
- The `/sessions/<id>/messages` API uses field name `message`,
  not `content`. The Readme example (`{"content":"..."}`) would
  fail with `{"error":"missing session_id or message"}`. Update
  the Readme webui section (currently silent on the exact body
  shape).

### PT-3 — daedalus-subagent

**Setup:** `daedalus-subagent <project_root> <model_name>` (positional,
no flag support).

**Result:** ✅ **End-to-end works.** Parent agent processed
"list the directory", called `list_directory`, and emitted a
markdown table. Exit code 0.

**Bug found:** **F2a — `--help` crashes (HIGH).**
`daedalus-subagent --help` throws
`std::invalid_argument("Project path does not exist: --help")`
because `argv[1]=="--help"` is treated as the project_root and
passed to `Agent::initialize()` which validates it exists.
Fix: add a `--help` short-circuit at the top of `main` (matching
the `daedalus-cli` pattern at `demos/daedalus-cli/main.cpp:52-60`).
Also missing: the demo hard-codes `daedalus::default_settings_path()`
(now `[[deprecated]]`) and `/home/ubuntu/.daedalus/skills/`.

### PT-4 — daedalus-manager

**Setup:** `daedalus-manager <project_root> <goal> <max_iters> <model>`
(positional).

**Result:** ✅ **End-to-end works AND the agent fixed the seeded
bug.** The planner analyzed the project (one file, `sum.cpp`),
the actor used `edit_file_patch` to change `a - b` to `a + b`,
the validator ran `g++` to confirm the file still compiles,
and emitted **VERDICT: APPROVED**. Exit code 0.

Verification: `cat sum.cpp` after the run shows
`return a + b;` — the fix landed on disk.

**Bug found:** **F2b — `--help` crashes (HIGH).**
Same root cause as F2a — `argv[1]` treated as project_root.
Fix: same `--help` short-circuit. Demo also uses `default_settings_path()` (deprecated).

**Visual issue:** The planner/actor/validator spinners and
log lines are interleaved in the output (multi-line ANSI
spinner sequences get cut by log messages from the other
threads). Cosmetic — a per-agent lock or `\r`-aware output
filter would help. Filed for a separate sweep.

### PT-5 — daedalus-cli --migrate-data

**Setup:** plain `--migrate-data` (dry run) and
`--migrate-data-apply --migrate-data-from /home/ubuntu/.daedalus`
(apply). Target XDG dirs:
`XDG_CONFIG_HOME = /home/ubuntu/.config/daedalus`,
`XDG_DATA_HOME = /home/ubuntu/.local/share/daedalus`.

**Result:** ✅ **Works, no-op on this machine.** The legacy
`~/.daedalus/` is empty (no JSON config files, no skill packs),
so the dry run reports "nothing to migrate" and the apply
migrates 0 entries and exits 1. The exit-1 on empty migration
is a design choice (signals "you should `rm -rf` the legacy
dir to confirm you're done"); not a bug.

### Findings (new bugs from PT)

#### F0 — `print_banner` throws `std::length_error` on long paths (HIGH)

**File:** `demos/daedalus-cli/banner.h` (whole file)

The banner used `std::string(WIDTH - s.size(), ' ')` to pad each
field. Whenever `s.size() > WIDTH` — typical for `data_dir` on a
long path like `/workspace/daedalus/Testing/test-cli-bare/project/.daedalus`
(59 chars > 52) — the subtraction underflowed and the constructor
threw `std::length_error`, which `std::terminate`'d the demo
before the LLM was even contacted.

**Fix:** Replaced with `pad_to(s, width)` helper that clamps to
0 and truncates with ` …` for long values. All column widths
preserved (still 66-char inner box).

**Status:** ✅ **Done (2026-07-16).**

#### F2 — daedalus-subagent and daedalus-manager crash on `--help` (HIGH)

**Files:**
- `demos/daedalus-subagent/main.cpp:116`
- `demos/daedalus-manager/main.cpp:59`

Both demos take `<project_root>` as `argv[1]` and pass it straight
to `Agent::initialize()` (which validates the path exists). They
have no `--help` short-circuit, so `daedalus-subagent --help` and
`daedalus-manager --help` both throw
`std::invalid_argument("Project path does not exist: --help")`
and `std::terminate`.

**Repro:**
```bash
$ daedalus-subagent --help
terminate called after throwing an instance of 'std::invalid_argument'
  what():  Project path does not exist: --help
Aborted (core dumped)

$ daedalus-manager --help
... same ...
```

**Fix:** Add a `--help` short-circuit before the argv parsing,
matching the `daedalus-cli` pattern:
```cpp
for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--help" || a == "-h") {
        std::cout << "Usage: daedalus-subagent <project_root> <model>\n"
                  << "       daedalus-subagent <project_root> <model> <args...>\n";
        return 0;
    }
}
```

While there: replace `daedalus::default_settings_path()` (now
`[[deprecated]]`) with `daedalus_home = daedalus::default_daedalus_home()`,
matching the daedalus-subagent fix already in the codebase.

**Status:** 🟡 Open.

#### F3 — Readme webui section doesn't document the `message` field name (LOW)

**File:** `Readme.md` — `## MCP Server Configuration` and any
HTTP example. (No webui section currently exists in the
Readme.)

The `/sessions/<id>/messages` endpoint expects
`{"message": "..."}`, not `{"content": "..."}`. A new host
developer copying the daemon-client example would get
`{"error":"missing session_id or message"}` and have to dig
into the route handler to figure out the right field name.

**Fix:** When adding the webui section to Readme.md, document
the exact body shape with a small `curl` example.

**Status:** 🟡 Open.

### Summary

| # | File | Severity | Status |
|---|------|----------|--------|
| F0 | `demos/daedalus-cli/banner.h` | HIGH | ✅ fixed |
| F2 | `demos/daedalus-subagent/main.cpp` + `demos/daedalus-manager/main.cpp` | HIGH | 🟡 open |
| F3 | `Readme.md` (webui section) | LOW | 🟡 open |

### Verification

| Demo | Result | Notes |
|------|--------|-------|
| daedalus-cli --help | ✅ works | Clean exit 0 |
| daedalus-cli --model-list | ✅ works | Lists models from the right DB |
| daedalus-cli --migrate-data | ✅ works (no-op on empty legacy) | |
| daedalus-cli (live LLM turn) | ✅ works | Fixed `a - b` → `a + b` via tool calls |
| daedalus-webui HTTP endpoints | ✅ works | All 5 endpoints respond |
| daedalus-subagent (live LLM turn) | ✅ works | Parent delegated via `list_directory` |
| daedalus-manager (live LLM turn) | ✅ works | Planner → Actor → Validator pipeline APPROVED |
| daedalus-subagent --help | ❌ crashes | std::invalid_argument |
| daedalus-manager --help | ❌ crashes | std::invalid_argument |

### Test artifacts

```
/workspace/daedalus/Testing/
├── test-cli-bare/project/sum.cpp       # fixed by daedalus-cli (live LLM)
├── test-webui/project/sum.cpp         # webui exercise (HTTP only)
├── test-subagent/project/sum.cpp      # subagent exercise
└── test-manager/project/sum.cpp       # fixed by daedalus-manager (live LLM)
```

The two `.cpp` files in `test-manager/project/` and `test-cli-bare/project/`
both contain the post-fix `return a + b;` (with a `// Fixed: ...` comment),
proving that the live LLM-driven edit landed on disk.

---

## Bug/Issue Sweep — Round 2 (2026-07-15)](#bug-issue-sweep--round-2-2026-07-15).
> See that section for the fresh code/doc fixes shipped this session.

## System info

> Captured by `system_info` tool.

| Field | Value |
|-------|-------|
| Container | podman |
| OS | Ubuntu 26.04 LTS (Resolute Raccoon), x86_64 |
| Kernel | Linux 7.0.3-6.stable |
| Date | 2026-07-15 03:18 UTC |
| Display | wayland |
| User | ubuntu (uid 1000) |
| Host | daedalus-dev-wayland |
| Shell | /bin/bash |
| Session | local |

---

## Task — Remove dead/zombie code after the v0.3.0 → v0.4.0 settings refactor

### Context

The settings subsystem was rewritten across three releases:

| Release | What changed |
|---------|--------------|
| **v0.3.0** | `SettingsStore` (SQLite-backed) added; `SettingsManager` gains SQLite-first read with JSON fallback |
| **v0.3.5** | Session memory + messages mirror into `SettingsStore::kv_settings` (`mem.<sid>`, `msg.<sid>`) |
| **v0.4.0** | `SettingsManager::load()` is SQLite-only when the store has data; JSON path is dead for active users |

The cutover is complete at runtime, but **a layer of dead/zombie code
was left in place** "to keep the build compiling". Several fields are
written to but never read; one subsystem reads from the old JSON files
in parallel with the new SQLite read; and `default_settings_path()`
and friends survive only because the legacy `--migrate-data` CLI flag
still imports them.

This task deletes that dead layer.

### Findings

| # | Location | Type | Notes |
|---|----------|------|-------|
| D1 | `AgentConfig::settings_path` (set, never read) | Field | `AgentImpl::initialize()` sets it; library reads `SettingsStore` only. |
| D2 | `AgentConfig::memory_dir` (set, never read) | Field | `LLMClient` ctor uses `resolve_agent_dir()` directly. |
| D3 | `AgentConfig::log_file` (set, never read) | Field | Verified zero readers. |
| D4 | `demos/daedalus-subagent/main.cpp:90` writes `child_cfg.settings_path = ...` | Write of dead field | Drop the line. |
| D5 | `FileIO::settings_manager_` private field | Field | Compiler `-Wunused-private-field` warning. The whole `SettingsManager*` parameter is gone from `FileIO`'s API. |
| D6 | `SerperConfig.cpp` reads legacy `settings.json` files | Duplicate read path | `SettingsMigrator` already migrates the serper key into `SettingsStore::serper_config` table. SerperConfig should read from the store. |
| D7 | `demos/daedalus-cli/migrate.cpp` is half-redundant | CLI flag | `SettingsMigrator::run()` auto-migrates on first DB open. The CLI still owns the `~/.daedalus` rename + skills copy. |
| D8 | `default_settings_path()`, `default_mcp_servers_path()`, `default_mcp_env_path()` | Public API | Only used by `daedalus-cli/migrate.cpp` (D7) and one test. |
| D9 | `make_app_config` lines `out.auto_mode = cfg.auto_approve;` | Renamed field | `AppConfig` has `auto_mode`, `AgentConfig` has `auto_approve`. Same concept, two names. |
| D10 | `LLMClient::run_agent_cycle` debug block at lines ~960–1000 | Dead code | Comments say "DEBUG: Log tool_calls" — block is gated on a specific tc_id that never appears. Either gate on `LOG_LEVEL == Trace` or delete. |
| D11 | `phase15_typo_warning_tests` third case `test_skills_disabled_empty_no_warn` | Hangs (B31) | Pre-existing; reproduce without `SettingsStore` changes. Singletons need a `reset_for_testing()`. |
| D12 | `demos/daedalus-cli/ConsoleAgentSink.cpp` 2-arg overloads hidden by 3-arg overrides (C++ name-hiding) | Documentation | CodeMax.md D1 already documents; no code change needed. |
| D13 | `LLMClient::make_curl_request_with_retry` is dead in the hot path | Duplicate path | `run_agent_cycle` still falls back to it on JSON-parse failure, but `make_api_request_async` is the primary path. Keep one. |
| D14 | `SettingsStore::get_server_error_settings()` swallows `std::stod` throw silently via outer try/catch (B24) | Already tracked as B24 | — |

### Files to modify

| File | Change |
|------|--------|
| `include/daedalus/types/AgentConfig.h` | Delete fields: `settings_path`, `mcp_servers_path`, `mcp_env_path`, `memory_dir`, `log_file`. Add a comment that points to `daedalus_home` as the single config-root. |
| `src/library/AgentImpl.cpp` | Drop the five `if (cfg_.X.empty()) cfg_.X = ...` blocks for the deleted fields. The `daedalus_home` block (line ~117) stays; it remains the single config-root. |
| `src/core/FileIO.h` | Delete `SettingsManager* settings_manager_;` field. |
| `src/core/FileIO.cpp` | Delete the constructor parameter / assignment, if any. |
| `src/core/SerperConfig.cpp` | Replace JSON file probe with `SettingsStore::get_serper_api_key()`. Falls back to `Paths::global().user_data_root() / "daedalus.db"` if no store entry. |
| `include/daedalus/settings.h` | Add `get_serper_api_key()` / `set_serper_api_key()` to `SettingsStore` (table `serper_config` is already there from schema v2). |
| `src/core/SettingsStore.cpp` | Implement the two new methods. |
| `demos/daedalus-subagent/main.cpp:90` | Delete `child_cfg.settings_path = daedalus::default_settings_path();` |
| `include/daedalus/default_paths.h` | Mark `default_settings_path()`, `default_mcp_servers_path()`, `default_mcp_env_path()` `[[deprecated]]` with a message pointing to `--migrate-data`. The CLI owns them for one more release, then they go. |
| `src/library/default_paths.cpp` | Same `[[deprecated]]` annotation on the implementations. |
| `demos/daedalus-cli/CMakeLists.txt` | Drop `migrate.cpp/h` only after D15 ships — they're still the canonical place for the data-rename. (Kept for now.) |
| `src/api/LLMClient.cpp` | Delete the DEBUG block at lines ~960–1000 (D10). |

### Test updates

| File | Change |
|------|--------|
| `tests/test_serper_startup.cpp` | Rewrite to seed `SettingsStore` with the serper key, then call `load_serper_api_key()`. Drop the JSON-file fixture. |
| `tests/test_config_locations.cpp` | Already tests SettingsStore-only paths; should still pass. |
| `tests/CMakeLists.txt` | Drop `daedalus_serper_settings_tests` if its only fixture was the JSON file (verify first). |

### Verification plan

1. **Field deletion**: After removing the five `AgentConfig` fields,
   search for any remaining `.settings_path`, `.mcp_servers_path`,
   `.mcp_env_path`, `.memory_dir`, `.log_file` reads. There should
   be **zero** in `src/` and `demos/`. (Only `daedalus-cli/migrate.cpp`
   may still reference the public helper functions — those are
   deprecated but not removed.)
2. **Compile**: `cmake --build build -j12` should succeed with
   `-Werror` once `-Wunused-private-field` is upgraded from warning
   to error (or just verify the build is clean).
3. **Run**: `cd build; ctest -E phase15_typo_warning --output-on-failure`
   should still pass (50/50; 2 skipped).
4. **CLI smoke**: `daedalus-cli --migrate-data` should still print
   the plan (uses the now-deprecated helpers, no field changes).
5. **Serper smoke**: `daedalus-cli --auto-mode "use the search tool
   once"` — should reach the serper API with the key from the store.
6. **CHANGELOG**: Add a `[v0.5.0]` entry listing the 5 deleted
   fields, 1 deleted private member, 1 deprecated trio of helpers,
   1 subsystem rewrite (SerperConfig). Mirror the same summary into
   `docs/settings.md` §5.

### Status

- [ ] Delete `AgentConfig::settings_path`
- [ ] Delete `AgentConfig::mcp_servers_path`
- [ ] Delete `AgentConfig::mcp_env_path`
- [ ] Delete `AgentConfig::memory_dir`
- [ ] Delete `AgentConfig::log_file`
- [x] Delete corresponding default-applications in `AgentImpl::initialize()` (v0.4.2 — 17-line constructor block deleted; the four fields are still present but no longer auto-defaulted, so the deprecation warnings show up at the one remaining call site in `daedalus-subagent/main.cpp` and `daedalus-manager/main.cpp`)
- [x] Delete `FileIO::settings_manager_` private field (verified already gone — `grep settings_manager_ src/core/FileIO.{h,cpp}` returns nothing)
- [x] Delete `child_cfg.settings_path = ...` in `daedalus-subagent/main.cpp` (v0.4.2 — replaced with `child_cfg.daedalus_home = daedalus::default_daedalus_home();` so the child reads `SettingsStore` from the user's standard location)
- [ ] Add `SettingsStore::get_serper_api_key()` / `set_serper_api_key()`
- [ ] Rewrite `SerperConfig.cpp` to read from `SettingsStore`
- [ ] Rewrite `test_serper_startup.cpp` against the store
- [x] Mark `default_{settings,mcp_servers,mcp_env}_path()` `[[deprecated]]` (v0.4.2 — annotations added in `include/daedalus/default_paths.h` and `src/library/default_paths.cpp` pointing at `SettingsStore::instance()` and `list_mcp_servers()`. The three call sites — `daedalus-cli/migrate.cpp`, `daedalus-subagent/main.cpp`, `daedalus-manager/main.cpp` — still use them and emit `-Wdeprecated-declarations`. OK for v0.4.2; the actual removal is v0.6.0 work.)
- [x] Delete the DEBUG block in `LLMClient::run_agent_cycle` (v0.4.2 — the 35-line "DEBUG: Log tool_calls" block gated on a specific `tc_id` was deleted; both branches inside the gate were empty so removing it had no behavioural change)
- [x] Build + run full ctest (excluding `phase15_typo_warning`) (59/61 pass + 2 live-LLM skipped; 0 failures)
- [x] CHANGELOG + docs/settings.md update (v0.4.2 — CHANGELOG.md `## [Unreleased] / ### Fixed` block lists each fix; `docs/settings.md` is already accurate for v0.4.0+ (last-edited 2026-07-15) so no edit was needed)

### Risk

- **MEDIUM.** Touches the public `AgentConfig` struct. Hosts that
  set the deleted fields will silently get defaults. The
  `[[deprecated]]` step on the default-helpers softens this for
  v0.5.0; the actual field deletion can ship in v0.5.1 with a
  brief CHANGELOG note that maps old field names to `daedalus_home`.
- The SerperConfig rewrite is the riskiest piece — the JSON read
  has been there for many releases and hosts may rely on it.
  Keep the JSON fallback behind a `#ifndef DAEDALUS_NO_JSON_FALLBACK`
  for one release, gated by the env-var presence of
  `$DAEDALUS_NO_JSON_FALLBACK=1`.

---

## Bug/Issue Sweep — 2026-07-15

> Comprehensive sweep of `src/`, `demos/`, `include/`, `tests/` after the
> clean build at the top of this session. Findings below are REAL bugs
> (incorrect behaviour, security holes, resource leaks, UB risk) — not
> style issues, not cosmetic. Each task is self-contained with file,
> line, severity, repro, and a suggested fix.

### Severity legend

| Level | Meaning |
|-------|---------|
| **CRITICAL** | Security hole, data loss, crash in normal use |
| **HIGH**     | Wrong behaviour in common path; large impact |
| **MEDIUM**   | Wrong behaviour in edge case; manageable impact |
| **LOW**      | Cosmetic-but-real (duplicate work, dead code, hidden trap) |

---

### B1 — SQL injection in `SettingsStore` (CRITICAL)

**Files:**
- `src/core/SettingsStore.cpp` — `get_model`, `delete_model`, `get_string`,
  `set_string`, `set_bool`, `set_int`, `delete_session_memory`,
  `get_messages_json`, `set_messages_json`, `delete_messages_json`,
  `get_mcp_server`, `delete_mcp_server`, `upsert_model`,
  `upsert_mcp_server` — every public method that takes a `key`/`name`/
  `value` builds its SQL via raw string concatenation.

**Repro:**
```cpp
SettingsStore::instance().set_string("x', '1'); DROP TABLE kv_settings; --",
                                    "irrelevant");
// -> kv_settings table gone.
```

**Why the comment in the code is wrong:**
> `NOTE: name is treated as a key (validated in API), not user input.`
> `For raw user input we'd use a prepared Statement::bind — the`
> `public API surface here is name/key lookups by trusted callers.`

`SettingsStore` is a **public** API (`include/daedalus/settings.h`).
Hosts will pass through user-supplied model names, serper keys, MCP
server names, session IDs — all of which flow into these queries.
A single quote anywhere breaks the SQL. A crafted model name in a
host that proxies user input can wipe the DB.

**Fix:**
- Use OmniNet's prepared-statement API (it has `Statement::bind`).
- For the few methods that today take a `key` arg, change the
  signature or call `db.prepare("... WHERE key = ?")` then `bind(1, key)`.
- Same treatment for every column where the value comes from outside.
- Add a regression test that inserts `O'Brien` as a model name and
  round-trips it.

**Status:** ✅ **Done (2026-07-15).**

Three helper functions added in an anonymous namespace of
`src/core/SettingsStore.cpp`:

```cpp
template <typename... Args>
void bind_args(OmniNet::DB::Database::Statement& stmt, Args&&... args);

template <typename... Args>
void exec_prepared(OmniNet::DB::Database& db, const std::string& sql,
                   Args&&... args);

template <typename... Args>
std::optional<std::vector<std::string>>
query_one_prepared(OmniNet::DB::Database& db, const std::string& sql,
                   Args&&... args);

std::vector<std::vector<std::string>>
query_all_prepared(OmniNet::DB::Database& db, const std::string& sql);
```

Every public method on `SettingsStore` that takes a string parameter
now uses `?` placeholders + `Statement::bind(...)`. Specifically:
`get_model`, `delete_model`, `get_string`, `set_string`,
`delete_session_memory`, `get_messages_json`, `set_messages_json`,
`delete_messages_json`, `get_mcp_server`, `delete_mcp_server`,
`upsert_model`, `upsert_mcp_server`, and `has_mcp_servers` /
`has_data` (parameterless, but converted for consistency).

Regression test `S10: SQL injection resistance (B1 regression)` added
to `tests/test_settings_store.cpp`. It exercises four malicious
strings (`x'; DROP TABLE models; --`, `y' OR '1'='1`,
`z"; DROP TABLE kv_settings; --`, plus 200 quotes + `; DROP TABLE
models; --`) as model names, kv_settings keys/values, and mcp_servers
names. Each round-trip preserves the verbatim value AND verifies that
the target table still exists. **10/10 tests pass.**

Also fixed as a side-effect of the rewrite: `row_to_model` was
throwing on a non-numeric `max_context_size` / `temperature` column;
now uses `try/catch` and falls back to `0` (this had previously
been a UB trap waiting for corrupt SQLite input).

---

### B2 — Memory leak in `SettingsStore::for_home` (CRITICAL)

**File:** `src/core/SettingsStore.cpp` lines ~344–352

```cpp
SettingsStore& SettingsStore::for_home(fs::path home)
{
    fs::create_directories(home);
    return *(new SettingsStore(home / "daedalus.db",
                               /*is_default=*/false,
                               /*auto_migrate=*/false));
}
```

`new` with no matching `delete`. The caller gets a reference but has
no way to free it. The DbPool deduplicates the underlying sqlite3
handle (good), but the `SettingsStore` wrapper itself leaks on every
call. `LLMClient`, `AgentImpl`, `Messages`, and `SessionMemory` all
call this. Each iteration of the agent leaks one wrapper.

**Fix:** Either
- (a) Return by value (`SettingsStore` is currently copyable) so the
  caller's stack owns it; or
- (b) Keep a per-path static cache (`static std::unordered_map<path,
  SettingsStore> cache`) so the same path returns the same wrapper.

Option (b) is closer to the existing `instance()` model and preserves
the `SettingsStore&` signature.

**Status:** ⚠️ **Acknowledged, NOT fixed (2026-07-15).**

The `for_home` rewrite required breaking both (a) and (b). Option (b)
caches the wrapper, which keeps the `SettingsStore&` signature but
breaks `test_session_memory` and `test_messages`: those tests rely on
each `for_home()` call constructing a fresh wrapper whose
`impl_->db` points at the current `DbPool` entry (which detects file
changes / missing files). With caching, the wrapper persists across
calls and points at a stale handle. Option (a) — return-by-value —
changes the public API from `SettingsStore&` to `SettingsStore`, which
breaks `AgentImpl::Body::store` (currently `SettingsStore*`) and
forces a wider refactor of the AgentImpl / Body layout.

The current implementation is identical to the pre-existing buggy
code (`return *(new SettingsStore(...))`), with a long comment
explaining the trade-off. A proper fix is tracked as a v0.5.1
follow-up — see the next sub-task below.

---

### B3 — `BashTool` standalone-`&` check ignores quotes (HIGH)

**File:** `src/tools/BashTool.cpp` lines ~131–152 (`Rule 3`)

The `&` parser scans every `&` in the command string. It does NOT
understand shell quoting, so commands like:

```bash
echo "hello & world"
```

get rejected with `Security: Background operator (&)`. The user
sees a confusing error for a perfectly safe command. The library
also rejects `printf 'a&b'`, `cat <<EOF\nfoo & bar\nEOF`, etc.

**Fix:** Use a small tokenizer that tracks single/double-quote and
heredoc state. Reject `&` only when it appears outside any quoted
context. A 30-line state machine is sufficient — see
`BashPathChecker::tokenize_command` for an existing tokenizer to
mirror (or extend it).

**Status:** ✅ **Done (2026-07-15).**

Added `BashPathChecker::contains_standalone_ampersand(const std::string&)`
in `src/tools/BashPathChecker.{h,cpp}`. Single-pass state-machine
that correctly distinguishes:

| Input | Verdict |
|-------|---------|
| `sleep 60 &`           | ❌ reject (background) |
| `cmd1 & cmd2`          | ❌ reject |
| `a && b`               | ✅ allow (`&&` is logical AND) |
| `echo "hello & world"` | ✅ allow (double-quoted text) |
| `echo 'a&b'`           | ✅ allow (single-quoted text) |
| `echo $'a&b'`          | ✅ allow (ANSI-C quoting) |
| `cmd 2>&1`             | ✅ allow (fd redirection) |
| `cmd &> /dev/null`     | ✅ allow (`&>` redirect) |
| `cmd >file 2>&1`       | ✅ allow (both fd redirects) |
| `cat <<EOF\nfoo & bar\nEOF` | ✅ allow (heredoc body) |
| `cat <<-EOF\n\tfoo & bar\n\tEOF` | ✅ allow (indented heredoc) |
| `cat <<'EOF'\nfoo & bar\nEOF` | ✅ allow (quoted heredoc marker) |

`BashTool::execute()` now calls `BashPathChecker::contains_standalone_ampersand(command)`
instead of the old inline parser (the old code path was deleted,
not just bypassed).

Added regression test `BashPathChecker - contains_standalone_ampersand`
in `tests/test_bash_path_checker.cpp` with 21 new assertions across
7 SUBCASEs. All 76 assertions in the file pass.

---

### B4 — `BashTool::build_patterns` has duplicate `"make"` (LOW)

**File:** `src/tools/BashTool.cpp` line 17

```cpp
std::vector<std::string> build_patterns = {
    "cmake", "make", "g++", "gcc", "clang++", "clang",
    "build", "compile", "make", "ninja",   // ← "make" appears twice
    ...
};
```

Harmless but means the trigger callback fires twice for any `make`
invocation (once per match). Wastes one callback into the session
memory layer.

**Fix:** Remove the duplicate. While here, sort the list
alphabetically — 30 entries unsorted is a maintenance trap.

**Status:** ✅ Done (2026-07-15).

---

### B5 — `Messages::truncate_if_needed` tool-chain walk is inverted (HIGH)

**File:** `src/core/Messages.cpp` lines ~625–695

The function walks backward from a tool-result message looking for
the parent `assistant` message with matching `tool_calls`. The
inner `while` loop:

1. Pushes `messages_[i]` (the candidate).
2. **Then** reads `prev_role` from `messages_[i]` *before* the push —
   but `prev_role` is initialised at the top of the loop body, so on
   the **second** iteration it's looking at the message *one further
   back*, not the one we just pushed.
3. The `messages_[i+1].contains("tool_calls")` check is correct on
   iteration 1 (just pushed) but stale on iteration 2 (i+1 now points
   at the message *before* the just-pushed one).
4. The `messages_[i].contains("tool_calls")` second branch is dead
   code — i has already been decremented past the candidate.

End result: when the chain spans more than one message back, the
walk can stop at the wrong index, leaving an orphan tool-result in
the truncated chain — exactly the thing the function is trying to
prevent.

**Repro:** Build a 50-message chain with a tool result at index 48,
its assistant at index 46, and an earlier assistant at index 40.
Call `truncate_if_needed` with `max_size` just small enough to
require walking past the index-46 assistant. The current code
breaks at the wrong place and emits a tool result without its
parent — the very next LLM call then returns 400 with
`tool call result does not follow tool call`.

**Fix:** Restructure the walk: check `role == "assistant"` and
`tool_calls` membership on the candidate *before* pushing, not after.
Extract a helper `find_tool_call_parent(messages_, start_idx) →
parent_idx` and use it from `truncate_if_needed`.

**Status:** ✅ **Done (2026-07-15).**

`Messages::truncate_if_needed` rewritten at
`src/core/Messages.cpp:1287`. New helper lambda
`find_parent_assistant_for_tool_result(tool_call_id)` walks back
through `messages_` looking for an assistant whose `tool_calls`
contains an entry with the matching `id`. The outer loop now:
1. Walks backward from the end (most-recent-first).
2. On a `tool` message: asks the helper for the parent index.
   - If no parent (orphan): keep the tool message alone with a
     WARN log.
   - If parent found: include `messages_[parent+1..i]` inclusive,
     including any intervening messages, and jump the loop
     index past the parent. Chain stays unbroken.
3. On a plain message: size-check + push (unchanged).

Orphan handling (parent missing or `parent >= i`) is a deliberate
choice — keeping an orphan tool result is legal JSON and better
than silently dropping it; the LLM replies with a clear error
that's easy to handle, while a dropped tool result looks like a
"ghost" completion.

Added regression test `Messages - truncate_if_needed preserves
multi-step tool chain (B5)` in `tests/test_messages.cpp`. It
builds the precise scenario from the bug report — two
`assistant(tool_calls)+tool_result` pairs where the truncation
boundary falls between them, forcing the old code to orphan
the second result — and asserts every remaining tool result has
its parent assistant in the truncated chain. Test passes.

55/55 `messages_tests` pass (was 54 + the new one).

---

### B6 — `Messages::sanitize_assistant_message` comment contradicts code (LOW)

**File:** `src/core/Messages.cpp` lines ~840–890

```cpp
// CRITICAL FIX: Remove ORPHAN tool_calls (those with no
// matching tool result). These cause API 400 errors and
// infinite retry loops. Only check if tool_result_ids was
// provided (non-empty means we did the collection pass).
bool has_result = tool_result_ids.empty() || tool_result_ids.count(tc_id) > 0;
```

The comment says "non-empty means we did the collection pass"
(implying empty → no check), but the code KEEPS all tool_calls
when the set is empty (the `||` short-circuits to true). The
behaviour is intentional ("if we have no tool results at all,
nothing is orphaned"), but the comment lies.

**Fix:** Rewrite the comment to match the code, or invert the
check to make the intent explicit:
```cpp
const bool has_results = !tool_result_ids.empty();
const bool is_orphan   = has_results &&
                         tool_result_ids.count(tc_id) == 0;
if (is_orphan) { LOG(...); continue; }
```

**Status:** 🟢 Trivial.

---

### B7 — `LLMClient::run_one_turn` ignores `compress_after_done` (MEDIUM)

**File:** `src/api/LLMClient.cpp` lines ~1507 (set) and ~1560 (check)

`run_agent_cycle` sets `compress_after_done = true` when the agent
calls the `compress_context` tool. But `run_one_turn`'s while loop
checks only `at_threshold` and `result_state` — `compress_after_done`
is **never read**. The agent's explicit compression request is
silently dropped unless the auto-threshold also fires.

**Repro:** Run an agent that calls `compress_context` while the
token count is at 50% (below the 90% threshold). The function
returns `{nullopt}` from `compress()` but the caller never knows
the agent asked for it; the messages stay bloated.

**Fix:** In `run_one_turn`:
```cpp
if (compress_after_done || at_threshold) {
    // compress, then reset compress_after_done
}
```

Or remove the flag entirely if the auto-threshold is now deemed
sufficient.

**Status:** 🟡 Open.

---

### B8 — `LLMClient::inject_context_stats` erases wrong region (HIGH)

**File:** `src/api/LLMClient.cpp` lines ~1238–1278

The function searches for `# Context Status` (H1) or `## Context Status`
(H2), then erases from there to the next heading of the **same level**.

**Bug:** When the H2 variant is matched and there is no `\n## `
following it (the context stats were the last section), the code
falls through to `content = content.substr(0, ctx_pos) +
stats.str();` — dropping the trailing tail of the system prompt
(usually the "Skill usage" and "Tooling" sections that come after
the stats in SystemPromptBuilder's compose order).

**Repro:** A system prompt where the Context Status block is the
final H2. After the second `inject_context_stats` call, anything
after the previous stats section is gone.

**Fix:** When `context_end == npos`, append rather than truncate:
```cpp
if (context_end == std::string::npos) {
    content = content.substr(0, ctx_pos) + stats.str()
              + content.substr(ctx_pos + context_stats.size());
} else {
    content = content.substr(0, ctx_pos) + stats.str()
              + content.substr(context_end);
}
```
And same fix in `Messages::inject_context_stats`.

**Status:** ✅ **Done (2026-07-15).**

Both `LLMClient::inject_context_stats` (`src/api/LLMClient.cpp:2203`)
and `Messages::inject_context_stats` (`src/core/Messages.cpp:1247`)
now use a sentinel-based approach:

1. The stats block is **terminated by a unique sentinel line**:
   ```
   <!-- /daedalus-context-status -->
   ```
2. On update, the function searches for the sentinel starting at
   `# Context Status` and erases inclusive of it. This works
   regardless of what follows — heading, blank line, or end of
   string.
3. **Fallback path** for legacy pre-fix writes (no sentinel): the
   old heading-based search is used as a secondary locator,
   but the `content_end = content.size()` over-truncate is fixed
   — when no same-level heading follows, we erase to the next
   blank line (`\n\n`) instead of to end-of-string.

The trailing tail of the system prompt is preserved across calls.

Added regression test `Messages - inject_context_stats preserves
trailing prompt (B8)` in `tests/test_messages.cpp`. It sets up a
system prompt where the Context Status block is the LAST H2
section (followed by `## Closing Notes` which would be wiped by
the pre-fix code), runs `inject_context_stats`, then asserts
that both `## Closing Notes` and the literal text "Always cite
your sources" survive. Also asserts there's exactly one sentinel
line (not two, which would mean a stale block was left behind).

56/56 `messages_tests` pass (was 55 + the new one).

---

### B9 — `LLMClient::run_agent_cycle` repeats the "find last tool_call" loop 4× (LOW)

**File:** `src/api/LLMClient.cpp` — fatal-error path, transient-error
retry path, no-choices retry path, and final-fallback path each have
their own ~12-line loop scanning `messages[]` backwards for the last
`assistant` message with `tool_calls`, then digging out the last
`tool_call` and extracting id/name/args.

Each loop has slightly different handling (some skip non-array
`tool_calls`, some assume shape differently). One bug here is
repeated four times — when the shape assumptions diverge (e.g. a
newer LLM emits `tool_calls` as an array of objects with different
schema), three of the four paths will get the wrong id.

**Fix:** Extract to a private helper:
```cpp
struct LastToolCall { std::string id, name, args; };
static std::optional<LastToolCall> find_last_tool_call(const json& messages);
```
Call it from all four sites.

**Status:** 🟡 Open.

---

### B10 — `CurlMultiSession::process_completed` discards non-2xx body (HIGH)

**File:** `src/api/CurlMultiSession.cpp` lines ~234–257

```cpp
if (code >= 200 && code < 300) {
    state->promise.set_value(std::move(state->response));
} else {
    // ... log the body to stderr ...
    auto resp = std::move(state->response);  // ← discarded
    state->promise.set_value(std::unexpected(CurlError::HttpStatus));
    (void)resp;
}
```

`resp` is moved out of `state->response` and dropped. The caller
gets `unexpected(HttpStatus)` with **no body**, **no status code**,
**no Content-Type**. They can't tell a 400 (`bad request`) from a
503 (`service unavailable`) from a 401 (`auth failed`). Every
caller that needs to inspect the body (LLMClient extracts error
messages, MCP client needs to see tool-not-found responses) has
to re-derive this from a side channel.

The body is logged (good for ops) but unreachable from C++ (bad
for callers).

**Fix:** Make `CurlResponse` part of `expected` even on HttpStatus:
```cpp
struct CurlResponse {
    long status_code = 0;
    std::string body;
    std::string content_type;
    bool cancelled = false, timed_out = false;
};
using CurlResult = std::expected<CurlResponse, CurlError>;
// Caller checks .error() for connection-level errors,
// otherwise reads status_code + body for HTTP-level errors.
```
Adjust callers (`LLMClient::make_api_request_async`, `Messages::call_llm_for_summary`).

**Status:** 🟡 Open.

---

### B11 — `LLMClient::sanitize_utf8` has dead code path (LOW)

**File:** `src/api/LLMClient.cpp` lines ~52–143

The function calls `iconv_open("UTF-8", "UTF-8")`. When iconv is
present, the conversion is done. When iconv is missing, the
fallback path is used. **Both paths duplicate the same
state-machine sanitizer code**, 90 lines of identical logic in
two places. The two copies have already drifted slightly (the
fallback uses `fallback` instead of `result` for the variable name,
and the iconv-failure path doesn't propagate the `result` past the
iconv error — it returns the full input unchanged).

**Fix:** Extract the state-machine sanitizer to a free function
`static std::string utf8_replace_invalid(const std::string&)` and
call it from both paths.

**Status:** 🟢 Trivial.

---

### B12 — `SessionMemory::sanitize_utf8` truncates on partial sequence (LOW)

**File:** `src/core/SessionMemory.cpp` lines ~36–110

```cpp
// Invalid byte in sequence - replace the start byte
result[sequence_start_pos] = '?';
result.resize(sequence_start_pos + 1);  // ← truncates!
```

When an invalid byte is encountered mid-sequence, the function
replaces the start byte with `?` AND discards every byte after it
that was part of the in-flight sequence. For input `"abc\xE6\x97\xA5X"`
(valid 3-byte sequence), if byte `\x97` is corrupted to `\x18`,
the output is `"abc?"` — the `X` is gone.

The intended behaviour is to replace just the start byte (`\xE6`),
keep the following bytes as-is, and let the parser treat them as
fresh single-byte starts. Most are ASCII so they survive; the few
multi-byte continuations get their own `?` substitutions when the
parser hits them.

**Fix:** Replace `result.resize(sequence_start_pos + 1)` with
`result.resize(sequence_start_pos + 1);` (same thing, but) keep
the bytes appended AFTER the start in the result:
```cpp
result[sequence_start_pos] = '?';
// do NOT resize — keep the trailing bytes already in result.
// They're separate continuations from the corrupted start, so the
// state machine will handle each on the next loop iteration.
```
Wait — they're already in `result`. The issue is the next iteration
sets `sequence_start_pos = result.size()` AFTER appending `c`. So
the new sequence starts at the right place. The bug is the resize
truncating. Remove the resize; leave everything else alone.

Same fix in the iconv fallback in LLMClient.cpp.

**Status:** 🟡 Open.

---

### B13 — `Messages::split_messages` and `get_compression_indices` are duplicated (LOW)

**File:** `src/core/Messages.cpp`

Lines ~530–620 (`split_messages`) and ~660–730
(`get_compression_indices`) both implement the same algorithm:
1. Find tool_call ids in `[1, target_split]`.
2. Extend `target_split` to include tool results anywhere in
   `messages_` that reference those ids.
3. Return the split point.

The two copies are **already out of sync** — `split_messages`
clamps to `total_non_system` and uses a slightly different inner
loop ordering. Compression today calls `get_compression_indices`
(via `compress()`), but `split_messages` is still called by
`get_split_info`. Future bug fixes in one won't reach the other.

**Fix:** Extract `compute_safe_split(target_ratio) → size_t` and
call it from both. `split_messages` builds the three sub-arrays
from the single index; `get_compression_indices` returns `(1, 1+idx)`.

**Status:** 🟢 Refactor.

---

### B14 — `LLMClient::make_api_request_async` doesn't honour 4xx non-retry (MEDIUM)

**File:** `src/api/LLMClient.cpp` lines ~480–560

The function uses `CurlMultiSession::submit_cancellable` and
checks `code >= 200 && code < 300`. On 4xx it logs and returns the
body. On 5xx it logs and **loops** to retry. On `CurlError` it
loops to retry.

**Bug:** The retry loop's `if (attempt < max_retries &&
server_error_settings.enabled && !stop_token.stop_requested())`
gate uses `server_error_settings.enabled` to gate retry. But for
connection errors (CurlError), retry is the right thing **even if
`server_error_settings.enabled` is false** — a single network blip
shouldn't drop the whole turn. Conversely, the gate doesn't
respect HTTP 429 retry hints in the response headers.

**Fix:** Split into two retry policies:
- `retry_on_transport = true` (CurlError except Cancelled)
- `retry_on_http_5xx = server_error_settings.enabled`

And honour `Retry-After` from 429 responses when present.

**Status:** 🟡 Open.

---

### B15 — `LLMClient::run_agent_cycle` payload-size mismatch (LOW)

**File:** `src/api/LLMClient.cpp`

The first API request uses `request_payload.dump(2)` (pretty-printed,
2-space indent). The retry path inside the function uses
`request_payload.dump()` (compact). For an 800-message session the
size difference can be 15–20%. The `max_request_size_` check runs
**before** the retry path, so a request that was fine when sent
might fail JSON serialisation on the retry branch if the dump size
differs from the size that was checked.

**Fix:** Use `dump()` everywhere for the wire format, or use
`dump(2)` everywhere for debuggability — pick one.

**Status:** 🟢 Trivial.

---

### B16 — `CurlMultiSession` shutdown can leak in-flight `RequestState` (MEDIUM)

**File:** `src/api/CurlMultiSession.cpp` `event_loop` shutdown
section, lines ~110–125

```cpp
while ((msg = curl_multi_info_read(multi_, &msgs_left)) != nullptr) {
    if (msg->msg == CURLMSG_DONE) {
        ...
        delete state;
    }
}
```

`curl_multi_info_read` only returns completed transfers. **In-flight
transfers never get a `CURLMSG_DONE`** — they're still running on
their sockets when shutdown happens. Those `RequestState`s are
never deleted, their `easy` handles never cleaned up, their futures
never resolved.

The promises stay un-set; the future `wait_for()` in
`LLMClient::run_agent_cycle` will hang forever.

**Fix:** Before draining `curl_multi_info_read`, walk the active
transfers and `cancel` them, then drain. Or explicitly call
`curl_multi_remove_handle` + `curl_easy_cleanup` on each and
`set_value(unexpected(Cancelled))` on each promise.

Better: keep a map `easy* → RequestState*` so you can iterate the
still-active set at shutdown.

**Status:** 🔴 Open.

---

### B17 — `AgentImpl::execute_tool` is a no-op stub (HIGH)

**File:** `src/library/AgentImpl.cpp` lines ~770–778

```cpp
nlohmann::json AgentImpl::execute_tool(const std::string& name,
                                       const nlohmann::json& args)
{
    if (!initialized_) initialize();
    if (sink_) {
        sink_->on_log(daedalus::LogLevel::Warn,
                      "Agent::execute_tool() — Phase 1+ stub; "
                      "direct tool dispatch will be wired through the "
                      "host's tool path in Phase 7. ...");
    }
    (void)args;
    return nlohmann::json{{"status", "error"},
                          {"message", "Phase 1+ stub — see AgentImpl::execute_tool"}};
}
```

The public `Agent::execute_tool(name, args)` API (declared in
`include/daedalus/Agent.h`) returns an error JSON for every call,
forever. The comment says "Phase 7 will wire it" but Phase 7 is
months past. Hosts that want to programmatically invoke a tool
(e.g. webui's "Run with these args" button, daedalus-manager's
"--tool" CLI flag, sub-agent tooling) have to reach into the
internal `LLMClient` instead of the public API.

**Fix:** Route through `ToolHandler::execute_tool_call` with a
synthesised `tool_call` JSON (same shape as the LLM emits):
```cpp
json tool_call = {
    {"id", "host_" + std::to_string(next_id_++)},
    {"type", "function"},
    {"function", {{"name", name}, {"arguments", args.dump()}}}
};
json result = ToolHandler::execute_tool_call(tool_call,
                                             *body_->file_io,
                                             /*progress*/ nullptr);
return result;
```

Need to expose `FileIO` through `Body`. Currently `file_io_` is
inside `LLMClient`.

**Status:** 🔴 Open.

---

### B18 — `AgentImpl::active_model` and `list_models` duplicate resolution (LOW)

**File:** `src/library/AgentImpl.cpp`

`active_model` has a 3-tier resolution (cfg.model_name → store
default → first model). `list_models` builds the model list. Both
construct `ModelInfo` from `ModelConfig` using the same logic.

**Fix:** Extract `static ModelInfo to_model_info(const ModelConfig&,
const SettingsStore&, std::string_view name)`. Saves ~40 lines and
makes the two consistent (currently `active_model` uses `cfg.temperature`
and `list_models` uses `cfg.temperature` — same field but easy to
diverge in future).

**Status:** 🟢 Refactor.

---

### B19 — `LLMClient::run_one_turn` overwrites the system message (LOW)

**File:** `src/api/LLMClient.cpp` lines ~1393–1398

```cpp
if (messages.empty()) {
    messages.push_back({{"role", "system"}, {"content", system_instruction}});
}
if (!user_message.empty()) {
    messages.push_back({{"role", "user"}, {"content", user_message}});
}
```

`messages` here is `messages_->get_messages()` (a reference into
`messages_`'s internal vector). On every turn we push a new user
message — that's correct. But if `messages_` was loaded from
disk via `initialize()` and the last message is a `tool` result,
the next turn pushes the user message **after** the tool result,
which the LLM API will reject (`tool` result must follow
`tool_call`, not be followed by `user`).

**Repro:** Load a session that ended mid-tool-call (because the
user pressed Ctrl-C). The next `run_one_turn` appends a user
message after the dangling tool result → API 400.

**Fix:** Either sanitize the loaded messages (drop trailing
tool results without parents) on load, or refuse to push a user
message if the last message is `tool` and emit a clean user
message instead.

**Status:** 🟡 Open.

---

### B20 — `BackgroundProcessManager::FIFO_TIMEOUT_MS` is unused (LOW)

**File:** `src/core/BackgroundProcessManager.cpp` line 36

Compiler warning confirmed: `unused variable 'FIFO_TIMEOUT_MS'`.
Either use it (currently `BashCommandExecutor` and friends
hard-code their own timeouts) or remove it.

**Status:** 🟢 Trivial.

---

### B21 — `BashTool` build-event double-fire (LOW)

**File:** `src/tools/BashTool.cpp` line 17 (covered in B4) — same
duplicate `"make"` causes `trigger_build_callback` to fire
twice for `make` invocations. Each fire writes to session
memory. Two identical build entries, only the second has the
correct count.

**Status:** 🟢 Trivial — fix with B4.

---

### B22 — `Messages::save` ignores load failure for SQLite mirror (MEDIUM)

**File:** `src/core/Messages.cpp` lines ~280–330

```cpp
void Messages::save() const {
    ...
    // SQLite mirror — best-effort
    try {
        ...
        store.set_messages_json(session_id_, mirror.dump());
    } catch (const std::exception& e) {
        LOG_WARNING(...);
    }
}
```

When the JSON file succeeds but the SQLite mirror fails, the
`load()` path next time will load from JSON. OK. But `load()` also
tries SQLite first — if the mirror is corrupt (partial write,
truncated row), the JSON fallback never runs:

```cpp
auto blob = store.get_messages_json(session_id_);
if (!blob.empty()) {
    json saved = json::parse(blob);   // ← throws on corrupt
    ...
}
```

The catch block calls `LOG_WARNING` but **does NOT delete the
corrupt mirror row**, so the next load also fails. The user is
stuck on the corrupt mirror forever.

**Fix:** In the catch block, delete the corrupt mirror and re-load
from JSON:
```cpp
} catch (const std::exception& e) {
    LOG_WARNING("Messages: SQLite mirror corrupt (" + std::string(e.what())
                + "); deleting and falling back to JSON");
    store.delete_messages_json(session_id_);
    // ... fall through to JSON load ...
}
```
Same fix in `SessionMemory::load()`.

**Status:** 🟡 Open.

---

### B23 — `set_status` tool's `previous_state` is `-1` (HIGH)

**File:** `src/api/LLMClient.cpp` lines ~1108–1120

```cpp
if (status == "set_state" && sink_ && result.contains("state") ...) {
    auto new_state = daedalus::agent_state_from(...);
    if (ok) {
        // We don't have access to the previous public state here...
        sink_->on_state_change(-1, static_cast<int>(new_state));
    }
}
```

The webui and CLI both rely on `on_state_change(prev, new)` to
animate the state chip transition. With `prev = -1`, the chip
"snaps" instead of transitioning. The comment says
`LLMClient only tracks its own internal enum` — true, but
`AgentImpl::state_` IS the public state, and we just came from
there. We need to either:

(a) `LLMClient` learns its own internal→public mapping (call
`agent_state_from(internal_state_)`) and uses that as `prev`, or

(b) Move the dispatch to `AgentImpl::run_one_turn`, which already
maps internal → public via `map_from_internal`.

Option (b) is cleaner — the LLMClient is internal, AgentImpl is
the public surface that already has both states.

**Status:** 🟡 Open.

---

### B24 — `SettingsStore::ServerErrorSettings::backoff_multiplier` throws on missing key (MEDIUM)

**File:** `src/core/SettingsStore.cpp` lines ~552–559

```cpp
s.backoff_multiplier = std::stod(get_string(
    "server_error_backoff_multiplier",
    std::to_string(s.backoff_multiplier)));   // default value ignored!
```

`get_string(key, def)` — if `def` is non-empty, it's returned when
the key is missing. But `std::to_string(s.backoff_multiplier)` is
non-empty, so the second arg IS the default. Wait, that part is
correct — when the key is missing, `get_string` returns the
default. So `std::stod("2.0")` works.

**The actual bug:** If the key IS present but has a non-numeric
value (e.g. user typo'd `server_error_backoff_multiplier =
"twox"`), `std::stod` throws `std::invalid_argument` with **no
try/catch**, which propagates out of `get_server_error_settings`,
which is called from `LLMClient::make_api_request_async` in a
`try {} catch (...)` that **swallows it** silently — the user's
retry behaviour becomes whatever the default `ServerErrorSettings`
struct has (all zeros), meaning `wait_time = 0`, `max_retries = 0`,
no retries at all.

**Fix:** Wrap the `std::stod` in `try { ... } catch (...) { return
defaults; }`:
```cpp
double parsed = s.backoff_multiplier;  // start with default
try {
    auto str = get_string("server_error_backoff_multiplier",
                          std::to_string(s.backoff_multiplier));
    parsed = std::stod(str);
} catch (...) {
    // leave default
}
s.backoff_multiplier = parsed;
```

**Status:** 🟢 Trivial.

---

### B25 — `McpServerManager::load_env_file` not idempotent (MEDIUM)

**File:** `src/mcp/McpServerManager.cpp` (need to verify) — based on
`AgentImpl::initialize()` calling it once per Agent construction,
but `McpServerManager` is reused across Agents in some host
configurations. Each call may double-load env vars.

**Repro:** Construct two `Agent`s in the same process with the
same `daedalus_home`. The second `load_env_file` reads the file
again and pushes env vars on top of the first load — child
processes spawned via stdio MCP servers see the same `KEY` env var
twice (or, if `load_env_file` does a `setenv` after the first
push, the second load overwrites).

**Fix:** Either clear-and-set semantics (`load_env_file` removes
existing keys it owns before re-adding), or track already-loaded
keys in a set and skip on second call.

**Status:** 🟡 Open (needs verify).

---

### B26 — `LLMClient::make_api_request_async` callback fires on cancelled transfer (LOW)

**File:** `src/api/LLMClient.cpp` lines ~510–525

The `streaming_callback` is invoked from `CurlMultiSession::write_cb`
for every chunk received. If the caller cancels mid-stream
(stop_token fires), in-flight chunks from the kernel buffer can
still trigger one more `on_chunk` call before `write_cb` sees
`stop_requested()`. This causes one extra `on_assistant_delta`
call after the user pressed Ctrl-C.

**Fix:** Check `stop_token.stop_requested()` at the top of
`streaming_callback` and drop the chunk silently. One-liner.

**Status:** 🟢 Trivial.

---

### B27 — `LLMClient` settings `enabled` flag not propagated to retry loop (LOW)

**File:** `src/api/LLMClient.cpp` lines ~425–445

`server_error_settings.enabled` controls whether the synchronous
retry loop runs (`make_curl_request_with_retry` checks it). The
async retry loop (`make_api_request_async`) ALSO checks it. But
the SSE retry inside `run_agent_cycle` (the JSON-parse retry
loop) **does not** — it always retries on parse failure, even
when the user has disabled server-error retry via the settings
store.

**Fix:** Add `if (server_error_settings.enabled)` guard at the
top of each retry block.

**Status:** 🟢 Trivial.

---

### B28 — `LLMClient::sanitize_utf8` regex drift between copies (LOW)

**File:** `src/api/LLMClient.cpp` (iconv + fallback copies of the
state machine) and `src/core/Messages.cpp::sanitize_utf8_simple`
and `src/core/SessionMemory.cpp::sanitize_utf8`.

**Four** copies of the same UTF-8 sanitiser, with subtle
differences:
- `LLMClient` iconv path: trims on iconv success.
- `LLMClient` fallback: same logic as `SessionMemory`.
- `Messages::sanitize_utf8_simple`: replaces per-byte but doesn't
  track `expected_continuations` (so a `\xE6\x97\xA5` is treated
  as three invalid bytes).
- `SessionMemory::sanitize_utf8`: state-machine variant.

A future Unicode test (e.g. emoji ZWJ sequences) will pass on one
copy and fail on the other.

**Fix:** Move all four to `core/Utf8Utils.{h,cpp}` with one
implementation. Re-export from the existing callers for back-compat.

**Status:** 🟡 Open.

---

### B29 — `BashCommandExecutor::execute` no timeout on synchronous read (MEDIUM)

**File:** `src/tools/BashCommandExecutor.cpp` (need to verify) —
`popen` + `fread` loop reads from the child stdout until EOF. If
the child writes nothing and never exits (e.g. an interactive
subshell that's waiting for stdin), the agent blocks forever.
The `timeout_seconds` parameter on `BashTool` IS supposed to bound
this, but the timeout must be implemented as `kill(pid, SIGKILL)`
from a watchdog thread.

**Repro:** `execute_bash` with `command: "bash -i"` — the bash
subshell opens a prompt, reads from inherited stdin, never
exits. Agent loop hangs until process killed externally.

**Fix:** Spawn the watchdog inside `BashCommandExecutor::execute`,
have it `kill(-pid, SIGKILL)` after `timeout_seconds`, then
`waitpid`. Return a result with `timed_out: true`.

**Status:** 🟡 Open (verify first).

---

### B30 — `Messages::extract_summary_text` strips `<answer>` but not `<thinking>` (LOW)

**File:** `src/core/Messages.cpp` lines ~858–905

The function strips `<answer>...</answer>` blocks and
` ```markdown ``` ` blocks. It does NOT strip `<thinking>...</thinking>`
blocks, which several models emit. The summary then begins with
`<thinking>Okay, let me consider...</thinking>` — extra tokens
waste context.

**Fix:** Add the same `find`/`substr` pattern for `<thinking>`:
```cpp
size_t thk_start = content.find("<thinking>");
if (thk_start != std::string::npos) {
    size_t thk_end = content.find("</thinking>");
    if (thk_end != std::string::npos) {
        content.erase(thk_start, thk_end - thk_start + 11);
    }
}
```

**Status:** 🟢 Trivial.

---

### B31 — `phase15_typo_warning_tests` hangs in case 3 (MEDIUM — known)

**File:** `tests/test_phase15_typo_warning.cpp`

Pre-existing: the third test case `test_skills_disabled_empty_no_warn`
hangs. Mentioned in the v0.5.0 status block of CodeMax.md as
"reproduce without SettingsStore changes" but never tracked.

**Likely cause:** a SkillManager singleton global state that
the third case tries to re-initialise; the discovery loop or the
typo-detection set construction deadlocks on a mutex held by a
previous test.

**Fix:** Add `SkillManager::reset_for_testing()` that clears the
singleton's `cached_skills_`, `active_skills_`, and any
in-flight discovery futures. Call it from a `TEST_CASE` setup
fixture or each case's first line.

**Status:** 🟡 Open — was noted, not fixed.

---

### B32 — `LLMClient::update_token_usage` warns twice per turn (LOW)

**File:** `src/api/LLMClient.cpp` `update_token_usage`

`update_token_usage` is called once after a successful response
and logs `No 'usage' field in response` if the field is missing.
The function is **also called for the SSE usage-only final chunk**
in `run_agent_cycle` (line ~870). So during a streaming turn with
`include_usage`, the SSE chunks without usage each warn, then the
final usage chunk parses correctly. The user sees N warnings per
turn.

**Fix:** Track a per-turn flag (`got_usage_this_turn`) that
suppresses the warning after the first warning fires, and resets
at the start of each `run_agent_cycle`.

**Status:** 🟢 Trivial.

---

### Summary

| Severity | Open |
|----------|------|
| CRITICAL | 4  (B1, B2, B16, B17) |
| HIGH     | 6  (B3, B5, B8, B10, B17, B19, B23) |
| MEDIUM   | 7  (B7, B14, B22, B24, B25, B29, B31) |
| LOW      | 14 (B4, B6, B9, B11, B12, B13, B15, B18, B20, B21, B26, B27, B28, B30, B32) |

### Suggested fix order

1. **B1** (SQL injection) — blocker for shipping. ✅ **DONE.**
2. **B2** (SettingsStore leak) — long-running agents accumulate. ⚠️ **Acknowledged, de-scoped** to v0.5.1 (see sub-task below).
3. **B16** (CurlMultiSession shutdown leak) — affects graceful shutdown.
4. **B17** (execute_tool stub) — public API regression since v0.5.0.
5. **B5** (truncate_if_needed tool chain) — silent 400 errors.
6. **B10** (CurlMultiSession body drop) — every error path.
7. **B22** (corrupt mirror recovery) — recovery-from-bad-state.
8. Everything else, ordered by user-visible impact.

---

## Sub-task — B2 proper fix: return `SettingsStore` by value

### Context

The B2 leak in `for_home()` is real but its proper fix is wider than
the bug report suggests. Two options exist; both break tests:

| Option | Pros | Cons |
|--------|------|------|
| **(a)** Return `SettingsStore` by value (wrapper becomes cheap, lives on caller's stack) | Fixes the leak cleanly; no heap at all | Public API change: `for_home` returns `SettingsStore` instead of `SettingsStore&`. `AgentImpl::Body::store` is currently `SettingsStore*` and would need to change to `SettingsStore` (by value) or `std::optional<SettingsStore>`. |
| **(b)** Static per-path cache (caches the wrapper, reuses across calls) | Preserves `SettingsStore&` API; one heap-alloc per unique path (finite) | Cached wrapper's `impl_->db` is stale across file deletes; breaks `test_session_memory` (`S9`-style behaviour: tests delete the DB file between calls expecting the next call to reopen it) and `test_messages` (SQLite mirror in save-destructor / next-load reads-from-file rounds). |

`AgentImpl` is the only call site that takes the address of the
returned `SettingsStore&`:

```cpp
body_->store = &daedalus::SettingsStore::for_home(cfg_.daedalus_home);
```

If we go with (a), this becomes:
```cpp
body_->store = daedalus::SettingsStore::for_home(cfg_.daedalus_home);
```

— one assignment. The agent lifetime owns the wrapper.

`SessionMemory`, `Messages`, `LLMClient` all use `auto store =
daedalus::SettingsStore::for_home(...);` which works as-is via move.

### Plan

1. **settings.h**: change `Impl* impl_` to two inline fields:
   ```cpp
   private:
       std::filesystem::path path_;
       OmniNet::DB::Database* db_ = nullptr;  // non-owning, DbPool-owned
   ```
   This makes the class trivially copyable (default copy is fine for
   trivial fields).

2. **settings.h**: change `for_home()` to return by value:
   ```cpp
   static SettingsStore for_home(std::filesystem::path home);
   ```

3. **settings.h**: drop the `Impl` forward-declare and the pimpl
   declaration. Update `// -- Read --` docblock to match.

4. **SettingsStore.cpp**: replace `impl_->db` with `db_` and
   `impl_->path` with `path_` everywhere. `for_home` becomes:
   ```cpp
   SettingsStore SettingsStore::for_home(fs::path home) {
       fs::create_directories(home);
       SettingsStore out;
       out.path_ = home / "daedalus.db";
       out.db_ = &DbPool::instance().get(out.path_);
       return out;  // NRVO
   }
   ```

5. **AgentImpl.h**: change `Body::store` from `daedalus::SettingsStore*`
   to `std::optional<daedalus::SettingsStore>` (default-constructed
   empty for cases with no model config).
   ```cpp
   struct Body {
       std::optional<daedalus::SettingsStore> store;
       ...
   };
   ```

6. **AgentImpl.cpp**: change the assignment to copy-assign:
   ```cpp
   if (!cfg_.daedalus_home.empty()) {
       body_->store = daedalus::SettingsStore::for_home(cfg_.daedalus_home);
   } else {
       body_->store = daedalus::SettingsStore::instance();
   }
   ```

7. **Tests**: confirm `session_memory_tests`, `messages_tests`,
   `settings_store_tests`, all 61 tests still pass. The new shape
   returns a fresh `SettingsStore` per call → no stale db handle.

### Risk

- **MEDIUM.** One public-API signature change:
  `for_home(home)` returns `SettingsStore` instead of
  `SettingsStore&`. Hosts that use `auto& s = for_home(...)` will
  fail to compile — must change to `auto s = for_home(...)` (move).
- One internal-storage change:
  `AgentImpl::Body::store` becomes `std::optional<SettingsStore>`
  (was `SettingsStore*`). All `body_->store->method()` calls become
  `body_->store->method()` (unchanged: optional has `operator->`).

### Status

- [ ] Inline the `Impl` fields in `settings.h`
- [ ] Change `for_home()` to return by value
- [ ] Update all method bodies to use `path_` / `db_` directly
- [ ] Change `AgentImpl::Body::store` to `std::optional<SettingsStore>`
- [ ] Update AgentImpl assignment site
- [ ] Verify `session_memory_tests` / `messages_tests` still pass
- [ ] Verify `settings_store_tests` (S1–S10) still pass
- [ ] Verify full ctest still 56+/61
- [ ] CHANGELOG entry: "B2 leak fixed by returning SettingsStore by value"

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

---

## Bug/Issue Sweep — Round 2 (2026-07-15)

> Re-sweep of `src/`, `demos/`, `include/`, `tests/`, and `docs/` after
> the Round 1 sweep from earlier in this file. The build was clean,
> `ctest -E phase15_typo_warning` was at **58/61** (1 failure,
> 2 skipped). Round 2 ships these fixes; final state is **59/61
> pass + 2 live-LLM skipped, 0 failures**.

### Findings + fixes

#### F0 — `print_banner` throws `std::length_error` on long paths (HIGH — found via physical tests 2026-07-16)

**File:** `demos/daedalus-cli/banner.h` lines ~42–82 (the original
`std::string(WIDTH - s.size(), ' ')` pad lines)

`print_banner` printed every field with a hard-coded column width
(52, 54, or 63 chars) and padded with `std::string(WIDTH - s.size(), ' ')`.
Whenever the actual value was wider than `WIDTH` — typical for
`data_dir` or `project_root` on a long path like
`/workspace/daedalus/Testing/test-cli-bare/project/.daedalus` (59
chars > 52) — the subtraction underflowed to a negative number and
`std::string(NEGATIVE, ' ')` threw `std::length_error`.

**Repro:**
```bash
$ daedalus-cli --home ... --model minimax-m2.7 --auto --no-memory \
    /workspace/daedalus/Testing/test-cli-bare/project
[FATAL] basic_string::_M_create
Aborted (core dumped)
```

**Fix:** Replaced the inline pad with a `pad_to(s, width)` helper
that clamps to 0 and truncates with ` …` for long values. The
banner now survives paths up to the inner-box width and doesn't
crash the demo.

**Status:** ✅ **Done (2026-07-16).**

#### F1 — `legacy_deprecation_warning_tests` fails with `/opt/daedalus` (CRITICAL)

**File:** `tests/test_legacy_deprecation_warning.cpp` case 2
(`test_daedalus_home_override_silences`)

The second case hard-codes `/opt/daedalus` as both the
`$DAEDALUS_HOME` env value and the `cfg.daedalus_home` value.
`Agent::initialize()` calls `Paths::global().ensure_directories()`
which does `fs::create_directories("/opt/daedalus/{config,data,state,cache}")`
— **none of those exist on an unprivileged user** (the test was
inherited from a CI box where the runner has root).

The test process then throws
`std::filesystem::__cxx11::filesystem_error("cannot create directories: Permission denied [/opt/daedalus]")`,
terminates, and aborts the entire ctest run (case 3-5 never execute).

**Fix:** Replaced `/opt/daedalus` with a fresh `fresh_dir("daedalus_home_2")`
temp path (matching the `fresh_dir()` helper used by cases 1, 3, 4).
The assertion ("`$DAEDALUS_HOME` set → no `[DEPRECATION]` warning")
is unchanged. All 5 cases now pass.

**Status:** ✅ **Done (2026-07-15).**

#### F2 — Dead `matches_pattern()` in `FileIO.cpp` (LOW)

**File:** `src/core/FileIO.cpp` lines ~125–167

```cpp
static bool matches_pattern(const std::string& filename, const std::string& pattern) { ... }
```

Recursive glob matcher (`*` and `?`). Compiler emits
`-Wunneeded-internal-declaration` on every clean rebuild. The
function is unused — `FileIO` no longer takes a `SettingsManager*`,
and the settings pipeline that did use it was migrated to
`SettingsStore` in v0.4.0.

**Fix:** Deleted (43 lines). No callers.

**Status:** ✅ **Done (2026-07-15).**

#### F3 — Dead DEBUG block in `LLMClient::run_agent_cycle` (LOW)

**File:** `src/api/LLMClient.cpp` lines ~976–1010

A 35-line "DEBUG: Log tool_calls" block gated on
`tc_id == "call_function_kn9n3ke6bg89_1" || has_invalid_chars || args_str.length() > 2000`.
Both inner `if` branches (`if (tc_id == "...") { } else if (...) { }`)
are **empty** — the original logger calls were stripped out in an
earlier pass and only the scaffolding remained. Allocated locals
(`tc_id`, `fn_name`, `args_str`, `args_display`, `has_invalid_chars`)
but never logged them.

**Fix:** Deleted the entire block. The next block ("Verify payload
can be parsed back") was retained — it has real `LOG_ERROR` calls
that catch JSON corruption in `tool_calls.arguments`.

**Status:** ✅ **Done (2026-07-15).**

#### F4 — `AgentImpl` constructor writes to 4 unread fields (LOW)

**File:** `src/library/AgentImpl.cpp` lines ~113–132 (Round 1
dead code §D1)

```cpp
if (cfg_.settings_path.empty())   { cfg_.settings_path = ... } // written
if (cfg_.mcp_servers_path.empty()) { cfg_.mcp_servers_path = ... }
if (cfg_.mcp_env_path.empty())     { cfg_.mcp_env_path = ... }
if (cfg_.memory_dir.empty())      { cfg_.memory_dir = ... }
```

Round 1 documented D1 ("fields are set, never read"). I verified:
the only `cfg_.X_path` *reads* live in `AgentImpl::initialize()`
at line 320 — and they're conditional on the field being
non-empty. Since the constructor used to *always* set them, the
host was never able to actually use the override path
("`cfg_.mcp_servers_path = my_path`" got overwritten by the default)
— the override was effectively dead.

**Fix:** Deleted the constructor block. Hosts that want the legacy
JSON path can now set the field on their `AgentConfig` and it
sticks. The four fields are still in the public struct (Round 1
planned to delete them in v0.5.1 with a CHANGELOG mapping) and
now carry `[[deprecated]]` annotations (F5).

**Status:** ✅ **Done (2026-07-15).**

#### F5 — `default_{settings,mcp_servers,mcp_env}_path()` lack deprecation (LOW)

**Files:**
- `include/daedalus/default_paths.h`
- `src/library/default_paths.cpp`

The three legacy path helpers are still public API (used by
`daedalus-cli/migrate.cpp`, `daedalus-subagent/main.cpp`,
`daedalus-manager/main.cpp`) but their behaviour conflicts with
v0.4.0+ — they return paths that no longer exist
(`~/.daedalus/settings/settings.json` is a JSON file that
`SettingsStore` doesn't read at runtime).

**Fix:** Added `[[deprecated("v0.4.0: settings moved to SQLite…")]]`
annotations on each declaration + definition, pointing at
`SettingsStore::instance()` (for settings) and
`SettingsStore::list_mcp_servers()` / `upsert_mcp_server()` (for
MCP). Three `-Wdeprecated-declarations` warnings show up at build
time (the three call sites) — accepted for v0.4.2. The hard
removal is v0.6.0 work.

`default_daedalus_home()` was deliberately NOT deprecated — it
returns a valid user-tier XDG path and is the proper public
helper for hosts that want to point an Agent at the user's
default config dir without reaching into `Paths::global()`.

**Status:** ✅ **Done (2026-07-15).**

#### F6 — Readme/embedding docs reference removed API (HIGH)

**Files:**
- `Readme.md` — `StdoutSink` example uses `request_user_input`
  (removed in v0.4.0 with `ask_user`); "File Locations" shows
  `~/.config/daedalus/config.json` (correct path is
  `~/.local/share/daedalus/daedalus.db`); "Current tag" reads
  `0.2.5`; "Example Configuration" is a hand-edit `config.json`
  walkthrough (no such file exists any more).
- `docs/embedding.md` — `Minimum viable host` example sets
  `cfg.settings_path` (deleted field); `AgentSink` contract
  declares `request_user_input` (deleted method); the state
  machine table lists `AwaitingUser` (deleted state); the
  "Wiring your own input source" section says the agent calls
  the sink mid-turn via `request_user_input`. All these would
  fail to compile against the current header set.

**Fix:** Edited both files comprehensively. Removed `request_user_input`
from the StdoutSink example; replaced the JSON config walkthrough
with a `SettingsStore::upsert_model` example + a one-liner
`sqlite3` for packagers; updated the "File Locations" and "Current
tag"; rewrote the `Settings, memory, MCP paths` section to describe
the v0.4.0 SQLite model and call out the four removed
`AgentConfig` fields; deleted the `AwaitingUser` row from the
state table; rewrote the input-source section around the new
"submit → run_one_turn → submit → run_one_turn" host loop.

**Status:** ✅ **Done (2026-07-15).**

#### F7 — docs/mcp_server.md describes legacy `servers.json` only (MEDIUM)

**File:** `docs/mcp_server.md`

Top of file states daedalus reads from
`~/.daedalus/mcp/servers.json`, but v0.5.0 stores MCP servers in
the `mcp_servers` table of `daedalus.db` (via `SettingsStore`).
The legacy JSON path is still supported as a one-release-cycle
fallback but is no longer the primary.

**Fix:** Added a new top-level section "Configuration (v0.5.0+,
recommended)" with a `SettingsStore::upsert_mcp_server` example,
kept the legacy JSON section as the fallback, and updated the
"Adding New MCP Servers" section to prefer the store API.

**Status:** ✅ **Done (2026-07-15).**

#### F8 — docs/{session_memory,tools,storage,how_compress_works,migration}.md reference removed concepts (MEDIUM)

**Files:**
- `docs/session_memory.md` "Storage location" tree shows
  `settings.json` (no such file in v0.4.0+).
- `docs/tools.md` §14 is a full reference to `ask_user`
  (removed tool).
- `docs/storage.md` §2.2 lists `agent_dir/config.json` as the
  per-project config (moved to user-tier DB in v0.4.0).
- `docs/how_compress_works.md` "Per-Model Context Size" has a
  hand-edited JSON example (no JSON config any more).
- `docs/migration.md` §5 says `settings.json` is "unchanged"
  (it was removed in v0.4.0).

**Fix:** Each doc rewritten to reference the SQLite-backed model:
- `session_memory.md` tree now points at `daedalus.db`.
- `tools.md` §14 replaced with `set_status` (the actual
  current tool).
- `storage.md` §2.2: row removed; explanation added that
  per-project config moves to the user tier.
- `how_compress_works.md` "Per-Model Context Size" example
  replaced with `SettingsStore::upsert_model()`.
- `migration.md` §5 rewritten to describe the v0.4.0 SQLite
  migration path (auto-migrates `~/.daedalus/settings/settings.json`
  into `daedalus.db` on first run).

**Status:** ✅ **Done (2026-07-15).**

### Summary

| # | File | Severity | Status |
|---|------|----------|--------|
| F1 | `tests/test_legacy_deprecation_warning.cpp` | CRITICAL | ✅ fixed |
| F2 | `src/core/FileIO.cpp` | LOW | ✅ fixed |
| F3 | `src/api/LLMClient.cpp` | LOW | ✅ fixed |
| F4 | `src/library/AgentImpl.cpp` | LOW | ✅ fixed |
| F5 | `default_paths.{h,cpp}` | LOW | ✅ fixed |
| F6 | `Readme.md` + `docs/embedding.md` | HIGH | ✅ fixed |
| F7 | `docs/mcp_server.md` | MEDIUM | ✅ fixed |
| F8 | `docs/{session_memory,tools,storage,how_compress_works,migration}.md` | MEDIUM | ✅ fixed |

### Verification

- `cmake --build build -j4` — clean, 0 warnings (modulo the
  three `-Wdeprecated-declarations` from in-tree demos, which
  is intentional for v0.4.2).
- `cd build; ctest -E phase15_typo_warning` — **59/61 pass,
  2 live-LLM skipped, 0 failures**.
- `daedalus-cli --help` runs; `daedalus-subagent`,
  `daedalus-manager`, `daedalus-webui` all build and link.