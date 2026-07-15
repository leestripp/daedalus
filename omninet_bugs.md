# OmniNet bugs found in daedalus

> **Status (2026-07-15):**
> - **Bug 1** (first row lost) ✅ Fixed upstream in OmniNet 0.8.7 — see
>   [CHANGELOG.md](CHANGELOG.md) §v0.4.0.
> - **Bug 2** (phantom empty row after DELETE/reopen) ✅ Fixed upstream
>   in OmniNet 0.8.8-1-gfbecf55 — see [CHANGELOG.md](CHANGELOG.md)
>   §v0.4.0.
> - **Bug 3** (mutex held for iterator lifetime) ⚠️ Open, benign — see
>   below.

Only **open** issues are kept in this file. Closed bugs are folded
into the release notes in [CHANGELOG.md](CHANGELOG.md).

---

## Bug 3 — `QueryResult` mutex held for entire iterator lifetime

**Severity:** Low (benign — serialises concurrent queries, which is
safe but limits throughput)
**Component:** `OmniNet::DB::Database::QueryResult`
**Discovered in:** OmniNet 0.8.6 (pre-existing)
**Status:** ⚠️ Open, not blocking

`QueryResult` constructor calls `m_db.m_mutex.lock()`. The destructor
only unlocks if `!m_atEnd && m_stmt`. So when a query has any results,
the mutex is held from construction through full iteration —
serialising all concurrent queries against the same `Database`.

### Symptom

If two threads query the same `OmniNet::DB::Database` handle (the same
sqlite3 connection), the second query's `step()` will block until the
first query's iterator is fully consumed/destroyed. No corruption
(mutex is doing its job), but no parallelism either.

### Suggested upstream fix

In `QueryResult::fetchNext()`, on the no-more-rows path, call
`m_db.m_mutex.unlock()` so the destructor only needs to handle the
user-abandoned-mid-iteration case.

### daedalus status

Settings reads/writes are infrequent and not on a hot path, so the
serialisation is invisible. Left as-is. Re-evaluate if we ever need
concurrent SQLite access on a shared `Database` handle.