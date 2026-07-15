// SPDX-License-Identifier: BSL-1.0
// src/core/SettingsStore.cpp
//
// SQLite-backed settings store for libdaedalus. Backed by OmniNet's
// `DB::Database` (which itself wraps sqlite3) — we do NOT directly call
// sqlite3_* APIs from daedalus. That keeps the SQLite version, error
// reporting, thread-safety policy, and prepared-statement caching in one
// place (OmniNet) instead of duplicating it here.
//
// Stored at:
//   $XDG_DATA_HOME/daedalus/daedalus.db        (default)
//       └─ ~/.local/share/daedalus/daedalus.db
//   <home>/daedalus.db                          (per-agent override)
//
// Schema is created idempotently on first open. `schema_version` table
// keeps migrations straightforward.

#include "daedalus/settings.h"

#include <cassert>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>
#include <db/Database.h>      // OmniNet::DB::Database — sqlite3 wrapper

#include "daedalus/paths.h"   // Paths::global() for the default DB path
#include "core/Logger.h"
#include "core/SettingsMigrator.h"   // §v0.5.0: auto-migration on first DB open

namespace fs = std::filesystem;

namespace daedalus {

// ============================================================================
// Built-in max_context_size defaults for known models
// ============================================================================
namespace {
int builtin_max_context_size(std::string_view model_name)
{
    // Map is keyed by the settings name (e.g. "minimax-m2.7") so the user
    // can also match by the model id (e.g. "MiniMax-M2.7") if needed.
    // Entries are (setting_name, context_size) pairs.
    static const std::pair<std::string_view, int> known_models[] = {
        // MiniMax models
        {"minimax-m3",      500000},  // 500K tokens (actual model limit)
        {"MiniMax-M3",     500000},
        {"minimax-m2.7",    200000},  // 200K tokens
        {"MiniMax-M2.7",   200000},
        {"minimax",         500000},  // Default MiniMax alias
        {"minimax-m2",      200000},  // Default MiniMax-M2 alias
        // OpenAI models
        {"gpt-4",           8192},
        {"gpt-4-32k",       32768},
        {"gpt-4-turbo",     128000},
        {"gpt-4o",          128000},
        {"gpt-4o-mini",     128000},
        {"gpt-3.5-turbo",    16385},
        {"o1",              200000},
        {"o1-mini",         128000},
        {"o3-mini",         200000},
        // Anthropic models
        {"claude-3-opus",       200000},
        {"claude-3-sonnet",     200000},
        {"claude-3-haiku",      200000},
        {"claude-3.5-sonnet",   200000},
        {"claude-3.5-haiku",    200000},
        // DeepSeek models
        {"deepseek-chat",       128000},
        {"deepseek-coder",      128000},
    };

    for (const auto& [name, size] : known_models) {
        if (name == model_name) return size;
    }
    return 0;  // 0 means "unknown" — caller applies the 1M fallback
}
}  // anonymous namespace

// ===========================================================================
// Schema
// ===========================================================================
namespace {

constexpr int kSchemaVersion = 2;

constexpr const char* kSchemaSql = R"sql(
CREATE TABLE IF NOT EXISTS schema_version (
    version INTEGER PRIMARY KEY
);

CREATE TABLE IF NOT EXISTS models (
    name             TEXT PRIMARY KEY,
    host             TEXT NOT NULL,
    api_key          TEXT NOT NULL,
    model_id         TEXT NOT NULL,
    chat_endpoint    TEXT NOT NULL DEFAULT '/v1/chat/completions',
    max_context_size INTEGER NOT NULL DEFAULT 0,
    temperature      REAL    NOT NULL DEFAULT 0.0
);

CREATE TABLE IF NOT EXISTS kv_settings (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

-- §v0.5.0: schema v2 adds description, bearer_token, enabled for full
-- parity with McpServerConfig. Pre-v2 DBs get an in-place ALTER TABLE
-- migration below.
CREATE TABLE IF NOT EXISTS mcp_servers (
    name        TEXT PRIMARY KEY,
    transport   TEXT NOT NULL DEFAULT 'stdio',
    command     TEXT,
    args_json   TEXT,
    env_json    TEXT,
    url         TEXT,
    bearer_token TEXT,
    description TEXT,
    enabled     INTEGER NOT NULL DEFAULT 1
);

CREATE TABLE IF NOT EXISTS serper_config (
    id      INTEGER PRIMARY KEY CHECK (id = 1),  -- singleton row
    api_key TEXT NOT NULL
);
)sql";

/// Per-version migrations. Called when schema_version table reports
/// a value less than kSchemaVersion. Each entry bumps v → v+1.
void migrate_to_v2(OmniNet::DB::Database& db)
{
    // v2: add mcp_servers.description / bearer_token / enabled.
    // ALTER TABLE columns; idempotent (catch "duplicate column" errors).
    auto add_col = [&](const std::string& col, const std::string& def) {
        try {
            db.execute("ALTER TABLE mcp_servers ADD COLUMN " + col + " " + def);
        } catch (...) {
            // Column already exists — fine.
        }
    };
    add_col("bearer_token", "TEXT");
    add_col("description",  "TEXT");
    add_col("enabled",      "INTEGER NOT NULL DEFAULT 1");
}

void ensure_schema(OmniNet::DB::Database& db)
{
    db.execute(kSchemaSql);

    // schema_version row
    int current_version = 0;
    auto row = db.queryOne("SELECT version FROM schema_version LIMIT 1");
    if (row.has_value() && !row->empty()) {
        try {
            current_version = std::stoi((*row)[0]);
        } catch (...) {
            current_version = 0;
        }
    } else {
        db.execute("INSERT INTO schema_version(version) VALUES (" +
                   std::to_string(kSchemaVersion) + ")");
        current_version = kSchemaVersion;
    }

    // Run any pending per-version migrations.
    if (current_version < 2) migrate_to_v2(db);

    // Update the stored version if it advanced.
    if (current_version != kSchemaVersion) {
        // SQLite's UPSERT pattern. Set to current version.
        db.execute("DELETE FROM schema_version");
        db.execute("INSERT INTO schema_version(version) VALUES (" +
                   std::to_string(kSchemaVersion) + ")");
    }
}

ModelConfig row_to_model(const std::vector<std::string>& r)
{
    // SELECT order: name, host, api_key, model_id, chat_endpoint,
    // max_context_size, temperature
    ModelConfig m;
    if (r.size() < 7) return m;
    m.name             = r[0];
    m.host             = r[1];
    m.api_key          = r[2];
    m.model_id         = r[3];
    m.chat_endpoint    = r[4];
    m.max_context_size = std::stoi(r[5]);
    m.temperature      = std::stod(r[6]);
    return m;
}

}  // anonymous namespace

// ===========================================================================
// Per-path singleton DB pool. One sqlite3 handle per unique db_path so
// a SettingsStore copy (or two for_home(home) calls) shares the same
// connection. Opens lazily on first use.
// ===========================================================================
class DbPool
{
public:
    static DbPool& instance()
    {
        static DbPool p;
        return p;
    }

