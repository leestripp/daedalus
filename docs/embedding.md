# Embedding daedalus

daedalus ships as a static library (`libdaedalus.a`) that exposes a narrow
public API. This document is the complete embedding guide: minimum viable
host, sink contract, config, lifecycle, cancellation, composition
patterns, and network/MCP integration.

**See also:** `docs/index.md` for a map of all documentation. For
the conversation-history lifecycle (atomic save, write-through
auto-flush, SIGSEGV/SIGABRT handler, drop-last-message recovery),
see `docs/crash_resilience.md`.

daedalus ships as a static library (`libdaedalus.a`) that exposes a narrow
public API. This document is the embedding guide for host applications.

## Minimum viable host

```cpp
#include <daedalus.h>
#include <iostream>

class StdoutSink : public daedalus::AgentSink {
public:
    void on_assistant_delta(std::string_view text) noexcept override {
        std::cout << text << std::flush;
    }
    void on_tool_call(std::string_view name,
                      const nlohmann::json& /*args*/) noexcept override {
        std::cerr << "\n[tool] " << name << " ...\n";
    }
    void on_tool_result(std::string_view name,
                        const nlohmann::json& result) noexcept override {
        std::cerr << "[tool] " << name << " -> " << result.dump() << "\n";
    }
    void on_state_change(int /*from*/, int to) noexcept override {
        std::cerr << "\n[state] "
                  << daedalus::to_string(static_cast<daedalus::AgentState>(to))
                  << "\n";
    }
    void on_log(daedalus::LogLevel lvl, std::string_view msg) noexcept override {
        std::cerr << "[" << daedalus::to_string(lvl) << "] " << msg << "\n";
    }
    bool request_command_approval(std::string_view cmd) override {
        std::cerr << "Approve: " << cmd << " [y/N] " << std::flush;
        char c{}; std::cin.get(c);
        std::cin.ignore(1 << 16, '\n');
        return c == 'y' || c == 'Y';
    }
};

int main() {
    daedalus::AgentConfig cfg;
    cfg.project_root = ".";
    cfg.daedalus_home = daedalus::Paths::global().user_root();   // $XDG_DATA_HOME/daedalus
    cfg.session_id = "demo-session";

    auto sink = std::make_shared<StdoutSink>();
    daedalus::Agent agent(std::move(cfg), sink);
    agent.initialize();

    std::string input;
    while (std::getline(std::cin, input) && input != "/quit") {
        agent.submit_user_message(std::move(input));
        agent.run_one_turn();      // one user msg → loop → stop/cancel/error
        std::cout << '\n';
    }
    // dtor calls shutdown() automatically; explicit call is also fine.
}
```

Link with `-ldaedalus -lcurl -lpthread -lssl -lcrypto -lpng -lpoppler-cpp`
(or whatever subset your host needs).

## Lifecycle

| Step | Call | Notes |
|------|------|-------|
| Construct | `Agent(cfg, sink)` | Does not touch network/disk. |
| Initialize | `agent.initialize()` | Opens `SettingsStore`, loads MCP, skills, memory. Idempotent. |
| Drive a turn | `agent.submit_user_message(t)` then `agent.run_one_turn()` | Returns when agent emits `stop()`, errors, or is cancelled. |
| Inspect state | `agent.state()`, `agent.stats()` | Read-only. |
| Cancel | `agent.cancel()` (or `run_one_turn(stop_token)`) | Idempotent. |
| Tear down | `agent.shutdown()` | Idempotent. Called by dtor. |

### The AgentSink Contract

`daedalus::AgentSink` is the **only** way the library talks to the host.
There is no `std::cout`, no `std::cin`, no signal handler, no global
callback. The host implements `AgentSink`, hands a `shared_ptr` to the
`Agent` at construction time, and observes the agent's life as a
sequence of sink calls.

All methods have `{}` defaults. **The only one the host is required to
implement is `on_log`** (and even that has a stderr default). All others
may be left as no-ops.

```cpp
struct daedalus::AgentSink {
    virtual ~AgentSink() = default;

    // Streaming (one per SSE chunk)
    virtual void on_assistant_delta(std::string_view text) noexcept;

    // Tool lifecycle
    virtual void on_tool_call(std::string_view name,
                              const nlohmann::json& args) noexcept;
    virtual void on_tool_result(std::string_view name,
                                const nlohmann::json& result) noexcept;

    // State / lifecycle
    virtual void on_state_change(int from, int to) noexcept;
    virtual void on_log(daedalus::LogLevel level,
                        std::string_view message) noexcept;

    // Host I/O requests (block the agent thread)
    virtual bool        request_command_approval(std::string_view command);

    // Optional notifications
    virtual void on_bell() noexcept;
    virtual void on_compaction(std::size_t before, std::size_t after) noexcept;
};
```

**Event sequence (typical interactive turn):**

