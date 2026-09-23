// SPDX-License-Identifier: BSL-1.0
// src/tools/McpControlTool.h
//
// §MCP-CONTROL-CORE (2026-09-22): agent-driven MCP server enable/disable.
//
// Why: MCP tool schemas can dominate the context window (a single busy
// server can contribute tens of thousands of tokens of tool definitions,
// sent with EVERY request). Before this tool, an agent that was done
// with a server had to ask the user to switch it off. Now the agent can
// shed the context itself — disable_server() disconnects the transport,
// drops the server's proxied tools from the factory on the next
// refresh, and the per-iteration Context Status roster (§LIST-TOOLS-CORE)
// reflects the change immediately.
//
// Semantics are RUNTIME-ONLY: the SettingsStore row (mcp_servers.enabled)
// is never written — that column remains the host/user's persistent
// control. A disabled server comes back on the next session unless the
// user disables it in the store.
//
// Actions (single tool, one schema — the point is a SMALL surface):
//   list     — compact table of configured servers: transport,
//              connected/enabled state, tool count. (mcp_status remains
//              the verbose diagnostic with config sources and errors.)
//   enable   — reconnect a runtime-disabled server.
//   disable  — disconnect a server and drop its tools from the surface.
//
// Enable/disable go through an injected handler (wired by AgentImpl to
// Agent::set_mcp_server_enabled) so the per-agent ToolFilter and sink
// logging stay consistent. Without a handler (direct ToolHandler users)
// the tool falls back to manager-only operations; enable then only works
// for servers whose config the manager already knows.
#pragma once

#include <functional>
#include <string>
#include <nlohmann/json.hpp>
#include "BaseTool.h"
#include "mcp/McpServerManager.h"

using json = nlohmann::json;

class McpControlTool : public BaseTool {
public:
    /// Returns true when the enable/disable succeeded.
    using ControlHandler = std::function<bool(const std::string& name,
                                              bool enabled)>;

    /// `manager` matches McpStatusTool's contract: owned by the Agent,
    /// non-null in practice (the factory only registers this tool while
    /// a manager is installed). `handler` may be empty — see header note.
    McpControlTool(mcp::McpServerManager* manager, ControlHandler handler);

    std::string get_name() const override { return "mcp_control"; }
    std::string get_description() const override {
        return "List, enable, or disable MCP servers at runtime. MCP tool "
               "schemas are sent with every request and can consume a large "
               "share of the context window — when you are finished with a "
               "server (e.g. an image/video generation server), disable it "
               "to reclaim that space instead of asking the user. "
               "action='list' shows configured servers with connection "
               "state and tool counts; 'enable'/'disable' take a server "
               "'name'. Runtime-only: changes last for this session and "
               "never touch the user's stored configuration; a disabled "
               "server's tools disappear from your tool surface on the "
               "next request.";
    }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;

private:
    json do_list();
    json do_set_enabled(const std::string& name, bool enabled);

    mcp::McpServerManager* manager_;
    ControlHandler handler_;
};