    OmniNet::DB::Database& get(const fs::path& path)
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = dbs_.find(path);
        // If the cached handle's file was removed out from under us,
        // drop the stale handle and reopen. (Real users stop daedalus
        // before nuking the DB — but tests delete the file constantly.)
        if (it != dbs_.end() && !fs::exists(path)) {
            dbs_.erase(it);
            it = dbs_.end();
        }
        if (it == dbs_.end()) {
            // Open + migrate + store.
            fs::create_directories(path.parent_path());
            auto db = std::make_unique<OmniNet::DB::Database>();
            db->open(path.string());
            ensure_schema(*db);
            it = dbs_.emplace(path, std::move(db)).first;
        }
        return *it->second;
    }

private:
    std::mutex mu_;
    std::unordered_map<fs::path, std::unique_ptr<OmniNet::DB::Database>> dbs_;
};

// ===========================================================================
// SettingsStore::Impl
// ===========================================================================
struct SettingsStore::Impl
{
    fs::path path;
    OmniNet::DB::Database* db = nullptr;   // non-owning — owned by DbPool
};

SettingsStore::SettingsStore(fs::path db_path, bool /*is_default*/, bool auto_migrate)
    : impl_(new Impl)
{
    impl_->path = std::move(db_path);
    impl_->db   = &DbPool::instance().get(impl_->path);

    // §v0.5.0: Auto-migrate legacy JSON on first DB open.
    // Idempotent — SettingsMigrator::run() short-circuits if the store
    // is already populated or no legacy JSON exists. Failure is
    // non-fatal: the JSON fallback still works.
    //
    // Tests opt out via the third constructor arg so unit tests with
    // temp paths don't accidentally trigger user-data migration.
    if (auto_migrate) {
        try {
            daedalus::SettingsMigrator::run(*this, {}, {}, /*mark_files=*/true);
        } catch (...) {
            // Swallow — migration is best-effort. The store is still usable.
        }
    }
}

