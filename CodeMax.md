# CodeMax — working memory

## System info

> Captured by `system_info` tool.

| Field | Value |
|-------|-------|
| Container | podman |
| OS | Ubuntu 26.04 LTS (Resolute Raccoon), x86_64 |
| Kernel | Linux 7.0.3-6.stable |
| Date | 2026-07-14 04:15 UTC |
| Display | wayland |
| User | ubuntu (uid 1000) |
| Host | daedalus-dev-wayland |
| Shell | /bin/bash |
| Session | local |

## Project layout

- `/workspace/daedalus` — the C++ agent library + demos (CMake build)
- `/workspace/Testing` — manual + automated test suites for the demos

## Task — Remove `ask_user` Tool

### Context

The `ask_user` tool is broken and needs to be removed along with all its dead/zombie code.

### Files to Modify

| File | Change |
|------|--------|
| `src/tools/UserTool.h` | Remove `AskUserTool` class declaration and forward declaration |
| `src/tools/UserTool.cpp` | Remove `AskUserTool::get_definition()` and `AskUserTool::execute()` |
| `src/api/LLMClient.h` | Update comment at line ~167 mentioning "ask_user" |
| `src/api/LLMClient.cpp` | Remove ask_user handling block (~lines 1967-2007), update comment at line ~1797 |
| `include/daedalus/AgentSink.h` | Remove `request_user_input()` virtual method |
| `include/daedalus/types/AgentState.h` | Remove `AwaitingUser` enum value |
| `src/library/AgentState.cpp` | Remove `AwaitingUser` from `to_string()` and `agent_state_from()` |
| `src/library/AgentImpl.cpp` | Remove CHAT → AwaitingUser mapping (line ~53) |
| `src/tools/SetStatusTool.cpp` | Remove "AwaitingUser" from valid states array |
| `demos/daedalus-cli/ConsoleAgentSink.cpp` | Remove `request_user_input()` method |
| `demos/daedalus-webui/web_sse_sink.h` | Remove `request_user_input()` method and comment |
| `src/core/utils.cpp` | Update comment mentioning request_user_input |

### Tests to Update

| File | Change |
|------|--------|
| `tests/test_agent_state_transitions.cpp` | Update CHAT documentation comments |
| `tests/test_set_status_tool.cpp` | Remove "AwaitingUser" from test data |

### Code Reference Summary

The ask_user tool:
- **Returns**: `{"status": "ask_user", "question": "..."}`
- **Handled in**: `LLMClient.cpp` lines ~1967-2007 - calls `sink_->request_user_input()`, returns `AgentState::CHAT`
- **State mapping**: Internal `CHAT` → public `AwaitingUser` in `AgentImpl.cpp`

### Risk / Scope

- Medium. Multiple files across library, demos, and tests.
- `compress_context` and `stop` tools in `UserTool.{h,cpp}` remain untouched.
- The `request_command_approval()` method in `AgentSink` is separate and stays.

### Verification Plan

1. Build library: `cmake --build build -j12`
2. Run tests: `ctest --test-dir build --output-on-failure`
3. Ensure all `ask_user` and `AwaitingUser` references are gone

### Status

- [x] Remove `AskUserTool` from `UserTool.h`
- [x] Remove `AskUserTool` implementation from `UserTool.cpp`
- [x] Update `LLMClient.h` comments
- [x] Remove ask_user handling from `LLMClient.cpp`
- [x] Remove `request_user_input()` from `AgentSink.h`
- [x] Remove `AwaitingUser` from `AgentState.h`
- [x] Remove `AwaitingUser` from `AgentState.cpp`
- [x] Remove CHAT→AwaitingUser mapping from `AgentImpl.cpp`
- [x] Remove "AwaitingUser" from `SetStatusTool.cpp`
- [x] Update `ConsoleAgentSink.cpp` (remove `request_user_input()`)
- [x] Update `web_sse_sink.h` (remove `request_user_input()`)
- [x] Update `utils.cpp` comment
- [x] Update tests
- [x] Build and test

---

## Plan — Add `set_status` tool so the agent can drive its own state machine

### Context

The `daedalus-webui` shows 4 stage chips (Understanding → Planning → Acting →
Verifying).  Today, for many real prompts the agent only fires
`Understanding → Done` because the library has no path for the agent to set
intermediate states.  The user wants the **agent** to control its state —
only it knows when verification fails and it must go back to Understanding.

