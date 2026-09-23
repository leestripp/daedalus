# Agent State Machine

> **Redirect (2026-07-13):** this document has been migrated into the
> public API header. The state enum, state table, and state flow are
> fully documented there. This file is kept as a stub to avoid breaking
> any external links.
>
> **Full reference:** see `include/daedalus/types/AgentState.h` — the public
> `daedalus::AgentState` enum and what each state means. The agent drives
> state transitions itself via the `set_status` tool; `Verifying` is a
> decision gate where the agent inspects build/test output and loops back
> to `Understanding`/`Planning` on failure, and `Documenting` is used for
> README/CHANGELOG/docs updates after verification passes. For host-facing
> lifecycle guidance, see [`docs/embedding.md`](embedding.md).
