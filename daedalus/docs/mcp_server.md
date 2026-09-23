# MCP Server Integration Guide

This document explains how the Model Context Protocol (MCP) works and
how daedalus integrates with MCP servers. **v0.5.0+:** server
configuration is stored in the `mcp_servers` table of the SQLite-backed
`SettingsStore` (alongside models, kv_settings, etc.). The legacy
`servers.json` file is still supported as a fallback so existing
installs keep working — see
[`docs/storage.md`](storage.md) for the migration recipe.

> **Fixed 2026-09-17 (BUG-001):** the v0.5.0 migration never actually
> implemented the `servers.json` read — the file was silently ignored
> and zero MCP tools appeared with zero diagnostics. The fallback is
> now real (merged at agent init, store wins on name conflicts), load
> outcomes are logged through the host sink, and the agent has a
> built-in [`mcp_status`](#diagnosing-mcp-problems-mcp_status)
> introspection tool.

## Diagnosing MCP problems (`mcp_status`)

The agent has a built-in `mcp_status` tool (no arguments). It reports:

- every configured server with its transport, **config source**
  (`sqlite_store` vs `servers.json`), connected/healthy state, and
  discovered tool count / tool names,
- the exact `servers.json` path the agent read, whether it existed,
  and any parse/validation error,
- the host-disabled server list, the `.env` file path, and the
  manager's last error,
- a hint naming the writable config locations when zero servers are
  configured.

`mcp_status` is registered whenever MCP is enabled — **including when
zero servers are configured**, because that is exactly the case that
needs diagnosing. If `mcp__<server>__<tool>` tools are missing, call
`mcp_status` first.

## Runtime enable/disable (`mcp_control`, §MCP-CONTROL-CORE)

The agent can shed MCP context itself with the `mcp_control` tool —
important because every connected server's tool schemas are sent with
**every** request, and a busy server can contribute tens of thousands of
tokens. When the agent is finished with a server it can disable it
instead of asking the user:

```json
{"action": "list"}                              // servers + state + tool counts
{"action": "disable", "name": "comfy-mcp"}      // disconnect, drop its tools
{"action": "enable",  "name": "comfy-mcp"}      // reconnect
```

Semantics:

- **Runtime-only.** The SettingsStore row (`mcp_servers.enabled`) is
  never written — that column stays the host/user's persistent control.
  A runtime-disabled server returns next session.
- Takes effect on the **next request**: `disable_server()` marks the
  tool surface dirty, `LLMClient` refreshes at the cycle boundary, and
  the per-iteration Context Status roster (§LIST-TOOLS-CORE) reflects
  the new count immediately.
- The tool delegates to `Agent::set_mcp_server_enabled()` (the same
  public host API) via an injected handler, so the per-agent
  `ToolFilter` and sink logging stay consistent. Unknown server names
  are rejected; repeating an action is a no-op success.
- `mcp_status` remains the verbose diagnostic (config sources, errors);
  `mcp_control` is the compact operational tool.

### Config locations and precedence (summary)

| Source | Location | Notes |
|--------|----------|-------|
| SQLite store (authoritative) | `$XDG_DATA_HOME/daedalus/daedalus.db`, `mcp_servers` table | Written via `SettingsStore::upsert_mcp_server()`. Wins name conflicts. |
| `servers.json` fallback | `<daedalus_home>/mcp/servers.json` when the host sets `AgentConfig::daedalus_home`, else `$XDG_CONFIG_HOME/daedalus/mcp/servers.json` | Standard `mcpServers` JSON (see below). Read once at agent init. |
| `.env` | `$XDG_CONFIG_HOME/daedalus/mcp/.env` | `${VAR}` substitution at connect time. |

### Reload semantics

- Config is read **once per agent at `initialize()`**; connects happen
  lazily on the first turn (parallel, circuit-breaker protected).
- Editing `servers.json` mid-session takes effect in the **next
  session** (or new `Agent`). Hosts can change servers live via
  `Agent::set_mcp_server_enabled()` / `SettingsStore::upsert_mcp_server()`
  + `McpServerManager::force_reconnect()` — no app restart needed.
- A server whose launch/handshake fails is recorded in its circuit
  breaker and surfaced via `mcp_status` (`connected:false`) and the
  host log sink — never silently.

## Overview

MCP is an open protocol that enables AI applications to connect to external data sources and tools. It uses JSON-RPC 2.0 for communication and supports two transport mechanisms:

1. **Stdio Transport** - Server runs as a subprocess, communication via stdin/stdout
2. **Streamable HTTP** - Server runs as an HTTP service

## Configuration (v0.5.0+, recommended)

MCP servers live in the `mcp_servers` table of the same SQLite database
that holds models and kv_settings:

```
$XDG_DATA_HOME/daedalus/daedalus.db    ← SettingsStore
└── (tables: models, mcp_servers, kv_settings, …)
```

Use the `daedalus::SettingsStore` API to add a server:

```cpp
#include <daedalus/settings.h>
auto& store = daedalus::SettingsStore::instance();
daedalus::SettingsStore::McpServer srv{
    .name        = "minimax-web-search",
    .command     = "uvx",
    .args        = {"minimax-coding-plan-mcp", "web-search"},
    .transport   = "stdio",
    .description = "Web search using MiniMax MCP",
    .env         = { {"MINIMAX_API_HOST", "https://api.minimax.io"},
                     {"MINIMAX_API_KEY", "${MINIMAX_API_KEY}"} },
};
store.upsert_mcp_server(srv);
```

Env-var substitution (`${VAR_NAME}`) happens at connect time, so a
restart picks up the current value of `MINIMAX_API_KEY` without DB
rewrite.

## Configuration (legacy JSON, still supported)

```
$XDG_CONFIG_HOME/daedalus/mcp/
├── servers.json    # Server definitions
├── .env            # API keys and secrets
└── .env.example    # Template for .env
```

### servers.json Format

```json
{
  "mcpServers": {
    "server-name": {
      "command": "uvx",
      "args": ["package-name", "tool-name"],
      "env": {
        "ENV_VAR": "value"
      },
      "description": "Server description"
    }
  }
}
```

### Environment Variables

API keys and sensitive data are stored in `~/.daedalus/mcp/.env`:

```bash
MINIMAX_API_KEY=your-api-key-here
```

Environment variables are referenced in `servers.json` using `${VAR_NAME}` syntax, which gets substituted at runtime.

### Current Configuration

daedalus currently has these servers registered by default:

| Server | Package | Description |
|--------|---------|-------------|
| `minimax-web-search` | `minimax-coding-plan-mcp` | Web search using MiniMax |
| `minimax-read-image` | `minimax-coding-plan-mcp` | Image understanding via MiniMax |
| `wikipedia` | `mcp-server-wikipedia` | Wikipedia search |

Example entry:

```json
{
  "mcpServers": {
    "minimax-web-search": {
      "command": "uvx",
      "args": ["minimax-coding-plan-mcp", "web-search"],
      "env": {
        "MINIMAX_API_HOST": "https://api.minimax.io",
        "MINIMAX_API_KEY": "${MINIMAX_API_KEY}"
      },
      "description": "Web search using MiniMax MCP"
    }
  }
}
```

## How MCP Works

### Protocol Basics

Every MCP message is a JSON-RPC 2.0 object:

```json
// Request
{"jsonrpc": "2.0", "id": 1, "method": "tools/list", "params": {}}

// Response
{"jsonrpc": "2.0", "id": 1, "result": {"tools": [...]}}

// Notification (no response expected)
{"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}}
```

### Message Flow

1. **Client → Server**: Send `initialize` request with client capabilities
2. **Server → Client**: Return server capabilities and info
3. **Client → Server**: Send `notifications/initialized` notification
4. **Client → Server**: Send `tools/list` to discover available tools
5. **Server → Client**: Return list of tool definitions
6. **Client → Server**: Send `tools/call` to invoke a tool
7. **Server → Client**: Return tool execution results

## Stdio Transport

The stdio transport is the most common way to run MCP servers as subprocesses.

### How It Works

1. The client forks a child process running the MCP server command
2. Parent process creates pipes to communicate with the child:
   - `stdin_pipe[1]` → child stdin
   - `stdout_pipe[0]` ← child stdout
3. All JSON-RPC messages are sent as newline-delimited JSON strings

### Example Server Launch

```bash
# The client spawns the server as a subprocess
uvx minimax-coding-plan-mcp web-search

# Environment variables are passed for API keys
MINIMAX_API_KEY=xxx uvx minimax-coding-plan-mcp web-search
```

## MCP Server Discovery

### List Available Tools

```json
// Request
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "tools/list",
  "params": {}
}

// Response
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "tools": [
      {
        "name": "web_search",
        "description": "Search the web for information",
        "inputSchema": {
          "type": "object",
          "properties": {
            "query": {
              "type": "string",
              "description": "The search query"
            }
          },
          "required": ["query"]
        }
      }
    ]
  }
}
```

### Call a Tool

```json
// Request
{
  "jsonrpc": "2.0",
  "id": 2,
  "method": "tools/call",
  "params": {
    "name": "web_search",
    "arguments": {
      "query": "MCP protocol specification"
    }
  }
}

// Response
{
  "jsonrpc": "2.0",
  "id": 2,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "Search results..."
      }
    ]
  }
}
```

## Adding New MCP Servers

**Preferred (v0.5.0+):** use the `SettingsStore` API to upsert a row
into the `mcp_servers` table. The new server is picked up the next
time `McpServerManager::load_from_store()` is called by your host:

```cpp
auto& store = daedalus::SettingsStore::instance();
store.upsert_mcp_server(daedalus::SettingsStore::McpServer{
    .name        = "github",
    .command     = "uvx",
    .args        = {"--from", "mcp-github", "github"},
    .transport   = "stdio",
    .description = "GitHub repository operations",
    .env         = { {"GITHUB_TOKEN", "${GITHUB_TOKEN}"} },
});
```

**Legacy (still supported):** edit
`$XDG_CONFIG_HOME/daedalus/mcp/servers.json`:

```json
{
  "mcpServers": {
    "github": {
      "command": "uvx",
      "args": ["--from", "mcp-github", "github"],
      "env": {
        "GITHUB_TOKEN": "${GITHUB_TOKEN}"
      },
      "description": "GitHub repository operations"
    },
    "filesystem": {
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-filesystem", "/allowed/path"],
      "description": "Read and write local files"
    }
  }
}
```

Then add the required environment variables to `$XDG_CONFIG_HOME/daedalus/mcp/.env`:

```bash
GITHUB_TOKEN=ghp_xxxxxxxxxxxx
```

## Popular MCP Servers

Here are some popular MCP servers you could add:

| Server | Package | Description |
|--------|---------|-------------|
| GitHub | `mcp-github` | Repository operations, issues, PRs |
| Filesystem | `@modelcontextprotocol/server-filesystem` | Read/write local files |
| Brave Search | `mcp-brave-search` | Web search via Brave |
| PostgreSQL | `mcp-server-postgres` | Database queries |
| SQLite | `@modelcontextprotocol/server-sqlite` | SQLite database access |
| Slack | `mcp-slack` | Slack messaging |
| Google Maps | `mcp-server-google-maps` | Location services |

## Unified `mcp` Tool Design

### Current Hardcoded Tools (To Replace)

daedalus currently has three hardcoded MCP tools in `src/tools/McpTools.cpp`:

| Tool | Name | Purpose |
|------|------|---------|
| `McpReadImageTool` | `mcp_read_image` | Image analysis via MiniMax |
| `McpWebSearchTool` | `mcp_web_search` | Web search via MiniMax |
| `McpWikiSearchTool` | `mcp_wiki_search` | Wikipedia search (via web search) |

Each tool is tightly coupled to specific MCP servers and specific tool names:

```cpp
// Current implementation calls specific tools:
client->understand_image(image_url, prompt);  // MiniMax-specific
client->web_search(query);                     // MiniMax-specific
```

This means adding new MCP servers or new tools requires code changes.

### Solution: Single Unified `mcp` Tool

The solution is a single generic `mcp` tool that routes calls to any configured MCP server:

#### Tool Definition

```json
{
  "type": "function",
  "function": {
    "name": "mcp",
    "description": "Call a tool from any configured MCP server. Routes to any MCP server's tools dynamically.",
    "parameters": {
      "type": "object",
      "properties": {
        "server": {
          "type": "string",
          "description": "Name of the MCP server to use (e.g., 'minimax-web-search', 'wikipedia', 'github'). Must be configured in ~/.daedalus/mcp/servers.json"
        },
        "tool": {
          "type": "string",
          "description": "Name of the tool on the MCP server (e.g., 'web_search', 'read_image', 'search'). Use 'list' to see available tools."
        },
        "arguments": {
          "type": "object",
          "description": "Arguments to pass to the MCP tool. Format depends on the specific tool.",
          "additionalProperties": true
        }
      },
      "required": ["server", "tool"]
    }
  }
}
```

#### Example Usage

```json
// Search the web
{
  "server": "minimax-web-search",
  "tool": "web_search",
  "arguments": {
    "query": "MCP protocol specification"
  }
}

// Read an image
{
  "server": "minimax-read-image",
  "tool": "read_image",
  "arguments": {
    "path": "/path/to/image.png",
    "prompt": "Describe this image"
  }
}

// Search Wikipedia
{
  "server": "wikipedia",
  "tool": "search",
  "arguments": {
    "query": "Model Context Protocol",
    "limit": 5
  }
}

// List available tools on a server
{
  "server": "minimax-web-search",
  "tool": "list"
}
```

#### Benefits

1. **Server agnostic** - One tool works for any MCP server
2. **Dynamic tool discovery** - Tools are discovered at runtime from servers
3. **Easy to extend** - Add new servers via configuration, not code
4. **Full MCP feature support** - Access all tools any server provides
5. **Consistent interface** - Single tool name in system prompts

## Implementation Guide

### McpTool Class

Replace the three hardcoded tools with a single `McpTool` in `src/tools/McpTools.cpp`:

```cpp
class McpTool : public BaseTool {
public:
    std::string get_name() const override { return "mcp"; }
    
    json get_definition() const override {
        return json::object({
            {"type", "function"},
            {"function", {
                {"name", get_name()},
                {"description", get_description()},
                {"parameters", get_parameters()}
            }}
        });
    }
    
    std::string get_description() const {
        return "Call a tool from any configured MCP server. "
               "Use server='server-name' and tool='tool-name'. "
               "Arguments vary by tool. Use tool='list' to see available tools on a server.";
    }
    
    json get_parameters() const {
        return json::object({
            {"type", "object"},
            {"properties", {
                {"server", json::object({
                    {"type", "string"},
                    {"description", "MCP server name from ~/.daedalus/mcp/servers.json"}
                })},
                {"tool", json::object({
                    {"type", "string"},
                    {"description", "Tool name on the MCP server (or 'list' to see tools)"}
                })},
                {"arguments", json::object({
                    {"type", "object"},
                    {"description", "Tool-specific arguments"},
                    {"additionalProperties": true}
                })}
            }},
            {"required", json::array({"server", "tool"})}
        });
    }
    
    json execute(const json& args, FileIO& file_io) override;
};
```

### Execute Implementation

```cpp
json McpTool::execute(const json& args, FileIO& file_io) {
    std::string server_name;
    std::string tool_name;
    json tool_args = json::object();
    
    // Parse arguments
    if (args.is_object()) {
        server_name = args.value("server", "");
        tool_name = args.value("tool", "");
        tool_args = args.value("arguments", json::object());
    }
    
    if (server_name.empty() || tool_name.empty()) {
        return json::object({
            {"status", "error"},
            {"message", "Missing 'server' or 'tool' parameter"}
        });
    }
    
    // Special case: list available tools
    if (tool_name == "list") {
        return list_server_tools(server_name);
    }
    
    // Get MCP client for this server
    mcp::McpClient* client = get_client_for_server(server_name);
    if (!client) {
        return json::object({
            {"status", "error"},
            {"message", "MCP server '" + server_name + "' not connected or not configured"}
        });
    }
    
    // Call the tool
    ToolResult result = client->call_tool(tool_name, tool_args);
    
    return json::object({
        {"status", result.success ? "success" : "error"},
        {"server", server_name},
        {"tool", tool_name},
        {"result", result.content},
        {"error", result.error.value_or("")}
    });
}
```

### Tool Registration

In `src/tools/ToolFactory.cpp`, replace:

```cpp
// OLD - Three hardcoded tools
register_tool(std::make_shared<McpReadImageTool>());
register_tool(std::make_shared<McpWebSearchTool>());
register_tool(std::make_shared<McpWikiSearchTool>());

// NEW - Single unified tool
register_tool(std::make_shared<McpTool>());
```

### McpServerManager for Multiple Servers

To support multiple servers, create a manager class:

```cpp
// src/mcp/McpServerManager.h
class McpServerManager {
public:
    static McpServerManager& instance();
    
    // Load configuration from ~/.daedalus/mcp/servers.json
    bool load_config(const std::string& config_path = "");
    
    // Connect to a specific server
    bool connect(const std::string& server_name);
    
    // Disconnect from a server
    void disconnect(const std::string& server_name);
    
    // Get client for a specific server
    mcp::McpClient* get_client(const std::string& server_name);
    
    // Get all available tools from all servers
    std::vector<ToolDefinition> get_all_tools();
    
    // Call tool on specific server
    ToolResult call_tool(const std::string& server_name,
                        const std::string& tool_name,
                        const json& args);
    
    // List available servers
    std::vector<std::string> list_servers();
    
private:
    McpServerManager() = default;
    
    std::map<std::string, std::unique_ptr<mcp::McpClient>> clients_;
    std::map<std::string, McpServerConfig> configs_;
};
```

## daedalus MCP Implementation

daedalus has an MCP client implementation in:

- `src/mcp/McpClient.cpp` - Core client for stdio transport
- `src/mcp/McpClient.h` - Header with structures
- `src/tools/McpTools.cpp` - Tool implementations (to be refactored)

### Key Components

```cpp
// Server configuration structure
struct McpServerConfig {
    std::string name;
    std::string command;
    std::vector<std::string> args;
    std::map<std::string, std::string> env;
    std::string description;
    bool enabled = true;
};

// Tool definition from MCP server
struct ToolDefinition {
    std::string name;
    std::string description;
    json input_schema;
};

// Tool call result
struct ToolResult {
    bool success;
    std::string content;
    std::optional<std::string> error;
};
```

### Current Implementation Status

The current implementation has:
- ✅ Stdio transport via fork/pipes
- ✅ JSON-RPC request/response handling
- ✅ Tool discovery via `list_tools()`
- ✅ Tool execution via `call_tool()`
- ✅ Environment variable substitution
- ✅ Per-agent runtime enable/disable of servers

### Tool schema normalisation (§MCP-SCHEMA-SANITIZE)

`McpProxiedTool::get_definition()` does **not** pass a server's
`inputSchema` to the LLM provider verbatim — it first normalises the
schema into the subset strict providers accept. Motivation: Moonshot
("moonshot flavored json schema") answers every chat request with
HTTP 400 when any tool schema contains constructs it cannot parse, and
the tool schema rides on every request. The rewrite rules:

- `anyOf` / `oneOf`: `{"type": "null"}` branches (the pydantic
  `Optional[T]` pattern) are dropped — optionality is already expressed
  by absence from `required`. One surviving branch is inlined; with
  several survivors the first (primary) branch is kept. The other forms
  remain usable server-side: the wire `tools/call` is never validated
  against this schema.
- Local `$ref` (`#/$defs/NAME`, `#/definitions/NAME`) is inlined;
  top-level `$defs` / `definitions` are then dropped.
- Schema-valued `additionalProperties` is flattened to `true`
  (booleans pass through untouched).

Schemas that were already clean come out byte-identical. Sibling
metadata (`title`, `description`, `default`) survives every rewrite.

### Runtime enable/disable

You can disable an MCP server at construction time:

```cpp
daedalus::AgentConfig cfg;
cfg.mcp_enabled = true;
cfg.mcp_servers_disabled = { "untrusted-server" };
```

Or toggle it on/off after `initialize()`:

```cpp
agent.set_mcp_server_enabled("untrusted-server", false);  // disconnect + hide tools
agent.set_mcp_server_enabled("untrusted-server", true);   // reconnect + restore tools

if (agent.is_mcp_server_enabled("untrusted-server")) { /* ... */ }
auto disabled = agent.disabled_mcp_servers();
```

Each `Agent` owns its own `McpServerManager`, so runtime toggles are isolated
per agent.

### Remaining Work

To complete the unified MCP tool support:

1. **Create `McpServerManager`** - Handle multiple server connections
2. **Load `servers.json`** - Parse and manage server configurations
3. **Refactor to single `mcp` tool** - Replace three hardcoded tools
4. **Update ToolFactory** - Register only the unified `McpTool`
5. **Update SystemPromptBuilder** - Single `mcp` tool in prompts

## Environment Variables in Configuration

Environment variables use `${VAR_NAME}` syntax and are substituted at runtime:

```json
{
  "command": "uvx",
  "args": ["--from", "mcp-github", "github"],
  "env": {
    "GITHUB_TOKEN": "${GITHUB_TOKEN}"
  }
}
```

## Protocol Version

daedalus uses protocol version `2025-06-18`. Most MCP servers are backward compatible.

## Debugging MCP Servers

### Common Issues

1. **Server not starting**: Check PATH includes command location (`uvx`, `npx`)
2. **API key missing**: Verify `~/.daedalus/mcp/.env` has required keys
3. **Timeout on tool calls**: Increase timeout in `send_request()`
4. **Invalid JSON responses**: Check server stderr for errors

### Debug Tips

- Run server manually to see stderr output: `uvx minimax-coding-plan-mcp web-search`
- Check `last_error()` from McpClient for connection issues
- Verify environment variables are set: `echo $MINIMAX_API_KEY`

## MiniMax MCP Tools

The `minimax-coding-plan-mcp` server provides two tools the agent can
call via the MCP protocol. These are exposed to the LLM as MCP tool
calls (not native daedalus tools).

### `understand_image`

Performs image analysis using MiniMax's Vision Language Model.

| Parameter | Type | Required | Description |
|-----------|------|:--------:|-------------|
| `prompt` | string | Yes | Question or analysis request |
| `image_url` | string | Yes | HTTP/HTTPS URL or local file path |

Supported formats: JPEG, PNG, GIF, WebP. Max file size: 20 MB.

```json
{
  "name": "understand_image",
  "description": "Performs image understanding and analysis, supporting multiple image input methods.",
  "input_schema": {
    "type": "object",
    "properties": {
      "prompt": { "type": "string", "description": "Question or analysis request" },
      "image_url": { "type": "string", "description": "Image source (HTTP/HTTPS URL or local file path)" }
    },
    "required": ["prompt", "image_url"]
  }
}
```

Example response (MCP JSON-RPC):

```json
{
  "jsonrpc": "2.0", "id": 1,
  "result": {
    "content": [{
      "type": "text",
      "text": "This image shows a VS Code editor window. The sidebar contains file explorer..."
    }]
  }
}
```

### `web_search`

Performs web searches, returning results with titles, URLs, and snippets.

| Parameter | Type | Required | Description |
|-----------|------|:--------:|-------------|
| `query` | string | Yes | Search query |

```json
{
  "name": "web_search",
  "description": "Performs web searches based on search queries, returning search results and related suggestions.",
  "input_schema": {
    "type": "object",
    "properties": {
      "query": { "type": "string", "description": "Search query" }
    },
    "required": ["query"]
  }
}
```

Example response:

```json
{
  "jsonrpc": "2.0", "id": 2,
  "result": {
    "content": [{
      "type": "text",
      "text": "Search results for 'MiniMax M2.7 API documentation':\n"
        + "1. MiniMax API Docs\n   URL: https://platform.minimax.io/docs\n..."
    }]
  }
}
```

## Resources

- [Official MCP Specification](https://modelcontextprotocol.io/specification/draft)
- [MCP GitHub Repository](https://github.com/modelcontextprotocol/modelcontextprotocol)
- [MCP Server Examples](https://github.com/modelcontextprotocol/servers)
- [JSON-RPC 2.0 Specification](https://www.jsonrpc.org/specification)