```
1. submit_user_message("fix the bug")            [host]
2. run_one_turn()                                [host]
3. on_state_change(Idle, Understanding)         [sink]
4. on_log(Info, "[PERSIST] Restored N messages")
5. (SSE tokens) on_assistant_delta(…)             [sink, many times]
6. on_tool_call("execute_bash", {...})           [sink]  # e.g. grep / sed / cat
7. on_tool_result("execute_bash", {...})         [sink]
8. on_tool_call("execute_bash", {...})           [sink]  # e.g. cmake --build
9. request_command_approval("cmake --build")     [sink — blocking]
10. on_tool_result("execute_bash", {...})        [sink]
13. on_state_change(Acting, Done)                [sink]
14. run_one_turn() returns                       [host]
```

### Callback Data Reference

**IMPORTANT: Know which callback receives what data.**

This section documents exactly what data each `AgentSink` callback receives,
in what format, and what it means for your host application.

### Callback Summary

| Callback | Data Format | Purpose |
|----------|-------------|---------|
| `on_assistant_delta` | `text` (string_view) | **Streaming text tokens** — raw text chunks from LLM as they arrive |
| `on_tool_call` | `name` + `call_id` + `args` (JSON) | **Structured tool input** — the tool the LLM wants to execute |
| `on_tool_progress` | `name` + `call_id` + `progress` (JSON) | **Tool progress updates** — `{stage, percent, message}` |
| `on_tool_result` | `name` + `call_id` + `result` (JSON) | **Structured tool output** — tool execution result |
| `on_state_change` | `from` + `to` (int enum values) | **State transitions** — agent lifecycle |
| `on_log` | `level` (enum) + `message` (string) | **Library diagnostics** — NOT LLM output |
| `on_compaction` | `before` + `after` (size_t) | **Context compression stats** |
| `on_bell` | none | **Notification signal** — ring the bell |
| `request_command_approval` | `command` (string) | **Bash approval** — return true/false |

---

### `on_assistant_delta(text)` — Streaming LLM Text

```cpp
void on_assistant_delta(std::string_view text) noexcept override;
```

**Called when:** LLM sends a text token via SSE streaming.

**Data:**
- `text`: Raw text chunk, typically 1-20 characters
- Called repeatedly as tokens arrive (100s of times per response)
- Empty chunks are possible

**What to do:**
- Accumulate in a buffer and display to user
- Stop any "thinking" spinner/animation when first text arrives
- **DO NOT** parse tool calls from this — use `on_tool_call` instead

**Example SSE events from webui:**
```json
{"event":"assistant_delta","data":"Hello"}
{"event":"assistant_delta","data":"!"}
{"event":"assistant_delta","data":" Let me find that"}
```

---

### `on_tool_call(name, call_id, args)` — Tool Execution Request

```cpp
void on_tool_call(std::string_view name,
                  std::string_view call_id,
                  const nlohmann::json& args) noexcept override;
```

**Called when:** LLM wants to execute a tool (before execution).

**Data:**
- `name`: Tool name (e.g., `"execute_bash"`, `"tell_user"`, `"mcp__<server>__<tool>"`)
- `call_id`: OpenAI-style correlation ID (empty string if not provided)
- `args`: Tool arguments as parsed JSON object

**Common tool argument formats:**

#### execute_bash
```json
{
  "name": "execute_bash",
  "call_id": "call_abc123",
  "args": {
    "command": "cmake --build build -j8"
  }
}
```

#### tell_user
```json
{
  "name": "tell_user",
  "call_id": "call_def456",
  "args": {
    "content": "I've analyzed the code and found the issue."
  }
}
```

#### MCP tools
```json
{
  "name": "mcp__filesystem__read_file",
  "call_id": "call_ghi789",
  "args": {
    "path": "/absolute/path/to/file"
  }
}
```

---

### `on_tool_progress(name, call_id, progress)` — Long-Running Tool Progress

```cpp
void on_tool_progress(std::string_view name,
                     std::string_view call_id,
                     const nlohmann::json& progress) noexcept override;
```

**Called when:** A long-running tool emits progress updates (between call and result).

**Data:**
```json
{
  "stage": "starting" | "executing" | "finalizing" | "done",
  "percent": 0-100,           // optional, for progress bars
  "message": "human-readable"  // optional, e.g., "Compiling..."
}
```

**Example (execute_bash during build):**
```json
{
  "name": "execute_bash",
  "call_id": "call_xyz",
  "progress": {
    "stage": "executing",
    "percent": 45,
    "message": "Building target: daedalus-cli"
  }
}
```

---

### `on_tool_result(name, call_id, result)` — Tool Execution Result

```cpp
void on_tool_result(std::string_view name,
                   std::string_view call_id,
                   const nlohmann::json& result) noexcept override;
```

**Called when:** Tool execution completes (success or failure).

**Common result formats by tool type:**

