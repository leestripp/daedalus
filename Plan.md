# AgentSink Logging & Messaging Reform — Plan.md

> **Status**: DRAFT — DO NOT IMPLEMENT UNTIL REVIEWED
> **Created**: 2026-07-18

---

## Problem Statement

The current `on_log` callback is being abused as a catch-all for everything:

1. **Developer-facing technical messages** (that should never reach the user) are mixed with user-facing output
2. **[Tool] [/Tool] blocks** are being sent as log messages — they're not logs, they're structured tool data
3. **[SYSTEM_PROMPT]**, **[MCP]**, **[Phase N]** tags pollute the output
4. No clear separation between:
   - What the USER should see (assistant text, tool results)
   - What the DEVELOPER should see (debug info, internal state)
   - What needs to go to SPECIFIC callbacks (tool calls → `on_tool_call`, etc.)

---

## Current Problems (Audit)

### 1. Tool Call/Result Blocks in Logs

**WRONG** (current):
```
[Tool] execute_bash: {"command": "ls -la"}
[/Tool]
```

**RIGHT** — use `on_tool_call()` and `on_tool_result()` callbacks, NOT logs.

### 2. System/Debug Tags in User Output

**WRONG**:
```
[SYSTEM_PROMPT] Merged system prompt from file
[Phase 8.5] Loading default model from settings
[MCP] Connecting to server "filesystem"
```

**RIGHT** — These are developer/debug messages. They should either:
- Be removed entirely (if not useful)
- Go to a separate debug log stream
- Only appear in verbose/debug mode

### 3. No Log Level Usage

Everything is `LogLevel::Info` or `LogLevel::Warn`. No `Debug`, no `Trace`.

---

## Proposed Solution

### Log Levels (XDG Log Spec Compliant)

| Level | Purpose | Example |
|-------|---------|----------|
| `Error` | Failures the user needs to know about | "API key missing", "Connection failed" |
| `Warn` | Recoverable issues | "Retrying...", "Using fallback" |
| `Info` | Significant user-facing events | "Model loaded", "Tool executed" |
| `Debug` | Developer debug info (verbose mode only) | "Cache hit", "Token count" |
| `Trace` | Internal state (never in production) | "Entering function X" |

### What Goes Where

#### User-Facing Output (Normal Mode)
- `on_assistant_delta()` — LLM text stream
- `on_tool_result()` — Tool execution results
- `on_state_change()` — State transitions (e.g., "Thinking...", "Using tools...")
- `on_log(Info/Error/Warn)` — ONLY messages that help the USER understand what's happening

#### Developer/Debug Output (Verbose Mode)
- `on_log(Debug)` — Internal decisions, cache hits, config loading
- `on_log(Trace)` — Function entry/exit, detailed state dumps

#### NEVER in Logs
- `[Tool]` blocks → Use `on_tool_call()` / `on_tool_result()`
- `[SYSTEM_PROMPT]` tags → Remove or make debug-only
- `[MCP]` connection messages → Debug level only
- `[Phase N.N]` tags → Remove or debug level
- Internal function names → Trace level only

---

## Implementation Checklist

### Phase 1: Audit & Categorize Existing Logs

- [ ] Search all `LOG_INFO`, `LOG_WARN`, `LOG_ERROR` calls in codebase
- [ ] Categorize each as: USER | DEVELOPER | REMOVE
- [ ] Create a log message catalog

### Phase 2: Fix the Callbacks

- [ ] Remove all `[Tool]` blocks from logs — they're already in `on_tool_call/result`
- [ ] Ensure `on_tool_call()` fires BEFORE tool execution
- [ ] Ensure `on_tool_result()` fires AFTER tool execution

### Phase 3: Log Level Assignment

- [ ] Assign proper log levels to all remaining log messages
- [ ] Add `Debug` level for verbose-only messages
- [ ] Add `Trace` level for internal state (disabled in release)

### Phase 4: Remove/Tag System Messages

- [ ] Remove `[SYSTEM_PROMPT]` logs OR change to Debug level
- [ ] Remove `[MCP]` connection logs from user output → Debug level
- [ ] Remove `[Phase N.N]` tags → Trace level or remove

### Phase 5: Testing

- [ ] Test that normal mode produces clean user output
- [ ] Test that verbose mode shows developer info
- [ ] Test that `on_tool_call/result` callbacks receive correct data

---

## Example: Before & After

### Before (Current Mess)
```
$ daedalus-cli /project

[SYSTEM_PROMPT] Merging system prompt from file
[Phase 8.5] Loading default model from settings
[MCP] Connecting to server "filesystem"
[Tool] execute_bash: {"command": "ls"}
[/Tool]
[Tool Result] exit_code=0
Thinking...
The files in the directory are:
  - file1.txt
  - file2.txt
```

### After (Clean)
```
$ daedalus-cli /project

[execute_bash] Running: ls
  - file1.txt
  - file2.txt
```

Verbose mode (`--verbose`):
```
$ daedalus-cli --verbose /project

[SYSTEM_PROMPT] Merging from .daedalus/system_prompt.txt
[MCP] Connected to "filesystem"
[execute_bash] Running: ls
  - file1.txt
  - file2.txt
```

---

## Technical Notes

### LogMessage Struct (Future)
Consider adding a structured log message:
```cpp
struct LogMessage {
    LogLevel level;
    std::string_view message;
    std::string_view category;  // "tool", "mcp", "system", "model"
    bool user_visible;          // false = developer only
};
```

### Verbose Mode
```cpp
virtual void on_log(LogLevel level, std::string_view msg, 
                    std::string_view category = "general") noexcept;
```

### Tool Callback Enforcement
- `on_tool_call` MUST be called before `on_tool_result`
- No duplicate logging of tool calls
- No `[Tool]` blocks in log output

---

## Files to Modify

1. `include/daedalus/AgentSink.h` — Add category parameter, consider structured messages
2. `src/core/Logger.cpp/h` — Add Debug/Trace levels, category support
3. `src/library/AgentImpl.cpp` — Audit and fix all log calls
4. `src/tools/*.cpp` — Remove [Tool] blocks, ensure callbacks used
5. `src/mcp/*.cpp` — Move MCP logs to Debug level
6. `demos/daedalus-cli/` — Update sink implementation

---

## Questions for Review

1. Should `[Tool]` blocks appear in verbose mode output at all, or is the callback sufficient?
2. Should `[SYSTEM_PROMPT]` be visible in verbose mode?
3. What's the minimum viable product for this reform?

---

## References

- [XDG Logging Spec](https://specifications.freedesktop.org logger-spec/latest/)
- Current AgentSink interface: `include/daedalus/AgentSink.h`
- Current Logger: `src/core/Logger.cpp`
