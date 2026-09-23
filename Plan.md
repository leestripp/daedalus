# Plan.md — daedalus monorepo consolidation

**Goal:** one repo (`/projects/AI/daedalus`, remote `gitlab.com/leestripp/daedalus`)
containing the library and both hosts, with a single clean build system:
one CMake configure/build, one `ctest` run, one cppcheck/clang-tidy entry point.

## Layout

```
/                      ← the daedalus repo (library history + tags preserved)
├── daedalus/          ← the static library (libdaedalus.a), demos, docs, tests
├── icarus/            ← CLI host (REPL + A2A server)
├── monkeybyte/        ← GTK4/gtkmm desktop host
├── CMakeLists.txt     ← NEW: superbuild root
├── Plan.md            ← this file
└── AGENTS.md          ← workspace guide (update at the end)
```

## Phase 1 — Git restructure ✅ DONE (commit 85c5130)

- [x] Backups of all three old `.git` dirs → `~/daedalus-git-backups-20260924/`
- [x] `daedalus/.git` moved to root (keeps library history, tags, GitLab remote — plain push, no force)
- [x] Old `icarus/.git` / `monkeybyte/.git` deleted (standalone histories in the backups + archived GitLab repos)
- [x] Root `.gitignore`; reorg committed as pure renames + new host trees

## Phase 2 — Superbuild root CMakeLists.txt ✅ DONE

- [x] `cmake_minimum_required(3.28)`, `project(daedalus-suite)`, C++23
- [x] Options:
  - `DAEDALUS_BUILD_ICARUS` — default **ON** when top-level, OFF when consumed via FetchContent (`PROJECT_IS_TOP_LEVEL`)
  - `DAEDALUS_BUILD_MONKEYBYTE` — default **ON** when top-level AND pkg-config finds gtkmm-4.0, else skip with a status message; OFF when consumed via FetchContent
  - Library's existing options (`DAEDALUS_BUILD_DEMOS`, per-demo switches, tests) unchanged
- [x] `add_subdirectory(daedalus)` → `icarus` → `monkeybyte` (gated)
- [x] Umbrella `static_analysis` target aggregating `daedalus_static_analysis` (clang-tidy), `icarus_cppcheck`, `monkeybyte_cppcheck` when their tools exist
- [x] ctest: label every test `lib` / `cli` / `app` so `ctest -L app` etc. works
  (deviation: labels are set in each component's own tests CMake — test names are
  not visible to `set_property` from the root scope)

## Phase 3 — Host CMake → dual-mode ✅ DONE

- [x] **icarus**: `if(TARGET daedalus)` → link the in-tree `daedalus` target, use `../daedalus` sources for the settings-TUI tabs, drop the `FetchContent_Populate` + `ExternalProject_Add` double-build. `else()` → keep today's standalone FetchContent path untouched (subfolder stays independently buildable).
- [x] **monkeybyte**: `if(TARGET daedalus)` → skip its FetchContent block entirely. `else()` → standalone path as today.
- [x] In-tree OmniNet include path for icarus (`<daedalus/settings.h>` → `<db/Database.h>`): resolve via `FetchContent_GetProperties(omninet)` (global properties work across sibling dirs).

## Phase 4 — Versioning (prefixed tags) ✅ DONE (tags icarus/1.0.1, monkeybyte/0.9.11 created)

Tag namespace going forward: bare `x.y.z` = library, `icarus/x.y.z`, `monkeybyte/x.y.z` = hosts.

- [x] daedalus lib: `git describe --tags --match '[0-9]*'` (ignore prefixed host tags)
- [x] icarus: `git describe --tags --abbrev=0 --match 'icarus/*'` → fallback `"dev"` until first `icarus/*` tag
- [x] monkeybyte: same with `--match 'monkeybyte/*'`
- [x] Ensure `git describe` runs against the repo root in both top-level and standalone-subdir configures (any subdir of the repo works)

## Phase 5 — Verification (gate) ✅ DONE

- [x] Clean top-level configure + build: `cmake -S . -B build && cmake --build build -j$(nproc)` builds lib + demos + tests + icarus + monkeybyte
- [x] `ctest --test-dir build --output-on-failure` — all three suites green in one run
- [x] `cmake --build build --target static_analysis` green (or tool-absent skips)
- [x] Standalone smoke tests: configure `icarus/` alone and `monkeybyte/` alone in scratch build dirs (dual-mode `else()` paths still work)
- [x] FetchContent smoke test: scratch consumer project fetching this repo builds **only** the library (no GTK required)
- [x] `./monkeybyte/run.sh`-style workflows still function (run.sh now supports both superbuild and standalone build trees) (GSETTINGS_SCHEMA_DIR, data dir resolution)

## Phase 6 — Docs & release

- [ ] Root `README.md` (GitLab front page: monorepo overview, build instructions, links into subfolders)
- [ ] Update root `AGENTS.md` (now describes the actual single repo, not a 3-repo workspace)
- [ ] Pointer edits in `daedalus/AGENTS.md`, `icarus/AGENTS.md`, `monkeybyte/AGENTS.md` (monorepo paths, new build commands)
- [ ] CHANGELOG entry (daedalus/CHANGELOG.md Unreleased: monorepo restructure)
- [ ] Push to GitLab; archive the old standalone `icarus` / `monkeybyte` repos (manual, user)

## Decisions locked in

- Prefixed host tags (`icarus/x.y.z`, `monkeybyte/x.y.z`); bare tags stay the library's
- Host subfolders remain standalone-configurable (dual-mode CMake) — no FetchContent of daedalus from GitLab inside this repo's own builds
- Host histories are NOT merged (archived in backups + old GitLab repos); library history is the monorepo history
