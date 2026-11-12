# Scheduled agent tasks — DONE (2026-11-11)

Implemented persistent per-project/per-agent scheduling with a
`std::jthread` dispatcher and the `schedule_task` add/list/edit/delete tool.
One-shot UTC tasks delete themselves before dispatch; recurring tasks advance
by their fixed interval and coalesce missed intervals. Due prompts use the
normal pending queue with source `trigger:task-<id>` and wake idle hosts
through `AgentSink::on_turn_requested()`. If the agent is actively busy, the
occurrence is dropped permanently instead of queued; recurring tasks retain
their next future slot, preventing unbounded backlogs.

- Storage: `agent_dir/scheduled_tasks.json`, with atomic temp-file + rename
  mutations and fail-closed malformed-file handling.
- Lifecycle: scheduler starts on agent initialization and joins on shutdown
  or destruction; overdue work fires on startup only when a host is active.
- Verification: full superbuild, all 113 CTests, umbrella static analysis,
  and repeated timing/routing suites passed, including 20 busy/drop runs.
- Contract documented in `daedalus/docs/tools.md`, `docs/storage.md`, root
  `AGENTS.md`, and `daedalus/CHANGELOG.md`.

# Second Brain multi-process hardening — DONE (2026-11-09)

The Global Second Brain is now safe for cooperating MonkeyByte/Daedalus
processes sharing one user data root. The original isolated repro lost 20 of 40
successful concurrent append markers; the new suite verifies every append,
unique same-title IDs, consistent readers, mixed mutations, transactional
rollback, and kill/reopen recovery.

- Added bounded RAII `flock(2)` at `<brain>/.lock`: shared reads, exclusive
  mutations/reindex, explicit five-second timeout errors.
- Notes now use same-directory temp-file + `fsync` + atomic rename + directory
  `fsync`; stale temporary files are ignored by reindex.
- Metadata/FTS upserts, deletes, and complete reindexes are transactional.
- `SettingsStore` verifies WAL and serializes schema initialization with
  `BEGIN IMMEDIATE`, closing the startup race found during stress testing.
- Reindex uses nanosecond mtime plus size; the Markdown tree remains the source
  of truth and repairs file/index crash windows.
- Added `second_brain_concurrency_tests` for independent/same-title capture,
  concurrent append/read, update/move/delete/backlinks/reindex, lock timeout,
  forced index rollback, and `SIGKILL` + WAL recovery. All paths are temporary;
  the real brain was never touched.
- Verification: concurrency/crash suite passed 20 consecutive runs; full
  superbuild succeeded; all 111 CTests passed (three live-LLM tests skipped as
  designed); umbrella static analysis passed.
- Contract documented in `daedalus/docs/storage.md`, `settings.md`, `tools.md`,
  root `AGENTS.md`, and `daedalus/CHANGELOG.md`.

## ChimpFlow voice support for MonkeyByte — DONE (2026-10-25)

Implemented local ChimpFlow STT/TTS settings and discovery, native PipeWire
capture/playback, microphone dictation with review-before-send, voice previews,
and FIFO assistant narration. Verification included the full 109/109 CTest
suite, clean static analysis, live gateway/audio tests, isolated UI smoke, and a
matching tested/installed MonkeyByte binary. The durable documentation is in
`monkeybyte/README.md`, the MonkeyByte section of root `AGENTS.md`,
and `monkeybyte/docs/design.md` (§CHIMPFLOW).

## Daedalus completed tasks

- **2026-09-21 — Global encrypted vault:** AES-256-GCM secrets storage, global
  key handling, public API, and the non-secret-listing `vault` tool.
- **2026-09-10 — Agent API and queue hardening (commit `2b25f27`):** typed
  `AgentState` sink events, `CompressResult`, read-only history snapshots,
  public auto-flush configuration, terminal turn-error reporting, and the
  synchronized pending-message queue/cancel API.
- **2026-09-02 — MonkeyByte crash and subprocess hangs:** fixed the Messages
  flush-worker race with published snapshots (§PERSIST-RACE), hardened malformed
  chain handling, and bounded both subprocess hang paths. Regression/stress
  testing and 95/95 CTest were recorded in the original completed plan.
- **2026-08-31 — Persistence and PDF follow-ups:** malformed-chain no-throw
  handling (SANITIZE-NOTHROW) and restoration of `read_pdf` as a core tool.

## MonkeyByte completed tasks

- **System file associations:** the FileKind classifier/launcher, workspace-tree
  routing, editor binary guard, and unit-test checklist are complete. Non-text
  files open through the user's associated system application.
- **Flush-worker SIGSEGV investigation — RESOLVED (2026-09-10):** fixed upstream
  in Daedalus by §PERSIST-RACE published snapshots. The evidence remains in
  `monkeybyte/docs/upstream-repro/`; the separate plan file has been retired.
