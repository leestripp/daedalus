# Active plan — Second Brain multi-process hardening (2026-11-09)

> **Status: PLANNED — DO NOT IMPLEMENT YET.** This section is the approved
> design for a later change. Wait until the other currently running agents have
> finished and lee explicitly asks for implementation. The only change authorised
> now is this plan.
>
> **Verdict driving this work:** the Second Brain is global per OS user and is
> shared across projects and hosts, but it is not currently safe for unrestricted
> simultaneous writes from separate MonkeyByte/Daedalus processes.

## 1. Goals and non-goals

### Goals

Make two or more MonkeyByte instances, on the same machine and user account,
safe to use the same Global Second Brain concurrently:

- No lost note contents from concurrent `capture`, `update`, `append`, `delete`,
  `file`, or `reindex` operations.
- Readers never observe a truncated or partially written Markdown note.
- Note IDs remain unique when processes capture the same title simultaneously.
- The Markdown files remain the source of truth; SQLite FTS5 remains a
  rebuildable index, not a second source of truth.
- Metadata and FTS updates are transactionally consistent with one another.
- A process crash releases its locks and leaves either complete old or complete
  new file content. A subsequent reindex repairs any file/index crash window.
- Lock contention and SQLite busy errors become bounded, contained tool errors;
  they must not crash or hang an agent indefinitely.
- Preserve the current user-visible API, PARA folders, note format, wiki-links,
  and global paths. No daemon and no new third-party dependency.

### Non-goals

- Do not make the brain project-local or change session-memory storage.
- Do not change the Markdown/frontmatter schema or note-ID format.
- Do not build a network/distributed locking protocol; this hardening is for
  local processes sharing one filesystem.
- Do not treat WAL as a substitute for logical file locking.
- Do not run concurrency tests against lee's real brain.

## 2. Confirmed current behaviour and failure evidence

The global locations are:

- Markdown source: `~/.local/share/daedalus/brain/` by default.
- SQLite index: `~/.local/share/daedalus/daedalus.db` by default.
- `DAEDALUS_HOME` / XDG overrides still apply. Processes with different user
  accounts or different data-root overrides do not share the same brain.

Relevant implementation facts:

- `SecondBrain::global()` deliberately ignores per-project `daedalus_home` and
  uses `Paths::global().user_data_root()` (`SecondBrain.cpp:177-184`).
- `SecondBrain::mu_` and the SettingsStore path mutex serialize threads within
  one process only. They do not coordinate two MonkeyByte processes.
- `capture()` performs a check-for-free-ID followed by a file write without an
  inter-process lock (`SecondBrain.cpp:239-290`).
- `update()` and `append()` are whole-note read/modify/write operations
  (`SecondBrain.cpp:301-332`), so separate processes can overwrite one another.
- Markdown output currently uses `std::ofstream(... | trunc)` directly on the
  destination (`SecondBrain.cpp:140-147`), not temporary-file-plus-rename.
- File output and index output are separate steps (`SecondBrain.cpp:506-522`).
  `upsert_brain_note()` issues the metadata upsert, FTS delete, and FTS insert
  as three independent statements (`SettingsStore.cpp:1601-1626`).
- SQLite has a five-second busy timeout (`SettingsStore.cpp:783-800`). The live
  database currently reports `journal_mode=delete`, not WAL.
- Existing Second Brain/settings tests pass, but they do not exercise separate
  processes sharing one brain.

An isolated two-process stress test against temporary storage demonstrated the
failure: each process successfully appended 20 distinct markers to the same
4 MiB note, with no reported errors, but only 20 of the expected 40 markers
survived (10 from each process). The large note deliberately widened the race
window; the unlocked read/modify/write defect applies to ordinary note sizes as
well. A baseline of 60 independent captures from two processes succeeded. All
test directories were removed and the real brain was untouched.

## 3. Design

### 3.1 Add an RAII inter-process brain lock

Add a small POSIX file-lock helper, preferably private to the core rather than
a new public Daedalus API:

- New files: `daedalus/src/core/InterprocessFileLock.{h,cpp}` (or an equivalently
  narrow `BrainFileLock`). Use the platform's existing POSIX support; no new
  dependency.
- Lock file: `<brain_root>/.lock`. It is runtime coordination state, not a note;
  keep it outside the five PARA folders. Do not put it in any `.daedalus*`
  directory.
- Use `flock` (or `fcntl` if portability review favours it) with an owned file
  descriptor and RAII release/close. The kernel releases the lock on normal
  exit, crash, or killed process. Never delete a lock file merely because it
  exists; existence does not mean the lock is held.
- Support shared and exclusive modes. Acquire in non-blocking mode with a short
  sleep/retry loop and a bounded deadline, handling `EINTR`. On timeout, return
  a clear error such as "Second Brain is busy in another process; try again".
  Never wait forever.
