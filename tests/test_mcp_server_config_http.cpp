/**
 * @file test_mcp_server_config_http.cpp
 * @brief Regression tests for MCP server config parsing (HTTP/streamable-http transport)
 *
 * Background:
 *   Prior to the HTTP-transport fix, `McpServerConfig::is_valid()` required
 *   non-empty `command` AND `args`, silently rejecting HTTP-based servers
 *   like:
 *
 *       "apollocore": {
 *           "url": "http://127.0.0.1:8765/mcp",
 *           "transport": "streamable-http"
 *       }
 *
 *   The user would see "[MCP] Loaded 3 configured server(s)" with no warning,
 *   making this very hard to diagnose. These tests pin both the old (stdio)
 *   path and the new (http) path so the configuration parsing cannot regress.
 *
 *   No live network connections are made; only `McpServerManager::load_config`
 *   is exercised against temp files.
 */

#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "mcp/McpServerConfig.h"
#include "mcp/McpServerManager.h"

using json = nlohmann::json;
using namespace mcp;

static int test_count = 0;
static int failures = 0;

#define TEST(name) int test_##name()

#define CHECK(cond) do { \
    test_count++; \
    if (!(cond)) { \
        std::cerr << "FAIL: " << #cond << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        failures++; \
        return 1; \
    } \
} while(0)

#define CHECK_EQ(a, b) do { \
    test_count++; \
    if (!((a) == (b))) { \
        std::cerr << "FAIL: " << #a << " == " << #b \
                  << " (" << (a) << " vs " << (b) << ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
        failures++; \
        return 1; \
    } \
} while(0)

namespace {

// Write content to a temp file with a stable per-test name. Returns the path.
std::filesystem::path write_temp_json(const std::string& name, const std::string& body) {
    auto dir = std::filesystem::temp_directory_path() / "daedalus_mcp_http_test";
    std::filesystem::create_directories(dir);
    auto path = dir / name;
    std::ofstream out(path);
    out << body;
    out.close();
    return path;
}

}  // namespace

// ============================================================================
// Test: parse_transport_kind - aliases
// ============================================================================
TEST(parse_transport_kind_empty_defaults_to_stdio) {
    CHECK_EQ(static_cast<int>(parse_transport_kind("")),      static_cast<int>(TransportKind::Stdio));
    CHECK_EQ(static_cast<int>(parse_transport_kind("stdio")), static_cast<int>(TransportKind::Stdio));
    CHECK_EQ(static_cast<int>(parse_transport_kind("garbage")),static_cast<int>(TransportKind::Stdio));
    return 0;
}

TEST(parse_transport_kind_http_aliases) {
    CHECK_EQ(static_cast<int>(parse_transport_kind("http")),            static_cast<int>(TransportKind::StreamableHttp));
    CHECK_EQ(static_cast<int>(parse_transport_kind("https")),           static_cast<int>(TransportKind::StreamableHttp));
    CHECK_EQ(static_cast<int>(parse_transport_kind("sse")),             static_cast<int>(TransportKind::StreamableHttp));
    CHECK_EQ(static_cast<int>(parse_transport_kind("streamable-http")), static_cast<int>(TransportKind::StreamableHttp));
    CHECK_EQ(static_cast<int>(parse_transport_kind("streamable_http")), static_cast<int>(TransportKind::StreamableHttp));
    return 0;
}

// ============================================================================
// Test: McpServerConfig::is_valid - stdio regression
// ============================================================================
// §FIX-MCP-STORE-LOAD (2026-07-17, plan R3-P1): stdio accepts an empty
// args vector — many CLI tools (`/bin/true`, `node`, `cat`) accept zero
// arguments. The previous contract required `!args.empty()`, which
// caused real, legitimate servers to be silently dropped by both the
// JSON and SQLite load paths (see test_mcp_load_from_agent.cpp).
TEST(is_valid_stdio_requires_command_only) {
    McpServerConfig c;
    c.name = "foo";

    // Missing command
    c.command = "";
    c.args = {"x"};
    c.transport = "stdio";
    CHECK(!c.is_valid());

    // Missing args is OK — args is OPTIONAL for stdio servers.
    c.command = "uvx";
    c.args = {};
    c.transport = "stdio";
    CHECK(c.is_valid());

    // Both present
    c.command = "uvx";
    c.args = {"server"};
    c.transport = "stdio";
    CHECK(c.is_valid());
    return 0;
}

