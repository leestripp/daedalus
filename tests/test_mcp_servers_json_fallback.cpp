// ============================================================================
// tests/test_mcp_servers_json_fallback.cpp
//
// §BUG-001 regression test (2026-09-17, MonkeyByte report): MCP server
// tools were never injected because the documented mcp/servers.json
// fallback silently no-opped after the v0.5.0 SQLite migration, and the
// agent had no introspection tool to diagnose it.
//
// Covers:
//   1. load_servers_json_fallback() parses the standard mcpServers shape
//      (stdio + http + bearerToken + "type" alias + env).
//   2. enabled:false / disabled:true / invalid entries are skipped.
//   3. The SQLite store wins on name conflicts; fallback-sourced servers
//      are tracked for introspection.
//   4. Missing file = quiet no-op; malformed JSON = recorded error.
//   5. McpStatusTool reports per-server state + config locations, and
//      ToolFactory::refresh_mcp_tools() registers mcp_status even with
//      zero servers configured (no connects attempted).
//
// Hermetic — temp dirs only, no process spawns, no network.
// ============================================================================

#include "mcp/McpServerManager.h"
#include "tools/McpStatusTool.h"
#include "tools/ToolFactory.h"
#include "core/FileIO.h"

#include <daedalus/settings.h>

// The default build is Release (-DNDEBUG), which would compile assert()
// out and make this regression test a no-op. Force assertions on.
#undef NDEBUG
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

namespace {

fs::path fresh_dir(const std::string& tag) {
    fs::path d = fs::temp_directory_path() / ("mcp-fallback-" + tag);
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

void write_servers_json(const fs::path& path, const std::string& body) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path);
    f << body;
}

const char* kServersJson = R"JSON({
  "mcpServers": {
    "from-file-stdio": {
      "command": "ssh",
      "args": ["lee@ubuntu-pc", "comfy-mcp"],
      "env": {"COMFY_BIN": "/home/lee/.venv/bin/comfy"},
      "description": "stdio over ssh"
    },
    "from-file-http": {
      "type": "streamable-http",
      "url": "http://127.0.0.1:8765/mcp",
      "bearerToken": "secret-token"
    },
    "store-wins": {
      "command": "from-file-command"
    },
    "off-by-enabled": {
      "command": "nope",
      "enabled": false
    },
    "off-by-disabled": {
      "command": "nope",
      "disabled": true
    },
    "host-disabled": {
      "command": "nope"
    },
    "invalid-no-command": {
      "args": ["nothing"]
    }
  }
})JSON";

// 1-3: parsing, skipping, precedence, source tracking.
void test_fallback_merge_semantics() {
    auto root = fresh_dir("merge");
    const fs::path home = root / "home";
    fs::create_directories(home);
    const fs::path servers_json = home / "mcp" / "servers.json";
    write_servers_json(servers_json, kServersJson);

    auto store = daedalus::SettingsStore::for_home(home);
    daedalus::SettingsStore::McpServer srv;
    srv.name    = "store-wins";
    srv.command = "from-store-command";
    store.upsert_mcp_server(srv);

    mcp::McpServerManager mgr;
    mgr.set_disabled_servers({"host-disabled"});
    assert(mgr.load_from_store(store));
    const size_t added = mgr.load_servers_json_fallback(servers_json);

    assert(added == 2);  // from-file-stdio + from-file-http only
    assert(mgr.servers_json_exists());
    assert(mgr.servers_json_error().empty());
    assert(mgr.servers_json_path() == servers_json);

    const auto* stdio_cfg = mgr.get_config("from-file-stdio");
    assert(stdio_cfg != nullptr);
    assert(stdio_cfg->is_stdio());
    assert(stdio_cfg->command == "ssh");
    assert(stdio_cfg->args.size() == 2);
    assert(stdio_cfg->env.at("COMFY_BIN") == "/home/lee/.venv/bin/comfy");
    assert(stdio_cfg->description == "stdio over ssh");
    assert(mgr.is_fallback_sourced("from-file-stdio"));

    const auto* http_cfg = mgr.get_config("from-file-http");
    assert(http_cfg != nullptr);
    assert(http_cfg->is_http());
    assert(http_cfg->url == "http://127.0.0.1:8765/mcp");
    assert(http_cfg->bearer_token == "secret-token");

    // Store wins on conflict, and is NOT marked fallback-sourced.
    const auto* conflict = mgr.get_config("store-wins");
    assert(conflict != nullptr);
    assert(conflict->command == "from-store-command");
    assert(!mgr.is_fallback_sourced("store-wins"));

    // Skipped entries never appear.
    assert(mgr.get_config("off-by-enabled") == nullptr);
    assert(mgr.get_config("off-by-disabled") == nullptr);
    assert(mgr.get_config("host-disabled") == nullptr);
    assert(mgr.get_config("invalid-no-command") == nullptr);

    assert(mgr.list_configured_servers().size() == 3);

    fs::remove_all(root);
    std::cout << "[ok] test_fallback_merge_semantics" << std::endl;
}

