# CodeMax — working memory

> **Transient.** This is the active session's working memory — what's
> in flight, what's about to be touched, what's recently shipped.
>
> For permanent history, see:
> - **`CHANGELOG.md`** — Keep-a-Changelog record of every shipped change.
> - **`docs/`** — per-feature specification, invariants, integration guides.

---

## Current state (2026-07-17)

- **Build:** clean (only OmniNet-internal warnings).
- **Tests:** `ctest` → **65/65 pass**, 2 live-LLM skipped, 0 failures.
  - `messages_tests` 73/73 + 1 skipped (203/203 assertions; 14 new CRASH-* cases).
  - `test_acp_basic` 40/40 (131/131 assertions).
  - `test_acp_end_to_end` 4/4 (42/42 assertions).
  - `phase15_typo_warning_tests` re-enabled (no `-E` exclusion needed).
- **libcurl:** fully gone from the build system. HTTP/SSE stack is `OmniNet::HTTP::Client`.
- **Open items:** none. See `CHANGELOG.md` for the v0.5.0 release notes.

---

## Recent activity

| Date | Item | Where |
|------|------|-------|
| 2026-07-17 | **SSE streaming token-count fix** — `HttpSession::submit_cancellable` was treating socket-wait timeouts as EOF, dropping the usage-only trailer chunk on slow providers (MiniMax/M2.7). Distinguishing `nullopt`-timeout from real EOF fixes `'usage' is null in SSE stream` and the zero-token-count UI bar. | [CHANGELOG v0.5.0](./CHANGELOG.md), [docs/sse_streaming.md](./docs/sse_streaming.md) |
| 2026-07-16 | **§CRASH-RESILIENCE** complete (5/5 phases). Atomic save, write-through auto-flush, async-signal-safe SIGSEGV/SIGABRT handler, drop-last-message recovery. | [CHANGELOG v0.5.0](./CHANGELOG.md), [docs/crash_resilience.md](docs/crash_resilience.md) |
| 2026-07-16 | **ACP sub-library** complete (16/16 tasks). `OmniNet` transport, structured SSE dispatch, e2e test. | [CHANGELOG v0.5.0](./CHANGELOG.md) |
| 2026-07-16 | **§NOMORE-CURL** complete (12/12 tasks). libcurl fully removed. | [CHANGELOG v0.5.0](./CHANGELOG.md) |
| 2026-07-16 | `Readme.md` + `docs/storage.md` updated with `messages.json.crash` row; new `docs/crash_resilience.md` + `docs/library_design.md` cross-refs. | [CHANGELOG v0.5.0](./CHANGELOG.md) |
| 2026-07-15 | Bug-sweep round 2 (B1–B32, F0–F8) closed. | [CHANGELOG v0.4.2 prep](./CHANGELOG.md) |

---

## Invariants

Permanent rules for `libdaedalus.a` code. See
**[docs/library_design.md](docs/library_design.md)** for the full
spec; the main one:

> **The library MUST NOT write to stdout, stderr, or stdin.** Use
> `AgentSink` for user-visible output and `core/Logger.h` for
> internal diagnostics. Demos (`demos/daedalus-cli/`) are exempt.

---

## Project layout

- `/workspace/daedalus` — C++ agent library + demos (CMake build).
- `/workspace/Testing` — manual + automated test suites for the demos.

## Related files

- `CHANGELOG.md` — what shipped, by version.
- `docs/index.md` — map of all `docs/*.md`.
- `docs/library_design.md` — invariants for `libdaedalus.a`.
- `docs/crash_resilience.md` — message persistence lifecycle.
- `docs/embedding.md` — how to host `libdaedalus.a`.
- `docs/sse_streaming.md` — SSE consumption pipeline, EOF gotcha, `usage`-trailer recovery. Read before touching `HttpSession` or `MiniMaxSseParser`.
