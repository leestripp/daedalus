# Plan — monkeybyte bug sweep (2026-10-22)

> **Status**: IMPLEMENTED (2026-10-22) — both fixes verified:
> full superbuild green (109/109 ctest) and live-verified on the isolated
> test instance: restored history renders user AND assistant bubbles with
> correct markdown; turn cancel works repeatedly (spam clicks), the state
> machine returns to Ready, and post-cancel turns complete normally.

Two upstream reports (see `prompt.txt`). Both reproduced against an isolated
test instance (`/tmp/mbtest`, own XDG dirs, control-server driven).

## Bug 1 — Stop button works once, then does nothing

**Repro** (control server `chat.send` + `chat.cancel`, same code path as the
Stop button): cancel during turn 1 works (`reason: "cancelled"`); a plain
second turn + cancel ALSO works — so the library-side cancel machinery
(`AgentImpl::cancel` latch, `LLMClient::cancel_source_` per-turn reseat,
`ToolHandler` cancel token, per-iteration checks) is sound.

**Root cause** (host side, `monkeybyte/src/monkeybyte/ui/chat/ChatController.cpp`):
- `cancel()` early-returns unless `state() == Processing`. The first click
  sets state to `Cancelling` and nothing ever moves it back to `Processing`
  while the drain loop keeps running.
- The turn-thread drain loop (§QUEUE-JOIN) starts a *queued* follow-up turn
  after a cancelled turn **without touching the state**: state stays
  `Cancelling` for the entire next turn.
- Every further Stop click hits the `state() != Processing` guard and no-ops
  — the exact "works once then dead" report. Spam clicks should re-fire the
  cancel, not be dropped.

**Fix** (ChatController only — the library needs no change):
1. `cancel()`: accept `Processing` **or** `Cancelling`; re-fire
   `sink_->cancel_all_approvals()` + `agent_->cancel()` on every press so
   queued/repeated presses keep hammering the in-flight turn.
2. Drain loop: when a new queued message is actually dispatched after a
   cancel, transition state back to `Processing` (honest bookkeeping — the
   UI shows "Thinking…" again and the Stop button stays live).
3. When a turn ends `cancelled`, clear `queued_messages_` — Stop means stop;
   a follow-up queued before the press must not run afterwards.

## Bug 2 — first load of the chat area is a mess

**Repro**: restart with a persisted session → only *user* bubbles restore;
every assistant final answer is missing; the conversation looks broken.
Live rendering during a turn is fine — only `restore_history()` is affected.

**Root cause** (`daedalus/src/core/Messages.cpp`, `history_snapshot()`):
assistant messages are skipped whenever they contain `tool_calls`
("elide pure tool-call assistant messages"). But daedalus appends the final
answer text to the SAME assistant message that carries the terminating
`stop` tool call — so every final answer is elided from the snapshot and
from the restored UI.

**Fix**: extract the text first; elide only when the message has no
renderable text (true pure tool-call plumbing). Assistant messages with
both text and tool_calls keep their text. Add a regression test in
`daedalus/tests/test_history_snapshot.cpp`.

## Verification

- `daedalus` unit tests (`test_history_snapshot`, monkeybyte tests if any apply)
  + full superbuild `ctest`.
- Live: restart the isolated instance → restored history shows user AND
  assistant bubbles (screenshot).
- Live: turn 1 → cancel; queued follow-up + second cancel → both turns stop,
  state returns to Ready (screenshot + control-server results).

## Docs

- `daedalus/CHANGELOG.md` + monkeybyte changelog/design notes per repo §Marker
  conventions.
