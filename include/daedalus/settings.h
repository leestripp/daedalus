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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace daedalus {

/// One configured LLM endpoint. Mirrors the JSON shape of `config.json::models`
/// but is a proper C++ struct — no JSON knowledge leaks into hosts.
struct ModelConfig
{
    std::string name;              // key in config.json::models ("minimax-m3", ...)
    std::string host;              // https://api.example.com
    std::string api_key;           // plaintext — same as the old config.json
    std::string model_id;          // actual model id ("MiniMax-M3", ...)
    std::string chat_endpoint;     // "/v1/chat/completions" — has a default
    int    max_context_size = 0;   // 0 = fall back to model's default
    double temperature     = 0.0;
};

/// Opaque handle to the settings database.
///
/// Obtained via:
///   - `SettingsStore::instance()`     — global default, one DB per user
///   - `SettingsStore::for_home(path)` — per-Agent DB under `path`
///
/// Copying the handle is cheap and shares the underlying connection.
/// Destruction does NOT close the DB — the process keeps one connection
/// per unique db_path. (Singleton-per-path.)
class SettingsStore
{
public:
    /// Global default. Singleton. First call opens the DB lazily.
    static SettingsStore& instance();

    /// Per-agent settings root. Pass the same `home` and you get the same
    /// handle (singleton per-path). Pass a different `home` and you get a
    /// distinct DB under `<home>/daedalus.db`.
    static SettingsStore for_home(std::filesystem::path home);

    // -- Read --
    std::optional<ModelConfig> get_model(std::string_view name) const;
    std::vector<ModelConfig>   list_models() const;

    std::string get_string(std::string_view key, std::string_view def = "") const;
    bool        get_bool  (std::string_view key, bool              def = false) const;
    int         get_int   (std::string_view key, int               def = 0)     const;

    // -- Write --
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
    /// Used by SettingsManager to decide whether the SQLite store is
    /// authoritative for v0.4.0 (and JSON can be ignored).
    bool has_data() const noexcept;

    // ---------------------------------------------------------------------
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

private:
    SettingsStore() = default;
    SettingsStore(std::filesystem::path db_path, bool is_default);
    struct Impl;
    Impl* impl_ = nullptr;        // pimpl'd; opaque to API users
};

} // namespace daedalus
