#include "McpStatusTool.h"
#include "core/FileIO.h"

McpStatusTool::McpStatusTool(mcp::McpServerManager* manager)
    : manager_(manager) {}

json McpStatusTool::get_definition() const {
    return {
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", get_description()},
            {"parameters", {
                {"type", "object"},
                {"properties", json::object()},
                {"required", json::array()}
            }}
        }}
    };
}

json McpStatusTool::execute(const json& /*args*/, FileIO& /*file_io*/) {
    json result = json::object();
    result["status"] = "success";

    if (!manager_) {
        result["mcp_enabled"] = false;
        result["message"] =
            "MCP is disabled for this agent (mcp_enabled=false) — no "
            "McpServerManager exists.";
        return result;
    }
    auto& mgr = *manager_;

    result["mcp_enabled"]   = true;
    result["config_loaded"] = mgr.is_config_loaded();

    // --- Per-server state ---------------------------------------------
    json servers = json::array();
    size_t connected_count = 0;
    for (const auto& name : mgr.list_configured_servers()) {
        const auto* cfg = mgr.get_config(name);
        const bool connected = mgr.is_connected(name);
        const bool healthy   = mgr.is_healthy(name);
        if (connected) ++connected_count;

        json s = json::object();
        s["name"]      = name;
        s["transport"] = (cfg && cfg->is_http()) ? "http" : "stdio";
        s["source"]    = mgr.is_fallback_sourced(name)
                             ? "servers.json" : "sqlite_store";
        if (cfg) {
            if (cfg->is_http()) {
                s["url"] = cfg->url;
            } else {
                s["command"] = cfg->command;
            }
            if (!cfg->description.empty()) {
                s["description"] = cfg->description;
            }
        }
        s["connected"] = connected;
        s["healthy"]   = healthy;
        const auto tools = mgr.get_tools(name);
        s["tool_count"] = tools.size();
        json tool_names = json::array();
        for (const auto& t : tools) {
            tool_names.push_back("mcp__" + name + "__" + t.name);
        }
        s["tools"] = std::move(tool_names);
        servers.push_back(std::move(s));
    }
    result["servers"]          = std::move(servers);
    result["configured_count"] = mgr.list_configured_servers().size();
    result["connected_count"]  = connected_count;

    // --- Host-disabled servers ----------------------------------------
    result["disabled_servers"] = json::array();
    for (const auto& name : mgr.get_disabled_servers()) {
        result["disabled_servers"].push_back(name);
    }

    // --- Config locations (the "where do I put config" answer) --------
    result["config_locations"] = json::object({
        {"sqlite_store",
         "mcp_servers table in the daedalus SettingsStore (authoritative); "
         "write via SettingsStore::upsert_mcp_server()"},
        {"servers_json", json::object({
            {"path",   mgr.servers_json_path().string()},
            {"exists", mgr.servers_json_exists()},
            {"error",  mgr.servers_json_error()},
            {"note",   "standard mcpServers JSON; merged as a fallback — "
                       "the store wins on name conflicts"},
        })},
        {"env_file", mgr.get_env_file_path()},
    });

    if (!mgr.get_last_error().empty()) {
        result["last_error"] = mgr.get_last_error();
    }

    // --- Zero-server diagnostic hint -----------------------------------
    if (mgr.list_configured_servers().empty()) {
        result["hint"] =
            "No MCP servers are configured, so no mcp__<server>__<tool> "
            "tools exist. Configure one via the host (SettingsStore::"
            "upsert_mcp_server) or write a standard mcpServers file to "
            "the servers_json path above, then start a new session.";
    }

    return result;
}
