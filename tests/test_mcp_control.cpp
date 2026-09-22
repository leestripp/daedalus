// SPDX-License-Identifier: BSL-1.0
// tests/test_mcp_control.cpp
//
// §MCP-CONTROL-CORE (2026-09-22): mcp_control tool — agent-driven MCP
// server list/enable/disable. Hermetic: no network, no process spawns —
// uses a standalone McpServerManager with add_config() (no connections
// are ever attempted) and fake control handlers.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "tools/McpControlTool.h"
#include "tools/ToolFactory.h"
#include "core/FileIO.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace {

static FileIO make_test_file_io() {
    const auto tmp = std::filesystem::temp_directory_path()
                     / "daedalus_mcp_control_tests";
    std::filesystem::create_directories(tmp);
    return FileIO(tmp);
}

static mcp::McpServerConfig make_http_config(const std::string& name,
                                             const std::string& desc = {}) {
    mcp::McpServerConfig cfg;
    cfg.name      = name;
    cfg.transport = "http";
    cfg.url       = "http://127.0.0.1:9/mcp";  // never contacted in tests
    cfg.description = desc;
    return cfg;
}

} // namespace

TEST_CASE("mcp_control definition exposes action enum and name") {
    mcp::McpServerManager mgr;
    McpControlTool tool(&mgr, {});
    CHECK(tool.get_name() == "mcp_control");
    const json def = tool.get_definition();
    CHECK(def["function"]["name"] == "mcp_control");
    const auto& props = def["function"]["parameters"]["properties"];
    REQUIRE(props.contains("action"));
    REQUIRE(props.contains("name"));
    const auto actions = props["action"]["enum"];
    CHECK(actions.size() == 3);
    CHECK(def["function"]["parameters"]["required"]
              == json::array({"action"}));
}

TEST_CASE("null manager reports MCP disabled") {
    McpControlTool tool(nullptr, {});
    FileIO fio = make_test_file_io();
    const json r = tool.execute({{"action", "list"}}, fio);
    CHECK(r.value("status", "") == "error");
    CHECK(r.value("message", "").find("mcp_enabled=false") != std::string::npos);
}

TEST_CASE("list shows configured servers with state") {
    mcp::McpServerManager mgr;
    mgr.add_config(make_http_config("alpha", "first server"));
    mgr.add_config(make_http_config("beta"));
    mgr.disable_server("beta");  // runtime-disabled before listing

    McpControlTool tool(&mgr, {});
    FileIO fio = make_test_file_io();
    const json r = tool.execute({{"action", "list"}}, fio);

    REQUIRE(r.value("status", "") == "success");
    CHECK(r.value("configured_count", 0) == 2);
    CHECK(r.value("enabled_count", 0) == 1);
    REQUIRE(r["servers"].size() == 2);

    // Find each server by name (config map order is not guaranteed).
    json alpha, beta;
    for (const auto& s : r["servers"]) {
        if (s.value("name", "") == "alpha") alpha = s;
        if (s.value("name", "") == "beta")  beta  = s;
    }
    REQUIRE(!alpha.is_null());
    REQUIRE(!beta.is_null());
    CHECK(alpha.value("enabled", false) == true);
    CHECK(alpha.value("transport", "") == "http");
    CHECK(alpha.value("description", "") == "first server");
    CHECK(beta.value("enabled", true) == false);
    CHECK(beta.value("connected", true) == false);
    CHECK(beta.value("tool_count", 1) == 0);
    CHECK(r.contains("hint"));
}

TEST_CASE("disable goes through the injected handler") {
    mcp::McpServerManager mgr;
    mgr.add_config(make_http_config("comfy"));

    std::vector<std::pair<std::string, bool>> calls;
    McpControlTool tool(&mgr,
        [&](const std::string& name, bool enabled) {
            calls.emplace_back(name, enabled);
            if (!enabled) mgr.disable_server(name);
            return true;
        });

    FileIO fio = make_test_file_io();
    const json r = tool.execute({{"action", "disable"}, {"name", "comfy"}}, fio);

    REQUIRE(r.value("status", "") == "success");
    CHECK(r.value("changed", false) == true);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].first == "comfy");
    CHECK(calls[0].second == false);
    CHECK(mgr.is_server_disabled("comfy"));
    // Runtime-only message must reassure the stored config is untouched.
    CHECK(r.value("message", "").find("NOT modified") != std::string::npos);
}

TEST_CASE("repeated disable is a no-op success, not an error") {
    mcp::McpServerManager mgr;
    mgr.add_config(make_http_config("comfy"));
    mgr.disable_server("comfy");

    McpControlTool tool(&mgr, {});
    FileIO fio = make_test_file_io();
    const json r = tool.execute({{"action", "disable"}, {"name", "comfy"}}, fio);
    CHECK(r.value("status", "") == "success");
    CHECK(r.value("changed", true) == false);
}

