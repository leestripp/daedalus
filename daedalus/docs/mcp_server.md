# MCP Server Integration

The Model Context Protocol (MCP) connects an agent to external tools. Daedalus
is an MCP **client**: each configured server is discovered independently and
each discovered tool is exposed to the model as `mcp__<server>__<tool>`.

The implementation supports stdio subprocesses and streamable HTTP using
JSON-RPC 2.0. There is no built-in MCP server registry, no default third-party
server, and no generic `mcp` routing tool.

## Configuration sources and precedence

| Source | Location | Behavior |
|---|---|---|
| SQLite (authoritative) | `<daedalus_home>/daedalus.db`, table `mcp_servers` | Loaded first; wins name conflicts |
| JSON fallback | `<AgentConfig::daedalus_home>/mcp/servers.json` when a home is explicitly set, otherwise `$XDG_CONFIG_HOME/daedalus/mcp/servers.json` | Standard `mcpServers` map; merged at initialization |
| Environment file | `$XDG_CONFIG_HOME/daedalus/mcp/.env` | Supplies values for `${VAR}` expansion |

The normal data home is `$XDG_DATA_HOME/daedalus`, or
`~/.local/share/daedalus`. The normal config home is
`$XDG_CONFIG_HOME/daedalus`, or `~/.config/daedalus`. See
[`storage.md`](storage.md) for path precedence.

Configuration is loaded during `Agent::initialize()`. Connections and tool
discovery happen lazily on the first turn. Editing `servers.json` during a
session does not mutate the current manager; create a new agent for a clean
reload. A host can change persistent rows with `SettingsStore` and use its MCP
manager/reconnect integration when it needs a live reconfiguration.

## SQLite configuration

```cpp
#include <daedalus/settings.h>

auto& store = daedalus::SettingsStore::instance();
store.upsert_mcp_server(daedalus::SettingsStore::McpServer{
    .name = "filesystem",
    .transport = "stdio",
    .command = "npx",
    .args = {"-y", "@modelcontextprotocol/server-filesystem", "/allowed/path"},
    .env = {},
    .description = "Workspace file access",
    .enabled = true,
});

store.upsert_mcp_server(daedalus::SettingsStore::McpServer{
    .name = "remote-tools",
    .transport = "streamable-http",
    .url = "https://tools.example.com/mcp",
    .bearer_token = "${TOOLS_TOKEN}",
    .description = "Remote tool service",
    .enabled = true,
});
```

`SettingsStore::McpServer` fields are:

| Field | Meaning |
|---|---|
| `name` | Unique configuration name; also used in the proxied tool name |
| `transport` | `stdio` or `streamable-http` |
| `command`, `args`, `env` | Executable, argument vector, and environment for stdio |
| `url` | Endpoint for streamable HTTP |
| `bearer_token` | Optional HTTP bearer credential |
| `description` | Human-readable host/settings description |
| `enabled` | Persistent host/user enable flag |

Use `list_mcp_servers()`, `get_mcp_server(name)`, and
`delete_mcp_server(name)` for the other CRUD operations. `${VAR}` expansion is
performed when the connection is made; storing a reference avoids rewriting a
secret into the row. Do not commit `.env` files or real bearer tokens.

### Legacy JSON fallback

```json
{
  "mcpServers": {
    "filesystem": {
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-filesystem", "/allowed/path"],
      "env": {},
      "description": "Workspace file access"
    },
    "remote-tools": {
      "transport": "streamable-http",
      "url": "https://tools.example.com/mcp",
      "bearer_token": "${TOOLS_TOKEN}",
      "description": "Remote tool service"
    }
  }
}
```

Store credentials in `$XDG_CONFIG_HOME/daedalus/mcp/.env`:

```bash
TOOLS_TOKEN=replace-me
```

The file is optional. The process environment is also consulted during value
substitution. JSON is a compatibility source, not a second authoritative
registry.

## Agent configuration and runtime controls

MCP is enabled by default. Hosts can disable it or initially suppress specific
servers:

```cpp
daedalus::AgentConfig cfg;
cfg.mcp_enabled = true;
cfg.mcp_servers_disabled = {"untrusted-server"};
```

