# Testing

How `libdaedalus.a` is tested — the two complementary test layers,
how they were built, and how to run them.

**Audience:** core contributors adding tests, debugging regressions,
or running pre-release verification.

---

## Overview

Two layers, each with a distinct purpose:

| Layer | Where | Speed | External deps | Purpose |
|-------|-------|-------|---------------|---------|
| **Unit tests** | `tests/test_*.cpp` (CTest targets) | Fast (≤6s total) | None | Verify individual components in isolation |
| **Integration tests** | `/workspace/Testing/test_*/` | Slow (1–10 min each) | Real MiniMax API | Verify end-to-end behavior with live LLM |

The unit tests are wired into CMake and run automatically on every
build via `ctest`. The integration tests live in a separate
repository (`/workspace/Testing/`) and require API credentials.

### Test pyramid

```
                    ┌──────────────┐
                    │ Integration  │   ← /workspace/Testing/test_*/
                    │   tests      │      Real LLM, real filesystem
                    ├──────────────┤
                    │  Unit tests  │   ← tests/test_*.cpp
                    │              │      doctest, mocked deps
                    └──────────────┘
```

Unit tests catch regressions quickly. Integration tests catch
behavioural drift that mocks can't simulate (e.g. real LLM SSE timing,
real chunk spacing, real model behavior).

---

## Unit Tests (`tests/`)

### Running

```bash
cd /workspace/daedalus
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

Expected: **66/66 pass**, 2 live-LLM skipped, 0 failures.

### Conventions

- All tests use **doctest** (`#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`).
- Test files are named `test_<component>.cpp`.
- Each test compiles into a standalone executable target named
  `daedalus_<name>_tests`.
- Targets are registered with `add_test()` in `tests/CMakeLists.txt`.
- Tests that require live LLM calls use `SKIP_RETURN_CODE 77` and the
  `live-llm` label. They are skipped unless `DAEDALUS_RUN_LIVE_LLM_TESTS=1`.

### Key test files

| Test | Purpose |
|------|---------|
| `test_messages.cpp` | Split/replace/compression logic (73 tests) |
| `test_minimax_sse_parser.cpp` | SSE wire format, `is_usage_only_chunk()` gate (BUG-001) |
| `test_token_usage_graceful.cpp` | Contract: missing/null `usage` JSON → tokens=0 + warning, no throw (15 tests) |
| `test_token_counts_sse.cpp` | Live LLM smoke test for SSE token-count path (skipped by default) |
| `test_no_content_duplicate.cpp` | Regression: streamed text must not duplicate in `content` (skipped by default) |
| `test_http_session.cpp` | HTTP session lifecycle, retries, cancellation |
| `test_acp_basic.cpp`, `test_acp_end_to_end.cpp` | ACP sub-library protocol tests |
| `test_crash_resilience.cpp` | Atomic save, SIGSEGV handler, drop-last-message recovery |
| `test_metrics.cpp` | Token/iteration metrics accounting |

### Adding a new unit test

1. Create `tests/test_<component>.cpp` with doctest.
2. Add an `add_executable` + `add_test` block in `tests/CMakeLists.txt`.
3. Reconfigure CMake: `cmake -S . -B build`.
4. Build & run: `cmake --build build -j$(nproc) --target <your_test>`.

---

## Integration Tests (`/workspace/Testing/`)

End-to-end tests that drive the actual `daedalus-cli` binary against
the real MiniMax API. Each test:

- Spawns `daedalus-cli` in a temp project + state directory.
- Sends scripted prompts via PTY (`script -q -e -c ...`).
- Captures stdout, stderr, and full REPL transcript.
- Asserts on file artifacts, command outputs, and log markers.

### Running a single test

```bash
bash /workspace/Testing/test02_hello_world/run.sh
```

### Running all tests

```bash
bash /workspace/Testing/run_all.sh
```

### Bootstrap pattern

Every integration test follows the same shape:

```bash
# 1. Clean runtime dir
RUNTIME_ROOT="/tmp/dt-<test-name>"
rm -rf "${RUNTIME_ROOT}"
mkdir -p "${RUNTIME_ROOT}/project" "${RUNTIME_ROOT}/state"

# 2. Bootstrap per-test state with the real daedalus.db
#    (v0.5.0+ schema is SQLite, not settings.json)
cp ~/.local/share/daedalus/daedalus.db "${RUNTIME_ROOT}/state/daedalus.db"

# 3. Run via the shared test_runner.py
python3 /workspace/Testing/lib/test_runner.py \
    --project "${RUNTIME_ROOT}/project" \
    --state   "${RUNTIME_ROOT}/state" \
    --prompts /workspace/Testing/<test>/prompts.txt \
    --model   "minimax-m2.7" \
    --timeout 300 \
    --check-file     "<expected-artifact>" \
    --check-contains "<expected-content>"

# 4. Verify build/run outputs in the project dir
(cd "${RUNTIME_ROOT}/project" && cmake -S . -B build && cmake --build build)
"./build/<binary>"   # must produce expected output
```