#### execute_bash — Success
```json
{
  "name": "execute_bash",
  "call_id": "call_abc",
  "result": {
    "status": "success",
    "command": "ls -la",
    "output": "total 12\ndrwxr-xr-x  4 user user 4096 Jul 18 08:00 .\ndrwxr-xr-x  2 user user 4096 Jul 18 08:00 src",
    "exit_code": 0
  }
}
```

#### execute_bash — Error (non-zero exit)
```json
{
  "name": "execute_bash",
  "call_id": "call_abc",
  "result": {
    "status": "error",
    "message": "Command failed with exit code 1",
    "command": "cmake --build build",
    "output": "CMake Error: ...\nmake: *** [Makefile:123: all] Error 1",
    "exit_code": 1
  }
}
```

#### execute_bash — Background process started
```json
{
  "name": "execute_bash",
  "call_id": "call_server",
  "result": {
    "status": "background",
    "process_id": "bg_1",
    "pid": 12345,
    "command": "python3 -m http.server 8000",
    "message": "Process started in background. Use action=poll with process_id to read output, action=send to write stdin, action=stop to terminate."
  }
}
```

#### tell_user
```json
{
  "name": "tell_user",
  "call_id": "call_def",
  "result": {
    "status": "success",
    "tool_name": "tell_user",
    "message": "User told"
  }
}
```

#### MCP tools
```json
{
  "name": "mcp__filesystem__read_file",
  "result": {
    "status": "success",
    "server": "filesystem",
    "tool": "read_file",
    "result": "... parsed JSON or string result ..."
  }
}
```

#### stop (agent finished)
```json
{
  "name": "stop",
  "result": {
    "status": "success",
    "message": "stop",
    "summary": "Fixed the memory leak in src/core/logger.cpp by adding proper cleanup."
  }
}
```

#### Error result (any tool)
```json
{
  "name": "some_tool",
  "result": {
    "status": "error",
    "message": "Error description here"
  }
}
```

---

### `on_state_change(from, to)` — Agent State Transition

```cpp
void on_state_change(int from, int to) noexcept override;
```

**Called when:** Agent transitions between states.

**Data:**
- `from`: Previous state (integer enum value)
- `to`: New state (integer enum value)

**State values:**
```cpp
enum class AgentState : std::uint8_t {
    Idle = 0,             // Constructed, no work in flight
    Understanding = 1,    // ANALYSE phase
    Planning = 2,         // PLANNING phase
    Acting = 3,           // ACT phase (tools, edits)
    Verifying = 4,        // BUILD_TEST phase
    AwaitingApproval = 5, // Bash approval in flight
    Compressing = 6,      // Context compression in progress
    Done = 7,             // Agent emitted stop() cleanly
    Cancelled = 8,        // Cancelled by host
    Error = 9             // Unrecoverable error
};
```

**Helper to get state name:**
```cpp
daedalus::to_string(static_cast<daedalus::AgentState>(to))
// Returns: "Idle", "Understanding", "Planning", "Acting", etc.
```

**Typical sequence:**
```
on_state_change(0, 1)   // Idle -> Understanding
on_state_change(1, 2)   // Understanding -> Planning
on_state_change(2, 3)   // Planning -> Acting
on_state_change(3, 7)   // Acting -> Done
```

**CLI behavior:** Shows a "Thinking..." spinner during Thinking states
(Understanding, Planning, Acting, Verifying, Compressing).

---

### `on_log(level, message)` — Library Diagnostics

```cpp
void on_log(daedalus::LogLevel level,
            std::string_view message) noexcept override;
```

**IMPORTANT: This is NOT for LLM data.** It's for library internal
diagnostics only.

**Data:**
- `level`: Severity level (see below)
- `message`: Human-readable diagnostic string

**LogLevel values:**
```cpp
enum class LogLevel : std::uint8_t {
    Trace,  // Very verbose — internal state machine transitions
    Debug,  // Diagnostic for library developers
    Info,   // High-level events (session restore, system prompt, etc.)
    Warn,   // Recoverable issues (MCP server down, retry, etc.)
    Error,  // Network failures, parse errors, tool errors
    Fatal   // Never seen — agent transitions to Error state instead
};
```

**Common log messages:**

| LogMessage | Level | Meaning |
|------------|-------|---------|
| `"[PERSIST] Restored N messages"` | Info | Session loaded from disk |
| `"[PERSIST] Saved N messages"` | Info | Session saved to disk |
| `"[SYSTEM_PROMPT] Merged project override: ..."` | Info | Project system prompt loaded |
| `"[MCP] WARNING: tool schemas consume ~N tokens..."` | Warn | Large MCP schemas detected |
| `"Waiting 2 seconds before retry..."` | Info | Retry backoff |
| `"[MEMORY] Conversation history wiped"` | Info | Memory cleared |
| `"Tool execute_bash failed: exit code 1"` | Error | Tool execution error |
| `"[COMPACT] 5000 -> 3000 tokens"` | Info | Context compression completed |