After initialization:

```cpp
agent.set_mcp_server_enabled("untrusted-server", false);
agent.set_mcp_server_enabled("untrusted-server", true);
const bool enabled = agent.is_mcp_server_enabled("untrusted-server");
const auto disabled = agent.disabled_mcp_servers();
```

Disabling disconnects the server and removes its proxied definitions from the
next model request. Enabling reconnects and rediscovers them. Runtime changes
are per-agent and do **not** update `mcp_servers.enabled`; the persistent state
therefore returns in a new session.

The model-facing controls are:

```json
{"action": "list"}
{"action": "disable", "name": "remote-tools"}
{"action": "enable", "name": "remote-tools"}
```

for `mcp_control`, and the no-argument `mcp_status` diagnostic. `mcp_status`
is registered whenever MCP is enabled, even if no servers are configured. It
reports sources, configuration paths and errors, connected/healthy state,
runtime-disabled names, discovered tool counts/names, and the manager's last
error. `mcp_control` is the compact operational interface. Hosts can also use
`mcp_status()` on `Agent` for the same diagnostic payload.

## Discovery and invocation

A normal stdio session is:

1. Start the configured command with pipes for stdin/stdout.
2. Send `initialize` with client capabilities.
3. Receive server capabilities, then send `notifications/initialized`.
4. Send `tools/list`.
5. Register each result as an `McpProxiedTool`.
6. Forward invocations with `tools/call` and convert the result to tool JSON.

Example JSON-RPC messages on the wire:

```json
{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"search","arguments":{"query":"MCP"}}}
```

The model sees a direct proxied name, for example:

```text
mcp__wikipedia__search
mcp__remote-tools__lookup
```

There is no call shaped as `mcp {server, tool, arguments}` and no special
`tool: "list"` operation. Use `list_tools` with an MCP filter or `mcp_status`
to inspect the current surface.

## Tool-schema normalization

Before a server's `inputSchema` is sent to a strict chat provider,
`McpProxiedTool` normalizes it:

- Null branches in `anyOf`/`oneOf` are removed; one survivor is inlined and,
  when several remain, the primary branch is selected.
- Local `#/$defs/...` and `#/definitions/...` references are inlined, then the
  definition maps are removed.
- Schema-valued `additionalProperties` becomes `true`; boolean values remain
  unchanged.
- Descriptions and other sibling metadata are retained.

This is only a provider-facing normalization. It does not change or revalidate
the server's own `tools/call` payload. A host can impose a schema-size soft cap
through the agent settings; see [`settings.md`](settings.md).

## Diagnostics and security

When expected `mcp__...` tools are absent, call `mcp_status` first. Check:

1. Whether MCP itself is enabled and the server is not runtime-disabled.
2. Whether the SQLite row or JSON fallback exists at the reported path.
3. Whether `command` is on `PATH` (for example `uvx` or `npx`) and whether the
   HTTP URL is reachable.
4. Whether required environment variables were present when the agent started
   the connection.
5. Server stderr, circuit-breaker/connection errors, and discovery results.

Run a stdio server manually with the same command and environment to reproduce
launch failures. Avoid logging resolved API keys.

MCP servers are executable third-party integrations. Stdio commands run with
the host user's privileges; HTTP servers receive the arguments the model sends.
Configure only trusted servers, restrict credentials and filesystem scopes, and
use per-agent runtime disabling when a server's tool schemas are no longer
needed. Runtime disabling also reduces prompt context on subsequent requests.

## Protocol and implementation references

- MCP protocol version used by the client: `2025-06-18`.
- Public agent controls: `include/daedalus/Agent.h`.
- Persistent schema/API: `include/daedalus/settings.h`.
- Client and transport implementation: `src/mcp/`.
- Dynamic proxy: `src/tools/McpProxiedTool.cpp`.
- Runtime/status tools: `src/tools/McpControlTool.cpp` and
  `src/tools/McpStatusTool.cpp`.
- Official specification: <https://modelcontextprotocol.io/specification/>
- JSON-RPC 2.0: <https://www.jsonrpc.org/specification>
