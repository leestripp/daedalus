# Tool Reference

Daedalus tools are OpenAI-compatible function definitions backed by local C++
implementations. The core library registers a focused set; hosts can add their
own `BaseTool` implementations and selected examples from `demos/example-tools/`.
The live, authoritative roster for an initialized agent is available through
`Agent::list_tools()` or the model-facing `list_tools` tool.

## Core tool surface

| Area | Tools |
|---|---|
| Execution | `execute_bash` |
| Turn control | `tell_user`, `compress_context`, `set_status`, `stop` |
| Introspection | `list_tools`; `mcp_status` when MCP is enabled |
| Skills | `discover_skills`, `load_skill`, `get_active_skills`, `search_skills`, `deactivate_skill`, `get_skill_context` |
| Session memory | `remember`, `forget`, `memories`, `memory_stats` |
| Knowledge/secrets | `brain`, `vault` |
| Todos | `todo_add`, `todo_edit`, `todo_delete`, `todo_list`, `todo_count`, `set_max_todos`, `todo_clear` |
| Scheduling | `schedule_task` |
| Search/input | `web_search`, `read_image`, `read_pdf` |
| MCP | `mcp_control`; dynamic `mcp__<server>__<tool>` definitions |

There are no built-in file read/write tools: agents use `execute_bash` and the
host's normal shell utilities for those operations. There is no built-in
`codegraph` tool. Git, grep/Wikipedia, system-info, background-process, and
network tools are examples, not implicit core registrations.

## `execute_bash`

Runs shell commands in the project root. This is also the built-in interface
for long-running processes; the separate `background_process` example tool is
not required.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `command` | string | run/start | Shell command; a trailing unquoted `&` requests background execution |
| `action` | string | No | `run` (default), `start`, `poll`, `send`, `stop` |
| `timeout` | integer | No | Synchronous timeout, 1–3600 seconds; default 60 |
| `background` | boolean | No | Shorthand for `action=start` |
| `detached` | boolean | No | For `start`; default `false`. Opt an intentional long-lived service out of the verified supervisor's required-job wait set |
| `process_id` | string | poll/send/stop | ID returned by `start` |
| `input` | string | send | Data written to process stdin |
| `clear_output` | boolean | No | Clear buffered output after `poll` |

```json
{"command":"cmake --build build -j4","timeout":600}
{"action":"start","command":"./build/my_server --port 8080"}
{"action":"send","process_id":"proc-1","input":"quit\n"}
{"action":"poll","process_id":"proc-1","clear_output":false}
{"action":"stop","process_id":"proc-1"}
```

Synchronous output is sanitized before it reaches the model: unsafe terminal
control sequences are removed while UTF-8 and ANSI SGR color/style sequences
are preserved. Long-running commands emit sanitized progress chunks between
`on_tool_call` and `on_tool_result`; incremental sanitization preserves split
escape sequences across chunk boundaries.

Multiple background jobs may run concurrently. After a start, do not poll
merely to wait for termination. A natural exit queues one
`trigger:background-process` steer completion containing final status and
output, then invokes `AgentSink::on_turn_requested()` so the host can wake the
same agent's scheduler. It is injected only at a complete tool-call boundary.

With verification enabled, a worker or verifier that stops while a required
job is outstanding is suspended instead of advancing to verification or final
`Done`; completion delivery resumes that same inner loop. `detached=true` is
for intentional long-lived services and excludes a job from this wait set, but
its eventual completion is still reported. Explicit `stop` does not generate a
natural-exit message. Poll only for intermediate output.

### Approval and security

`run` and `start` require host approval unless auto-approve or an applicable
approval cache permits the command. Denial returns a structured `denied`
result. Privilege-escalation commands and protected credential paths receive
additional checks, but the shell is **not an OS sandbox**: it inherits the
invoking user's access. Auto-approve is appropriate only when that access is
acceptable.

`Agent::set_auto_approve`, `clear_session_approvals`,
`clear_persisted_approvals`, and `set_remember_approvals` control host approval
behavior. `AgentSink::on_command_auto_approved()` is an informational audit
event for commands that bypass the interactive prompt.

