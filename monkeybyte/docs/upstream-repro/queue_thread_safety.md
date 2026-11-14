# daedalus bug report: user-message queue is not thread-safe

**Date:** 2026-09-10
**daedalus version:** main @ 78318ec (post the message-queue commit)
**Host:** MonkeyByte (GTK4 AI code editor, daedalus via CMake FetchContent)
**Platform:** Ubuntu 26.04 x86_64, gcc 15, libstdc++

## Symptom

`Agent::submit_user_message()` is documented as the way for a host to push
a user message that the next `run_one_turn()` consumes — the obvious usage
being a UI thread submitting while the agent thread is mid-turn. But the
queue backing it, `AgentImpl::pending_user_messages_`, is a plain
`std::vector<PendingUserMessage>` with **no synchronization anywhere**:

- `AgentImpl::submit_user_message()` → `pending_user_messages_.push_back(...)`
  (no lock)
- `AgentImpl::run_one_turn()` → `pending_user_messages_.empty() / front() /
  erase(begin())` (no lock)
- `AgentImpl::pending_count()` → `pending_user_messages_.size()` (no lock)
- `AgentImpl::cancel()` → `pending_user_messages_.empty()` (no lock)

A UI-thread `submit_user_message()` racing the agent thread's
`run_one_turn()` (or `cancel()` from any thread) is a data race on the
vector — undefined behavior: torn reads, lost messages, heap corruption.

## Reproducer sketch

```cpp
daedalus::Agent agent(cfg);
agent.initialize();
agent.submit_user_message("first");
std::jthread turn([&] { agent.run_one_turn(); });
// UI thread, while the turn above is running:
agent.submit_user_message("second"); // data race on pending_user_messages_
turn.join();
```

Run under ThreadSanitizer (`-fsanitize=thread`) and it should flag the
`push_back` vs. `empty()`/`front()`/`erase()` pair immediately.

## Expected

Queue operations are internally synchronized (e.g. a mutex guarding
`pending_user_messages_` in `submit_user_message`, `run_one_turn`'s
dequeue, `pending_count()`, and `cancel()`'s empty check), so the
documented "submit while a turn runs" pattern is safe. Ideally also an
API to inspect/drain pending messages without running them (e.g. for a
host Stop-button "abort everything" path) — currently a cancelled turn
leaves queued messages in place with no way to drop them.

## Host-side workaround (what MonkeyByte does)

Never call `submit_user_message()` / `pending_count()` from the UI thread.
The host keeps its own mutex-guarded queue (`ChatController::
queued_messages_`); the turn thread — the only thread that ever calls
`run_one_turn()` — pops from the host queue and submits to daedalus one
message at a time between turns, so daedalus's vector is only ever touched
by a single thread.

## Resolution (2026-09-10, daedalus main @ d73d1ba)

Upstream added the requested queue-management API:
`Agent::pending_messages()`, `edit_pending_message()`,
`remove_pending_message()`, `clear_pending_messages()` (see
`daedalus/Agent.h`, "Pending queue management"). Thread-safety was
addressed by contract instead of synchronization: the queue API is
documented as driving-thread-only ("Follows Agent's threading rules
(not thread-safe; call from the driving thread)").

MonkeyByte already satisfies that contract — the turn thread is the
only thread that ever touches the agent (host-side queue hands messages
over under `state_mutex_`). The host-side queue therefore stays, and
`clear_pending_messages()` is now used to drop messages stranded when a
cancel lands between `submit_user_message()` and `run_one_turn()`'s
consume (previously an unrecoverable off-by-one).

## Updated resolution (2026-11-02, background completion delivery)

The queue is now internally synchronized as originally requested:
`submit_user_message()`, `pending_count()`, queue inspection/editing, and
`cancel()` are callable from any thread. Only `run_one_turn()` remains
restricted to the single driving thread. This lets the background-process
monitor submit directly to the owning Agent and lets MonkeyByte perform its
final `pending_count()` race-closure check under `state_mutex_`.

The "Host-side workaround" and first "Resolution" sections above are retained
as historical context; their driving-thread-only queue contract is superseded.