The shared helpers live in `/workspace/Testing/lib/test_runner.py`.

### Existing integration tests

| Test | What it verifies |
|------|------------------|
| `test01` | Sanity: REPL boots, banner prints |
| `test02_hello_world` | Agent creates `hello.txt` with exact content |
| `test03_fizzbuzz` | Multi-step coding task |
| `test04_webui` | WebUI host integration |
| `test05_webui_model_visible` | Model is selectable in WebUI |
| `test06_bug001` | BUG-001 regression — SSE parser `is_usage_only_chunk()` gate |
| `test99` | Reserved for ad-hoc regression |
| `test_token_counts/` | **Token-count graceful degradation — see below** |

---

## `test_token_counts/` — Token-count graceful-degradation test

The most recent integration test (2026-07-17). Verifies that the
agent loop continues when token counts are missing/null during SSE
streaming, and that token counts eventually appear via the trailer
chunk recovery.

### Why this test exists

Prior to the SSE EOF fix (v0.5.0), MiniMax streaming would drop the
`usage`-only trailer chunk on slow connections, leaving the agent
loop with `last_total_tokens_ == 0` forever. The fix in
`HttpSession::submit_cancellable` (distinguishing socket-wait
timeouts from real EOF) makes the trailer recovery reliable. This
integration test is the canary.

### What it does

Two passes against the real MiniMax API, both must complete a
hello-world C++17 project from scratch:

| Pass | Mode | Token-count behavior |
|------|------|----------------------|
| 1 | `--no-stream` (buffered JSON) | Deterministic: 18,000+ tokens observed |
| 2 | `--stream` (SSE) | Trailer recovery: counts climb 0 → 18,000+ across turns |

Both passes assert:

1. `CMakeLists.txt` and `README.md` were created.
2. `main.cpp` exists and prints `Hello, World!`.
3. The binary builds with clang++.
4. `Token Usage: <N>` appears in the log (real counts).
5. The agent loop completes without exception or crash.

### File layout

```
/workspace/Testing/test_token_counts/
├── README.md         # This file's mirror
├── run.sh            # Main driver: bootstrap, run, assert
├── stream_runner.py  # Custom Python wrapper for pass 2 (--stream)
└── prompts.txt       # The 2 prompts sent to the agent
```

### Running

```bash
bash /workspace/Testing/test_token_counts/run.sh
```

Output ends with:

```
SUMMARY: 12 passed, 0 failed
✓ All checks passed
```

### Why a custom `stream_runner.py`?

`/workspace/Testing/lib/test_runner.py` always passes `--no-stream`
(deterministic token counts). Pass 2 of this test needs `--stream`
enabled to exercise the SSE graceful-degradation path. The wrapper
in `stream_runner.py` mirrors the runner's API but uses `--stream`.

### Why a SQLite bootstrap?

`test_runner.py`'s `bootstrap_settings()` copies `config.json` to
`<state>/settings/settings.json`, but v0.5.0+ reads from
`<state>/daedalus.db` (SQLite). `run.sh` copies the real
`~/.local/share/daedalus/daedalus.db` so the agent can load the
`minimax-m2.7` model.

### Evidence captured

Latest run shows:

```
Pass 1 (--no-stream): Token Usage: 18084
Pass 2 (--stream):    Token Usage: 17 → 17766
```

Pass 2's log shows the recovery path firing on every streaming turn:

```
[LLM] Stream finished with usage-only final chunk
      (recovered via synthesized_message; usage was non-null)
```

This is exactly the contract documented in
[`token_counts.md`](token_counts.md) § "Graceful Degradation".

---

## When to add which test

| You're testing… | Use |
|-----------------|-----|
| A helper function, parser, gate | Unit test in `tests/test_*.cpp` |
| Tool JSON schema validation | Unit test in `tests/test_*.cpp` |
| Crash handler, atomic save | Unit test in `tests/test_*.cpp` |
| End-to-end agent behavior | Integration test in `/workspace/Testing/test_*/` |
| Token count + LLM API contract | Integration test (see `test_token_counts/`) |
| SSE streaming with real LLM | Integration test (skipped unless `DAEDALUS_RUN_LIVE_LLM_TESTS=1`) |

---

## Related

- [`sse_streaming.md`](sse_streaming.md) — SSE pipeline, EOF gotcha, trailer recovery
- [`token_counts.md`](token_counts.md) — Token-count research + graceful-degradation contract
- [`crash_resilience.md`](crash_resilience.md) — What the crash-recovery unit tests verify
- [`embedding.md`](embedding.md) — How `AgentSink` is used in tests
- `tests/CMakeLists.txt` — Test target definitions
- `/workspace/Testing/README.md` — Integration-test layout & contracts
- `/workspace/Testing/lib/test_runner.py` — Shared integration-test helper