## Turn-control tools

### `tell_user`

Sends an informational message to the host/user without ending the turn.

| Parameter | Type | Required |
|---|---|---|
| `content` | string | Yes |

### `compress_context`

Requests compaction at the next cycle boundary. It does not interrupt an
in-flight tool call. Automatic threshold compaction and
`Agent::compress_context()` use the same mechanism. A running request is
therefore deferred; an idle host request can run immediately.

### `set_status`

Declares the agent's current phase. Valid values are `Idle`, `Understanding`,
`Planning`, `Acting`, `Verifying`, `Documenting`, `Done`, `Cancelled`, and
`Error` (case-insensitive). `Done` ends the agent loop like `stop`. The runtime
owns `AwaitingApproval` and `Compressing`; tools must not set those directly.
Hosts receive the transition through `AgentSink::on_state_change()`.

### `stop`

Ends the current turn successfully.

| Parameter | Type | Required |
|---|---|---|
| `summary` | string | Yes |

A final assistant response should be emitted before calling `stop`.

## Tool introspection

`list_tools` reflects the actual registry rather than the pinned system-prompt
text.

| Parameter | Type | Description |
|---|---|---|
| `filter` | string | Optional case-insensitive substring matched against name/description |
| `verbose` | boolean | Return full JSON schemas instead of the compact roster |

This is useful after runtime tool changes, on resumed sessions, or when an MCP
server has connected/disconnected. Host code can use `Agent::list_tools()` and
`Agent::list_tool_names()`.

## Session-memory tools

These tools manage explicit long-term project memory, not the complete
transcript and not the global Second Brain.

| Tool | Arguments / behavior |
|---|---|
| `remember` | Required `category`, `content`; optional `confidence` 1–5 and `tags`. Categories: `builds`, `errors`, `preferences`, `patterns`, `sessions`, `custom` |
| `forget` | Required memory `id`; remove one entry |
| `memories` | Optional `category`, `limit`, `show_unresolved_only`; list entries |
| `memory_stats` | No arguments; usage and storage summary |

Use `memories` to discover an ID before `forget`. See
[`session_memory.md`](session_memory.md) for persistence and recovery behavior.

## Second Brain: `brain`

`brain` manages one user-global plain-Markdown knowledge base shared by all
projects. Notes live in PARA folders: `inbox`, `projects`, `areas`, `resources`,
and `archives`. `[[note-id]]` in a body creates a wiki link. The absolute root is
`second_brain_directory` (default `<data_root>/brain`), editable in MonkeyByte
and icarus Agent settings. Changing it selects and reindexes another tree on the
next operation; it does not move notes.

| Action | Required arguments | Optional arguments |
|---|---|---|
| `capture` | `body` | `folder`, `title`, `tags` |
| `search` | `query` | `limit` |
| `read` | `id` | — |
| `list` | — | `folder`, `tag`, `limit` |
| `update` | `id` | `title`, `tags`, `body` |
| `append` | `id`, `markdown` | — |
| `file` | `id`, `folder` | — |
| `backlinks` | `id` | — |
| `delete` | `id` | — |
| `reindex` | — | — |

Markdown files are the source of truth; SQLite metadata/FTS5 is a rebuildable
index. Readers share `<brain>/.lock`, while mutations and reindex take it
exclusively. Note writes are atomic and metadata/FTS updates are transactional.
Lock or SQLite busy timeouts return contained tool errors. Use `reindex` after
manual file edits or crash recovery.

## Secrets vault: `vault`

The vault is a user-global encrypted password manager.

| Action | Behavior |
|---|---|
| `list` | Metadata only; never secret field values |
| `get` | Decrypt one entry's fields; these values enter model context/history |
| `store` | Create/replace a whole entry without echoing secret values |
| `edit` | Merge supplied fields into an existing entry without a get-first |
| `delete` | Remove an entry |

Common arguments are `name`, `kind` (`login`, `note`, `api_key`, `custom`),
`description`, and string-valued `fields`. Hosts that must prevent decrypted
secrets from entering model context should disable `vault` through
`AgentConfig::tools_disabled`. See [`vault.md`](vault.md).

