// SPDX-License-Identifier: BSL-1.0
// include/daedalus/settings.h
//
// Public, opaque settings API for libdaedalus.
//
// What this replaces (v0.3.0+):
//   - `~/.config/daedalus/config.json`        → `models` + `kv_settings`
//   - `~/.config/daedalus/mcp/servers.json`    → `mcp_servers`
//   - `~/.config/daedalus/mcp/.env`            → `kv_settings`
//   - `serper-search.api_key` in config.json   → `serper_config`
//
// What this DOES NOT touch:
//   - Skills subsystem (file-based by design — editable, version-controllable)
//   - Per-project session memory / messages — those move into the DB
//     separately in v0.3.5.
//
// Backed by SQLite at `<XDG_DATA_HOME>/daedalus/daedalus.db`
// (default `~/.local/share/daedalus/daedalus.db`).
//
// Thread-safety: all methods are safe to call from multiple threads.
// The underlying SQLite handle is opened in serialized mode.

#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// §B2 fix (v0.5.1): OmniNet::DB::Database is now referenced directly in
// the class body (no pimpl), so the full type must be visible here.
#include <db/Database.h>

#include "daedalus/types/ProviderConfig.h"


namespace daedalus {

/// Server error retry/backoff configuration. Mirrors
/// settings.json::server_error.
struct ServerErrorSettings
{
    bool enabled = true;
    int initial_wait_seconds = 30;
    int max_wait_seconds = 300;
    double backoff_multiplier = 2.0;
    int max_retries_per_call = 10;
};

/// SearXNG metasearch instance configuration (§SEARXNG-CORE, schema v7).
/// Mirrors the `searxng_config` table (singleton row, id=1).
/// Auth is HTTP Basic; an empty username means no auth header is sent.
struct SearxngSettings
{
    std::string base_url;   //!< e.g. "https://search.example.com" (no trailing slash)
    std::string username;   //!< HTTP Basic auth username; empty = unauthenticated
    std::string password;   //!< HTTP Basic auth password
    bool enabled = true;    //!< Master switch for the searxng_search tool
};

/// One row of the `web_search_providers` table (§WEB-SEARCH-CORE, schema v8).
///
/// The unified `web_search` tool walks enabled providers in priority order
/// and returns the first provider's results that succeeds (failover chain).
/// `name` is the stable provider id: "brave", "tavily", "exa", "firecrawl",
/// "serper", "serpapi", "searxng", "duckduckgo".
///
/// Field use is provider-dependent:
///   - api_key:    brave, tavily, exa, firecrawl, serper, serpapi
///   - base_url:   searxng only (self-hosted instance)
///   - username/password: searxng optional HTTP Basic auth
///   - duckduckgo: no fields needed (key-less HTML scrape)
struct WebSearchProviderSettings
{
    std::string name;          //!< Stable provider id (see list above)
    bool enabled = false;      //!< Participates in the failover chain
    std::string api_key;       //!< API key where required
    std::string base_url;      //!< searxng only; no trailing slash
    std::string username;      //!< searxng optional basic-auth user
    std::string password;      //!< searxng optional basic-auth pass
    int priority = 0;          //!< Lower = tried earlier in the chain
};

/// Opaque handle to the settings database.
///
/// Obtained via:
///   - `SettingsStore::instance()`     — global default, one DB per user
///   - `SettingsStore::for_home(path)` — per-Agent DB under `path`
///
/// The handle is cheap to copy. All copies share the same underlying
/// SQLite connection (singleton-per-path via DbPool). The dtor does NOT
/// close the DB — the process keeps one connection per unique path.
///
/// §B2 fix (v0.5.1): previously `for_home()` returned `SettingsStore&`
/// via a leaked `*(new SettingsStore)`. Now it returns by value, so the
/// wrapper is destroyed by the caller's RAII container. The underlying
/// sqlite3 handle lives in DbPool as before.
class SettingsStore
{
public:
    /// Global default. Singleton. First call opens the DB lazily.
    /// Returns by reference because the underlying static is a true
    /// singleton — the address is stable for the process lifetime.
    static SettingsStore& instance();

    /// Per-agent settings root. Pass the same `home` and you get a handle
    /// that shares the same DbPool connection as any prior call with the
    /// same `home`. Pass a different `home` and you get a distinct DB under
    /// `<home>/daedalus.db`.
    ///
    /// §B2 fix: returns by value (move-constructed from the internal
    /// temporary). The DbPool entry stays alive for the process duration;
    /// only the lightweight wrapper is destroyed when the returned value
    /// goes out of scope.
    static SettingsStore for_home(std::filesystem::path home);

