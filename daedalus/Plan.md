# Plan.md — Active Tasks

> Living task list. Completed tasks stay in place with a DONE marker and the
> commit/date that landed them.

---

## TASK-2026-09-21-01: VAULT — global encrypted secrets store (password manager)

> **Status** DONE (2026-09-21; commit pending)
> **Created**: 2026-09-21
> **Source**: user request — daedalus needs a password manager for logins,
> notes, API keys, etc.; secure; **global** (usable from every project,
> not per-project); SQLite-backed (existing daedalus DB chosen).

### Problem

Agents constantly need credentials (API keys, logins) and there is no
sanctioned place to keep them — they end up in cleartext `.env` files,
skill notes, or chat history. Requirements:

- **Secure at rest** — secrets encrypted, not plaintext rows.
- **Global** — one vault per user, reachable from every project/agent,
  independent of `AgentConfig::daedalus_home`.
- **Agent-usable** — a tool so the agent can store/retrieve secrets
  non-interactively, plus a public C++ API for hosts.

### Design decisions

- **Storage: the existing global DB.** New `vault_entries` table in the
  `SettingsStore` SQLite DB (`$XDG_DATA_HOME/daedalus/daedalus.db`,
  schema v6). The vault ALWAYS uses the global store +
  `SettingsStore::instance()` regardless of per-agent `daedalus_home` —
  that is what makes it global across projects. (Tests inject a
  temp-home store explicitly.)
- **Crypto: AES-256-GCM via OpenSSL EVP** — OpenSSL is already a public
  dependency; no new deps. Random 12-byte nonce per entry, 16-byte tag,
  AAD = entry name (ciphertext is bound to its key; rows cannot be
  swapped). Binary (nonce ‖ ciphertext ‖ tag) is base64'd into TEXT
  columns to match the store's string-only query helpers.
- **Master key: `vault.key` file.** 32 random bytes (`RAND_bytes`),
  written once to `<user_config_root>/vault.key` with 0600 perms (the
  config root is already owner-only 0750). Security level ≈ `~/.ssh`
  keys: filesystem permissions are the boundary, unlock is
  non-interactive (required for unattended agent access). A
  passphrase-wrapped key is explicitly future work.
- **Plaintext metadata, encrypted payload.** `name`, `kind`
  (`login`/`note`/`api_key`/`custom`), `description`, timestamps stay
  plaintext so `list` is useful without decryption; the secret fields
  (a JSON object) are the only encrypted blob.
- **Agent surface: ONE `vault` tool** with `action` ∈
  `list | get | store | edit | delete` (keeps the function-schema surface
  small). `list` NEVER returns secret values; `store` confirms without
  echoing; `get` returns the decrypted fields (they enter conversation
  history — documented; hosts can strip via tools_disabled).

### Scope

1. `src/core/SettingsStore.cpp`: `kSchemaVersion` 5 → 6;
   `migrate_to_v6()` + `kSchemaSql` gain `vault_entries(name PK, kind,
   description, nonce_b64, payload_b64, created_at, updated_at)`.
2. `include/daedalus/vault.h` + `src/core/Vault.cpp`: public
   `daedalus::SecretsVault` —
   `static SecretsVault global()` (global store + global key file) and
   `SecretsVault(SettingsStore, key_file)` (tests/per-DB);
   `store(VaultEntry)` / `get(name)` / `list()` (metadata only) /
   `remove(name)` / `exists(name)`. Thread-safe; throws
   `std::runtime_error` on crypto/IO failure. Key file auto-generated
   on first use (0600), zeroised on destruction where practical.
3. `src/tools/VaultTool.{h,cpp}`: `vault` tool with `action` enum;
   registered in `ToolFactory` ctor. No secret value is ever logged or
   included in `list`.
4. `CMakeLists.txt`: new sources.
5. `tests/test_vault.cpp`: hermetic temp-home — encrypt/decrypt
   round-trip, tamper detection (flipped byte fails GCM), wrong-key
   rejection, `list` hides fields, tool actions end-to-end, v5→v6
   migration on an existing DB.
6. `CHANGELOG.md`, `docs/vault.md`, Readme cross-ref.