## Skills

| Tool | Arguments |
|---|---|
| `discover_skills` | Optional `directory`; otherwise configured skill directories |
| `load_skill` | Required `skill_name` |
| `get_active_skills` | None |
| `search_skills` | Required `query` |
| `deactivate_skill` | Required `skill_name` |
| `get_skill_context` | None |

Discovery lists metadata; loading activates one skill's instructions.
`get_skill_context()` returns the combined active instructions. Runtime skill
enable/disable is a host policy and differs from model activation. See
[`skills.md`](skills.md).

## Web search: `web_search`

| Parameter | Type | Required | Description |
|---|---|---|---|
| `query` | string | Yes | Search text |
| `max_results` | integer | No | Default 8, clamped to 1–25 |
| `provider` | string | No | Force one configured provider |

Without `provider`, enabled rows from `web_search_providers` run in ascending
priority order with failover. Supported IDs are `brave`, `tavily`, `exa`,
`firecrawl`, `serper`, `serpapi`, `searxng`, and `duckduckgo`. A forced provider
may be disabled but must be configured. Results contain title, URL, and snippet;
failures identify each provider that failed.

DuckDuckGo is key-less and enabled on a fresh store. The old `serper_search` and
`searxng_search` tools have been removed. Provider settings are documented in
[`settings.md`](settings.md). A host with browser tools can additionally browse
a search-engine page; a host-provided `web_fetch` may also work on such URLs,
subject to rate limits.

## Image and PDF input

### `read_image`

| Parameter | Type | Required | Description |
|---|---|---|---|
| `path` | string | Yes | Project-relative or absolute local image path |
| `prompt` | string | No | Question/instruction attached with the image |

PNG/JPEG are decoded and downscaled to 512 pixels on the longest edge. GIF, BMP,
and WebP pass through with a 2 MB cap. SVG is rejected; read its XML as text.
The base64 image is attached to the next vision-capable request and is not
persisted in history. Text-only models drop image attachments with a warning.

Hosts can alternatively pass `ImageAttachment` values to
`Agent::submit_user_message()`, or use Markdown image references supported by
the message pipeline. Remote image URLs are downloaded before submission.

### `read_pdf`

| Parameter | Type | Required | Default |
|---|---|---|---|
| `path` | string | Yes | — |
| `start_page` | integer | No | 1 (one-indexed) |
| `max_pages` | integer | No | 10 |

Extracts text with poppler-cpp and returns page-delimited content. This is text
extraction, not visual PDF rendering.

## Todo tools

Todos persist per project and per agent in
`<project>/.daedalus/daedalus[-<agent>]/todos.json`. Valid states are `pending`,
`in_progress`, `done`, and `blocked`; `group` organizes phases.

| Tool | Arguments |
|---|---|
| `todo_add` | Required `text`; optional `status`, `group` |
| `todo_edit` | Required `id`; at least one of `text`, `status`, `group` |
| `todo_delete` | Required `id` |
| `todo_list` | Optional `filter` (`all`, `pending`, `in_progress`, `done`, `blocked`) and `group` |
| `todo_count` | None; totals plus per-group counts and maximum |
| `set_max_todos` | Required `max`, 1–1000; cannot be below current size |
| `todo_clear` | None |

The default maximum is 100.

## Scheduled tasks: `schedule_task`

Schedules persist per project/per-agent in `scheduled_tasks.json`. They run only
while the owning agent/host is active; this is not cron.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `action` | string | Yes | `add`, `list`, `edit`, `delete` |
| `id` | integer | edit/delete | Positive ID from add/list |
| `name` | string | add | Short label |
| `prompt` | string | add | Instructions submitted when due |
| `kind` | string | No | `once` (default) or `recurring` |
| `run_at` | string | one-shot add | UTC `YYYY-MM-DDTHH:MM:SSZ`; optional first run for recurring |
| `interval_seconds` | integer | recurring add | 1 second through 31,536,000 (one year) |

