// SPDX-License-Identifier: BSL-1.0
// src/core/SettingsMigrator.cpp
//
// One-shot JSON → SQLite migrator. Reads the legacy JSON config files
// and writes them into the new SettingsStore (SQLite). Idempotent —
// running it twice is a no-op (skipped because the SQLite already has data).
//
// Migrated sources (in priority order):
//   1. `~/.config/daedalus/config.json`        → models + kv_settings
//   2. `~/.config/daedalus/mcp/servers.json`    → mcp_servers
//
// NOT migrated (file-based by design):
//   - `~/.local/share/daedalus/skills/`        → already file-based
//   - `<project>/.daedalus*/sessions/*.json`   → moved in v0.3.5
//   - `<project>/.daedalus*/messages.json`     → moved in v0.3.5
//
// Behaviour:
//   - Auto-runs once on first SettingsStore access when the SQLite
//     store is empty AND legacy JSON exists. Marks migrated files
//     with a `.migrated` suffix so the user can see what happened.
//   - The CLI's existing `--migrate-data` flag invokes this explicitly.
//   - Failure to migrate is non-fatal — the JSON fallback still works.

#include "SettingsMigrator.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

#include <nlohmann/json.hpp>

#include "daedalus/paths.h"
#include "daedalus/settings.h"
#include "core/Logger.h"