**What to do:**
- Route to your log window, file, or console
- `Error` level from tool results is pre-formatted with the tool's
  output (exit codes, command output, etc.)
- This is NOT the place to display LLM responses — use `on_assistant_delta`

---

### `on_compaction(before, after)` — Context Compression Event

```cpp
void on_compaction(std::size_t before, std::size_t after) noexcept;
```

**Called when:** Context was compressed to fit within token budget.

**Data:**
- `before`: Token count before compression
- `after`: Token count after compression

**Example:**
```
on_compaction(8000, 4200)
// Context was compressed from 8000 tokens down to 4200 tokens
```

---

### `on_bell()` — Notification Signal

```cpp
void on_bell() noexcept;
```

**Called when:** Agent wants to notify the user (terminal bell, sound, etc.).

**What to do:**
- CLI: Print `\a` or ring terminal bell
- Web: Play a notification sound
- GUI: Flash window or show notification

---

### `request_command_approval(command)` — Bash Approval

```cpp
bool request_command_approval(std::string_view command);
```

**Called when:** `execute_bash` tool is invoked and `auto_approve` is false.

**Data:**
- `command`: The bash command string

**Returns:**
- `true`: Allow the command to execute
- `false`: Deny the command (`execute_bash` returns an error result to the LLM)

**Throws:** Can throw to abort the current turn.

**Blocking:** This callback runs on the agent's worker thread and blocks the
agent until the host returns. Interactive hosts should prompt the user;
headless hosts can implement a policy (allow-list, deny-list, always-deny,
etc.).

**Security pre-checks:** Before this callback is invoked, `BashTool` already
rejects privilege escalation (`sudo`, `su -`, ...), dangerous patterns
(`rm -rf /`, fork bombs, ...), and standalone background operators (`&`).
The sink is responsible for the final user/policy approval.