// ============================================================================
// Test: McpServerConfig::is_valid - http requires url
// ============================================================================
TEST(is_valid_http_requires_url) {
    McpServerConfig c;
    c.name = "apollocore";
    c.transport = "streamable-http";

    // No url
    c.url = "";
    CHECK(!c.is_valid());

    // url present (the user's actual apollocore entry shape)
    c.url = "http://127.0.0.1:8765/mcp";
    CHECK(c.is_valid());
    return 0;
}

TEST(is_valid_http_with_http_alias_accepted) {
    McpServerConfig c;
    c.name = "remote";
    c.transport = "http";   // alias
    c.url = "https://mcp.example.com";
    CHECK(c.is_valid());
    CHECK(c.is_http());
    CHECK(!c.is_stdio());
    return 0;
}

// ============================================================================
// Test: Empty `transport` defaults to stdio (back-compat for old configs)
// ============================================================================
TEST(transport_defaults_to_stdio_when_omitted) {
    McpServerConfig c;
    c.name = "legacy";
    c.transport = "";        // omitted -> stdio (back-compat)
    c.command = "uvx";
    c.args = {"server"};
    CHECK(c.is_valid());
    CHECK(c.is_stdio());
    CHECK(!c.is_http());
    return 0;
}

// ============================================================================
// Test: Behaviour name -> is_http/is_stdio agreement
// ============================================================================
TEST(is_http_is_stdio_partition) {
    McpServerConfig c;
    c.name = "x";

    // stdio
    c.transport = "stdio";
    c.command = "uvx"; c.args = {"x"};
    CHECK(c.is_stdio());
    CHECK(!c.is_http());

    // http
    c.transport = "streamable-http";
    c.url = "http://localhost:8000/mcp";
    CHECK(c.is_http());
    CHECK(!c.is_stdio());

    // empty -> stdio
    c.transport = "";
    c.command = "uvx"; c.args = {"x"};
    CHECK(c.is_stdio());
    CHECK(!c.is_http());
    return 0;
}

// ============================================================================
// Test: McpServerManager::load_config with the user's actual 4-server config
//         (regression for the "Loaded 3 configured server(s)" bug)
// ============================================================================
TEST(load_config_accepts_http_server_from_user_config) {
    // This is the user's actual ~/.daedalus/mcp/servers.json content.
    const std::string body = R"({
        "mcpServers": {
            "minimax-web-search": {
                "command": "uvx",
                "args": ["minimax-coding-plan-mcp", "web-search"],
                "env": {
                    "MINIMAX_API_HOST": "https://api.minimax.io",
                    "MINIMAX_API_KEY": "${MINIMAX_API_KEY}"
                },
                "description": "Web search using MiniMax MCP"
            },
            "minimax-read-image": {
                "command": "uvx",
                "args": ["minimax-coding-plan-mcp", "read-image"],
                "env": {
                    "MINIMAX_API_HOST": "https://api.minimax.io",
                    "MINIMAX_API_KEY": "${MINIMAX_API_KEY}"
                },
                "description": "Read and analyze images using MiniMax MCP"
            },
            "wikipedia": {
                "command": "uvx",
                "args": ["--from", "mcp-server-wikipedia", "wikipedia-mcp-server"],
                "description": "Search Wikipedia for information"
            },
            "apollocore": {
                "url": "http://127.0.0.1:8765/mcp",
                "transport": "streamable-http"
            }
        }
    })";

    auto path = write_temp_json("user_4_servers.json", body);

    McpServerManager& mgr = McpServerManager::instance();
    bool ok = mgr.load_config(path.string());
    CHECK(ok);

    auto servers = mgr.list_configured_servers();
    CHECK_EQ(static_cast<int>(servers.size()), 4);

    // All four names are present.
    bool has_minimax_search = false, has_minimax_image = false;
    bool has_wiki = false, has_apollocore = false;
    for (const auto& s : servers) {
        if (s == "minimax-web-search") has_minimax_search = true;
        if (s == "minimax-read-image") has_minimax_image = true;
        if (s == "wikipedia")          has_wiki = true;
        if (s == "apollocore")        has_apollocore = true;
    }
    CHECK(has_minimax_search);
    CHECK(has_minimax_image);
    CHECK(has_wiki);
    CHECK(has_apollocore);

    // The apollocore config should be the http-transport one and well-formed.
    const McpServerConfig* apollo = mgr.get_config("apollocore");
    CHECK(apollo != nullptr);
    CHECK(apollo->is_http());
    CHECK_EQ(apollo->url, std::string("http://127.0.0.1:8765/mcp"));
    CHECK_EQ(apollo->transport, std::string("streamable-http"));
    return 0;
}