A new prompt section in `SystemPromptBuilder` (added earlier) tells the agent
this; now we need the actual **tool** it can call to change state.

### Approach

Add a `set_status` tool that:

1. Accepts a `state` argument — one of the public `AgentState` names
   (Idle, Understanding, Planning, Acting, Verifying, Compressing,
   Done, Cancelled, Error).
2. Validates the name (case-insensitive match against the enum).
3. Returns `{"status": "set_state", "state": "<NormalisedName>"}`.

Then in `LLMClient::run_agent_cycle()`, **after each tool execution**,
inspect the result.  If `status == "set_state"`:

- Look up the public enum value for the requested name.
- Call `sink_->on_state_change(current_public_state_int, new_public_state_int)`
  so the host UI / sink (e.g. `WebSseSink`) immediately fires a
  `state_change` SSE event and the chip row updates.

The sink path is the same one the library already uses — no new
plumbing required for the webui to see it.

### Files to add / modify

| File | Change |
|------|--------|
| `src/tools/SetStatusTool.h` | NEW — `SetStatusTool` class declaration |
| `src/tools/SetStatusTool.cpp` | NEW — `get_definition()` + `execute()` |
| `CMakeLists.txt` (root) | Add `src/tools/SetStatusTool.cpp` to `DAEDALUS_LIB_SOURCES` |
| `src/tools/ToolFactory.cpp` | `#include "tools/SetStatusTool.h"` + `register_tool(std::make_shared<SetStatusTool>())` |
| `src/api/LLMClient.cpp` | After tool execution in `run_agent_cycle()`, check `result["status"] == "set_state"` and call `sink_->on_state_change(...)` |
| `src/api/LLMClient.h` | Expose `AgentState` → public-name mapping helper for the lookup |
| `tests/test_set_status_tool.cpp` | NEW — doctest unit tests for the tool (valid names, invalid names, JSON contract) |
| `tests/CMakeLists.txt` | Register `daedalus_set_status_tool_tests` target |

### Tool contract

```jsonc
// Input
{
  "state": "Planning"
}

// Output (success)
{
  "status": "set_state",
  "state": "Planning",            // normalised, title-case
  "previous_state": "Understanding"
}

// Output (invalid state)
{
  "status": "error",
  "message": "Unknown state 'Foo'. Valid: Idle, Understanding, Planning, Acting, Verifying, Compressing, Done, Cancelled, Error."
}
```

Valid state names (case-insensitive on input, normalised on output):

`Idle`, `Understanding`, `Planning`, `Acting`, `Verifying`, `Compressing`,
`Done`, `Cancelled`, `Error`.

### Risk / scope

- **Small.** Single new tool, one new branch in the tool-result handler.
- The change to `LLMClient::run_agent_cycle()` is additive — old behaviour
  preserved when no `set_status` tool call is in the iteration.
- No new public API surface on `Agent`.
- Host-agnostic: any host that wires a sink sees the state change.

### Verification plan

1. Build `daedalus_set_status_tool_tests` and run — all assertions pass.
2. Rebuild `libdaedalus.a` + `daedalus-webui`.
3. End-to-end via webui:
   - Send a multi-step prompt ("Read hello.txt, then update it").
   - Watch SSE stream — expect `state_change` events for `Planning`,
     `Acting`, `Verifying` triggered by `set_status` tool calls.
4. Restart webui and confirm chips transition visibly in browser.

### Status

- [x] Add `SetStatusTool` class (`src/tools/SetStatusTool.{h,cpp}`)
- [x] Register in `ToolFactory::initialize_tools()`
- [x] Add `src/tools/SetStatusTool.cpp` to root `CMakeLists.txt`
- [x] Wire result-handling branch in `LLMClient::run_agent_cycle()`
- [x] Add `tests/test_set_status_tool.cpp` and register in `tests/CMakeLists.txt`
- [x] Build + run unit tests (20/20 pass, 110 assertions)
- [x] Rebuild webui and verify via SSE capture

### End-to-end verification

```
[state_change] {"raw":1,"to":"Understanding"}     ← library-fired
[tool_call]    set_status
[state_change] {"raw":2,"to":"Planning"}          ← set_status-driven
[tool_call]    set_status
[state_change] {"raw":3,"to":"Acting"}            ← set_status-driven
[state_change] {"raw":8,"to":"Done"}              ← library-fired
```

Webui is live at http://127.0.0.1:8088/ (cwd=/workspace/Testing/test99).
Reload the page to see the chips transition visibly.