    // -- Read --
    std::optional<ProviderConfig> get_provider(std::string_view name) const;
    std::vector<std::string>      list_providers() const;
    std::vector<ProviderConfig>   list_providers_detailed() const;

    std::optional<ModelConfig> get_model(std::string_view name) const;
    std::vector<ModelConfig>   list_models() const;

    /// Effective settings for a model: model-specific values merged with
    /// the referenced provider's host/api_key/endpoints.
    std::optional<ResolvedModelConfig> resolve_model(std::string_view name) const;

    std::string get_string(std::string_view key, std::string_view def = "") const;
    bool        get_bool  (std::string_view key, bool              def = false) const;
    int         get_int   (std::string_view key, int               def = 0)     const;

    // -- Write --
    void upsert_provider(const ProviderConfig& p);
    void delete_provider(std::string_view name);

    void upsert_model (const ModelConfig& m);
    void delete_model (std::string_view name);

    void set_string(std::string_view key, std::string_view value);
    void set_bool  (std::string_view key, bool              value);
    void set_int   (std::string_view key, int               value);

    // -- Atomic --
    void transaction(std::function<void(SettingsStore&)> fn);

    // -- Introspection --
    std::filesystem::path db_path() const noexcept;

    /// True if this store has any user data (models OR non-schema kv rows).
    /// Used by subsystems to decide whether the SQLite store is
    /// authoritative for v0.4.0 (and JSON can be ignored).
    bool has_data() const noexcept;

    // -----------------------------------------------------------------------
    // v0.5.0: AgentImpl wires directly to SettingsStore ().
    // These are the remaining accessors needed by AgentImpl that weren't
    // in the initial v0.4.0 API.
    // -----------------------------------------------------------------------

    /// Get the default model name from kv_settings. Empty if none set.
    std::string get_default_model_name() const;

    /// Set the default model name in kv_settings.
    void set_default_model_name(std::string_view name);

    /// Get the current provider label from kv_settings. Empty if none set.
    std::string get_current_provider() const;

    /// Set the current provider label in kv_settings.
    void set_current_provider(std::string_view provider);

    /// Get auto_approve from kv_settings. Default false.
    bool get_auto_approve() const;

    /// Set auto_approve in kv_settings.
    void set_auto_approve(bool value);

    /// Get the resolved max_context_size for `model_name`.
    /// Returns the user's setting from `models` table if set (non-zero),
    /// otherwise falls back to the built-in default for known models.
    /// Returns 1000000 (1M) as the ultimate fallback for unknown models.
    int get_max_context_size_for_model(std::string_view model_name) const;

    // -----------------------------------------------------------------------
    // v0.5.0: remaining agent settings that LLMClient needs.
    // .
    // Defaults: max_iterations=100, max_tool_result=10KB, max_request=4MB,
    //           max_response=5MB.
    // -----------------------------------------------------------------------

    /// Get max_agent_iterations from kv_settings. Default 100.
    int get_max_agent_iterations() const;

    /// Get max_tool_result_size from kv_settings. Default 10000 (10KB).
    size_t get_max_tool_result_size() const;

    /// Get max_request_size from kv_settings. Default 4000000 (4MB).
    size_t get_max_request_size() const;

    /// Get max_response_size from kv_settings. Default 5000000 (5MB).
    size_t get_max_response_size() const;

    // -----------------------------------------------------------------------
    // v0.5.0: MCP tool schema soft-cap percent (used by LLMClient).
    // Default 5% — §18.9.C.
    // -----------------------------------------------------------------------

    /// Get mcp.tool_schema_soft_cap_percent from kv_settings. Default 5.
    int get_mcp_tool_schema_soft_cap_percent() const;

    // -----------------------------------------------------------------------
    // v0.5.0: setters for the values the agent can change at runtime.
    // (Reads exist for LLMClient; writes exist for /models + tests.)
    // -----------------------------------------------------------------------

    void set_max_agent_iterations(int v);
    void set_max_tool_result_size(size_t v);
    void set_max_request_size(size_t v);
    void set_max_response_size(size_t v);
    void set_mcp_tool_schema_soft_cap_percent(int v);

    // -----------------------------------------------------------------------
    // v0.5.0: server error retry settings.
    // -----------------------------------------------------------------------

    /// Get server_error settings from kv_settings. Returns defaults if none set.
    ServerErrorSettings get_server_error_settings() const;

    // v0.5.0: Serper.dev API key.
    //
    // Stored in the `serper_config` table (singleton row, id=1).
    // Populated by daedalus-cli --migrate-data or via set_serper_api_key().

    /// Get the Serper API key from the store. Empty string if not set.
    std::string get_serper_api_key() const;