// ============================================================================
// Test: load_config rejects entries with no command and no url (with no crash)
// ============================================================================
TEST(load_config_rejects_invalid_entry_safely) {
    const std::string body = R"({
        "mcpServers": {
            "good": {
                "command": "uvx",
                "args": ["server"],
                "description": "Valid stdio entry"
            },
            "bad": {
                "description": "Entry missing both stdio fields and http url"
            }
        }
    })";

    auto path = write_temp_json("invalid_entry.json", body);

    McpServerManager& mgr = McpServerManager::instance();
    bool ok = mgr.load_config(path.string());
    CHECK(ok);

    // Only the valid "good" entry survives.
    auto servers = mgr.list_configured_servers();
    CHECK_EQ(static_cast<int>(servers.size()), 1);
    CHECK_EQ(servers[0], std::string("good"));
    return 0;
}

// ============================================================================
// Test: load_config rejects http transport with empty url
// ============================================================================
TEST(load_config_rejects_http_with_empty_url) {
    const std::string body = R"({
        "mcpServers": {
            "broken-http": {
                "url": "",
                "transport": "streamable-http",
                "description": "HTTP server with empty url"
            }
        }
    })";

    auto path = write_temp_json("http_no_url.json", body);

    McpServerManager& mgr = McpServerManager::instance();
    bool ok = mgr.load_config(path.string());
    CHECK(ok);

    auto servers = mgr.list_configured_servers();
    CHECK_EQ(static_cast<int>(servers.size()), 0);
    return 0;
}

// ============================================================================
// Test: Bear token (camelCase alias) round-trip
// ============================================================================
TEST(load_config_accepts_bearertoken_camelcase) {
    const std::string body = R"({
        "mcpServers": {
            "secure": {
                "url": "https://mcp.example.com",
                "transport": "streamable-http",
                "bearerToken": "secret-token"
            }
        }
    })";

    auto path = write_temp_json("bearer.json", body);

    McpServerManager& mgr = McpServerManager::instance();
    bool ok = mgr.load_config(path.string());
    CHECK(ok);

    const McpServerConfig* cfg = mgr.get_config("secure");
    CHECK(cfg != nullptr);
    CHECK_EQ(cfg->bearer_token, std::string("secret-token"));
    return 0;
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "Running MCP HTTP-server config regression tests..." << std::endl;

    int local_failures = 0;
    local_failures += test_parse_transport_kind_empty_defaults_to_stdio();
    local_failures += test_parse_transport_kind_http_aliases();
    local_failures += test_is_valid_stdio_requires_command_only();
    local_failures += test_is_valid_http_requires_url();
    local_failures += test_is_valid_http_with_http_alias_accepted();
    local_failures += test_transport_defaults_to_stdio_when_omitted();
    local_failures += test_is_http_is_stdio_partition();
    local_failures += test_load_config_accepts_http_server_from_user_config();
    local_failures += test_load_config_rejects_invalid_entry_safely();
    local_failures += test_load_config_rejects_http_with_empty_url();
    local_failures += test_load_config_accepts_bearertoken_camelcase();

    std::cout << "\nResults: " << (local_failures == 0 ? "PASS" : "FAIL") << std::endl;
    std::cout << "Total checks: " << test_count << std::endl;
    std::cout << "Failed checks: " << local_failures << std::endl;

    return local_failures > 0 ? 1 : 0;
}