### Verification

- New tests pass; full ctest green; warning-clean under project flags.
- `strings`-level check: stored secret values do not appear in the DB
  file as plaintext (base64-of-ciphertext only).

---

## TASK-2026-09-10-06: STATECHANGE-TYPED — type-safe `AgentSink::on_state_change` (REQ-5)

> **Status** DONE (2026-09-10, commit 2b25f27)
> **Created**: 2026-09-10
> **Source**: prompt.txt (MonkeyByte upstream report vs d73d1ba), REQ-5

### Problem

`AgentSink::on_state_change(int from, int to)` passes states as
cast-to-int, forcing every host to re-implement the cast and risking
enum/int protocol drift.

### Scope

1. `include/daedalus/AgentSink.h`: add overload
   `on_state_change(daedalus::AgentState from, daedalus::AgentState to)`;
   default forwards to the legacy int overload so existing hosts keep
   working unchanged. Emitters call the typed overload.
2. `src/library/AgentImpl.cpp` (`set_state`): call the typed overload.
3. `tests/test_state_change_routing.cpp` (or new): typed sink receives
   AgentState values; legacy int-only sink still receives events via
   forwarding.
4. `CHANGELOG.md`.

### Verification

- New/updated tests pass; full ctest green; warning-clean.

---

## TASK-2026-09-10-05: COMPRESS-RESULT — `compress_context()` outcome feedback (REQ-3)

> **Status** DONE (2026-09-10, commit 2b25f27)
> **Created**: 2026-09-10
> **Source**: prompt.txt REQ-3

### Problem

`Agent::compress_context()` is `void`: a too-short history silently
no-ops (and silently arms a carry-over request), a busy turn silently
defers, and a missing model silently does nothing. Hosts must duplicate
internal thresholds to give the user any feedback.

### Scope

1. New public `daedalus::CompressResult` enum
   (`Compressed`, `Deferred`, `NothingToCompress`, `NoModel`, `Failed`)
   in `include/daedalus/types/CompressResult.h` (+ `to_string`).
2. `src/api/LLMClient.{h,cpp}`: `compress_context()` returns the mapped
   outcome (immediate path maps `CompressOutcome`; busy/try-lock paths
   return `Deferred`). Carry-over re-arm semantics unchanged
   (NothingToCompress/Failed still arm the next-turn retry — documented).
3. `AgentImpl` / `Agent`: `compress_context()` returns
   `CompressResult::NoModel` when no LLMClient exists; declaration in
   `include/daedalus/Agent.h` updated (`noexcept` retained; adding a
   return value is source-compatible for callers ignoring it).
4. Tests: NoModel without credentials; enum mapping unit coverage.
5. `CHANGELOG.md`, `docs/how_compress_works.md` cross-ref if needed.

### Verification

- Build warning-clean (callers ignoring a nodiscard-free enum are fine);
  ctest green.

---

## TASK-2026-09-10-04: HISTORY-SNAPSHOT — read-only conversation history view (REQ-2)

> **Status** DONE (2026-09-10, commit 2b25f27)
> **Created**: 2026-09-10
> **Source**: prompt.txt REQ-2

### Problem

No public way to read back the persisted/restored conversation, so every
GUI host keeps a parallel chat store that silently diverges after
compression/sanitization.

### Scope

1. New public `daedalus::HistoryEntry { role, content }` in
   `include/daedalus/types/HistoryEntry.h`.
2. `src/core/Messages.{h,cpp}`: `history_snapshot()` projection —
   user/assistant/system entries with their text content (array-form
   content flattened to text parts; pure tool-call assistant messages and
   `tool` role entries elided). Driving-thread contract.
3. `src/api/LLMClient` + `AgentImpl` + `Agent::history_snapshot()`
   forwarders; empty vector when no model configured.
4. Tests: seed a Messages chain (incl. tool_calls, multipart content),
   verify projection; Agent-level empty-without-model case.
5. `CHANGELOG.md`.

### Verification

- New tests pass; full ctest green; warning-clean.

---

## TASK-2026-09-10-03: AUTO-FLUSH-CFG — public kill switch for the flush worker (REQ-1)

