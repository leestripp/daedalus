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
//
// §B1 fix: every public method that takes a string argument now uses a
// prepared statement with `?` placeholders + `Statement::bind(...)`. No
// user-controlled value is ever concatenated into a SQL string.
//
// §B2 fix (v0.5.1): SettingsStore is no longer pimpl'd. The Impl fields
// (path_ + db_) are inlined into the class, making it copyable/movable.
// for_home() returns by value; the DbPool entry outlives all copies.

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

// ============================================================================
// Schema
// ============================================================================
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
    // Column names are constants — no user input flows here, raw execute()
    // is safe.
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

    // schema_version row. The constants here are integer literals — no
    // user input. Raw execute() is safe.
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
    try {
        m.max_context_size = std::stoi(r[5]);
        m.temperature      = std::stod(r[6]);
    } catch (...) {
        m.max_context_size = 0;
        m.temperature      = 0.0;
    }
    return m;
}

}  // anonymous namespace

// ============================================================================
// Prepared-statement helpers (§B1 fix)
// ============================================================================
//
// Every public method on SettingsStore that takes a string parameter
// routes through these helpers instead of concatenating into a SQL
// string. Bind accepts int, int, double, std::string, std::string_view
// — overloads on the argument type do the right thing.
//
// The fold expression `(stmt.bind(idx++, arg), ...)` expands left-to-right
// so idx increments cleanly across positions 1..N.
namespace {

template <typename... Args>
void bind_args(OmniNet::DB::Database::Statement& stmt, Args&&... args)
{
    int idx = 1;
    (stmt.bind(idx++, std::forward<Args>(args)), ...);
}

/// Execute a prepared statement (no result rows). Used for INSERT /
/// UPDATE / DELETE.
template <typename... Args>
void exec_prepared(OmniNet::DB::Database& db,
                   const std::string& sql,
                   Args&&... args)
{
    auto stmt = db.prepare(sql);
    bind_args(stmt, std::forward<Args>(args)...);
    stmt.step();  // INSERT/UPDATE/DELETE — discard the "no row" return.
}

/// Query first row via a prepared statement.
template <typename... Args>
std::optional<std::vector<std::string>>
query_one_prepared(OmniNet::DB::Database& db,
                   const std::string& sql,
                   Args&&... args)
{
    auto stmt = db.prepare(sql);
    bind_args(stmt, std::forward<Args>(args)...);
    if (!stmt.step()) return std::nullopt;
    std::vector<std::string> row;
    for (int i = 0; i < stmt.columnCount(); ++i) {
        row.push_back(stmt.columnText(i));
    }
    return row;
}

/// Query all rows via a prepared statement.
std::vector<std::vector<std::string>>
query_all_prepared(OmniNet::DB::Database& db, const std::string& sql)
{
    std::vector<std::vector<std::string>> rows;
    auto stmt = db.prepare(sql);
    while (stmt.step()) {
        std::vector<std::string> row;
        for (int i = 0; i < stmt.columnCount(); ++i) {
            row.push_back(stmt.columnText(i));
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

}  // anonymous namespace (prepared-statement helpers)

// ============================================================================
// Per-path singleton DB pool. One sqlite3 handle per unique db_path so
// a SettingsStore copy (or two for_home(home) calls) shares the same
// connection. Opens lazily on first use.
// ============================================================================
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

// ============================================================================
// SettingsStore ctor
// ============================================================================

// §B2 fix (v0.5.1): Impl fields (path_ + db_) are now inlined into the
// class. This constructor directly initialises those members instead of
// allocating a separate Impl struct.
//
// Note: the second constructor argument is kept for source compatibility
// but has no effect — auto-migration was removed in v0.5.2.
SettingsStore::SettingsStore(fs::path db_path, bool /*auto_migrate*/) {
    path_ = std::move(db_path);
    db_   = &DbPool::instance().get(path_);
}
// ============================================================================
// instance() / for_home()
// ============================================================================
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
    // v0.5.0: no auto-migration — the DB is the source of truth.
    // Populate it via the CLI or API before first use. (The migration
    // was a one-time event; there is no ongoing JSON→SQLite sync.)
    static SettingsStore s(default_db_path(), /*auto_migrate=*/false);
    return s;
}

SettingsStore SettingsStore::for_home(fs::path home)
{
    // §B2 fix (v0.5.1): return by value instead of leaking a heap allocation.
    //
    // The DbPool entry (the actual sqlite3 handle) lives for the process
    // duration. The returned SettingsStore wrapper is move-constructed from
    // the local temporary; when the caller's variable goes out of scope,
    // only the lightweight path_+db_ pair is destroyed — the DbPool entry
    // is untouched.
    //
    // auto_migrate=false: tests that call for_home() on temp paths must
    // not accidentally migrate user-data JSON files.
    fs::create_directories(home);
    return SettingsStore(home / "daedalus.db", /*auto_migrate=*/false);
}

// ============================================================================
// Read
// ============================================================================
std::optional<ModelConfig> SettingsStore::get_model(std::string_view name) const
{
    auto row = query_one_prepared(
        *db_,
        "SELECT name, host, api_key, model_id, chat_endpoint, "
        "max_context_size, temperature FROM models WHERE name = ?",
        std::string(name));
    if (!row.has_value()) return std::nullopt;
    return row_to_model(*row);
}

std::vector<ModelConfig> SettingsStore::list_models() const
{
    std::vector<ModelConfig> out;
    auto rows = query_all_prepared(
        *db_,
        "SELECT name, host, api_key, model_id, chat_endpoint, "
        "max_context_size, temperature FROM models");
    out.reserve(rows.size());
    for (const auto& row : rows) {
        out.push_back(row_to_model(row));
    }
    return out;
}

std::string SettingsStore::get_string(std::string_view key, std::string_view def) const
{
    auto row = query_one_prepared(
        *db_,
        "SELECT value FROM kv_settings WHERE key = ?",
        std::string(key));
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

// ============================================================================
// Write
// ============================================================================
void SettingsStore::upsert_model(const ModelConfig& m)
{
    exec_prepared(
        *db_,
        "INSERT INTO models(name, host, api_key, model_id, chat_endpoint, "
        "max_context_size, temperature) VALUES(?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(name) DO UPDATE SET "
        "host=excluded.host, api_key=excluded.api_key, "
        "model_id=excluded.model_id, chat_endpoint=excluded.chat_endpoint, "
        "max_context_size=excluded.max_context_size, "
        "temperature=excluded.temperature",
        m.name, m.host, m.api_key, m.model_id, m.chat_endpoint,
        static_cast<int64_t>(m.max_context_size),
        m.temperature);
}

void SettingsStore::delete_model(std::string_view name)
{
    exec_prepared(
        *db_,
        "DELETE FROM models WHERE name = ?",
        std::string(name));
}

void SettingsStore::set_string(std::string_view key, std::string_view value)
{
    exec_prepared(
        *db_,
        "INSERT INTO kv_settings(key, value) VALUES(?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
        std::string(key), std::string(value));
}

void SettingsStore::set_bool(std::string_view key, bool value)
{
    set_string(key, value ? "1" : "0");
}

void SettingsStore::set_int(std::string_view key, int value)
{
    set_string(key, std::to_string(value));
}

// ============================================================================
// Atomic
// ============================================================================
void SettingsStore::transaction(std::function<void(SettingsStore&)> fn)
{
    // OmniNet::DB::Database::transaction takes a no-arg lambda but our
    // API needs to pass `*this` so the callback can chain. Trampoline.
    SettingsStore& self = *this;
    db_->transaction([&self, &fn]() { fn(self); });
}

// ============================================================================
// Introspection
// ============================================================================
fs::path SettingsStore::db_path() const noexcept { return path_; }

bool SettingsStore::has_data() const noexcept
{
    // §fix (2026-07-18): SQLite prepare/step can throw; this function is
    // noexcept, so an escaping exception would call std::terminate.
    try {
        if (!list_models().empty()) return true;
        // Cheap kv_settings probe — one row, one column, no bindings.
        auto stmt = db_->prepare("SELECT 1 FROM kv_settings LIMIT 1");
        return stmt.step();
    } catch (...) {
        return false;
    }
}

// ============================================================================
// v0.3.5: per-project persistence (session memory + messages)
// ============================================================================
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
    exec_prepared(
        *db_,
        "DELETE FROM kv_settings WHERE key = ?",
        "mem." + std::string(session_id));
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
    exec_prepared(
        *db_,
        "DELETE FROM kv_settings WHERE key = ?",
        "msg." + std::string(session_id));
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
void SettingsStore::set_max_request_size(size_t v)         { set_int("max_request_size", static_cast<int>(v)); }
void SettingsStore::set_max_response_size(size_t v)        { set_int("max_response_size", static_cast<int>(v)); }
void SettingsStore::set_mcp_tool_schema_soft_cap_percent(int v) { set_int("mcp_tool_schema_soft_cap_percent", v); }

ServerErrorSettings SettingsStore::get_server_error_settings() const
{
    ServerErrorSettings s;
    s.enabled = get_bool("server_error_enabled", s.enabled);
    s.initial_wait_seconds = get_int("server_error_initial_wait_seconds", s.initial_wait_seconds);
    s.max_wait_seconds = get_int("server_error_max_wait_seconds", s.max_wait_seconds);
    // §B24 fix: stod on user-provided string can throw — guard it.
    try {
        s.backoff_multiplier = std::stod(
            get_string("server_error_backoff_multiplier",
                       std::to_string(s.backoff_multiplier)));
    } catch (...) {
        // Leave default value in place — settings were corrupt / unparseable.
    }
    s.max_retries_per_call = get_int("server_error_max_retries_per_call", s.max_retries_per_call);
    return s;
}

// ============================================================================
// v0.5.0: Serper.dev API key (serper_config table, singleton row id=1)
// ============================================================================

std::string SettingsStore::get_serper_api_key() const {
    if (!db_) return {};
    try {
        // Singleton row (id=1); if missing, return empty.
        auto result = query_one_prepared(*db_,
            "SELECT api_key FROM serper_config WHERE id = 1");
        if (!result || result->empty()) return {};
        return (*result)[0];
    } catch (const std::exception& e) {
        LOG_WARNING("SettingsStore::get_serper_api_key failed: "
                    + std::string(e.what()));
        return {};
    }
}

void SettingsStore::set_serper_api_key(std::string_view api_key) {
    if (!db_) return;
    try {
        exec_prepared(*db_,
            "INSERT INTO serper_config(id, api_key) VALUES (1, ?) "
            "ON CONFLICT(id) DO UPDATE SET api_key = excluded.api_key",
            std::string(api_key));
    } catch (const std::exception& e) {
        LOG_WARNING("SettingsStore::set_serper_api_key failed: "
                    + std::string(e.what()));
    }
}

// ============================================================================
// v0.5.0: MCP server registry
// ============================================================================
//
// These accessors store McpServer entries in the `mcp_servers` table.
// Hosts (or McpServerManager) call list_mcp_servers() on startup to
// load the user's configured MCP servers, replacing the old JSON file
// path. The one-shot JSON→SQLite migration is handled by daedalus-cli's
// --migrate-data flag (see src/tools/migrate.cpp).

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
    auto row = query_one_prepared(
        *db_,
        "SELECT name, transport, command, args_json, env_json, url, "
        "bearer_token, description, enabled FROM mcp_servers WHERE name = ?",
        std::string(name));
    if (!row.has_value()) return std::nullopt;
    return row_to_mcp(*row);
}

std::vector<SettingsStore::McpServer> SettingsStore::list_mcp_servers() const
{
    std::vector<McpServer> out;
    auto rows = query_all_prepared(
        *db_,
        "SELECT name, transport, command, args_json, env_json, url, "
        "bearer_token, description, enabled FROM mcp_servers ORDER BY name");
    out.reserve(rows.size());
    for (const auto& row : rows) {
        out.push_back(row_to_mcp(row));
    }
    return out;
}

void SettingsStore::upsert_mcp_server(const McpServer& s)
{
    nlohmann::json args_j = s.args;
    nlohmann::json env_j  = s.env;
    exec_prepared(
        *db_,
        "INSERT INTO mcp_servers(name, transport, command, args_json, env_json, "
        "url, bearer_token, description, enabled) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(name) DO UPDATE SET "
        "transport=excluded.transport, command=excluded.command, "
        "args_json=excluded.args_json, env_json=excluded.env_json, "
        "url=excluded.url, bearer_token=excluded.bearer_token, "
        "description=excluded.description, enabled=excluded.enabled",
        s.name, s.transport, s.command,
        args_j.dump(), env_j.dump(),
        s.url, s.bearer_token, s.description,
        static_cast<int>(s.enabled));
}

void SettingsStore::delete_mcp_server(std::string_view name)
{
    exec_prepared(
        *db_,
        "DELETE FROM mcp_servers WHERE name = ?",
        std::string(name));
}

bool SettingsStore::has_mcp_servers() const noexcept
{
    // §fix (2026-07-18): SQLite prepare/step can throw; this function is
    // noexcept, so an escaping exception would call std::terminate.
    try {
        auto stmt = db_->prepare("SELECT 1 FROM mcp_servers LIMIT 1");
        return stmt.step();
    } catch (...) {
        return false;
    }
}

} // namespace daedalus
