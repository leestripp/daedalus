#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "BaseTool.h"
#include "mcp/McpClient.h"
#include "mcp/McpServerManager.h"  // §Phase 11

using json = nlohmann::json;

/**
 * McpProxiedTool - Per-tool adapter that exposes a single MCP server tool
 * to the LLM as a native BaseTool.
 *
 * Each instance is parameterised by (server_name, tool_definition) and
 * produces an OpenAI-compatible function schema whose `parameters` is the
 * tool's `inputSchema` returned by the server's `tools/list` JSON-RPC call.
 *
 * Names follow the canonical MCP-host convention:
 *
 *     mcp__<servername>__<toolname>
 *
 * (matching Claude Code, Cursor, Cline, Continue, Zed — see Agent.md §18.1).
 *
 * §Phase 11: McpServerManager* is injected at construction (not fetched
 * via ::instance()). nullptr = MCP disabled; execute() becomes a no-op.
 *
 * Execute() is a thin wrapper around
 * `mcp_manager_->call_tool(server, tool, args)` — every existing
 * code path (stdio, http, error reporting, content extraction) is reused.
 */
class McpProxiedTool : public BaseTool {
public:
    // §Phase 11: mcp_manager added (injected instead of ::instance()).
    McpProxiedTool(std::string server_name, mcp::ToolDefinition def,
                   mcp::McpServerManager* mcp_manager);

    std::string get_name() const override;
    std::string get_description() const override;
    json        get_definition() const override;
    json        execute(const json& args, FileIO& file_io) override;

private:
    std::string               server_name_;
    mcp::ToolDefinition       def_;
    mcp::McpServerManager*   mcp_manager_;  // §Phase 11: injected; never nullptr in practice

    // Sanitise an arbitrary string so it is a valid identifier for an LLM
    // tool name. Replaces any character outside [A-Za-z0-9_-] with '_'.
    // Used to derive the `mcp__<server>__<tool>` name and to scrub the
    // description when needed.
    static std::string sanitize_name(const std::string& s);
};