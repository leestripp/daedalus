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
- **Tests:** `ctest` → **66/66 pass**, 2 live-LLM skipped, 0 failures.
  - `messages_tests` 73/73 + 1 skipped (203/203 assertions; 14 new CRASH-* cases).
  - `token_usage_graceful_tests` 15/15 (38/38 assertions) — **NEW** graceful-degradation contract.
  - `test_acp_basic` 40/40 (131/131 assertions).
  - `test_acp_end_to_end` 4/4 (42/42 assertions).
  - `phase15_typo_warning_tests` re-enabled (no `-E` exclusion needed).
- **libcurl:** fully gone from the build system. HTTP/SSE stack is `OmniNet::HTTP::Client`.
- **Open items:** 
  1. ~~**Verify token-count resilience**~~ ✅ **COMPLETE** — Added `token_usage_graceful_tests` (15/15 pass) AND live integration test `/workspace/Testing/test_token_counts/run.sh` (12/12 pass with real minimax-m2.7 API calls). Verified end-to-end:
     - Non-stream mode: token counts deterministic (21 → 18,540 over multiple turns)
     - Stream mode (SSE): trailer-chunk recovery works, no crashes, counts observed (18,540/200000 = 9.3%)
     - `pass 2 (stream)` log shows: `[LLM] Stream finished with usage-only final chunk (recovered via synthesized_message; usage was non-null)` — the EOF timeout fix in `HttpSession::submit_cancellable` is confirmed working.

## Token Count Research (2026-07-17)

SSE streaming token counts are **not universally available** across providers:

| Provider | Available | Requirement |
|----------|-----------|-------------|
| OpenAI | ✅ Yes | Must send `stream_options: { include_usage: true }` (added May 2024) |
| Azure OpenAI | ✅ Yes | Same as OpenAI |
| Anthropic Claude | ⚠️ Partial | `message_delta` has cumulative counts, but inconsistent |
| MiniMax | ✅ Yes | Same as OpenAI + EOF handling fix needed |

**Conclusion**: This is NOT a MiniMax-only issue. It's an industry-wide challenge addressed with the EOF fix in `HttpSession::submit_cancellable` and proper `stream_options` configuration.

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
- `docs/token_counts.md` — Token count research: OpenAI/Anthropic/MiniMax comparison, SSE streaming availability, root causes.
- `docs/testing.md` — Two-layer test strategy (unit + integration). Includes `test_token_counts/` SSE graceful-degradation canary.

## Live Integration Test (2026-07-17)

End-to-end test at `/workspace/Testing/test_token_counts/`. Drives
the actual daedalus-cli with `--model minimax-m2.7` against the real
MiniMax API. Two passes:

- **Pass 1 (--no-stream):** 3/3 file assertions pass; hello binary builds with clang++; `./build/hello_world` prints "Hello, World!".
- **Pass 2 (--stream):** SSE streaming; trailer-chunk recovery verified via `[LLM] Stream finished with usage-only final chunk (recovered via synthesized_message; usage was non-null)`. Token counts climb: 18,049 → 18,540.

**Result: 12/12 checks pass.** Both modes complete the agent loop and
build a working hello-world C++17 project.