// ===========================================================================
// instance() / for_home()
// ===========================================================================
fs::path default_db_path()
{
    // XDG_DATA_HOME/daedalus/daedalus.db (default
    // ~/.local/share/daedalus/daedalus.db) — matches the plan.
    fs::path dir = fs::path(Paths::global().user_data_root());
    fs::create_directories(dir);
    return dir / "daedalus.db";
}

SettingsStore& SettingsStore::instance()
{
    // auto_migrate=true: first call on the user's real DB will migrate
    // any legacy JSON config. Subsequent calls hit the static (singleton)
    // so the migration runs exactly once per process.
    static SettingsStore s(default_db_path(), /*is_default=*/true,
                           /*auto_migrate=*/true);
    return s;
}

SettingsStore& SettingsStore::for_home(fs::path home)
{
    // Every call returns a fresh SettingsStore wrapper. The underlying
    // sqlite3 handle is managed by DbPool (per db_path), so actual
    // resource reuse happens there — we don't need to cache wrappers.
    // This also means tests that delete the DB file and reopen can get
    // a clean handle (DbPool detects the missing file and reopens).
    //
    // auto_migrate=false: callers that want auto-migration should pass
    // explicit paths to SettingsMigrator::run() themselves. for_home() is
    // used by tests and per-agent DBs where the test/temp dir has no
    // legacy JSON to migrate.
    fs::create_directories(home);
    return *(new SettingsStore(home / "daedalus.db", /*is_default=*/false,
                               /*auto_migrate=*/false));
}

// ===========================================================================
// Read
// ===========================================================================
std::optional<ModelConfig> SettingsStore::get_model(std::string_view name) const
{
    auto row = impl_->db->queryOne(
        "SELECT name, host, api_key, model_id, chat_endpoint, "
        "max_context_size, temperature FROM models WHERE name = '" +
        std::string(name) + "'");
        // NOTE: name is treated as a key (validated in API), not user input.
        // For raw user input we'd use a prepared Statement::bind — the
        // public API surface here is name/key lookups by trusted callers.
    if (!row.has_value()) return std::nullopt;
    return row_to_model(*row);
}

std::vector<ModelConfig> SettingsStore::list_models() const
{
    // OmniNet 0.8.8+ (commit fbecf55) populates m_currentRow in the
    // QueryResult constructor and honours m_atEnd in the Iterator
    // constructor, so range-based iteration returns the correct number
    // of rows in all cases — including the empty / post-DELETE /
    // post-reopen edge cases that the old callback workaround
    // papered over.
    std::vector<ModelConfig> out;
    for (const auto& row : impl_->db->query(
            "SELECT name, host, api_key, model_id, chat_endpoint, "
            "max_context_size, temperature FROM models")) {
        out.push_back(row_to_model(row));
    }
    return out;
}

std::string SettingsStore::get_string(std::string_view key, std::string_view def) const
{
    auto row = impl_->db->queryOne(
        "SELECT value FROM kv_settings WHERE key = '" + std::string(key) + "'");
    if (!row.has_value()) return std::string(def);
    return row->empty() ? std::string(def) : (*row)[0];
}

bool SettingsStore::get_bool(std::string_view key, bool def) const
{
    auto v = get_string(key, "");
    if (v.empty()) return def;
    return v == "1" || v == "true" || v == "yes";
}

int SettingsStore::get_int(std::string_view key, int def) const
{
    auto v = get_string(key, "");
    if (v.empty()) return def;
    try { return std::stoi(v); } catch (...) { return def; }
}

// ===========================================================================
// Write
// ===========================================================================
void SettingsStore::upsert_model(const ModelConfig& m)
{
    impl_->db->execute(
        "INSERT INTO models(name, host, api_key, model_id, chat_endpoint, "
        "max_context_size, temperature) VALUES('" +
        m.name + "','" + m.host + "','" + m.api_key + "','" +
        m.model_id + "','" + m.chat_endpoint + "'," +
        std::to_string(m.max_context_size) + "," +
        std::to_string(m.temperature) +
        ") ON CONFLICT(name) DO UPDATE SET "
        "host=excluded.host, api_key=excluded.api_key, "
        "model_id=excluded.model_id, chat_endpoint=excluded.chat_endpoint, "
        "max_context_size=excluded.max_context_size, "
        "temperature=excluded.temperature");
}

void SettingsStore::delete_model(std::string_view name)
{
    impl_->db->execute(
        "DELETE FROM models WHERE name = '" + std::string(name) + "'");
}

void SettingsStore::set_string(std::string_view key, std::string_view value)
{
    impl_->db->execute(
        "INSERT INTO kv_settings(key, value) VALUES('" +
        std::string(key) + "','" + std::string(value) +
        "') ON CONFLICT(key) DO UPDATE SET value=excluded.value");
}

