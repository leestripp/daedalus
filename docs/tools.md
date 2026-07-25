![daedalus Banner](../images/daedalus_banner.png)

# Tool Definitions for daedalus

This document describes the tools available to the AI assistant in daedalus (an OpenAI-compatible API coding agent). These tools are passed to the LLM via JSON schema and executed locally with strict validation and security checks.

---

# Default Tool Set vs. Example Tools

Starting with the tool-trim refactor, `libdaedalus` intentionally exposes a
**minimal core tool set** by default. Only the tools required for a
functional autonomous agent are registered automatically:

| Tool | Purpose |
|------|---------|
| `execute_bash` | Run shell commands in the project root |
| `tell_user` | Send text back to the host / user |
| `set_status` | Declare the agent's own state mid-turn |
| `compress_context` | Request conversation compression |
| `stop` | End the agent loop cleanly |
| `remember` / `forget` / `memories` / `memory_stats` | Long-term memory management |
| `todo_add` / `todo_edit` / `todo_delete` / `todo_list` / `todo_count` / `set_max_todos` / `todo_clear` | Per-project, per-agent todo list |
| `discover_skills` / `load_skill` / `get_active_skills` / `search_skills` / `deactivate_skill` / `get_skill_context` | Skill loading and introspection |
| `mcp__<server>__<tool>` | Dynamically registered MCP proxied tools (when MCP is enabled) |

All other tools — Git, search, PDF, system info, background process, and
network tools — have been moved to `demos/example-tools/` as **reference
implementations** that you can copy, adapt, or register one at a time.

File I/O tools were removed entirely: the agent can read, write, and edit
files through `execute_bash` (e.g. `cat`, `sed`, `grep`) instead.

## Registering a brand-new custom tool

For tools **you write yourself**, use the public API: subclass
`daedalus::BaseTool` and register one instance with `Agent::register_tool()`.

```cpp
#include <daedalus.h>

class TimestampTool : public daedalus::BaseTool {
public:
    std::string get_name() const override { return "timestamp"; }
    std::string get_description() const override {
        return "Return the current UTC timestamp.";
    }
    nlohmann::json get_definition() const override {
        return {
            {"type", "function"},
            {"function", {
                {"name", "timestamp"},
                {"description", get_description()},
                {"parameters", {
                    {"type", "object"},
                    {"properties", nlohmann::json::object()},
                    {"required", nlohmann::json::array()}
                }}
            }}
        };
    }
    nlohmann::json execute(const nlohmann::json&,
                           daedalus::HostToolContext&) override {
        return {{"status", "success"}, {"timestamp", "2026-07-19T23:00:00Z"}};
    }
};

int main() {
    daedalus::Agent agent(cfg, sink);
    agent.register_tool(std::make_unique<TimestampTool>());
    agent.initialize();
    // The LLM can now call the "timestamp" tool.
}
```

See `docs/custom-tools.md` for the complete `daedalus::BaseTool` subclassing
guide.

## Registering an example tool from `demos/example-tools/`

The example tools are the internal FileIO-based tools that were moved out of
the library. Because they pre-date the public `daedalus::BaseTool` API, they
are registered directly with the internal `ToolFactory` via `ToolHandler`.
Hosts should opt in to **exactly** the tools they need:

```cpp
#include <daedalus.h>
#include "tools/ToolFactory.h"
#include "tools/ToolHandler.h"
#include "NetworkTool.h"   // from demos/example-tools/

daedalus::Agent agent(cfg, sink);
agent.initialize();

auto factory = ToolHandler::get_tool_factory();
if (factory) {
    factory->register_tool(std::make_shared<HttpRequestTool>());
    factory->register_tool(std::make_shared<DnsLookupTool>());
}
```

There is no bulk "register everything" helper — pick the individual tools
your host actually needs, or write your own with the public `BaseTool` API.

The detailed tool descriptions below cover both the core defaults and the
example tools so the schema reference stays in one place.

---

# Disabling tools at runtime

You can hide individual tools from the LLM and reject any attempts to call
them without recreating the `Agent`.

```cpp
agent.set_tool_enabled("execute_bash", false);   // hide it
agent.set_tool_enabled("execute_bash", true);    // restore it

if (agent.is_tool_enabled("execute_bash")) { /* ... */ }
auto disabled = agent.disabled_tools();
```

You can also set the initial disabled set at construction time via
`AgentConfig::tools_disabled`.

---

# Tool Call Pairs and Message Chain Integrity

This section explains how tool calls work in daedalus, why maintaining message chain integrity is critical, and how we fixed issues that could break the tool call sequence.

## How Tool Call Pairs Work

When the LLM decides to use a tool, it generates a message with `role: "assistant"` and `tool_calls` array. Each tool call has a unique `id` and specifies the function name and arguments. After the assistant message, there must be one or more `tool` result messages, each with a `tool_call_id` matching the tool call's `id`.

### Example of a Complete Tool Call Pair

```
Assistant Message (with tool_calls):
{
  "role": "assistant",
  "content": null,
  "tool_calls": [
    {
      "id": "call_abc123",
      "type": "function",
      "function": {
        "name": "execute_bash",
        "arguments": "{\"command\": \"cat src/main.cpp\"}"
      }
    }
  ]
}

Tool Result Message:
{
  "role": "tool",
  "tool_call_id": "call_abc123",
  "content": "{\"status\": \"success\", \"path\": \"src/main.cpp\", ...}"
}
```