> **Status** DONE (2026-09-10, commit 2b25f27)
> **Created**: 2026-09-10
> **Source**: prompt.txt REQ-1

### Problem

`MessageCompressionConfig::auto_flush_enabled` is internal-only; tests
and short-lived/embedded agents cannot disable the 100 ms-tick flush
worker through the public API.

### Scope

1. `include/daedalus/types/AgentConfig.h`: `bool session_auto_flush = true;`.
2. `src/api/LLMClient.h` `AppConfig`: matching field.
3. `AgentImpl::make_app_config`: copy through.
4. `src/api/LLMClient.cpp` (both Messages construction branches):
   `messages_config.auto_flush_enabled = config.session_auto_flush;`
   When disabled, saves still happen synchronously at existing quiescent
   points (Messages already handles `auto_flush_enabled == false`).
5. `CHANGELOG.md`.

### Verification

- Build + full ctest (existing Messages flush tests cover the
  disabled-worker behaviour at the core level).

---

## TASK-2026-09-10-02: TURN-ERROR — terminal failure visible after `run_one_turn()` (BUG-4)

> **Status** DONE (2026-09-10, commit 2b25f27)
> **Created**: 2026-09-10
> **Source**: prompt.txt BUG-4

### Problem

A terminal LLM failure (fatal 4xx, retries exhausted) ends the turn with
public `AgentState::Done`, indistinguishable from success. Hosts must
track `on_error(will_retry=false)` themselves across the turn boundary.

### Scope (query API — no state-machine semantics change)

1. `src/api/LLMClient.{h,cpp}`: record terminal errors —
   `emit_error(will_retry=false)` latches
   `turn_terminal_error_{kind,message}` (mutex-guarded; emit_error may
   run on HTTP worker threads); cleared at `run_one_turn()` entry.
   Getters: `turn_had_terminal_error()`, `last_terminal_error()`.
2. New public `daedalus::AgentError { kind, message }` in
   `include/daedalus/types/AgentError.h`.
3. `AgentImpl`: mirror the LLMClient outcome after each turn plus its
   own pre-dispatch terminal errors (no-model path); public
   `Agent::turn_had_terminal_error() const noexcept` and
   `Agent::last_error() const -> std::optional<AgentError>`.
   Driving-thread contract (same as the queue API).
4. `include/daedalus/AgentSink.h`: update the `on_error` doc note that
   currently tells hosts they MUST track terminal errors themselves.
5. Tests: no-model turn sets the flag + last_error; successful/idle
   paths leave it clear.
6. `CHANGELOG.md`.

### Verification

- New tests pass; full ctest green; warning-clean.

---

## TASK-2026-09-10-01: QUEUE-MUTEX — `cancel()` queue race + synchronized queue API (BUG-3 / REQ-4)

> **Status** DONE (2026-09-10, commit 2b25f27)
> **Created**: 2026-09-10
> **Source**: prompt.txt BUG-3 (fix option 1) + REQ-4

### Problem

`Agent::cancel()` reads `pending_user_messages_.empty()` unguarded while
the driving thread may be inside `submit_user_message()` /
`run_one_turn()` on the same vector — a data race (UB) through the one
API entry point hosts are most likely to call cross-thread (a UI Stop
button). Same root cause as the 2026-09-10 queue thread-safety report.

### Scope

1. `src/library/AgentImpl.h`: add `mutable std::mutex pending_mutex_`;
   guard `submit_user_message` (both), `pending_count`,
   `pending_messages`, `edit_pending_message`,
   `remove_pending_message`, `clear_pending_messages`, the `cancel()`
   `empty()` check, and the `run_one_turn()` peek/dequeue sequence
   (lock released before dispatching the turn so cross-thread
   submit/cancel stay live during the turn).
2. `include/daedalus/Agent.h`: update the threading contract — the queue
   API and `cancel()` become callable from any thread; keep
   `run_one_turn()` driving-thread-only.
3. `tests/test_pending_messages.cpp` (or new
   `test_queue_thread_safety.cpp`): concurrent submit/clear/
   pending_messages/cancel smoke test with invariant checks.
4. `CHANGELOG.md`.

