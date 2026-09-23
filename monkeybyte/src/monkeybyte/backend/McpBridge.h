#pragma once

#include <daedalus/settings.h>

#include <expected>
#include <string>
#include <vector>

namespace monkeybyte {

// UI-level MCP server model: flat command line + env as JSON object text.
struct McpServer {
    std::string name; // unique id
    std::string command;
    std::string env_json; // raw JSON object text
    bool enabled = true;
};

// Bridge to the MCP server registry the agent actually uses.
//
// daedalus loads MCP servers from its own SettingsStore at
// $XDG_DATA_HOME/daedalus/daedalus.db (McpServerManager::load_from_store),
// NOT from monkeybyte.db — so Settings → MCP Servers must read/write that
// store or the tab shows servers the agent never sees. These functions
// convert between the flat UI model and daedalus::SettingsStore::McpServer.
// Call sites pass daedalus::SettingsStore::instance(); tests can inject
// SettingsStore::for_home(<tempdir>).
namespace mcp_bridge {

std::vector<McpServer> list(daedalus::SettingsStore& store);

// Upsert by name. Fields the UI doesn't model (transport, url,
// bearer_token, description) are preserved when updating an existing
// server; new servers are created as stdio.
void save(daedalus::SettingsStore& store, const McpServer& s);

void remove(daedalus::SettingsStore& store, const std::string& name);

// Parse provider-supplied JSON (Claude/VS Code "mcpServers" style, a
// single keyed server object, or a bare single-server config) into
// daedalus server structs. Returns an error string on malformed JSON,
// a missing name, or a server with neither command nor url. (design §5.4)
std::expected<std::vector<daedalus::SettingsStore::McpServer>, std::string>
parse_json(const std::string& text);

} // namespace mcp_bridge

} // namespace monkeybyte
