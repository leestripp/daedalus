#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "BaseTool.h"
#include "mcp/McpServerManager.h"

using json = nlohmann::json;

/**
 * McpStatusTool — §BUG-001 fix (2026-09-17, MonkeyByte report).
 *
 * Agent-visible MCP introspection. Before this tool existed, the agent
 * could only infer MCP state from its own function schema: when a server
 * was misconfigured (or its config was written somewhere daedalus never
 * reads), the tools simply never appeared and the agent had no way to
 * diagnose why — no list of configured servers, no connection state, no
 * error surface.
 *
 * Reports, for every configured server: name, transport, config source
 * (SQLite store vs servers.json fallback), connection/health state and
 * discovered tool count — plus the servers.json fallback path/exists/
 * error, the host-disabled list, and the manager's last error. When zero
 * servers are configured the output includes a hint naming the exact
 * config locations daedalus reads.
 *
 * Registered by ToolFactory::refresh_mcp_tools() whenever an
 * McpServerManager exists — deliberately ALSO when zero servers are
 * configured, because that is the case that needs diagnosing.
 */
class McpStatusTool : public BaseTool {
public:
    // `manager` is owned by the Agent (never null in practice — the
    // factory only registers this tool while a manager is installed).
    explicit McpStatusTool(mcp::McpServerManager* manager);

    std::string get_name() const override { return "mcp_status"; }
    std::string get_description() const override {
        return "Show MCP server configuration and connection status: "
               "configured servers, config source, connected/healthy "
               "state, discovered tool counts, and load errors. Use this "
               "to diagnose missing mcp__<server>__<tool> tools.";
    }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;

private:
    mcp::McpServerManager* manager_;
};