### The Rule: `tool_call` → `tool` → `assistant` → `tool` → ...

The API enforces strict ordering: every `tool` message must immediately follow an `assistant` message that requested it. This is called **tool call chain integrity**.

## Why This Matters: The 2013 Error

If the tool call chain is broken, the API returns error **2013: "invalid params, tool call result does not follow tool call"**. This happens when:

1. An `assistant` message has `tool_calls`, but the next message is NOT the corresponding `tool` result
2. A `tool` message references a `tool_call_id` that doesn't exist in any preceding `assistant` message
3. Messages are compressed or truncated in a way that orphans a tool call from its results

### Example of a Broken Chain

```
Assistant (with tool_calls) → ??? → Assistant (with new tool_calls) → tool
                                    ↑
                                    This tool result has no parent!
```

## What Can Break Tool Call Chains

### 1. User Interruption (ESC key or cancel)

When the user calls `agent.cancel()` (or fires the `stop_token` passed
into `run_one_turn`) during an API wait:

- The `assistant` message with `tool_calls` has already been added to messages
- But no `tool` result messages exist yet
- This leaves an orphaned tool call

> **Removed in v0.4.0:** the `ask_user` tool. The agent no longer
> blocks mid-turn for user input. Cancellation is the host's
> responsibility (`agent.cancel()`).

### 2. Context Compression (50% Split)

The 50% split algorithm compresses the first half of messages while keeping the second half intact:

- **First 50%**: Summarized via LLM call (reduces historical bulk)
- **Last 50%**: Kept verbatim (preserves recent context and tool call chains)
- **Result**: `system + user(instruction + summary) + suffix(last 50%)`

This approach prevents tool call mismatches because:
1. The suffix (last 50%) contains complete tool call chains intact
2. The summarized prefix has no tool_calls, only the summary
3. Tool results in suffix reference tool_calls that remain in suffix

### 3. Message Truncation
When the request payload becomes too large and we truncate oldest messages:
- If truncation cuts in the wrong place, it can break tool call chains
- Cannot truncate just anywhere - must preserve complete pairs

### 4. Tool Execution Failure
When a tool permanently fails after all retries:
- The `tool` result with `status: "error"` is still added
- This is actually correct - the chain stays intact, LLM sees the error and decides what to do

### 5. Message Save/Load Corruption
When saving/loading messages to disk:
- If the saved file becomes corrupted or incomplete
- Tool call IDs in `tool` messages may not match any `assistant` message

## How We Fixed It

### Fix 1: Cleanup on Cancellation

When returning `AgentState::CANCELLED`, we now call `cleanup_incomplete_tool_calls()`:

```cpp
static void cleanup_incomplete_tool_calls(json& messages) {
    // Find the last assistant message with tool_calls but no results
    // and remove it to prevent broken chains
}
```

This removes any `assistant` message that has `tool_calls` but no corresponding `tool` results yet.

### Fix 2: Safe Truncation (Never Cut Tool Call Chains)

When truncating messages to fit in max_request_size:

```cpp
// If we encounter a tool result, we MUST keep going back to find
// its parent assistant message. We cannot cut the chain.
while (i >= 1) {
    if (role == "tool") {
        // Keep this tool result AND keep going back
        // until we find the assistant that made the call
    }
}
```

Truncation now walks backwards and preserves complete tool_call → tool chains.

### Fix 3: Message Validation on Save/Load

Before saving messages to disk (`messages.json`), we validate and fix:

```cpp
static bool validate_and_fix_message_chain(json& messages) {
    // 1. Build set of valid tool_call_ids from all assistant messages
    // 2. Remove tool results whose tool_call_id is not valid
    // 3. Remove assistant messages with tool_calls but no results
}
```

This ensures corrupted sessions are repaired on load.

### Fix 4: Compression Uses 50% Split

The new "50% Split" algorithm only summarizes the first half of messages, keeping the second half intact:

```cpp
// Split at 50% - first half summarized, second half preserved
size_t split_point = total_non_system / 2;

// First half (to be summarized)
for (size_t i = 1; i < messages_.size(); ++i) {
    if (current <= split_point) {
        middle.push_back(messages_[i]);  // Summarized
    } else {
        suffix.push_back(messages_[i]);  // Kept intact
    }
}

// Result: system + user(instruction + summary) + suffix(last 50%)
```

Tool call chains in the suffix remain complete because we never cut through them.

### Fix 5: Compression Only After Complete Iteration

The `compress_after_done` flag ensures compression only happens:
- After all tool calls in an iteration have been resolved
- After tool results have been added to messages
- When the message chain is complete and valid

### Fix 6: Tool Retry with Exponential Backoff

When a tool returns `status: "error"` or throws an exception:

```cpp
constexpr int max_tool_retries = 3;
for (int attempt = 1; attempt <= max_tool_retries; attempt++) {
    // Retry with backoff: 1s, 2s, 4s...
    // After 3 failures, mark with permanent_failure and add error result
}
```

The tool result is always added, keeping the chain intact. The LLM sees the error and can decide next steps.

## Best Practices to Avoid 2013 Errors