### Verification

- New concurrency test passes repeatedly; full ctest green;
  warning-clean.

---

## TASK-2026-08-31-02: SANITIZE-NOTHROW — malformed chain entries SIGABRT the flush worker

> **Status**: DONE (2026-08-31, uncommitted working tree)
> **Created**: 2026-08-31

### Problem

MonkeyByte coredump (2026-08-30): `Messages::sanitize_message_chain()` used
`entry.value("role", ...)` / `.value("tool_call_id", ...)` / `.value("id", ...)`,
which throw `json::type_error` when a chain entry is not an object, or when
`role`/`tool_call_id`/`id` is present but null or non-string. The exception
escaped `save()` onto the background flush worker thread → `std::terminate`
→ SIGABRT — intermittent, fired only when a malformed/partially-written chain
entry met a flush tick. Follow-up to the `daedalus_bugs.md` crash work
(§CRASH-6, 2026-08-05).

### Scope

1. `src/core/Messages.cpp`:
   - defensive string extraction (`chain_safe_str()`) in
     `sanitize_message_chain()` and `sanitize_assistant_message()`; drop
     non-object chain entries, tool results with missing `tool_call_id`,
     non-object `tool_call` entries, and `tool_call`s with non-object
     `function` instead of throwing
   - `save_impl()`: catch sanitize failures, re-mark `dirty_`, return
   - `flush_worker_main()`: last-resort catch around `flush_if_dirty()` so
     no persistence error can ride the worker into `std::terminate`
2. `tests/test_message_chain_validity.cpp`: SANITIZE-NOTHROW suite
3. `CHANGELOG.md` (Unreleased → Fixed)

### Verification

- `daedalus_message_chain_validity_tests` passes (incl. new suite)
- `ctest --test-dir build --output-on-failure` passes

---

## TASK-2026-08-31-01: Move `read_pdf` (PDFTool) back into core agent tools

> **Status**: DONE (2026-08-31)
> **Created**: 2026-08-31

### Problem

`ReadPDFTool` (`read_pdf`) was moved out of `libdaedalus` into
`demos/example-tools/` during the §TOOL-TRIM work, but poppler-cpp is still a
**REQUIRED** dependency of the library build
(`pkg_check_modules(POPPLER REQUIRED poppler-cpp)` in the root
`CMakeLists.txt`). Requiring the dependency while shipping the tool only as an
opt-in example is inconsistent — `read_pdf` should be a built-in agent tool,
like `read_image` and `serper_search` (which moved back for the same reason).

### Scope

1. `git mv demos/example-tools/PDFTool.{h,cpp}` -> `src/tools/`
2. Register `ReadPDFTool` in `ToolFactory::initialize_tools()` (§PDF-CORE)
3. Root `CMakeLists.txt`:
   - add `src/tools/PDFTool.cpp` to `TOOLS_SOURCES`
   - add `${POPPLER_INCLUDE_DIRS}` / `${POPPLER_LIBRARIES}` to the `daedalus`
     target (PRIVATE); fix the stale "PDF tools moved to demos" comment
4. `demos/example-tools/`: drop `PDFTool.cpp` from the example-tools library,
   remove poppler from its includes/links, drop the include from
   `example_tools.h`
5. `demos/daedalus-cli/main.cpp` + `demos/daedalus-webui/session_manager.h`:
   remove the explicit `ReadPDFTool` registration (now registered by default)
6. `tests/CMakeLists.txt`: point `daedalus_tool_definitions_tests` at the new
   `src/tools/PDFTool.cpp` path; assert `read_pdf` in default tool set
7. Docs: `docs/tools.md` (core tool table + notes), `AGENTS.md`
   (project-structure table), `CHANGELOG.md` (Unreleased)

### Verification

- `cmake --build build -j$(nproc)` succeeds
- `ctest --test-dir build --output-on-failure` passes
- `read_pdf` appears in the default tool definitions

---

## TASK-2026-09-02-01: Fix MonkeyByte crash + hangs from `daedalus_bugs.md`