- Establish and document one lock order: `SecondBrain::mu_` → inter-process
  brain lock → SettingsStore/SQLite mutex. No code may acquire these in reverse.
- Keep the existing process-local mutexes; they remain necessary for threads
  inside each host.

Lock coverage:

| Operation | Lock |
|---|---|
| `capture`, `update`, `append`, `remove`, `move`, `reindex` | Exclusive |
| `read`, `backlinks` | Shared |
| `list`, `search` | Shared, so they cannot observe a half-updated metadata/FTS pair |

Refactor public methods into lock-taking wrappers and private unlocked helpers
where needed. In particular, `update`/`append`/`move` must not call a public
`read()` that tries to upgrade a nested shared lock to exclusive: separate
`flock` descriptors can deadlock even in one process. ID lookup, note reading,
and modification must occur under one exclusive critical section.

The lock protects logical file/index operations across processes. SQLite
continues to protect its own database pages and remains the coordination
mechanism for unrelated SettingsStore users.

### 3.2 Make every note-file write atomic

Replace direct truncate-and-write output with a same-directory atomic writer:

1. Create a uniquely named temporary file in the destination PARA folder using
   exclusive creation; include process/thread uniqueness and retry collisions.
2. Write all bytes, flush and check the stream, `fsync` the temporary file, and
   close it before publishing.
3. Rename the temporary file over the destination on the same filesystem.
   Readers then see either the complete old file or the complete new file.
4. `fsync` the parent directory where supported for crash durability.
5. Clean up stale temporary files on error. Set deliberate permissions matching
   the brain's existing user-tier privacy policy rather than relying on an
   accidental process umask.

Apply this helper to capture, update, append, and the new-file side of move.
Do not expose temporary names to note enumeration or FTS. Keep delete and move
ordering loss-averse: never remove the last complete copy before a complete
replacement exists. Under the exclusive brain lock, check move targets rather
than accidentally overwriting a duplicate ID.

File and SQLite commits cannot be one atomic transaction. The defined order is
**complete atomic file publication first, SQLite index transaction second**, all
under the exclusive brain lock. If the process dies between them, the complete
Markdown file wins and `reindex` reconciles the cache. Errors should state when
file content was published but indexing failed, rather than implying that no
change occurred.

### 3.3 Make each metadata/FTS change one SQLite transaction

In `SettingsStore`:

- Wrap the `brain_notes` upsert plus `brain_fts` delete/insert in one
  transaction for `upsert_brain_note()`.
- Wrap both deletes in `delete_brain_note()` in one transaction.
- Keep using the per-path recursive mutex and prepared statements. Check and
  propagate SQLite errors; roll back on every failure.
- Refactor internal row-write helpers so an outer reindex transaction cannot
  accidentally issue nested `BEGIN TRANSACTION` statements. One outer operation
  owns each transaction boundary.

For `reindex()`:

1. Hold the exclusive brain lock for the whole scan/apply operation.
2. Scan and parse Markdown into an in-memory desired change set first.
3. Apply all required metadata/FTS upserts and removals in one SQLite
   transaction, rather than holding a database transaction during filesystem
   iteration or committing each note independently.
4. Resolve any legacy duplicate-ID files deterministically and report them
   clearly; never silently discard note content during reconciliation.
5. Return accurate indexed/removed counts only after commit. On failure, roll
   back the entire index change set and leave the previous usable index intact.

A normal mutation updates its one index row before releasing the brain lock, so
readers see a consistent operation. A crash between file and DB commit remains
recoverable by reindex, as documented above.

### 3.4 Enable WAL and retain bounded busy handling

- Enable SQLite WAL when opening the SettingsStore database, after setting the
  busy timeout and before normal traffic. Verify the returned journal mode is
  actually `wal`; do not assume `PRAGMA journal_mode=WAL` succeeded.
- Keep the existing five-second SQLite busy timeout and bounded error
  containment. Add retries only if tests show they are necessary; never retry
  forever.
- WAL is for reader/writer concurrency and reduced contention. The inter-process
  brain lock and atomic file publication are still required for correctness.
- Document `daedalus.db-wal` / `daedalus.db-shm` and safe backup/checkpoint
  behaviour. Perform the first live WAL transition only after all running
  MonkeyByte/Daedalus processes using that DB have stopped.

## 4. Implementation sequence for the later authorised change

1. **Pin the bug hermetically.** Add a standalone two-process integration test
   using temporary XDG/home paths. Include the concurrent-append regression
   above and same-title captures. Confirm it fails before the fix and never
   touches the real user store.
2. **Add lock + atomic writer.** Implement the RAII lock, lock-order/unlocked
   helper refactor, and atomic note publication. Run the race test; all appends
   and captures must survive.