1. **Never add assistant messages with tool_calls without corresponding tool results**
   - Always add tool results immediately after execution

2. **Never truncate messages in the middle of a tool call chain**
   - When truncating from the end, walk backwards and preserve complete chains

3. **Always validate message chains before save/load**
   - Remove orphaned tool results and incomplete tool calls

4. **Only compress context when no pending tool calls exist**
   - Compression at end of iteration, not during tool processing

5. **Cleanup incomplete tool calls before returning CANCELLED**
   - User interruption should not leave broken chains

## Summary

Tool call chain integrity is critical for the OpenAI API. Any break in the sequence (assistant → tool → assistant → tool) causes error 2013. The fixes we implemented ensure:

- Cancellations clean up incomplete tool calls
- Truncation preserves complete chains  
- Save/load validates and repairs chains
- Compression skips transient tool messages
- Tool retries keep chains intact with error results

This prevents the 2013 error and ensures reliable operation across sessions.

---

## Tool Definitions in the OpenAI API

### Where Tools Go in the JSON Message

Tool definitions are placed in the **`tools`** field of the chat completion request payload:

```json
{
  "model": "gpt-4",
  "messages": [...],
  "tools": [...],
  "stream": false,
  "temperature": 0.0
}
```

Each tool definition follows this format:

```json
{
  "type": "function",
  "function": {
    "name": "tool_name",
    "description": "What the tool does",
    "parameters": {
      "type": "object",
      "properties": {
        "param_name": {
          "type": "string",
          "description": "Parameter description"
        }
      },
      "required": ["param_name"]
    }
  }
}
```

### How Often Tools Are Sent

**Tool definitions are sent with EVERY chat completion request.** This is by design in the OpenAI API because:
- The model decides whether to call tools based on the conversation context
- Without the tool definitions in each request, the model won't know what tools are available
- The conversation history grows across requests, but the tool definitions remain the same

in daedalus, tool definitions are:
1. Created **once** at application startup
2. Passed to every `run_agent_cycle()` call
3. Included in every API request payload

---

## Calling Bash Commands

When using bash commands in daedalus, remember the following:

- **The Agent is always in the root folder of the project**
- All bash commands should be run as such, relative paths can be used based on the project's root path
- Use relative paths from the project root instead of absolute paths
- For CMake projects, use the build folder for compilation

### Example Bash Commands

```bash
# Compile a CMake project (from build folder)
cd build/
cmake ..
make -j8

# Run a single C++ file
g++ -o output src/main.cpp

# Run git commands
git status
git add -A
git commit -m "Your message"

# List files
ls -la
ls src/
```

### Build System Guidelines (CMake)

If the project uses CMake (has CMakeLists.txt), you MUST use the build folder:
1. First cd into the build folder: `cd build/`
2. Run `cmake ..` to configure the project
3. Run `make -j8` (or `make -j4`) to compile
4. **NEVER run cmake from the project root folder** - it will pollute the source tree

## 1. `tell_user`

### Purpose
Sends a message to the user. Use this to communicate results, status updates, or ask the user to review something.

### Parameters

| Name       | Type    | Required | Description                    |
|------------|---------|----------|--------------------------------|
| content    | string  | Yes      | The message to send to the user|

### Example Usage

```json
{
  "name": "tell_user",
  "arguments": {
    "content": "I've analyzed the code and found that the main.cpp file needs refactoring."
  }
}
```

---

## 2. `execute_bash`

### Purpose
Executes a shell command. Supports both synchronous execution and detached
background execution for servers or long-running tools.

Use synchronous mode for builds (`cmake`, `make`), tests, file operations
(`cat`, `sed`, `grep`), git commands, etc. Use background mode when the agent
needs to start a server or daemon and continue its reasoning loop.

### Parameters

| Name         | Type    | Required | Description                                                              |
|--------------|---------|----------|--------------------------------------------------------------------------|
| command      | string  | *        | The shell command to run. Required for `run` / `start`.                  |
| action       | string  | No       | `run` (default), `start`, `poll`, `send`, `stop`.                        |
| background   | bool    | No       | Shorthand for `action=start`.                                            |
| timeout      | integer | No       | Sync timeout in seconds (1–3600, default 60).                            |
| process_id   | string  | *        | Required for `poll`, `send`, `stop`. Returned by `start`.                |
| input        | string  | *        | Data to send to a background process's stdin. Required for `send`.       |
| clear_output | bool    | No       | If true, clear the output buffer after reading (action=poll only).       |

A trailing unquoted `&` in `command` is auto-detected as background intent
(`action=start`) and the `&` is stripped before execution.

### Approval

`execute_bash` is the only core tool that asks the host for approval. Before a
`run` or `start` command executes, the library calls
`AgentSink::request_command_approval(command)`.

- Return `true` to allow the command.
- Return `false` to deny it (`execute_bash` returns an error result to the LLM).
- Throw an exception to abort the current turn.

`poll`, `send`, and `stop` do not require approval.

See `docs/embedding.md` § `request_command_approval(command)` for the full
host-side contract.

### Synchronous mode (`action=run`, default)

```json
{
  "name": "execute_bash",
  "arguments": {
    "command": "cmake --build build -j8",
    "timeout": 120
  }
}
```

Result:

```json
{
  "status": "success",
  "command": "cmake --build build -j8",
  "output": "...",
  "exit_code": 0
}
```

**Live output streaming:** while the command runs, sanitised stdout/stderr
chunks are pushed to `AgentSink::on_tool_progress()` as
`{"stage":"output","output_chunk":"..."}` events, so hosts can display
output in real time instead of waiting for the result. The chunks are
sanitised incrementally (`SanitizedOutputStream`), so concatenating them in
order always reproduces the exact sanitised `output` field — a chunk
boundary can never corrupt an escape sequence, UTF-8 character, or CR/LF
pair. See `docs/embedding.md` § `on_tool_progress` for the host contract.

### Background mode (`action=start` or `background=true`)

```json
{
  "name": "execute_bash",
  "arguments": {
    "command": "python3 -m http.server 8000"
  }
}
```

or explicitly:

```json
{
  "name": "execute_bash",
  "arguments": {
    "command": "python3 -m http.server 8000",
    "action": "start"
  }
}
```

Result:

```json
{
  "status": "background",
  "process_id": "bg_1",
  "pid": 12345,
  "command": "python3 -m http.server 8000",
  "message": "Process started in background. Use action=poll with process_id to read output, action=send to write stdin, action=stop to terminate."
}
```

### Poll / send / stop

```json
// Read latest output
{ "name": "execute_bash", "arguments": { "action": "poll", "process_id": "bg_1" } }

// Send stdin data
{ "name": "execute_bash", "arguments": { "action": "send", "process_id": "bg_1", "input": "hello\n" } }

// Terminate
{ "name": "execute_bash", "arguments": { "action": "stop", "process_id": "bg_1" } }
```

### Security
- Dangerous commands are blocked before approval is requested: `rm -rf /`,
  fork bombs, privilege escalation (`sudo`, `su -`, ...), etc.