**Session/persisted approvals:** If the user approves with a session save
(e.g. the CLI's `s` option), `BashTool` stores the pattern in
`<project>/.daedalus/daedalus[-<name>]/approvals.json` and future matching
commands bypass the sink. The sink only sees commands that are not already
approved.

**Auto-approve:** If `AgentConfig::auto_approve` is `true` (OR'd with
`SettingsStore::get_auto_approve()`), this callback is **never called**.

---

### Event Sequence (Typical Interactive Turn)

```
1. submit_user_message("fix the bug")           [host]
2. run_one_turn()                               [host]
3. on_state_change(Idle, Understanding)         [sink]
4. on_log(Info, "[PERSIST] Restored 42 messages")
5. (SSE tokens) on_assistant_delta("I'm analyzing...")  [sink, many times]
6. on_state_change(Understanding, Planning)     [sink]
7. on_tool_call("execute_bash", "call_1", {...})  [sink]  # e.g. grep / sed
8. on_tool_result("execute_bash", "call_1", {...}) [sink]
9. on_state_change(Planning, Acting)            [sink]
10. on_tool_call("execute_bash", "call_2", {...}) [sink]  # e.g. cmake --build
11. request_command_approval("cmake --build .")  [sink — blocking]
12. on_tool_result("execute_bash", "call_2", {...}) [sink]
15. on_state_change(Acting, Done)                [sink]
16. run_one_turn() returns                       [host]
```

---

### Threading Notes

All sink methods are called on the **agent's worker thread**. Marshal to
your UI thread as needed:

**GTK4:**
```cpp
void on_assistant_delta(std::string_view t) noexcept override {
    g_idle_add(+[](gpointer data) -> gboolean {
        auto* s = static_cast<std::string*>(data);
        gtk_label_set_text(label_, gtk_label_get_text(label_) + *s);
        delete s;
        return G_SOURCE_REMOVE;
    }, new std::string(t));
}
```

**Web SSE:**
```cpp
void on_assistant_delta(std::string_view t) noexcept override {
    json j = {{"text", std::string(t)}};
    sse_->broadcast("assistant_delta", j.dump());
}
```

All sink methods are `noexcept` **except** the two `request_*` methods —
throwing from those aborts the current turn.

**Order guarantee**: within a single turn, sink calls are **strictly
ordered** on the same thread. Multiple Agents are independently in-order.

### Settings, memory, MCP paths

> **v0.4.0+:** daedalus no longer reads `settings.json` at runtime —
> the SQLite-backed `daedalus::SettingsStore` is the single source of
> truth. The `AgentConfig` `daedalus_home` field points the Agent at
> the store directory (`$XDG_DATA_HOME/daedalus/daedalus.db` by
> default). The legacy `cfg.settings_path` /
> `cfg.mcp_servers_path` / `cfg.mcp_env_path` / `cfg.memory_dir`
> fields have been removed — see `docs/settings.md` for the new API.

The same priority applies to the `daedalus_home` field. Use
`$DAEDALUS_HOME` for tests / CI / shared configs; use
`AgentConfig::daedalus_home` when the path is host-specific.

```cpp
// Normal host: settings live under $XDG_DATA_HOME/daedalus
daedalus::AgentConfig cfg;
cfg.daedalus_home = daedalus::Paths::global().user_root();

// Isolated test: settings live under /tmp/test-daedalus/
daedalus::AgentConfig cfg;
cfg.project_root  = "/tmp/test-daedalus/project";
cfg.daedalus_home = "/tmp/test-daedalus";   // SettingsStore will look here
```

#### `cfg.daedalus_home` — per-Agent config directory (§Phase 8.5)

If you want a single Agent to have its own isolated
settings/skills/memory directory — set `cfg.daedalus_home`. The Agent
hands this path to `SettingsStore::for_home()` and gets a private
DB without env-var gymnastics.

```cpp
// Two Agents in one process, each with its own config:
daedalus::AgentConfig cfg_a;
cfg_a.daedalus_home = "/tmp/agent-a/.daedalus";

daedalus::AgentConfig cfg_b;
cfg_b.daedalus_home = "/tmp/agent-b/.daedalus";

// Agent A reads /tmp/agent-a/.daedalus/daedalus.db
// Agent B reads /tmp/agent-b/.daedalus/daedalus.db
// They don't see each other's settings, MCP, or memory.
daedalus::Agent agent_a(std::move(cfg_a), sink_a);
daedalus::Agent agent_b(std::move(cfg_b), sink_b);
```

Resolution priority (highest first):

| Priority | Source | Used for |
|----------|--------|----------|
| 1 | `cfg.daedalus_home` | Explicit per-Agent override |
| 2 | `$DAEDALUS_HOME` env var | Global override (CI / testing / shared) |
| 3 | `$XDG_DATA_HOME/daedalus` (or `$HOME/.daedalus` legacy) | Per-user default |

Two Agents in one process can have entirely separate configurations
without using environment variables.

#### `cfg.agent_name` — per-Agent runtime-state namespace (§Phase 8.8)

If you want two Agents in the **same project** to have isolated
runtime state — separate `messages.json` and `memory.json` so they
don't trample each other — set `cfg.agent_name` on each. Empty
(default) means the default namespace. Non-empty values namespace the
data folder under a single `.daedalus/` base:

| `cfg.agent_name` | Data lives at |
|------------------|---------------|
| `""` (default)   | `<project_root>/.daedalus/daedalus/` |
| `"bob"`          | `<project_root>/.daedalus/daedalus-bob/` |
| `"agent_007"`    | `<project_root>/.daedalus/daedalus-agent_007/` |

```cpp
// Two agents, same project, no collision:
daedalus::AgentConfig cfg_a;
cfg_a.project_root = "/work/myproject";
cfg_a.agent_name   = "alice";   // uses /work/myproject/.daedalus/daedalus-alice/

daedalus::AgentConfig cfg_b;
cfg_b.project_root = "/work/myproject";
cfg_b.agent_name   = "bob";     // uses /work/myproject/.daedalus/daedalus-bob/

// Each Agent's `messages.json` is fully isolated. Two agents can
// run in parallel threads, fork/clone
// (cp -r .daedalus/daedalus-bob .daedalus/daedalus-clone), or get
// cleanly deleted (rm -rf .daedalus/daedalus-bob/) without touching
// the other.
daedalus::Agent agent_a(std::move(cfg_a), sink_a);
daedalus::Agent agent_b(std::move(cfg_b), sink_b);
```

Validation (enforced by `Agent::initialize()`):

- Empty string is allowed (backward-compat default).
- 1–64 chars, must match `[A-Za-z0-9_.-]+`.
- Must NOT start with `.` (hidden dir / `..` escape).
- Must NOT start with `-` (looks like a CLI flag).
- Must NOT contain `/`, `\`, whitespace, NUL, or other punctuation.

A bad name throws `std::invalid_argument` from
`Agent::initialize()` with a clear message — no silent
filesystem write happens.

**Orthogonal to `daedalus_home`** (Phase 8.5): `daedalus_home` is
the *config-source* axis (the `SettingsStore` DB, MCP servers, skills).
`agent_name` is the *runtime-state* axis (`messages.json`,
`memory.json` inside the project). They don't combine into one
path.

Namespaced: `messages.json`, `memory.json`, session files.
NOT namespaced (project-shared): `skills/`.
The `SettingsStore` DB path is governed by `daedalus_home` (Phase
8.5), not by `agent_name` — a "bob" agent reads the same DB as
any other agent in the project.

Helper:
```cpp
bool daedalus::is_valid_agent_name(std::string_view name);
fs::path daedalus::resolve_agent_dir(const fs::path& project_root,
                                    std::string_view agent_name);
```

| Cancel | `agent.cancel()` or pass `std::stop_token` into `run_one_turn` | Idempotent. |
| Tear down | `agent.shutdown()` | Idempotent. Called by dtor. |

## Configuration

`daedalus::AgentConfig` is the single config the host fills in. See
[`include/daedalus/types/AgentConfig.h`](../include/daedalus/types/AgentConfig.h)
for the full field list. The most-used fields:

| Field | Purpose |
|-------|---------|
| `project_root` | Working directory. The agent reads and writes under this. |
| `daedalus_home` | Base directory for the SQLite settings store (and any per-Agent data). |
| `model_name` | Looked up in `SettingsStore` → host / key / endpoint / model id. |
| `api_key`, `api_host`, `chat_endpoint` | Optional overrides (skip settings lookup). |
| `auto_approve` | If true, `request_command_approval` is never called. OR'd with `SettingsStore::get_auto_approve()`. |
| `max_agent_iterations` | Hard cap. The agent stops when this is reached. |
| `memory_enabled` / `clear_memory` | Toggle persistence. |
| `session_id` | Opaque correlation id (logged, surfaced to sinks). |
| `system_prompt` | Override the role the agent plays. See [`docs/agent_system_prompts.md`](./agent_system_prompts.md) — the library appends 9 more sections (tool defs, MCP status, skills, memory, context stats, and the two **always-on** `# State Machine` + `# Reserved Agent Directories` blocks) below your text. |
| `stream` | `true` (default): SSE streaming — assistant text arrives token-by-token via `on_assistant_delta` (requires an SSE-capable sink). `false`: buffered — full reply arrives in one chunk. |
| `temperature` | Sampling temperature. See "Temperature priority chain" below. |
| `mcp_enabled` | `true` (default): load MCP servers from `SettingsStore::list_mcp_servers()` (or legacy JSON). `false`: skip MCP entirely (no servers spawned, no MCP tools registered, no `# Active MCP Servers` block). |
| `mcp_servers_disabled` | Per-server gate. Set of server names (matching the `name` column in the `mcp_servers` table) that the host has disabled. Empty = all enabled. Disabled servers never spawn, never enter the tool registry, and don't appear in the system prompt. See "Per-server MCP gate" below. |
| `skills_dirs` | Directories to scan for `SKILL.md` files. Default empty = no skills. |
| `skills` | Names to pre-activate. Loaded from any dir in `skills_dirs`. |
| `skills_disabled` | Per-skill gate. Set of skill names (matching `metadata.name`) that the host has disabled. Empty = all enabled. Disabled skills don't appear in the catalog, can't be pre-activated, and `load_skill <name>` fails at runtime. See "Per-skill gate" below. |

If `model_name` is empty *and* there are no explicit `api_key` + `api_host`,
`initialize()` skips creating the internal `LLMClient`. This is the
"smoke-test" mode: the agent constructs, accepts messages, and reports
`Error` when asked to act. Useful for hosts that just want to verify
their wiring without burning API quota.

### Streaming vs buffered responses (§Phase 9.8)

The `stream` field controls how the assistant's reply is delivered:

- **`stream = true` (default)**: The MiniMax API returns SSE
  (`data: {...delta...}` per token). Each token fires `on_assistant_delta`
  on the sink.  Hosts with an SSE-capable sink (daedalus-webui, CLI demo)
  show the reply appearing word-by-word in real time.

- **`stream = false`**: The MiniMax API returns a buffered JSON response.
  The full reply arrives in one `on_done` call.  Use this for hosts that
  do not support progressive rendering (test fixtures, deterministic-replay
  tools, embedded agents that wrap their own batching).

```cpp
// Streaming (default — webui, CLI demo)
daedalus::AgentConfig cfg;
cfg.stream = true;  // default; can be omitted

// Buffered (off-switch — test fixtures, embedded batch tools)
daedalus::AgentConfig cfg;
cfg.stream = false;
```

Both modes produce identical final text; only the delivery cadence differs.
The off-switch path has the same token-count, tool-call, and error
handling behaviour as the streaming path.

### Per-server MCP gate (§Phase 15)

`cfg.mcp_servers_disabled` is a `std::set<std::string>` of server
names (matching the keys in `mcp_servers.json`). The disabled set
filters at three layers:

1. **`McpServerManager::load_from_store()`** (or `load_config()`) drops
   disabled entries from `configs_`, so the spawn step never even sees
   them.
2. **`list_connected_servers()` / `get_all_tools()`** filter at the
   output boundary — defense-in-depth if `set_disabled_servers()` is
   called after `load_config()` (e.g., reload at runtime).
3. **`build_system_instruction()`** therefore never sees a disabled
   server in the `# Active MCP Servers` block or in the tool JSON
   schema.

```cpp
daedalus::AgentConfig cfg;
// Default: load every server from the SettingsStore registry.
cfg.mcp_enabled             = true;

// We want MCP enabled, but not the (costly) web-search server.
cfg.mcp_servers_disabled    = { "minimax-web-search", "minimax-wiki" };
```

If `cfg.mcp_enabled = false`, `mcp_servers_disabled` is irrelevant —
no servers spawn at all. The two switches are complementary, not
overlapping.

If a name in `mcp_servers_disabled` doesn't match any key in
`servers.json`, `Agent::initialize()` does NOT throw — it's
silently ignored (the servers.json may be reloaded later and the
name may appear). Hosts that want strict validation can call
`mcp_manager->list_configured_servers()` after construction and
warn on missing names.

### Per-skill gate (§Phase 15)

`cfg.skills_disabled` is a `std::set<std::string>` of skill names
(matching `metadata.name` from the YAML frontmatter in
`SKILL.md`). Same three-layer pattern:

1. **`SkillManager::get_visible_cached_skills()`** excludes disabled
   entries from the catalog the agent sees.
2. **`SkillManager::get_visible_active_skills()`** excludes disabled
   entries even if they were previously activated (i.e., disabling
   at runtime removes the skill mid-session).
3. **`SkillManager::activate_skill()`** refuses disabled entries.
   The `load_skill` tool surfaces this as:
   ```
   {"error":"skill 'firefox' is disabled by host config"}
   ```

```cpp
daedalus::AgentConfig cfg;
cfg.skills_dirs = { daedalus::Paths::global().global_skills_dir() }; // discover
cfg.skills      = { "memory-leak" };                                // pre-activate
cfg.skills_disabled = { "firefox" };                                 // but not this one
```

Empty `skills_disabled` (default) is fully backward-compatible —
all discovered skills are visible.

### Per-project `system_prompt.txt` override (§Phase 14.7 Q2)

If `<project_root>/.daedalus/daedalus/system_prompt.txt` exists,
`Agent::initialize()` reads it and **appends** it to `cfg.system_prompt`
(or uses it as-is when `cfg.system_prompt` is empty). Demo's text is the
base; project text is concatenated after with a blank-line separator.

```
$XDG_DATA_HOME/daedalus/                          ← user config & settings (v0.4.0+)
└── daedalus.db                                  ← SettingsStore (SQLite)

<project_root>/                                  ← runtime state
└── .daedalus/
    └── daedalus[-<agent_name>]/
        ├── messages.json
        ├── memory.json
        └── system_prompt.txt      ← (NEW) merged into cfg.system_prompt
```

Use this to ship **project-local** rules that should apply to every
Agent in the project, regardless of which demo or host is driving it:

```text
# <project_root>/.daedalus/daedalus/system_prompt.txt
This project uses Python 3.12 + uv. Always run `uv run pytest` instead of
plain `pytest`. Use the `ruff` formatter, never black. Database migrations
live in `db/migrations/` and must be applied in alphabetical order.
```

The library emits a `[SYSTEM_PROMPT]` [INFO] log line when the merge
happens so hosts can audit what got injected. Missing file, missing
`.daedalus/daedalus/` dir, or empty file → silent no-op (no warning, no
error).

### Temperature priority chain (§Phase 8.7)

The sampling temperature sent on every LLM request is resolved from a
3-level chain (highest priority first):

1. **`cfg.temperature >= 0.0`** — explicit session override. Hosts set
   this in their `AgentConfig`. Use it to bias the model per-session
   (e.g. 0.7 for a creative-writing demo, 0.0 for a strict code review).
2. **Model's value from `SettingsStore`** — set per model entry via
   `SettingsStore::upsert_model({ ..., .temperature = 0.7 })`:
   ```cpp
   store.upsert_model(daedalus::ModelConfig{
       .name        = "minimax-m3",
       .model_id    = "MiniMax-M3",
       .temperature = 0.7,
   });
   store.upsert_model(daedalus::ModelConfig{
       .name        = "minimax-m2.7",
       .model_id    = "MiniMax-M2.7",
       .temperature = 0.0,   // deterministic for code agents
   });
   ```
   Lets one SettingsStore serve multiple use cases: deterministic for
   the code agent, exploratory for the brainstorming demo.
3. **`0.0`** — library default. Deterministic (good for code agents).

The sentinel `cfg.temperature = -1.0` (the default) means "use the
model's value", so hosts that don't care can simply not set it.

```cpp
// Tight code review (deterministic)
daedalus::AgentConfig cfg;
cfg.temperature = 0.0;

// Creative brainstorming session
cfg.temperature = 0.7;

// Use the model's SettingsStore value (default — omit the field)
cfg.temperature = -1.0;
```

`AgentConfig::temperature` is sent both on the main chat request
(`run_agent_cycle`) and on the context-compression summary request
(`call_llm_for_summary`), so summaries follow the same sampling
behaviour as the live conversation. Read back what was actually used
via `agent.active_model().temperature` after `initialize()`.

## State machine

`agent.state()` returns one of:

| State | Meaning |
|-------|---------|
| `Idle` | Constructed, no work in flight. |
| `Understanding` | Analysing the request. |
| `Planning` | Research / strategy. |
| `Acting` | Running tools. |
| `Verifying` | Build / test phase. |
| `AwaitingApproval` | Bash approval in flight (sink blocked). |
| `Compressing` | Context compression in progress. |
| `Done` | Agent emitted `stop()` cleanly. |
| `Cancelled` | Host cancelled (cancel() or stop_token). |
| `Error` | Unrecoverable error. |

> **Removed in v0.4.0:** `AwaitingUser`. The `ask_user` tool and the
> sink-blocking `request_user_input` were removed; the agent has no
> mid-turn user-prompting path. If your host needs to interrupt a
> turn, call `agent.cancel()` and prompt the host's outer loop.

See [`sink.md`](sink.md) for the events the sink receives as the
state machine transitions.

## Threading

`Agent` instances are **not thread-safe to share**. Construct one per
session / per UI pane. To drive multiple Agents concurrently, give each
its own thread.

The agent calls sink methods on its own thread. Hosts that need to
marshal events to a different thread (e.g. GTK4 main loop, web SSE)
should post from inside the sink callback:

```cpp
void on_assistant_delta(std::string_view text) noexcept override {
    g_idle_add(+[](gpointer data) -> gboolean {
        auto* s = static_cast<GtkWidget*>(data);
        // append `s` to your label
        return G_SOURCE_REMOVE;
    }, my_label);
}
```

## Cancellation

`run_one_turn(std::stop_token)` honours the stop token. The token fires
on the calling thread; the agent checks it between API chunks and
between tool calls. `cancel()` is equivalent (and idempotent).

The library never installs signal handlers. Hosts own signals
(`SIGINT`, `SIGTERM`, etc.) and decide how to propagate to the agent.

## Wiring your own input source (§Phase 5)

The library is **input-source agnostic**. It never reads from
`std::cin`, never installs signal handlers, never sets terminal
raw mode. There is exactly one way to feed input to a running
agent:

1. **`Agent::submit_user_message(std::string)`** — push text into
   the agent's pending-message queue. The next `run_one_turn()`
   consumes it. Use this from any host (CLI, web, tests,
   background job) that has its own input pipeline.

> **Removed in v0.4.0:** `AgentSink::request_user_input(...)` and the
> `ask_user` tool. The agent has no mid-turn user-prompting path.
> If your host needs a back-and-forth, submit the next user message
> in the host's outer loop between `run_one_turn()` calls (or call
> `agent.cancel()` to interrupt the current turn first).