3. **Add index transactions.** Make single-note and delete operations atomic at
   the SQLite level, then make reindex scan-first/apply-in-one-transaction.
4. **Enable WAL.** Add verified WAL setup and lock-contention tests. Keep this
   separate in review from the logical race fix so WAL cannot mask a missing
   inter-process lock.
5. **Exercise crash/recovery and error paths.** Add killed-writer, lock-timeout,
   and index-failure tests; verify complete files, released locks, rollback,
   and reindex recovery.
6. **Run full verification and document.** Only after isolated multi-process
   tests pass, run the full superbuild/CTest/static-analysis bar and update the
   storage/settings/agent documentation and changelog.

## 5. Required tests and acceptance criteria

### Hermetic multi-process integration tests

Use `fork`/`exec` (or two independently executed test workers) with a fresh
temporary brain per test and explicit start barriers. Do not inherit one SQLite
connection across `fork`.

- **Concurrent append:** two processes append 20 unique markers each to one
  note. All 40 markers and the original body must survive; every marker appears
  exactly once. Repeat enough times to catch flaky scheduling.
- **Same-title capture:** both processes capture identical titles concurrently.
  Every call returns a distinct ID; all Markdown files and index rows exist;
  no body is overwritten.
- **Independent capture/update:** parallel distinct-note writes all survive and
  are searchable.
- **Concurrent update:** operations serialise so a later operation reads the
  latest complete note; no silent stale read/modify/write loss.
- **Move/delete/update combinations:** outcomes may be legitimate
  not-found/lock-timeout results, but never truncated files, accidental
  resurrection, duplicate note IDs, or index rows pointing at missing content.
- **Readers during writes:** `read`, `search`, `list`, and `backlinks` never see
  partial Markdown or a half-applied metadata/FTS update.
- **Reindex during writes:** after activity quiesces, disk and index agree
  exactly; no newly written note is removed by a stale reindex scan.
- **Busy handling:** hold the brain lock and SQLite lock independently in test
  workers; operations either succeed within their bounds or return a contained,
  explanatory error without corrupting data.

### Atomicity and crash tests

- A reader looping during large-note replacement sees only complete old or new
  content, never an empty/truncated/intermediate target.
- A failed temporary write/rename leaves the previous destination intact.
- Killing a writer releases the inter-process lock; another process can proceed
  without manual lock-file deletion.
- Killing a writer before publication preserves the old note; killing it after
  publication leaves a complete new note. A crash between file publication and
  index commit is repaired by `brain reindex`.
- Fault injection between metadata update, FTS delete, and FTS insert proves
  rollback restores the prior index state.
- Stale temporary files are not parsed as notes and are cleaned up safely.

### Regression and release gate

- Existing `second_brain_tests`, `brain_tool_tests`, and SettingsStore suites
  remain green.
- New multi-process suites pass repeatedly (not just once) with no timeout or
  `database is locked` flakes.
- Clean superbuild, full `ctest --test-dir build --output-on-failure`, and
  umbrella `static_analysis` pass with no new warnings; `git diff --check` is
  clean.
- Run one final isolated two-MonkeyByte smoke against a temporary shared data
  root. Do not experiment on lee's real notes.

**Acceptance means:** unrestricted concurrent brain use by two local MonkeyByte
processes no longer loses successful writes, readers never see partial files,
and crash/reindex recovery is demonstrated. Merely passing the old single-
process tests or enabling WAL is not acceptance.

## 6. Expected files and documentation for implementation

Likely implementation surfaces (adjust only if code inspection justifies it):

- `daedalus/src/core/InterprocessFileLock.{h,cpp}` — new RAII primitive.
- `daedalus/src/core/SecondBrain.{h,cpp}` — lock coverage, unlocked helpers,
  atomic writes, loss-averse move/delete and reindex coordination.
- `daedalus/src/core/SettingsStore.cpp` and, only if necessary,
  `daedalus/include/daedalus/settings.h` — brain-index transaction/batch support
  and verified WAL setup.
- `daedalus/tests/test_second_brain.cpp`, a new
  `test_second_brain_concurrency.cpp`, relevant SettingsStore tests, and
  `daedalus/tests/CMakeLists.txt`.
- `daedalus/docs/storage.md`, `daedalus/docs/settings.md`, root `AGENTS.md`, and
  `daedalus/CHANGELOG.md` after behaviour is verified. Document global scope,
  lock/crash semantics, WAL files/backups, and the fact that project-tier
  session memory remains separate.

No implementation, live database migration, or real-brain reindex is authorised
by this planning step.


---

# Completed plan history

> These are DONE summaries, not pending work. Detailed implementation records
> live in Git history, the component documentation, and changelogs. All active
> planning belongs above in this single root-level `plan.md`; do not recreate
> component-specific plan files.

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