`edit` changes supplied fields. `list` returns tasks and `now_utc`. Overdue work
fires once at startup; missed recurring intervals coalesce. One-shot state is
deleted and recurring state advanced before queuing, so dispatch is not
replayed after a crash.

If the agent is busy when a task becomes due, that occurrence is dropped
permanently instead of accumulating behind the turn. Terminal states are not
busy. The queued source is `trigger:task-<id>` and the normal
`on_turn_requested()` callback wakes an idle host.

## MCP tools

Each discovered MCP tool is a first-class definition named
`mcp__<server>__<tool>`. Its schema comes from that server after provider-safe
normalization. There is no generic `mcp` tool.

- `mcp_status` has no arguments and reports configuration sources, connection
  health, paths, errors, and discovered tools. It is present whenever MCP is
  enabled, including with zero configured servers.
- `mcp_control` takes `action=list|enable|disable`, plus `name` for enable or
  disable. Changes are runtime-only and affect the next request's tool surface.

See [`mcp_server.md`](mcp_server.md).

## Optional example tools

The CLI demo registers these examples individually; embedding hosts choose
which ones they need.

| Example family | Tools | Reference |
|---|---|---|
| Git | `git_status`, `git_branch`, `git_commit`, `git_add`, `git_switch`, `git_diff`, `git_push`, `git_merge` | `demos/example-tools/GitTool.*` |
| Search/system | `grep_search`, `wiki_search`, `system_info` | `SearchTool.*`, `SystemInfoTool.*` |
| Legacy process example | `background_process` | `BackgroundTool.*`; built-in `execute_bash` already supports background work |
| Network | `http_request`, `dns_lookup`, `tcp_check`, `tls_inspect`, `websocket_connect`, `websocket_send`, `websocket_recv`, `websocket_close` | [`network-tools.md`](network-tools.md) |

Example registration uses the same public API as a custom tool:

```cpp
#include "GitTool.h"
agent.register_tool(std::make_unique<GitStatusTool>());
```

Hosts may re-register a name to replace it or remove it through the registry's
`unregister_tool` support. Runtime disabling through
`Agent::set_tool_enabled(name, false)` hides and rejects a tool without
destroying its registration. `AgentConfig::tools_disabled` applies initial host
policy.

## Custom tools

Subclass `daedalus::BaseTool`, provide a unique name/description and JSON
schema, implement `execute`, and register a `std::unique_ptr` with the agent.
`daedalus::HostTool` is a deprecated alias for `BaseTool`.

```cpp
class TimestampTool final : public daedalus::BaseTool {
public:
    std::string get_name() const override { return "timestamp"; }
    std::string get_description() const override {
        return "Return the current UTC timestamp.";
    }
    nlohmann::json get_definition() const override {
        return {
            {"type", "function"},
            {"function", {
                {"name", get_name()},
                {"description", get_description()},
                {"parameters", {
                    {"type", "object"},
                    {"properties", nlohmann::json::object()},
                    {"required", nlohmann::json::array()}
                }}
            }}
        };
    }
    nlohmann::json execute(const nlohmann::json&, FileIO&) override;
};

agent.register_tool(std::make_unique<TimestampTool>());
```

Override the progress-aware execute overload for long-running tools and emit
bounded structured progress. Tool exceptions/errors must be contained in the
returned JSON rather than escaping into the agent loop. See
[`custom-tools.md`](custom-tools.md).

## Tool-call/message-chain integrity

Every assistant tool call must have a following `tool` message with the same
`tool_call_id` before the next model request:

```text
assistant(tool_calls=[id=call-1])
tool(tool_call_id=call-1, content={...})
assistant(...)
```

Cancellation, compaction, truncation, persistence, and retry paths preserve this
invariant. Failed/denied tools still produce a result message. Compaction uses
a safe boundary and never leaves a tool result orphaned from its call. Hosts
must not independently delete one side of a pair when editing restored message
history.

Definitions are sent with model requests according to the current registry;
they are not part of message history. Consequently old transcripts do not prove
that a tool is currently available—use `list_tools`.