// Helper macro for consistent logging through the Logger.
#define LOG_CERR(level, expr) do {                          \
        std::ostringstream _daedalus_msg;                   \
        _daedalus_msg << expr;                               \
        LOG_##level(_daedalus_msg.str());                    \
    } while (0)

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace daedalus {

namespace {

// Rename <file> to <file>.migrated. If the target already exists, just
// leave the source in place (don't clobber a previous migration backup).
void mark_migrated(const fs::path& file)
{
    if (!fs::exists(file)) return;
    fs::path target = file.string() + ".migrated";
    if (fs::exists(target)) return;        // don't overwrite older backup
    std::error_code ec;
    fs::rename(file, target, ec);
    if (ec) {
        LOG_CERR(WARNING, "[Migrator] Could not rename "
                  << file.string() << " → " << target.string()
                  << " (" << ec.message() << ")");
    }
}

// Default paths (when caller passes empty paths) — these are what the
// auto-migration machinery uses in production.
fs::path default_config_json_path()
{
    return Paths::global().user_config_root() / "config.json";
}

fs::path default_mcp_servers_path()
{
    return Paths::global().user_config_root() / "mcp" / "servers.json";
}

// ===========================================================================
// config.json → models + kv_settings
// ===========================================================================
int migrate_config_json(const fs::path& src, SettingsStore& store)
{
    std::ifstream in(src);
    if (!in.is_open()) return 0;

    json j;
    try {
        in >> j;
    } catch (const json::parse_error& e) {
        LOG_CERR(ERROR, "[Migrator] Failed to parse "
                  << src.string() << ": " << e.what());
        return 0;
    }

    int count = 0;

    // Scalar kv_settings: keys we know about and care about.
    // Anything else stays in the (now .migrated) JSON for the user to read.
    // JSON key → store key. The store uses "default_model_name" to avoid
    // collision with the models table.
    static const std::map<std::string, std::string> kKvKeys = {
        {"default_model",         "default_model_name"},
        {"auto_approve",          "auto_approve"},
        {"max_agent_iterations",  "max_agent_iterations"},
        {"max_request_size",      "max_request_size"},
        {"max_response_size",     "max_response_size"},
        {"max_tool_result_size",  "max_tool_result_size"},
    };
    for (const auto& [json_key, store_key] : kKvKeys) {
        if (!j.contains(json_key)) continue;
        const auto& v = j[json_key];
        if (v.is_boolean()) {
            store.set_bool(store_key, v.get<bool>());
            ++count;
        } else if (v.is_number_integer()) {
            store.set_int(store_key, v.get<int>());
            ++count;
        } else if (v.is_number_float()) {
            // store.set_double doesn't exist yet — store as int via truncation
            store.set_int(store_key, static_cast<int>(v.get<double>()));
            ++count;
        } else if (v.is_string()) {
            store.set_string(store_key, v.get<std::string>());
            ++count;
        }
    }

    // Models block
    if (j.contains("models") && j["models"].is_object()) {
        for (auto& [model_name, model_data] : j["models"].items()) {
            ModelConfig m;
            m.name             = model_name;
            m.host             = model_data.value("host", "");
            m.api_key          = model_data.value("api_key", "");
            m.model_id         = model_data.value("model", model_name);
            m.chat_endpoint    = "/v1/chat/completions";
            m.max_context_size = model_data.value("max_context_size", 0);
            m.temperature      = model_data.value("temperature", 0.0);
            if (model_data.contains("endpoints") &&
                model_data["endpoints"].is_object() &&
                model_data["endpoints"].contains("chat")) {
                m.chat_endpoint = model_data["endpoints"]["chat"]
                                      .get<std::string>();
            }
            // Skip placeholder api_keys — they'd fail validation later.
            if (m.api_key.empty() || m.api_key == "YOUR_API_KEY_HERE") {
                LOG_CERR(WARNING, "[Migrator] Skipping model '"
                          << m.name << "' (placeholder api_key)");
                continue;
            }
            store.upsert_model(m);
            ++count;
        }
    }

    // serper-search block — also lives inside config.json
    if (j.contains("serper-search") && j["serper-search"].is_object()) {
        auto api_key = j["serper-search"].value("api_key", "");
        if (!api_key.empty() && api_key != "YOUR_SERPER_API_KEY") {
            // Store in kv_settings for v0.3.0; SerperConfig.cpp can also
            // be migrated to read from here in a future release.
            store.set_string("serper.api_key", api_key);
            ++count;
        }
    }

    return count;
}

// ===========================================================================
// mcp/servers.json → mcp_servers
// ===========================================================================
// mcp/servers.json → mcp_servers (v0.5.0)
// ===========================================================================
int migrate_mcp_servers_json(const fs::path& src, SettingsStore& store)
{
    std::ifstream in(src);
    if (!in.is_open()) return 0;

    json j;
    try {
        in >> j;
    } catch (const json::parse_error& e) {
        LOG_CERR(ERROR, "[Migrator] Failed to parse "
                  << src.string() << ": " << e.what());
        return 0;
    }

    int count = 0;

    // Two top-level shapes are accepted: {"mcpServers": {...}} (the
    // current shape) and {"servers": [...]}.
    auto try_ingest = [&](const json& servers) {
        if (!servers.is_object()) return;
        for (auto& [name, cfg] : servers.items()) {
            SettingsStore::McpServer s;
            s.name        = name;
            s.transport   = cfg.value("transport", "stdio");
            s.command     = cfg.value("command", "");
            s.url         = cfg.value("url", "");
            s.description = cfg.value("description", "");
            s.enabled     = cfg.value("enabled", true);
            // bearer_token: accept snake_case or camelCase
            if (cfg.contains("bearer_token") && cfg["bearer_token"].is_string()) {
                s.bearer_token = cfg["bearer_token"].get<std::string>();
            } else if (cfg.contains("bearerToken") && cfg["bearerToken"].is_string()) {
                s.bearer_token = cfg["bearerToken"].get<std::string>();
            }
            // args: JSON array of strings
            if (cfg.contains("args") && cfg["args"].is_array()) {
                for (const auto& a : cfg["args"]) {
                    if (a.is_string()) s.args.push_back(a.get<std::string>());
                }
            }
            // env: JSON object of string→string
            if (cfg.contains("env") && cfg["env"].is_object()) {
                for (auto it = cfg["env"].begin(); it != cfg["env"].end(); ++it) {
                    if (it.value().is_string()) {
                        s.env[it.key()] = it.value().get<std::string>();
                    }
                }
            }
            store.upsert_mcp_server(s);
            ++count;
        }
    };

    if (j.contains("mcpServers")) try_ingest(j["mcpServers"]);
    else if (j.contains("servers")) try_ingest(j["servers"]);

    return count;
}

}  // namespace

// ===========================================================================
// Public API
// ===========================================================================

bool SettingsMigrator::needs_migration(const SettingsStore& store,
                                       const fs::path& config_json_path,
                                       const fs::path& mcp_servers_path)
{
    // If the store already has any models or kv entries, we're done.
    // (No migration is needed — the store is the source of truth.)
    if (!store.list_models().empty())         return false;
    // §v0.5.0: The store uses "default_model_name" as the kv_settings key.
    if (!store.get_string("default_model_name", "").empty()) return false;
    // §v0.5.0: presence of MCP servers also means we're past the
    // JSON→SQLite cutover.
    if (store.has_mcp_servers()) return false;
    auto cfg = config_json_path.empty() ? default_config_json_path() : config_json_path;
    if (fs::exists(cfg)) return true;
    auto mcp = mcp_servers_path.empty() ? default_mcp_servers_path() : mcp_servers_path;
    return fs::exists(mcp);
}

MigrateReport SettingsMigrator::run(SettingsStore& store,
                                    const fs::path& config_json_path,
                                    const fs::path& mcp_servers_path,
                                    bool mark_files)
{
    MigrateReport r;

    if (!needs_migration(store, config_json_path, mcp_servers_path)) {
        r.skipped = true;
        r.reason  = "store already populated or no legacy JSON found";
        return r;
    }

    // config.json
    auto cfg_path = config_json_path.empty()
                        ? default_config_json_path()
                        : config_json_path;
    if (fs::exists(cfg_path)) {
        r.items_migrated += migrate_config_json(cfg_path, store);
        r.files_touched.push_back(cfg_path);
        if (mark_files) mark_migrated(cfg_path);
    }

    // mcp/servers.json
    auto mcp_path = mcp_servers_path.empty()
                        ? default_mcp_servers_path()
                        : mcp_servers_path;
    if (fs::exists(mcp_path)) {
        r.items_migrated += migrate_mcp_servers_json(mcp_path, store);
        r.files_touched.push_back(mcp_path);
        if (mark_files) mark_migrated(mcp_path);
    }

    return r;
}

}  // namespace daedalus