    /// Set the Serper API key. Empty string clears the row.
    void set_serper_api_key(std::string_view api_key);

    // -----------------------------------------------------------------------
    // §SEARXNG-CORE (schema v7): self-hosted SearXNG metasearch instance.
    //
    // Stored in the `searxng_config` table (singleton row, id=1).
    // Configured by the host's settings UI or via set_searxng_settings().

    /// Get the SearXNG instance config. Default-constructed (empty base_url,
    /// enabled=true) if no row exists.
    SearxngSettings get_searxng_settings() const;

    /// Set the SearXNG instance config. An empty base_url disables the tool
    /// at request time (descriptive error), same contract as serper_search.
    void set_searxng_settings(const SearxngSettings& settings);

    // -----------------------------------------------------------------------
    // §WEB-SEARCH-CORE (schema v8): unified web_search provider registry.
    //
    // Stored in the `web_search_providers` table (one row per provider).
    // The unified `web_search` tool reads the full chain per call — Settings
    // edits take effect without a restart. The legacy serper_config /
    // searxng_config tables are copied into this table by migrate_to_v8 and
    // are no longer consulted by the tool.

    /// All provider rows ordered by priority (lower first). Empty when the
    /// store is unreadable; on a fresh schema the 8 built-in providers are
    /// seeded (duckduckgo enabled, everything else disabled).
    std::vector<WebSearchProviderSettings> get_web_search_providers() const;

    /// Upsert one provider row (matched by name). Unknown names are stored
    /// verbatim — forward-compatible with providers added later.
    void set_web_search_provider(const WebSearchProviderSettings& settings);

    // -----------------------------------------------------------------------
    // v0.3.5: per-project persistence (session memory + messages).
    //
    // For v0.3.5 these are write-through mirrors of the existing JSON
    // files at <data_dir>/memory/memory.json and <data_dir>/messages.json.
    // The data is stored in the same SQLite DB under namespaced keys:
    //
    //   "mem.<session_id>"        → JSON-encoded SessionMemory payload
    //   "msg.<session_id>"        → JSON-encoded messages array
    //
    // v0.4.0 swaps to DB-primary and deletes the JSON files.
    // ---------------------------------------------------------------------

    /// Get the persisted session-memory JSON for `session_id`. Empty if none.
    std::string get_session_memory(std::string_view session_id) const;

    /// Persist the session-memory JSON. Stores under "mem.<session_id>".
    void set_session_memory(std::string_view session_id, std::string_view json_blob);

    /// Drop the persisted session-memory for `session_id`.
    void delete_session_memory(std::string_view session_id);

    /// Get the persisted messages JSON for `session_id`. Empty if none.
    std::string get_messages_json(std::string_view session_id) const;

    /// Persist the messages JSON. Stores under "msg.<session_id>".
    void set_messages_json(std::string_view session_id, std::string_view json_blob);

    /// Drop the persisted messages for `session_id`.
    void delete_messages_json(std::string_view session_id);

    // -----------------------------------------------------------------------
    // v0.5.0: MCP server registry. The `mcp_servers` table replaces the
    // legacy `servers.json` file. Populated via upsert_mcp_server() or
    // daedalus-cli --migrate-data.
    // ----------------------------------------------------------------------

    /// One MCP server config. Mirrors the McpServerConfig struct but lives
    /// in the public API so hosts can read/write the registry directly.
    struct McpServer
    {
        std::string name;
        std::string transport = "stdio";        // stdio | streamable-http
        std::string command;                    // stdio: executable
        std::vector<std::string> args;          // stdio: argv
        std::map<std::string, std::string> env; // stdio: env vars
        std::string url;                        // http: endpoint
        std::string bearer_token;               // http: Authorization header
        std::string description;
        bool enabled = true;
    };

    /// Get one MCP server by name.
    std::optional<McpServer> get_mcp_server(std::string_view name) const;

    /// List all configured MCP servers.
    std::vector<McpServer> list_mcp_servers() const;

    /// Insert or replace one MCP server.
    void upsert_mcp_server(const McpServer& s);

    /// Delete one MCP server by name.
    void delete_mcp_server(std::string_view name);

    /// True if any MCP servers are registered.
    bool has_mcp_servers() const noexcept;

    // -----------------------------------------------------------------------
    // §TASK-2026-09-21-01 (schema v6): encrypted secrets vault — raw row
    // accessors. These move OPAQUE base64 blobs only; all crypto lives in
    // daedalus::SecretsVault (include/daedalus/vault.h). The vault table is
    // global by convention (SecretsVault::global() always targets the
    // process-wide store) so secrets are reachable from every project.
    // ----------------------------------------------------------------------

