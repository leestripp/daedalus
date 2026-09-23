#include "monkeybyte/backend/McpBridge.h"

#include <daedalus/settings.h>
#include <nlohmann/json.hpp>

#include <iterator>
#include <sstream>

namespace monkeybyte::mcp_bridge {

namespace {

using DaedalusMcpServer = daedalus::SettingsStore::McpServer;

// Flat display form: "cmd arg1 arg2"; http servers show their endpoint.
std::string display_command(const DaedalusMcpServer& s) {
    std::string cmd = s.command;
    for (const auto& arg : s.args) {
        cmd += ' ';
        cmd += arg;
    }
    return cmd.empty() ? s.url : cmd;
}

std::string env_to_json(const std::map<std::string, std::string>& env) {
    nlohmann::json j = nlohmann::json::object();
    for (const auto& [k, v] : env) j[k] = v;
    return j.dump();
}

} // namespace

std::vector<McpServer> list(daedalus::SettingsStore& store) {
    std::vector<McpServer> out;
    for (const auto& s : store.list_mcp_servers()) {
        McpServer m;
        m.name = s.name;
        m.command = display_command(s);
        m.env_json = env_to_json(s.env);
        m.enabled = s.enabled;
        out.push_back(std::move(m));
    }
    return out;
}

void save(daedalus::SettingsStore& store, const McpServer& s) {
    DaedalusMcpServer d;
    // Preserve fields the UI doesn't model (transport, url, bearer_token,
    // description) when updating an existing server.
    if (auto existing = store.get_mcp_server(s.name)) d = *existing;

    d.name = s.name;
    d.enabled = s.enabled;

    const bool unchanged_display =
        !d.name.empty() && s.command == display_command(d);
    if (!unchanged_display) {
        // "command args..." → argv split on whitespace (no quoting).
        std::istringstream iss(s.command);
        const std::vector<std::string> argv{std::istream_iterator<std::string>{iss},
                                            std::istream_iterator<std::string>{}};
        if (!argv.empty()) {
            d.transport = "stdio";
            d.command = argv.front();
            d.args.assign(argv.begin() + 1, argv.end());
        }
    }

    d.env.clear();
    const auto j = nlohmann::json::parse(s.env_json, nullptr, false);
    if (j.is_object()) {
        for (const auto& [k, v] : j.items())
            if (v.is_string()) d.env[k] = v.get<std::string>();
    }

    store.upsert_mcp_server(d);
}

void remove(daedalus::SettingsStore& store, const std::string& name) {
    store.delete_mcp_server(name);
}

std::expected<std::vector<DaedalusMcpServer>, std::string>
parse_json(const std::string& text) {
    const auto root = nlohmann::json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object())
        return std::unexpected("Not a valid JSON object.");

    // Normalize to a list of (name, config) pairs.
    std::vector<std::pair<std::string, nlohmann::json>> entries;
    if (const auto it = root.find("mcpServers");
        it != root.end() && it->is_object()) {
        // Claude/VS Code wrapper: name = key.
        for (const auto& [k, v] : it->items()) entries.emplace_back(k, v);
    } else if (root.contains("command") || root.contains("url")) {
        // Bare single-server config: name from its "name" field.
        const std::string name =
            root.value("name", std::string{});
        entries.emplace_back(name, root);
    } else {
        // Single keyed object: { "name": { ...config... } }.
        for (const auto& [k, v] : root.items())
            if (v.is_object()) entries.emplace_back(k, v);
    }

    if (entries.empty())
        return std::unexpected("No MCP servers found in the JSON.");

    auto parse_one = [](const std::string& name, const nlohmann::json& cfg)
        -> std::expected<DaedalusMcpServer, std::string> {
        if (name.empty())
            return std::unexpected("A server entry is missing its name.");
        if (!cfg.is_object())
            return std::unexpected("Server \"" + name + "\" must be an object.");

        DaedalusMcpServer s;
        s.name = name;
        s.enabled = cfg.value("enabled", true);

        if (const auto u = cfg.find("url"); u != cfg.end() && u->is_string()) {
            s.transport = "streamable-http";
            s.url = u->get<std::string>();
            std::string tok = cfg.value("bearer_token", std::string{});
            if (tok.empty()) {
                if (const auto h = cfg.find("headers");
                    h != cfg.end() && h->is_object()) {
                    tok = h->value("Authorization", std::string{});
                    constexpr std::string_view prefix = "Bearer ";
                    if (tok.starts_with(prefix)) tok.erase(0, prefix.size());
                }
            }
            s.bearer_token = tok;
        } else if (const auto c = cfg.find("command");
                   c != cfg.end() && c->is_string()) {
            s.transport = "stdio";
            s.command = c->get<std::string>();
            if (const auto a = cfg.find("args"); a != cfg.end() && a->is_array())
                for (const auto& v : *a)
                    if (v.is_string()) s.args.push_back(v.get<std::string>());
            if (const auto e = cfg.find("env"); e != cfg.end() && e->is_object())
                for (const auto& [k, v] : e->items())
                    if (v.is_string()) s.env[k] = v.get<std::string>();
        } else {
            return std::unexpected(
                "Server \"" + name + "\" has neither \"command\" nor \"url\".");
        }
        return s;
    };

    std::vector<DaedalusMcpServer> out;
    for (const auto& [name, cfg] : entries) {
        auto s = parse_one(name, cfg);
        if (!s) return std::unexpected(s.error());
        out.push_back(std::move(*s));
    }
    return out;
}

} // namespace monkeybyte::mcp_bridge