TEST_CASE("enable goes through the injected handler") {
    mcp::McpServerManager mgr;
    mgr.add_config(make_http_config("comfy"));
    mgr.disable_server("comfy");

    bool handler_ran = false;
    McpControlTool tool(&mgr,
        [&](const std::string& name, bool enabled) {
            handler_ran = true;
            CHECK(name == "comfy");
            CHECK(enabled == true);
            return true;  // fake a successful reconnect
        });

    FileIO fio = make_test_file_io();
    const json r = tool.execute({{"action", "enable"}, {"name", "comfy"}}, fio);
    REQUIRE(r.value("status", "") == "success");
    CHECK(r.value("changed", false) == true);
    CHECK(handler_ran);
}

TEST_CASE("manager-only fallback disables without a handler") {
    mcp::McpServerManager mgr;
    mgr.add_config(make_http_config("comfy"));

    McpControlTool tool(&mgr, {});  // no handler — direct ToolHandler users
    FileIO fio = make_test_file_io();
    const json r = tool.execute({{"action", "disable"}, {"name", "comfy"}}, fio);
    REQUIRE(r.value("status", "") == "success");
    CHECK(mgr.is_server_disabled("comfy"));
}

TEST_CASE("unknown server names are rejected, not silently toggled") {
    mcp::McpServerManager mgr;
    mgr.add_config(make_http_config("comfy"));

    // No handler: the tool itself rejects unknown names without touching
    // the manager.
    {
        McpControlTool tool(&mgr, {});
        FileIO fio = make_test_file_io();
        const json r = tool.execute({{"action", "disable"}, {"name", "comyf"}}, fio);
        CHECK(r.value("status", "") == "error");
        CHECK(r.value("message", "").find("Unknown MCP server") != std::string::npos);
        CHECK(!mgr.is_server_disabled("comyf"));
    }

    // With a handler wired, the handler is the authority (it validates
    // against the SettingsStore, like AgentImpl does). A rejecting
    // handler must surface an error — the tool must not override it.
    {
        bool handler_ran = false;
        McpControlTool tool(&mgr,
            [&](const std::string& name, bool) {
                handler_ran = true;
                return name == "comfy";  // mimic store-backed validation
            });
        FileIO fio = make_test_file_io();
        const json r = tool.execute({{"action", "disable"}, {"name", "comyf"}}, fio);
        CHECK(r.value("status", "") == "error");
        CHECK(r.value("message", "").find("Unknown MCP server") != std::string::npos);
        CHECK(handler_ran);
    }
}

TEST_CASE("handler path enables servers the manager never loaded") {
    // §MCP-CONTROL-CORE fix (2026-09-22 review): a server can be known to
    // the SettingsStore but absent from the manager's configs (host-
    // disabled at startup, upserted mid-session). AgentImpl's enable path
    // loads it from the store — the tool must NOT reject the name before
    // the handler runs.
    mcp::McpServerManager mgr;  // deliberately EMPTY — no configs at all

    bool handler_ran = false;
    McpControlTool tool(&mgr,
        [&](const std::string& name, bool enabled) {
            handler_ran = true;
            CHECK(name == "store-only");
            CHECK(enabled == true);
            return true;  // store lookup + reconnect succeeded
        });

    FileIO fio = make_test_file_io();
    const json r = tool.execute({{"action", "enable"}, {"name", "store-only"}}, fio);
    REQUIRE(r.value("status", "") == "success");
    CHECK(r.value("changed", false) == true);
    CHECK(handler_ran);
}

TEST_CASE("argument validation: missing name, unknown action") {
    mcp::McpServerManager mgr;
    mgr.add_config(make_http_config("comfy"));
    McpControlTool tool(&mgr, {});
    FileIO fio = make_test_file_io();

    {
        const json r = tool.execute({{"action", "disable"}}, fio);
        CHECK(r.value("status", "") == "error");
        CHECK(r.value("message", "").find("requires a 'name'") != std::string::npos);
    }
    {
        const json r = tool.execute({{"action", "explode"}}, fio);
        CHECK(r.value("status", "") == "error");
        CHECK(r.value("message", "").find("Unknown action") != std::string::npos);
    }
}

TEST_CASE("factory registers mcp_control alongside mcp_status on refresh") {
    mcp::McpServerManager mgr;  // zero configs → no connect attempts
    ToolFactory factory;
    CHECK(!factory.has_tool("mcp_control"));

    factory.set_mcp_manager(&mgr);
    bool handler_called = false;
    factory.set_mcp_control_handler(
        [&](const std::string&, bool) { handler_called = true; return true; });
    factory.refresh_mcp_tools();

    CHECK(factory.has_tool("mcp_status"));
    CHECK(factory.has_tool("mcp_control"));

    // Re-refresh with a DIFFERENT manager must not duplicate the tools.
    mcp::McpServerManager mgr2;
    factory.set_mcp_manager(&mgr2);
    factory.refresh_mcp_tools();
    int count = 0;
    for (const auto& name : factory.get_tool_names()) {
        if (name == "mcp_control") ++count;
    }
    CHECK(count == 1);
    (void)handler_called;
}