void SettingsStore::set_bool(std::string_view key, bool value)
{
    set_string(key, value ? "1" : "0");
}

void SettingsStore::set_int(std::string_view key, int value)
{
    set_string(key, std::to_string(value));
}

// ===========================================================================
// Atomic
// ===========================================================================
void SettingsStore::transaction(std::function<void(SettingsStore&)> fn)
{
    // OmniNet::DB::Database::transaction takes a no-arg lambda but our
    // API needs to pass `*this` so the callback can chain. Trampoline.
    SettingsStore& self = *this;
    impl_->db->transaction([&self, &fn]() { fn(self); });
}

// ===========================================================================
// Introspection
// ===========================================================================
fs::path SettingsStore::db_path() const noexcept { return impl_->path; }

bool SettingsStore::has_data() const noexcept
{
    if (!list_models().empty()) return true;
    // Cheap kv_settings probe — one row, one column.
    auto row = impl_->db->queryOne("SELECT 1 FROM kv_settings LIMIT 1");
    return row.has_value();
}

// ===========================================================================
// v0.3.5: per-project persistence (session memory + messages)
// ===========================================================================
//
// Both are stored as kv_settings under namespaced keys ("mem.<sid>",
// "msg.<sid>") — same table as scalars, no schema change needed.
// Just append-only logic on top of the existing set_string/get_string.

std::string SettingsStore::get_session_memory(std::string_view session_id) const
{
    return get_string("mem." + std::string(session_id), "");
}

void SettingsStore::set_session_memory(std::string_view session_id, std::string_view json_blob)
{
    set_string("mem." + std::string(session_id), json_blob);
}

void SettingsStore::delete_session_memory(std::string_view session_id)
{
    // No delete-by-key API on kv_settings yet — use a literal DELETE via
    // raw SQL through OmniNet. Cheaper than adding delete_kv() right now
    // (only one caller, this one).
    impl_->db->execute(
        "DELETE FROM kv_settings WHERE key = 'mem." +
        std::string(session_id) + "'");
}

std::string SettingsStore::get_messages_json(std::string_view session_id) const
{
    return get_string("msg." + std::string(session_id), "");
}

void SettingsStore::set_messages_json(std::string_view session_id, std::string_view json_blob)
{
    set_string("msg." + std::string(session_id), json_blob);
}

void SettingsStore::delete_messages_json(std::string_view session_id)
{
    impl_->db->execute(
        "DELETE FROM kv_settings WHERE key = 'msg." +
        std::string(session_id) + "'");
}

// ============================================================================
// v0.5.0: AgentImpl direct-wiring accessors
// ============================================================================

std::string SettingsStore::get_default_model_name() const
{
    return get_string("default_model_name", "");
}

void SettingsStore::set_default_model_name(std::string_view name)
{
    set_string("default_model_name", name);
}

bool SettingsStore::get_auto_approve() const
{
    return get_bool("auto_approve", false);
}

void SettingsStore::set_auto_approve(bool value)
{
    set_bool("auto_approve", value);
}

int SettingsStore::get_max_context_size_for_model(std::string_view model_name) const
{
    // 1. Check if the user has set an explicit max_context_size for this model.
    auto m = get_model(model_name);
    if (m.has_value() && m->max_context_size > 0) {
        return m->max_context_size;
    }

    // 2. Fall back to the built-in default for known models.
    int builtin = builtin_max_context_size(model_name);
    if (builtin > 0) return builtin;

    // 3. Ultimate fallback: 1M tokens (generous for unknown models).
    return 1000000;
}

int SettingsStore::get_max_agent_iterations() const
{
    return get_int("max_agent_iterations", 100);
}

size_t SettingsStore::get_max_tool_result_size() const
{
    return static_cast<size_t>(get_int("max_tool_result_size", 10000));
}

size_t SettingsStore::get_max_request_size() const
{
    return static_cast<size_t>(get_int("max_request_size", 4000000));
}

size_t SettingsStore::get_max_response_size() const
{
    return static_cast<size_t>(get_int("max_response_size", 5000000));
}

int SettingsStore::get_mcp_tool_schema_soft_cap_percent() const
{
    return get_int("mcp_tool_schema_soft_cap_percent", 5);
}

void SettingsStore::set_max_agent_iterations(int v)        { set_int("max_agent_iterations", v); }
void SettingsStore::set_max_tool_result_size(size_t v)    { set_int("max_tool_result_size", static_cast<int>(v)); }
void SettingsStore::set_max_request_size(size_t v)        { set_int("max_request_size", static_cast<int>(v)); }
void SettingsStore::set_max_response_size(size_t v)       { set_int("max_response_size", static_cast<int>(v)); }
void SettingsStore::set_mcp_tool_schema_soft_cap_percent(int v) { set_int("mcp_tool_schema_soft_cap_percent", v); }