### Example: minimal std::getline host

```cpp
struct StdcinSink : public daedalus::AgentSink {
    void on_log(daedalus::LogLevel, std::string_view msg) noexcept override {
        std::cerr << msg << '\n';
    }
    bool request_command_approval(std::string_view cmd) override {
        std::cout << "\nRun? " << cmd << " [y/n] " << std::flush;
        std::string line; std::getline(std::cin, line);
        return !line.empty() && (line[0] == 'y' || line[0] == 'Y');
    }
};
```

### Example: host with line-editing + history (linenoise)

If your host wants arrow keys, history recall, completion, etc. —
typical of a CLI REPL — bring your own input library. The
daedalus library doesn't ship one. The pattern:

1. Drop a copy of [linenoise](https://github.com/antirez/linenoise)
   (or [linenoise-ng](https://github.com/arangodb/linenoise-ng), or
   [readline](https://tiswww.case.edu/php/chet/readline/rltop.html),
   or your favourite equivalent) into **your host's** source tree.
2. In your host's main loop, call your input library's line-editing
   function and feed the result into `agent.submit_user_message()`
   between `run_one_turn()` calls.
3. On shutdown call your library's history-save function if you want
   persistent history.

The daedalus library doesn't care which input library you use, how
you format the prompt, or whether you support multi-line edit —
those are all host concerns.

## Composition patterns

The library has **one** loop. Patterns like "manager" or "sub-agent"
that used to be flags are now host-side loops driving multiple Agents:

- **Manager**: spawn N Agents with different `system_prompt`s, drive
  them sequentially in the host's loop.
- **Sub-agent**: parent host spawns child Agents (in-process or via
  `Subprocess`) to handle subtasks, then resumes the parent.
- **Multi-pane TUI**: one Agent per pane, all driven by a single
  event loop on the main thread, each Agent on its own worker thread.
- **Web UI**: one Agent per HTTP session; agent events broadcast over SSE.

See the `demos/` directory for working examples (Phase 7 will land
the canonical TUI / web / manager demos).