    /// One raw vault row. `nonce_b64`/`payload_b64` are base64'd binary
    /// (12-byte nonce ‖ ciphertext ‖ 16-byte GCM tag). Never plaintext.
    struct VaultRow
    {
        std::string name;
        std::string kind = "custom";        // login | note | api_key | custom
        std::string description;
        std::string nonce_b64;
        std::string payload_b64;
        std::string created_at;             // ISO-8601 UTC
        std::string updated_at;             // ISO-8601 UTC
    };

    /// Insert or replace one vault row (updated_at is refreshed; an
    /// existing row's created_at is preserved).
    void upsert_vault_row(const VaultRow& r);

    /// Fetch one vault row by name.
    std::optional<VaultRow> get_vault_row(std::string_view name) const;

    /// All vault rows, ordered by name.
    std::vector<VaultRow> list_vault_rows() const;

    /// Delete one vault row by name (missing name is a no-op).
    void delete_vault_row(std::string_view name);

    // -----------------------------------------------------------------------
    // §SECOND-BRAIN (schema v10): Global Second Brain index. The markdown
    // files under <user_data_root>/brain/ are the SOURCE OF TRUTH; these
    // tables are a rebuildable cache (metadata + FTS5 full-text index) so
    // search/list stay fast. File I/O and frontmatter parsing live in
    // daedalus::SecondBrain (src/core/SecondBrain.h) — these accessors only
    // move rows in/out of brain_notes / brain_fts. Global by convention,
    // like the vault: SecondBrain::global() targets the process-wide store.
    // -----------------------------------------------------------------------

    /// One brain_notes metadata row. `tags` is comma-separated.
    struct BrainNoteRow
    {
        std::string id;                   // filename stem, unique across folders
        std::string folder = "inbox";     // inbox|projects|areas|resources|archives
        std::string title;
        std::string tags;                 // comma-separated, may be empty
        std::string created;              // ISO-8601 UTC
        std::string updated;              // ISO-8601 UTC
        int64_t     mtime = 0;            // file mtime (ns) — reindex diff
        int64_t     size  = 0;            // file size (bytes)   — reindex diff
    };

    /// One search hit: the metadata row plus an FTS5 snippet and bm25 rank.
    struct BrainSearchHit
    {
        BrainNoteRow note;
        std::string  snippet;             // body excerpt with **match** markers
        double       rank = 0.0;          // bm25 score (lower = better in FTS5)
    };

    /// Insert or replace one note in the index: brain_notes metadata plus
    /// the brain_fts full-text row (title/tags/body). `body` is the raw
    /// markdown without frontmatter.
    void upsert_brain_note(const BrainNoteRow& r, std::string_view body);

    /// Fetch one metadata row by id.
    std::optional<BrainNoteRow> get_brain_note(std::string_view id) const;

    /// Metadata rows, newest-updated first. Empty folder = all folders.
    std::vector<BrainNoteRow> list_brain_notes(std::string_view folder = {},
                                               int limit = 100) const;

    /// All metadata rows (reindex diffs need mtime/size for every file).
    std::vector<BrainNoteRow> all_brain_notes() const;

    /// Full-text search. `fts_query` must already be FTS5-sanitised
    /// (SecondBrain quotes terms); it is bound as a MATCH parameter, never
    /// concatenated into SQL. Ordered by bm25 rank, best first.
    std::vector<BrainSearchHit> search_brain_notes(std::string_view fts_query,
                                                   int limit = 20) const;

    /// Delete one note from metadata + FTS index (missing id is a no-op).
    void delete_brain_note(std::string_view id);

private:
    // §B2 fix (v0.5.1): Impl fields are now inlined directly into the class
    // (not pimpl'd). This makes SettingsStore copyable/movable so
    // for_home() can return by value without leaking.
    //
    // db_ is non-owning — the actual sqlite3 handle is owned by DbPool
    // (singleton-per-path). Multiple SettingsStore instances can share the
    // same db_ pointer safely because SQLite runs in serialized mode.
    //
    // §webui-model-edit: a shared recursive mutex is used to serialise all
    // prepared-statement access. Prepared statements do not acquire OmniNet's
    // internal database mutex, so without this two threads could step on the
    // same sqlite3 handle concurrently and silently lose writes.
    std::filesystem::path path_;
    OmniNet::DB::Database* db_ = nullptr;
    mutable std::shared_ptr<std::recursive_mutex> mutex_;

    /// Internal ctor. `auto_migrate` is accepted for source compatibility
    /// but has no effect — auto-migration was removed in v0.5.2.
    SettingsStore(std::filesystem::path db_path, bool auto_migrate = false);
};

} // namespace daedalus