ServerErrorSettings SettingsStore::get_server_error_settings() const
{
    ServerErrorSettings s;
    s.enabled = get_bool("server_error_enabled", s.enabled);
    s.initial_wait_seconds = get_int("server_error_initial_wait_seconds", s.initial_wait_seconds);
    s.max_wait_seconds = get_int("server_error_max_wait_seconds", s.max_wait_seconds);
    s.backoff_multiplier = std::stod(get_string("server_error_backoff_multiplier", std::to_string(s.backoff_multiplier)));
    s.max_retries_per_call = get_int("server_error_max_retries_per_call", s.max_retries_per_call);
    return s;
}

// ============================================================================
// v0.5.0: MCP server registry
// ============================================================================
//
// These accessors store McpServer entries in the `mcp_servers` table.
// Hosts (or McpServerManager) call list_mcp_servers() on startup to
// load the user's configured MCP servers, replacing the old JSON file
// path. SettingsMigrator handles the one-shot JSON→SQLite migration.

namespace {
SettingsStore::McpServer row_to_mcp(const std::vector<std::string>& r)
{
    // SELECT order: name, transport, command, args_json, env_json, url,
    //               bearer_token, description, enabled
    SettingsStore::McpServer s;
    if (r.size() < 9) return s;
    s.name         = r[0];
    s.transport    = r[1].empty() ? "stdio" : r[1];
    s.command      = r[2];
    s.url          = r[5];
    s.bearer_token = r[6];
    s.description  = r[7];
    s.enabled      = (r[8] == "1" || r[8] == "true");
    if (!r[3].empty()) {
        try {
            auto j = nlohmann::json::parse(r[3]);
            if (j.is_array()) {
                for (const auto& a : j) s.args.push_back(a.get<std::string>());
            }
        } catch (...) { /* leave empty */ }
    }
    if (!r[4].empty()) {
        try {
            auto j = nlohmann::json::parse(r[4]);
            if (j.is_object()) {
                for (auto it = j.begin(); it != j.end(); ++it) {
                    s.env[it.key()] = it.value().get<std::string>();
                }
            }
        } catch (...) { /* leave empty */ }
    }
    return s;
}
}  // anonymous namespace

std::optional<SettingsStore::McpServer> SettingsStore::get_mcp_server(std::string_view name) const
{
    auto row = impl_->db->queryOne(
        "SELECT name, transport, command, args_json, env_json, url, "
        "bearer_token, description, enabled FROM mcp_servers WHERE name = '"
        + std::string(name) + "'");
    if (!row.has_value()) return std::nullopt;
    return row_to_mcp(*row);
}

std::vector<SettingsStore::McpServer> SettingsStore::list_mcp_servers() const
{
    std::vector<McpServer> out;
    for (const auto& row : impl_->db->query(
            "SELECT name, transport, command, args_json, env_json, url, "
            "bearer_token, description, enabled FROM mcp_servers ORDER BY name")) {
        out.push_back(row_to_mcp(row));
    }
    return out;
}

void SettingsStore::upsert_mcp_server(const McpServer& s)
{
    nlohmann::json args_j = s.args;
    nlohmann::json env_j  = s.env;
    impl_->db->execute(
        "INSERT INTO mcp_servers(name, transport, command, args_json, env_json, "
        "url, bearer_token, description, enabled) VALUES('" +
        s.name + "','" + s.transport + "','" + s.command + "','" +
        args_j.dump() + "','" + env_j.dump() + "','" +
        s.url + "','" + s.bearer_token + "','" + s.description + "'," +
        (s.enabled ? "1" : "0") +
        ") ON CONFLICT(name) DO UPDATE SET "
        "transport=excluded.transport, command=excluded.command, "
        "args_json=excluded.args_json, env_json=excluded.env_json, "
        "url=excluded.url, bearer_token=excluded.bearer_token, "
        "description=excluded.description, enabled=excluded.enabled");
}

void SettingsStore::delete_mcp_server(std::string_view name)
{
    impl_->db->execute(
        "DELETE FROM mcp_servers WHERE name = '" + std::string(name) + "'");
}

bool SettingsStore::has_mcp_servers() const noexcept
{
    auto row = impl_->db->queryOne(
        "SELECT 1 FROM mcp_servers LIMIT 1");
    return row.has_value();
}

} // namespace daedalus