// 4: missing file is a quiet no-op; malformed JSON records an error.
void test_fallback_missing_and_malformed() {
    auto root = fresh_dir("missing");

    mcp::McpServerManager mgr;
    assert(mgr.load_servers_json_fallback(root / "nope.json") == 0);
    assert(!mgr.servers_json_exists());
    assert(mgr.servers_json_error().empty());

    const fs::path bad = root / "bad.json";
    write_servers_json(bad, "{ not json ");
    assert(mgr.load_servers_json_fallback(bad) == 0);
    assert(mgr.servers_json_exists());
    assert(!mgr.servers_json_error().empty());

    const fs::path shape = root / "shape.json";
    write_servers_json(shape, R"({"servers": {}})");
    assert(mgr.load_servers_json_fallback(shape) == 0);
    assert(!mgr.servers_json_error().empty());

    fs::remove_all(root);
    std::cout << "[ok] test_fallback_missing_and_malformed" << std::endl;
}

// 5: mcp_status reports everything the agent needs for self-diagnosis.
void test_status_tool_output() {
    auto root = fresh_dir("status");
    const fs::path home = root / "home";
    fs::create_directories(home);
    const fs::path servers_json = home / "mcp" / "servers.json";
    write_servers_json(servers_json, kServersJson);

    mcp::McpServerManager mgr;
    // No store, no host-disabled list here: from-file-stdio,
    // from-file-http, store-wins and host-disabled are all file-added.
    assert(mgr.load_servers_json_fallback(servers_json) == 4);

    McpStatusTool tool(&mgr);
    FileIO fio(root);
    const json out = tool.execute(json::object(), fio);

    assert(out["status"] == "success");
    assert(out["mcp_enabled"] == true);
    assert(out["config_loaded"] == false);  // store never loaded here
    assert(out["configured_count"] == 4);
    assert(out["connected_count"] == 0);
    assert(!out.contains("hint"));  // servers ARE configured

    bool saw_stdio = false;
    for (const auto& s : out["servers"]) {
        if (s["name"] == "from-file-stdio") {
            saw_stdio = true;
            assert(s["source"] == "servers.json");
            assert(s["transport"] == "stdio");
            assert(s["command"] == "ssh");
            assert(s["connected"] == false);
            assert(s["tool_count"] == 0);
            assert(s["tools"].empty());
        }
    }
    assert(saw_stdio);

    const auto& loc = out["config_locations"]["servers_json"];
    assert(loc["path"] == servers_json.string());
    assert(loc["exists"] == true);
    assert(loc["error"].get<std::string>().empty());

    // Zero-server case: hint must name the config locations.
    mcp::McpServerManager empty_mgr;
    empty_mgr.load_servers_json_fallback(root / "absent.json");
    McpStatusTool empty_tool(&empty_mgr);
    const json empty_out = empty_tool.execute(json::object(), fio);
    assert(empty_out["configured_count"] == 0);
    assert(empty_out.contains("hint"));

    fs::remove_all(root);
    std::cout << "[ok] test_status_tool_output" << std::endl;
}

// 5b: refresh_mcp_tools registers mcp_status even with zero servers —
// and attempts no connects (nothing to connect to).
void test_factory_registers_status_tool() {
    auto root = fresh_dir("factory");

    mcp::McpServerManager mgr;
    mgr.load_servers_json_fallback(root / "absent.json");
    // config_loaded_ stays false: the status tool must still register.

    ToolFactory factory;
    factory.set_mcp_manager(&mgr);
    factory.refresh_mcp_tools();
    assert(factory.has_tool("mcp_status"));

    FileIO fio(root);
    const json out = factory.execute_tool("mcp_status", json::object(), fio);
    assert(out["status"] == "success");
    assert(out["configured_count"] == 0);
    assert(out.contains("hint"));

    fs::remove_all(root);
    std::cout << "[ok] test_factory_registers_status_tool" << std::endl;
}

}  // namespace

int main() {
    test_fallback_merge_semantics();
    test_fallback_missing_and_malformed();
    test_status_tool_output();
    test_factory_registers_status_tool();
    std::cout << "all MCP servers.json fallback tests passed" << std::endl;
    return 0;
}
