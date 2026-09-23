// SPDX-License-Identifier: BSL-1.0
// src/tools/McpControlTool.cpp — see header for §MCP-CONTROL-CORE rationale.

#include "tools/McpControlTool.h"

#include "core/Logger.h"

McpControlTool::McpControlTool(mcp::McpServerManager* manager,
                               ControlHandler handler)
    : manager_(manager), handler_(std::move(handler)) {}

json McpControlTool::get_definition() const {
    return {
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", get_description()},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"action", {
                        {"type", "string"},
                        {"enum", json::array({"list", "enable", "disable"})},
                        {"description", "list = show configured servers and "
                                        "state; enable/disable = toggle one "
                                        "server (requires 'name')."}
                    }},
                    {"name", {
                        {"type", "string"},
                        {"description", "Server name (required for "
                                        "enable/disable), as shown by "
                                        "action='list'."}
                    }}
                }},
                {"required", json::array({"action"})}
            }}
        }}
    };
}

json McpControlTool::execute(const json& args, FileIO& /*file_io*/) {
    if (!manager_) {
        return json::object({
            {"status", "error"},
            {"message", "MCP is disabled for this agent "
                        "(mcp_enabled=false) — no McpServerManager exists."}
        });
    }

    const std::string action = args.value("action", "");
    if (action == "list") {
        return do_list();
    }
    if (action == "enable" || action == "disable") {
        const std::string name = args.value("name", "");
        if (name.empty()) {
            return json::object({
                {"status", "error"},
                {"message", "action='" + action + "' requires a 'name' "
                            "argument. Use action='list' to see server names."}
            });
        }
        return do_set_enabled(name, action == "enable");
    }

    return json::object({
        {"status", "error"},
        {"message", "Unknown action '" + action +
                    "'. Valid actions: list, enable, disable."}
    });
}

json McpControlTool::do_list() {
    auto& mgr = *manager_;
    json servers = json::array();
    size_t enabled_count = 0;

    for (const auto& name : mgr.list_configured_servers()) {
        const bool disabled = mgr.is_server_disabled(name);
        if (!disabled) ++enabled_count;

        json s = json::object();
        s["name"]      = name;
        s["enabled"]   = !disabled;
        s["connected"] = mgr.is_connected(name);
        s["tool_count"] = mgr.get_tools(name).size();
        if (const auto* cfg = mgr.get_config(name)) {
            s["transport"] = cfg->is_http() ? "http" : "stdio";
            if (!cfg->description.empty()) {
                s["description"] = cfg->description;
            }
        }
        servers.push_back(std::move(s));
    }

    return json::object({
        {"status", "success"},
        {"configured_count", servers.size()},
        {"enabled_count", enabled_count},
        {"servers", servers},
        {"hint", "action='disable' name='<server>' disconnects a server and "
                 "reclaims its tool-schema context for this session; "
                 "'enable' reconnects it. Runtime-only — the stored "
                 "configuration is never modified."}
    });
}

json McpControlTool::do_set_enabled(const std::string& name, bool enabled) {
    auto& mgr = *manager_;

    const bool known    = (mgr.get_config(name) != nullptr);
    const bool disabled = mgr.is_server_disabled(name);

    // §MCP-CONTROL-CORE fix (2026-09-22 review): the manager is NOT the
    // authority on "known" servers — AgentImpl::set_mcp_server_enabled
    // falls back to the SettingsStore, so a server the manager never
    // loaded (host-disabled at startup, upserted mid-session) is still
    // enable-able through the handler. Only reject unknown names when
    // there is NO handler to consult.
    if (!known && !handler_) {
        return json::object({
            {"status", "error"},
            {"message", "Unknown MCP server '" + name +
                        "'. Use action='list' to see configured servers."}
        });
    }

    // No-op short-circuit — only meaningful when the manager actually
    // tracks this server; otherwise the handler is the authority on
    // current state. no_op = (enable && already enabled) ||
    // (disable && already disabled). Treating a repeat as success (not
    // an error) keeps the agent from being punished for state drift
    // between turns.
    const bool no_op = (enabled == !disabled);
    if (known && no_op) {
        return json::object({
            {"status", "success"},
            {"server", name},
            {"enabled", enabled},
            {"changed", false},
            {"message", "Server '" + name + "' is already " +
                        (enabled ? "enabled." : "disabled.")}
        });
    }

    bool ok = false;
    if (handler_) {
        // Full coordination path: AgentImpl validates against the store,
        // updates the per-agent ToolFilter and logs to the sink.
        ok = handler_(name, enabled);
    } else if (!enabled) {
        mgr.disable_server(name);
        ok = true;
    } else {
        // No handler: `known` is guaranteed true (early return above).
        ok = mgr.enable_server(*mgr.get_config(name));
    }

    if (!ok) {
        return json::object({
            {"status", "error"},
            {"server", name},
            {"message", known
                ? ("Failed to " + std::string(enabled ? "enable" : "disable") +
                   " server '" + name + "'." +
                   (enabled ? " The server may be unreachable — "
                               "check the mcp_status tool for the "
                               "manager's last error."
                             : ""))
                : ("Unknown MCP server '" + name +
                   "' — not in the manager or the stored configuration. "
                   "Use action='list' to see configured servers.")}
        });
    }

    LOG_INFO("[MCP] Agent '" + name + "' " +
             (enabled ? "enabled" : "disabled") + " via mcp_control tool");

    return json::object({
        {"status", "success"},
        {"server", name},
        {"enabled", enabled},
        {"changed", true},
        {"message", enabled
            ? "Server '" + name + "' re-enabled — its tools reappear on "
              "the next request."
            : "Server '" + name + "' disabled for this session — its tool "
              "schemas leave the context on the next request. The stored "
              "configuration was NOT modified; it returns next session."}
    });
}
