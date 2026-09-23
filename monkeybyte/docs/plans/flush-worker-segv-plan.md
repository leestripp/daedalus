# Plan: daedalus flush-worker SIGSEGV (MonkeyByte crash)

> **RESOLVED upstream (2026-09-10):** fixed in daedalus as
> §PERSIST-RACE — `Messages` publishes `persist_snapshot_` at
> mark-dirty time under the lock and `save_impl()` copies the
> snapshot instead of the live chain. Kept for the record.

## Root cause (evidence: gdb catch, 2026-09-02)

MonkeyByte crashed under gdb. Backtrace (/tmp/mb_gdb.log, thread 50):

```
SIGSEGV in std::_Rb_tree<string, pair<const string, json>>::_M_root(this=0x0)
  stl_tree.h:1358
...
#18 Messages::save_impl()   build/_deps/daedalus-src/src/core/Messages.cpp:855
#19 Messages::save()        Messages.cpp:841
#20 Messages::flush_if_dirty() Messages.cpp:1146
#21 Messages::flush_worker_main() Messages.cpp:1187
```

`save_impl()` line 855 is `snapshot = messages_;` (under `messages_mutex_`).
The deep copy of the chain hits a `std::map<string,json>` node whose
internal pointer is null — i.e. one json value inside `messages_` is
**moved-from / half-mutated in memory**. This is the SAME signature as the
flush-worker crash daedalus 1.0.2 claimed to fix — but 1.0.2 wrapped
`flush_if_dirty()` in try/catch, and **try/catch cannot catch a SIGSEGV**.
Still present on daedalus main @ a671a4e.

Key facts:
- Persisted `<workspace>/.daedalus/daedalus/messages.json` parses fine
  (191 entries) — corruption is created **in-memory at runtime**, not
  loaded from disk. Wiping the file will NOT prevent recurrence.
- `MessageCompressionConfig::auto_flush_enabled` (src/core/Messages.h) is
  an INTERNAL header — not exposed in include/daedalus/, so the host
  (monkeybyte) cannot disable the flush worker via public API.
- The mutex is held during the snapshot, so either (a) some daedalus code
  path mutates `messages_` WITHOUT `messages_mutex_`, or (b) a moved-from
  json is appended to the chain (stream-recovery / orphan-tool-call
  cleanup paths are suspects — session logs show "Removing orphan
  tool_call (no result)" entries).

## Policy constraint

NEVER patch daedalus source (hard rule — upstream library, local patches
destroyed on refetch). The fix must come from upstream.

## Plan

1. [x] Write upstream bug report → `docs/upstream-repro/flush_worker_segv.md`
       (backtrace, root-cause analysis, suggested fixes: audit all
       `messages_` mutations for mutex coverage; defensive null-check in
       the snapshot copy; optionally run the snapshot copy in a forked
       child or guard with a signal handler if upstream wants belt+braces).
2. [x] Copy raw backtrace to `docs/upstream-repro/flush_worker_backtrace.txt`.
3. [x] MonkeyByte-side mitigation: NONE available via public API
       (auto_flush_enabled not exposed). Document the limitation in the
       report so upstream knows a host-side kill switch would help.
4. [ ] After upstream fix lands, refetch + rebuild + re-run the gdb repro
       to verify.
5. [x] Commit Plan.md + report files.

## User workaround until upstream fixes it

None that fully prevents it (worker is internal). Crash loses only the
current in-flight turn; restart the app and continue. `/clear` reduces
chain size (less to copy) but does not prevent corruption.