> **Status**: DONE (2026-09-02) — 95/95 ctest green; race reproduced
>   5/8 SIGSEGV pre-fix, 10/10 clean post-fix, TSAN-clean; both Subprocess
>   hangs reproduced pre-fix (external timeout kill) and fixed.
> **Created**: 2026-09-02
> **Source**: `daedalus_bugs.md` (MonkeyByte GTK4 host, daedalus main post-1.0.2)

### BUG 1 — SIGSEGV: flush worker crashes deep-copying `messages_`

**Root cause (confirmed by backtrace `_M_root(this=0x0)` = torn read of a
json object header):** the LLMClient turn path mutates `messages_` through
the raw `get_messages()` reference WITHOUT holding `messages_mutex_`, while
the flush worker deep-copies `messages_` in `save_impl()` under the mutex.
The mutex is useless when one side doesn't hold it. §PERSIST-DIRTY
(5d7aa77) activated the race: `mark_dirty()` is now called mid-turn, waking
the worker to snapshot while the main thread keeps mutating. The 1.0.2
try/catch (SANITIZE-NOTHROW) can't help — SIGSEGV is not a C++ exception.

**Fix (§PERSIST-RACE): published-snapshot design** — the worker never
reads `messages_` again:

1. New `persist_snapshot_` member (guarded by `messages_mutex_`) + private
   `mark_dirty_locked()`: snapshots the chain and sets `dirty_` in one
   locked step, always called on the mutating thread at a quiescent point.
2. `mark_dirty()` locks + calls it; all internal mutators (`add_*`,
   `set_messages`, `clear`, `cleanup_dangling_tool_results`, `compress`,
   `truncate_if_needed`) call `mark_dirty_locked()` inside their existing
   critical sections.
3. `save_impl()` copies `persist_snapshot_` — never `messages_`.
4. Public `save()` refreshes `persist_snapshot_` from `messages_` first
   (preserves legacy semantics: raw push + save() still persists; the
   worker path uses `flush_if_dirty` which does NOT refresh).
5. Hardening (completes SANITIZE-NOTHROW): `chain_safe_str` for the
   remaining `.value()` throw paths — `sanitize_assistant_message`,
   `cleanup_dangling_tool_results`, `truncate_if_needed`, plus
   LLMClient's `messages.back()` role check. `compress()` now marks dirty
   (its chain rewrite was never persisted). `truncate_if_needed` takes
   `messages_mutex_` (was fully unlocked).

**Deliberately skipped from the report's suggestions:** #3 element-wise
defensive copy — a torn json read is SIGSEGV, not catchable; the root
fix eliminates the torn read. #4 public auto-flush kill switch —
deferred; root fix + regression tests are the answer, not a workaround.

### BUG 2 — `execute_bash` infinite hangs in `Subprocess::execute_combined`

1. **Hang A (§SUBPROC-EOF-HANG):** EOF branch's blocking
   `waitpid(pid, &status, 0)` ignores the timeout — replace with
   WNOHANG polling against the remaining deadline; on expiry `kill_group()`
   + `timed_out = true`.
2. **Hang B (§SUBPROC-DRAIN-HANG):** timeout-path drain loop spins
   forever when a `setsid` grandchild keeps the pipe write ends open —
   apply the normal path's bounded drain (100 iterations x 10ms) and fix
   the loop condition from `&&` to `||`.

### Tests

- New `tests/test_subprocess_timeout.cpp`: Hang A repro
  (`exec sleep 300 >&- 2>&-`, timeout 2s, must return promptly with
  `timed_out`), Hang B repro (`setsid sleep 613 & sleep 100`), plus
  normal-output and plain-timeout sanity cases.
- `tests/test_messages.cpp`: §PERSIST-RACE stress (auto_flush 1ms,
  raw-ref push_back + mark_dirty loop, then flush_now + reload verify);
  malformed-chain save() no-throw (SANITIZE-NOTHROW regression);
  save()-after-raw-push semantics; cleanup_dangling with non-object tail.

### Verification

- New tests run pre-fix to confirm failure/hang, pass post-fix
- One-off `-fsanitize=thread` build of the messages stress test:
  race report pre-fix, clean post-fix
- Full suite: `ctest --test-dir build --output-on-failure`