- Session approval option (e.g. the CLI's `s` reply) saves a wildcard pattern to
  `.daedalus/daedalus[-<agent_name>]/approvals.json`; matching commands bypass
  the sink in future sessions.
- Binary output is detected and truncated to prevent JSON parsing errors.
- **Chained commands supported**: `&&`, `||`, and `;` operators with conditional
  execution.
- **Process isolation**: Sync commands run in isolated subprocess for crash
  protection. Background processes are fully detached via `setsid()` with
  stdout/stderr captured to files.
- **Timeout protection**: 60-second default timeout for sync commands,
  configurable from 1–3600 seconds. Background processes run until stopped or
  until they exit on their own.

### Auto Mode
- Set `AgentConfig::auto_approve = true` (or the equivalent setting in the
  SQLite store) to skip `request_command_approval` entirely.
- Security checks still apply; only the host callback is skipped.

---

## 3. `set_status`

> **Removed in v0.4.0:** `ask_user` and the sink-blocking
> `request_user_input`. Replaced by `set_status` (see §14) for
> the only host-loop interaction the agent still drives:
> declaring its own state. Mid-turn user prompting was removed;
> use `tell_user` (informational), `set_status` (state
> machine), or cancel + restart for back-and-forth.

### Purpose

Lets the agent declare its own `AgentState` mid-turn. Useful when
the agent wants to surface progress phases (e.g. "I am now
Verifying") without going through the implicit transitions the
loop already drives. The agent's chosen state is propagated
through the sink as `on_state_change(prev, new)` so hosts can
re-render the status chip.

### Parameters

| Name  | Type   | Required | Description                                                  |
|-------|--------|----------|--------------------------------------------------------------|
| state | string | Yes      | One of `Idle`, `Understanding`, `Planning`, `Acting`, `Verifying`, `AwaitingApproval`, `Compressing`, `Done`, `Cancelled`, `Error`. Case-insensitive. |

### Example Usage

```json
{
  "name": "set_status",
  "arguments": { "state": "Verifying" }
}
```

---

## 4. `compress_context`

### Purpose
Allows the agent to request context compression when it no longer needs older parts of the conversation. The agent knows best when it has gathered all the information it needs.

### Parameters

| Name   | Type   | Required | Description                                        |
|--------|--------|----------|----------------------------------------------------|
| reason | string | Yes      | Brief explanation of why context can be compressed |

### Example Usage

```json
{
  "name": "compress_context",
  "arguments": {
    "reason": "Completed code research, no longer need early grep_search results"
  }
}
```

---

## 5. `stop`

### Purpose
Signals that the task is complete. The agent calls this tool when it has finished the user's request - all research, planning, edits, and verification are done. This ends the autonomous agent loop.

### Parameters

| Name    | Type   | Required | Description                          |
|---------|--------|----------|--------------------------------------|
| summary | string | Yes      | Brief summary of what was accomplished |

### Example Usage

```json
{
  "name": "stop",
  "arguments": {
    "summary": "Fixed the memory leak in FileIO.cpp by properly releasing the file handle in the destructor"
  }
}
```

---

## 6. `git_status`

### Purpose
Shows the working tree status. Displays which files have been modified, staged, or are untracked.

### Parameters
None required.

### Example Usage

```json
{
  "name": "git_status",
  "arguments": {}
}
```

### Response Format

```json
{
  "status": "success",
  "output": "On branch devel\nChanges not staged for commit:\n  modified:   src/main.cpp\nUntracked files:\n  new_file.cpp"
}
```

---

## 7. `git_branch`

### Purpose
Lists all branches in the repository. Shows both local and remote branches.

### Parameters
None required.

### Example Usage

```json
{
  "name": "git_branch",
  "arguments": {}
}
```

---

## 8. `git_commit`

### Purpose
Commits staged changes to the repository. Automatically stages all modified files before committing.

### Parameters

| Name    | Type   | Required | Description                    |
|---------|--------|----------|--------------------------------|
| message | string | Yes      | The commit message describing the changes |

### Example Usage

```json
{
  "name": "git_commit",
  "arguments": {
    "message": "Add new feature for handling user authentication"
  }
}
```

---

## 9. `git_add`

### Purpose
Stages files for commit. Adds files to the Git staging area.

### Parameters

| Name  | Type   | Required | Description                                                     |
|-------|--------|----------|-----------------------------------------------------------------|
| files | string | Yes      | The files to stage. Use '-A', 'all', or '.' to stage all files, or specify a specific file/directory path |

### Example Usage

```json
{
  "name": "git_add",
  "arguments": {
    "files": "src/main.cpp"
  }
}
```

---

## 10. `git_switch`

### Purpose
Switches to a different branch. Supports creating new branches with the optional `create` parameter.

### Parameters

| Name   | Type    | Required | Description                                      |
|--------|---------|----------|--------------------------------------------------|
| branch | string  | Yes      | The name of the branch to switch to             |
| create | boolean | No       | Create and switch to a new branch (default: false) |

### Example Usage

```json
{
  "name": "git_switch",
  "arguments": {
    "branch": "feature/new-ui"
  }
}
```

### Example Usage (create new branch)

```json
{
  "name": "git_switch",
  "arguments": {
    "branch": "feature/new-feature",
    "create": true
  }
}
```

### Response Fields
The tool returns a JSON response with these fields:

| Field           | Type   | Description                                    |
|-----------------|--------|------------------------------------------------|
| status          | string | "success" or "error"                           |
| command         | string | The git command that was executed              |
| previous_branch | string | The branch before the switch                   |
| new_branch      | string | The branch after the switch (verified)         |
| output          | string | Git command output                             |
| exit_code       | number | Exit code from git command                     |
| message         | string | Human-readable status message                 |

### Example Success Response
```json
{
  "status": "success",
  "command": "git switch -c feature-branch",
  "previous_branch": "devel",
  "new_branch": "feature-branch",
  "output": "Switched to branch 'feature-branch'",
  "exit_code": 0,
  "message": "Successfully created and switched to 'feature-branch'"
}
```

---

## 11. `git_push`

### Purpose
Pushes commits to the remote repository. Supports force push and upstream tracking options.

### Parameters

| Name        | Type    | Required | Description                                      |
|-------------|---------|----------|--------------------------------------------------|
| force       | boolean | No       | Force push (requires explicit approval)          |
| set_upstream | boolean | No       | Set upstream tracking (-u flag)                  |

### Example Usage

```json
{
  "name": "git_push",
  "arguments": {}
}
```

### Example Usage (set upstream)

```json
{
  "name": "git_push",
  "arguments": {
    "set_upstream": true
  }
}
```

### Example Usage (force push)

```json
{
  "name": "git_push",
  "arguments": {
    "force": true
  }
}
```

### Response Format

```json
{
  "status": "success",
  "command": "git push -u origin feature-branch",
  "output": "Total 5 (delta 3), reused 0 (delta 0)\nremote:\nremote: Create pull request for feature-branch:...",
  "exit_code": 0,
  "message": "Successfully pushed to origin"
}
```

### Security
- Force push requires explicit user approval regardless of auto mode
- Force push shows warning in user output

---

## 12. `git_diff`

### Purpose
Shows changes between the working tree and the staging area. Displays differences in files.

### Parameters

| Name | Type   | Required | Description                                          |
|------|--------|----------|------------------------------------------------------|
| file | string | No       | Optional: The specific file to show diff for        |

### Example Usage

```json
{
  "name": "git_diff",
  "arguments": {}
}
```

### Example Usage (specific file)

```json
{
  "name": "git_diff",
  "arguments": {
    "file": "src/main.cpp"
  }
}
```

### Response Format

```json
{
  "status": "success",
  "command": "git diff",
  "output": "diff --git a/src/main.cpp b/src/main.cpp\n...",
  "exit_code": 0
}
```

---

## 13. `git_merge`

### Purpose
Merge a branch into the current branch. Supports regular merges, --no-ff merges, squash merges, and aborting a merge in progress.

### Parameters

| Name            | Type    | Required | Description                                                              |
|-----------------|---------|----------|--------------------------------------------------------------------------|
| branch          | string  | Yes      | The name of the branch to merge into the current branch                  |
| no_fast_forward | boolean | No       | Use --no-ff flag to create a merge commit even when merge resolves as fast-forward |
| squash          | boolean | No       | Use --squash to squash all commits from the branch into a single commit |
| abort           | boolean | No       | Abort the current merge in progress and restore the pre-merge state     |

### Example Usage

```json
{
  "name": "git_merge",
  "arguments": {
    "branch": "feature/my-feature"
  }
}
```

### Example Usage (--no-ff merge)

```json
{
  "name": "git_merge",
  "arguments": {
    "branch": "feature/my-feature",
    "no_fast_forward": true
  }
}
```

### Example Usage (squash merge)

```json
{
  "name": "git_merge",
  "arguments": {
    "branch": "feature/my-feature",
    "squash": true
  }
}
```

### Example Usage (abort merge)

```json
{
  "name": "git_merge",
  "arguments": {
    "abort": true
  }
}
```

### Response Format (success)

```json
{
  "status": "success",
  "command": "git merge feature/my-feature",
  "output": "Merge made by the 'ort' strategy.\n...",
  "exit_code": 0
}
```

### Response Format (conflict)

```json
{
  "status": "error",
  "command": "git merge feature/my-feature",
  "output": "Auto-merging src/main.cpp\nCONFLICT (content): Merge conflict in src/main.cpp\nAutomatic merge failed; fix conflicts and then commit the result.",
  "exit_code": 1,
  "has_conflicts": true,
  "message": "Merge conflict detected. Resolve conflicts manually and commit, or use git_merge with abort=true to cancel."
}
```

### Notes

- Merge conflicts are detected by checking for "CONFLICT" or "conflict" in the output
- When conflicts occur, the tool returns an error status with `has_conflicts: true`
- Use `abort=true` to cancel a merge in progress and restore the pre-merge state

---


## 14. `mcp` - Unified MCP Tool

### Purpose
Call a tool from any configured MCP server. Routes to any MCP server's tools dynamically based on the server and tool parameters. MCP servers are read from the SQLite `SettingsStore` (`$XDG_DATA_HOME/daedalus/daedalus.db`, overridable with `cfg.daedalus_home`); legacy `~/.daedalus/mcp/servers.json` is honoured as a read fallback.

### Configuration
Legacy MCP servers may still be configured via JSON in `~/.daedalus/mcp/servers.json`:

```json
{
  "mcpServers": {
    "minimax-web-search": {
      "command": "uvx",
      "args": ["minimax-coding-plan-mcp", "web-search"],
      "env": {"MINIMAX_API_KEY": "${MINIMAX_API_KEY}"}
    },
    "wikipedia": {
      "command": "uvx",
      "args": ["--from", "mcp-server-wikipedia", "wikipedia-mcp-server"]
    }
  }
}
```

Environment variables are stored in `$XDG_CONFIG_HOME/daedalus/mcp/.env` (overridable with `$DAEDALUS_HOME` or `cfg.daedalus_home`).

### Parameters

| Name      | Type   | Required | Description                                           |
|-----------|--------|----------|-------------------------------------------------------|
| server    | string | Yes      | Name of the MCP server (e.g., 'minimax-web-search') |
| tool      | string | Yes      | Name of the tool on the server (e.g., 'web_search') |
| arguments | object | No       | Arguments to pass to the MCP tool                    |

### Example Usage

**Web search:**
```json
{
  "name": "mcp",
  "arguments": {
    "server": "minimax-web-search",
    "tool": "web_search",
    "arguments": {"query": "MCP protocol specification"}
  }
}
```

**Wikipedia search:**
```json
{
  "name": "mcp",
  "arguments": {
    "server": "wikipedia",
    "tool": "search",
    "arguments": {"query": "Model Context Protocol", "limit": 5}
  }
}
```

**Image analysis:**
```json
{
  "name": "mcp",
  "arguments": {
    "server": "minimax-read-image",
    "tool": "read_image",
    "arguments": {"path": "/path/to/image.png", "prompt": "Describe this image"}
  }
}
```

**List available servers:**
```json
{
  "name": "mcp",
  "arguments": {
    "server": "servers",
    "tool": "servers"
  }
}
```

**List tools on a server:**
```json
{
  "name": "mcp",
  "arguments": {
    "server": "minimax-web-search",
    "tool": "list"
  }
}
```

### Response Format

```json
{
  "status": "success",
  "server": "minimax-web-search",
  "tool": "web_search",
  "result": "...tool response content..."
}
```

### Error Response

```json
{
  "status": "error",
  "server": "minimax-web-search",
  "tool": "web_search",
  "message": "Server 'unknown-server' not found in configuration."
}
```

### Notes
- Server names come from the SettingsStore MCP registry (legacy `~/.daedalus/mcp/servers.json` merged as a fallback)
- Use `tool='servers'` to list all configured servers
- Use `tool='list'` to list tools available on a specific server
- Environment variables use `${VAR_NAME}` syntax in config
- Multiple servers can be configured and used simultaneously

---

## 15. `read_pdf`

Extract text content from a PDF file.

### Tool Definition
```json
{
  "type": "function",
  "function": {
    "name": "read_pdf",
    "description": "Read a PDF file and extract its text content. Returns the text extracted from all pages or a specific page range.",
    "parameters": {
      "type": "object",
      "properties": {
        "path": {
          "type": "string",
          "description": "Path to the PDF file (relative to project root or absolute). Example: 'documents/report.pdf'"
        },
        "start_page": {
          "type": "integer",
          "description": "Starting page number (1-indexed). Default: 1"
        },
        "max_pages": {
          "type": "integer",
          "description": "Maximum number of pages to extract. Default: 10"
        }
      },
      "required": ["path"]
    }
  }
}
```

### Parameters
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `path` | string | Yes | Path to the PDF file |
| `start_page` | integer | No | Starting page (1-indexed, default: 1) |
| `max_pages` | integer | No | Max pages to extract (default: 10) |

### Example Usage
```json
{
  "name": "read_pdf",
  "arguments": {
    "path": "documents/report.pdf",
    "start_page": 1,
    "max_pages": 5
  }
}
```

### Return Value
```json
{
  "status": "success",
  "path": "documents/report.pdf",
  "num_pages": 25,
  "start_page": 1,
  "end_page": 5,
  "pages_extracted": 5,
  "text": "\n--- Page 1 ---\n[extracted text content]\n\n--- Page 2 ---\n[extracted text content]\n...",
  "message": "Successfully extracted text from 5 page(s)"
}
```

### Notes
- Uses Poppler C++ library for text extraction
- PDF files must contain text content (image-based PDFs require OCR)
- Encrypted/password-protected PDFs cannot be read
- Text is extracted page-by-page with `--- Page N ---` markers
- Use `start_page` and `max_pages` to read large PDFs in chunks

---

## 16. Skill Management Tools

daedalus supports the Anthropic Agent Skills open standard for extending agent capabilities. Skills are packages of instructions, scripts, and resources that agents can discover and use to perform tasks more accurately and efficiently.

### 12.1 `discover_skills`

Lists all available skills with their metadata (name, description, version). Use this to find skills that might be relevant to the current task.

#### Parameters

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `directory` | string | No | Optional directory to search for skills. Defaults to configured directories. |

#### Example Usage

```json
{
  "name": "discover_skills",
  "arguments": {}
}
```

#### Return Value

```json
{
  "status": "success",
  "skills": [
    {
      "name": "cpp-best-practices",
      "description": "Writes or reviews C++ code using modern C++ best practices",
      "version": "1.0.0",
      "path": ".agents/skills/cpp-best-practices",
      "is_active": false
    },
    {
      "name": "cmake-helper",
      "description": "Helps with CMake project configuration and build system setup",
      "version": "1.0.0",
      "path": ".agents/skills/cmake-helper",
      "is_active": false
    }
  ],
  "count": 4
}
```

### 12.2 `load_skill`

Loads a specific skill by name, adding its instructions to the agent's context. Use this when you need specialized knowledge or workflows.

#### Parameters

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `skill_name` | string | Yes | Name of the skill to load (e.g., 'cpp-best-practices', 'cmake-helper', 'git-workflow', 'code-review') |

#### Example Usage

```json
{
  "name": "load_skill",
  "arguments": {
    "skill_name": "cpp-best-practices"
  }
}
```

#### Return Value

```json
{
  "status": "success",
  "skill_name": "cpp-best-practices",
  "description": "Writes or reviews C++ code using modern C++ best practices",
  "activated": true,
  "context": "## Skill: cpp-best-practices\n\nThis skill provides guidance for writing high-quality, modern C++ code...\n",
  "message": "Skill 'cpp-best-practices' loaded and activated."
}
```

### 12.3 `get_active_skills`

Returns a list of currently active (loaded) skills. Active skills are automatically included in the agent's context.

#### Parameters

None required.

#### Example Usage

```json
{
  "name": "get_active_skills",
  "arguments": {}
}
```

#### Return Value

```json
{
  "status": "success",
  "skills": [
    {
      "name": "cpp-best-practices",
      "description": "Writes or reviews C++ code using modern C++ best practices"
    }
  ],
  "count": 1
}
```

### 12.4 `search_skills`

Searches skills by query. Matches against skill names and descriptions.

#### Parameters

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `query` | string | Yes | Search query to match against skill names and descriptions |

#### Example Usage

```json
{
  "name": "search_skills",
  "arguments": {
    "query": "cmake build"
  }
}
```

#### Return Value

```json
{
  "status": "success",
  "query": "cmake build",
  "results": [
    {
      "name": "cmake-helper",
      "description": "Helps with CMake project configuration and build system setup",
      "is_active": false
    }
  ],
  "count": 1
}
```

### 12.5 `deactivate_skill`

Deactivates a skill by name, removing it from the active context.

#### Parameters

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `skill_name` | string | Yes | Name of the skill to deactivate |

#### Example Usage

```json
{
  "name": "deactivate_skill",
  "arguments": {
    "skill_name": "cpp-best-practices"
  }
}
```

#### Return Value

```json
{
  "status": "success",
  "skill_name": "cpp-best-practices",
  "deactivated": true,
  "message": "Skill 'cpp-best-practices' deactivated."
}
```

### 12.6 `get_skill_context`

Returns the combined context from all active skills. This shows the instructions that will be included in the agent's context.

#### Parameters

None required.

#### Example Usage

```json
{
  "name": "get_skill_context",
  "arguments": {}
}
```

#### Return Value

```json
{
  "status": "success",
  "context": "## Skill: cpp-best-practices\n\nThis skill provides guidance for writing high-quality, modern C++ code...\n\n## Skill: cmake-helper\n\nThis skill provides guidance for creating and maintaining CMake configurations...\n",
  "has_context": true
}
```

---

## 17. `background_process`

Manage background processes for server applications. Start processes in background, send input, poll output, and stop processes. Use this when you need to run a server or long-running process and communicate with it while continuing the reasoning loop.

### Actions

| Action | Description |
|--------|-------------|
| `start` | Start a process in background |
| `send` | Send input to process stdin |
| `poll` | Get output collected so far |
| `stop` | Kill the process |
| `list` | List all background processes |

### Parameters

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `action` | string | Yes | Action to perform: start, send, poll, stop, or list |
| `command` | string | No | Command to execute (use with action=start) |
| `cwd` | string | No | Working directory for the process (use with action=start) |
| `process_id` | string | No | Process ID returned from start action (use with action=send, poll, stop) |
| `input` | string | No | Input data to send to process stdin (use with action=send) |
| `clear_output` | boolean | No | Whether to clear output after reading (use with action=poll, default false) |

### Example Usage

#### Start a background process

```json
{
  "name": "background_process",
  "arguments": {
    "action": "start",
    "command": "./my_server --port 8080",
    "cwd": "/home/user/project"
  }
}
```

#### Response

```json
{
  "status": "success",
  "process_id": "bg_001",
  "pid": 12345,
  "command": "./my_server --port 8080",
  "started_at": "2024-01-15T10:30:00Z"
}
```

#### Send input to a process

```json
{
  "name": "background_process",
  "arguments": {
    "action": "send",
    "process_id": "bg_001",
    "input": "GET /health HTTP/1.1\nHost: localhost\n\n"
  }
}
```

#### Poll process output

```json
{
  "name": "background_process",
  "arguments": {
    "action": "poll",
    "process_id": "bg_001",
    "clear_output": false
  }
}
```

#### Response

```json
{
  "status": "success",
  "process_id": "bg_001",
  "stdout": "HTTP/1.1 200 OK\nContent-Type: application/json\n\n{\"status\": \"healthy\"}",
  "stderr": "",
  "exited": false
}
```

#### Stop a background process

```json
{
  "name": "background_process",
  "arguments": {
    "action": "stop",
    "process_id": "bg_001"
  }
}
```

#### List all processes

```json
{
  "name": "background_process",
  "arguments": {
    "action": "list"
  }
}
```

#### Response

```json
{
  "status": "success",
  "processes": [
    {
      "process_id": "bg_001",
      "command": "./my_server --port 8080",
      "pid": 12345,
      "started_at": "2024-01-15T10:30:00Z",
      "exited": false
    }
  ],
  "count": 1
}
```

### Notes

- Only processes within the project directory can be started
- Output is captured to files to prevent memory issues with large outputs
- Process IDs are unique identifiers returned by the start action
- Use the `list` action to see all active background processes
- Processes are automatically cleaned up when daedalus exits

---

## Todo list tools

The agent has a per-project, per-agent todo list that persists in
`<project_root>/.daedalus/daedalus[-<agent_name>]/todos.json`.
Default maximum is 100 items; change it with `set_max_todos`.

Todos have a `status` (`pending`, `in_progress`, `done`, or `blocked`) and an
optional `group` (phase/category, e.g. `Understand`, `Plan`, `Implement`,
`Test`, `Verify`). Use groups to keep multi-step tasks organised and use
`in_progress` to show which single task is currently active.

### `todo_add`

| Name   | Type    | Required | Description                                              |
|--------|---------|----------|----------------------------------------------------------|
| text   | string  | Yes      | Task description                                         |
| status | string  | No       | `pending` (default), `in_progress`, `done`, `blocked`    |
| group  | string  | No       | Phase/category (default: `General`)                      |

### `todo_edit`

| Name   | Type    | Required | Description                                              |
|--------|---------|----------|----------------------------------------------------------|
| id     | integer | Yes      | Todo id to update                                        |
| text   | string  | No       | New task description                                     |
| status | string  | No       | `pending`, `in_progress`, `done`, or `blocked`           |
| group  | string  | No       | New phase/category                                       |

At least one of `text`, `status`, or `group` must be provided.

### `todo_delete`

| Name  | Type    | Required | Description                        |
|-------|---------|----------|------------------------------------|
| id    | integer | Yes      | Todo id to remove                  |

### `todo_list`

| Name   | Type   | Required | Description                                                  |
|--------|--------|----------|--------------------------------------------------------------|
| filter | string | No       | `all` (default), `pending`, `in_progress`, `done`, `blocked` |
| group  | string | No       | Optional phase/category filter                               |

Returns a grouped view by `group`, then status, plus a progress summary.

### `todo_count`

No parameters. Returns `total`, `pending`, `in_progress`, `done`, `blocked`,
`max_todos`, and `by_group` counts.

### `set_max_todos`

| Name  | Type    | Required | Description                        |
|-------|---------|----------|------------------------------------|
| max   | integer | Yes      | New maximum (1-1000)               |

Cannot be set lower than the current list size.

### `todo_clear`

No parameters. Removes every todo and resets the list.

---

All tools are validated before execution:

- **Path restriction**: All file operations must be within the project's base directory
- **Protected files**: Files named `prompt.txt` cannot be modified or deleted
- **Safe filesystem operations**: Directory creation uses safe filesystem APIs
- **Shell command sandbox**: Dangerous patterns are blacklisted, user confirmation required
- **Skill script execution**: Scripts are restricted to skill's `scripts/` directory only
