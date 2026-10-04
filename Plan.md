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

## Phase 4 — Versioning ✅ DONE (REVISED: single unified stream)

**Decision change (2026-09-24):** the prefixed-tag scheme (`icarus/x.y.z`,
`monkeybyte/x.y.z`) was implemented, then rejected by the user — a monorepo
should have ONE version. The prefixed tags were deleted (local + remote).
Going forward: bare `x.y.z` tags at the root cover the library AND both hosts.

- [x] daedalus lib: `git describe --tags --always --dirty` (original behaviour)
- [x] icarus: `git describe --tags --abbrev=0` → fallback `"dev"` (no `--match`)
- [x] monkeybyte: same, `^v?[0-9]+\.[0-9]+\.[0-9]+` with `v` strip
- [x] `git describe` runs against the repo in both top-level and standalone-subdir configures (any subdir works)

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

---

# Plan — Message headers: source + timestamp on every message (§MSG-HEADERS, 2026-10-04)

**Problem.** Messages are bare `{role, content}` JSON. The model cannot tell an
old/already-addressed message from a fresh one: compression summaries read like
a repeating event, triggers/A2A messages are indistinguishable from the human
user, crash-restored messages look new. Root cause: no origin, no time.

**Decision (approved by user).**
- (a) Header rendered into wire content + `_meta` stripped before send
  (never send unknown keys to strict providers — LM Studio/qwen3 templates,
  Anthropic conversion).
- (b) Free-form `kind:id` source string (not an enum/object) so future sources
  (devices, email, …) need zero schema change.

## Envelope

```json
{
  "role": "user",
  "content": "…",
  "_meta": { "source": "trigger:nightly-backup",
             "received_at": "2026-10-04 07:27:44 AEST" }
}
```

- `_meta` underscore-prefixed = internal, non-wire (matches `_invalid_args`).
- `received_at` stamped by the library at creation, always **UTC**
  (`%Y-%m-%d %H:%M:%S UTC`, §MSG-HEADERS-UTC — one standard reference
  regardless of where in the world agent or user run). Hosts never pass
  a timestamp.
- Source conventions: `user:<name>`, `assistant`, `tool:<name>`,
  `system:context-compression`, `system:context-status`, `a2a:<agent>`,
  `trigger:<name>`, `email:<addr>` — extensible by convention.

Wire rendering (payload-build time in `run_agent_cycle`):

```
[from: trigger:nightly-backup · received 2026-10-04 07:27:44 UTC]

<original content>
```

`_meta` is stripped from the wire copy. Stored chain keeps it.

## Changes by component

1. **daedalus core — Messages** (`src/core/Messages.{h,cpp}`):
   - `stamp(json& msg, std::string_view source)` static helper: writes
     `_meta.source` + `_meta.received_at` (current local time).
   - `add_system/add_user/add_assistant/add_tool_result` gain defaulted
     `source` param; always stamp `received_at`.
   - Compression synthetic summary message → `system:context-compression`.
   - Old chains without `_meta` load untouched (absence = legacy signal).
2. **daedalus api — LLMClient** (`src/api/LLMClient.cpp`, `ContextStatus.h`):
   - Raw `push_back` sites in `run_one_turn` (system + user) stamp via helper.
   - `run_agent_cycle`: build wire copy — expand `_meta` into the
     `[from: … · received …]` content prefix, strip `_meta`, send copy.
     Stored chain is never mutated by expansion (prefix-cache stability).
   - Assistant/tool-result pushes during the cycle stamp with role/tool name.
   - `upsert_context_status_message` → `system:context-status`.
3. **daedalus api — Agent** (`include/daedalus/Agent.h`, `src/library/AgentImpl.{h,cpp}`):
   - `submit_user_message(text, imgs, source)` overload; existing overloads
     default source to `user:<username>`.
   - `PendingUserMessage` gains `source` (queue/edit/clear API unchanged).
4. **Hosts**: `HistoryEntry` gains `source` + `received_at` (additive);
   icarus A2A backend passes `a2a:<peer>`; monkeybyte/icarus callers of
   submit_user_message unchanged (default source applies).
5. **Tests**: update exact-content expectations; add coverage: stamp on add_*,
   wire expansion + strip, compression-summary source, submit overload source,
   round-trip through save/load.

## Verification bar

Clean superbuild, full `ctest` green, `static_analysis` no new warnings.

---

# Plan — Queue join: drain ALL queued user messages as ONE message (§QUEUE-JOIN, 2026-10-04)

**Problem.** When the agent is busy, user messages accumulate in a queue.
On drain we dispatch exactly ONE message per turn, so a burst of messages
("do X", "also change Y", "correction: actually Z") is answered one at a
time — the model never sees the user's full intent in one context, answers
to a stale message arrive after newer ones, and N queued messages cost N
full turns. The agent doesn't care how many physical messages it receives;
the user often wants to add a correction right after a message. Joining
the whole queue into a single message at drain time is strictly better.

**Decision (approved by user, prompt.txt).**
- At drain time (agent becomes ready to run), JOIN every queued user
  message into ONE message, send it as a single turn, then the queue is
  empty. One turn per drain, not one turn per message.
- Queue semantics (submit / edit / remove / clear, FIFO order) are
  UNCHANGED while queued — joining happens only at the consume point.
- Text parts are joined with a blank line in FIFO order. Image
  attachments concatenate in queue order. No numbering/labels: the
  user's own words are the content (§MSG-HEADERS still stamps the single
  combined origin on the wire).

## Changes by component

1. **daedalus core — AgentImpl** (`src/library/AgentImpl.cpp`, §QUEUE-JOIN):
   - `run_one_turn()` dequeue block: instead of taking `front()` and
     erasing one element, consume the WHOLE `pending_user_messages_` under
     `pending_mutex_`: join texts with `"\n\n"`, concatenate images,
     merge `source` (all identical → that source; mixed → distinct
     sources in first-appearance order joined with `", "`, so the wire
     header stays truthful for e.g. `user:lee, a2a:peer`).
   - Single message consumed → the `Agent.h` off-by-one warning relaxes:
     run_one_turn() consumes the ENTIRE queue; extra submits while a turn
     runs simply accumulate for the next drain. Update the doc comment.
   - Host-facing queue API (`pending_count/pending_messages/edit/remove/
     clear`) untouched — hosts still see and edit individual messages
     while they wait.
2. **monkeybyte — ChatController** (`src/monkeybyte/ui/chat/ChatController.cpp`):
   - Turn-thread drain loop: pop ALL of `queued_messages_` in one
     state_mutex_ hold, join with `"\n\n"`, single
     `submit_user_message()` + `run_one_turn()`. Matches the daedalus
     change: while the user was typing during a busy turn, their whole
     burst becomes one message. FIFO preserved (error still stops the
     drain with leftovers queued for next send).
3. **icarus**: no change — the REPL blocks during `run_one_turn()` so its
   queue never accumulates interactively; A2A submits land in the daedalus
   queue and are covered by change 1.
4. **Tests**:
   - daedalus: extend `test_pending_messages.cpp` — submit 3 messages
     (incl. one with an image), `run_one_turn()` with no model configured
     (the existing no-client error path echoes `pending.text`): the
     terminal error must contain the joined text, images/source merged,
     and `pending_count()` must be 0 (whole queue consumed by one turn).
   - monkeybyte: `unit_tests` — ChatController-level join test if the
     drain loop is reachable headless; otherwise rely on the daedalus
     test + manual smoke.

## Verification bar

Clean superbuild, full `ctest` green, `static_analysis` no new warnings.
