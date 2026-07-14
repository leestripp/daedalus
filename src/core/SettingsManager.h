#pragma once

#include <string>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <optional>

#include "SkillsSettings.h"
#include "core/SerperConfig.h"

using json = nlohmann::json;

// Custom exception for settings-related errors
class SettingsError : public std::runtime_error {
public:
    explicit SettingsError(const std::string& message) : std::runtime_error(message) {}
};

struct ModelConfig {
    std::string name;
    std::string host;
    std::string api_key;
    std::string model;
    std::string chat_endpoint;
    // Default 0 means "not set" - get_max_context_size_for_model() will
    // return the 1M fallback. This is important because the load() function
    // does NOT merge with defaults, so if a user's settings.json doesn't
    // have max_context_size, the struct default is what gets used.
    // Setting to 0 ensures the fallback (1M) is triggered correctly.
    int max_context_size = 0;  // 0 = use fallback (1M)
    double temperature = 0.0; // §Phase 8.7: sampling temperature, default 0.0
};

// §18.9.C — soft cap (percent of context window) for the combined MCP
// tool schema. When the actual schema token count exceeds this fraction of
// the model's max_context_size, the LLMClient logs a warning at startup
// suggesting the user prune servers.json. Default 5%.
struct McpSettings {
    int tool_schema_soft_cap_percent = 5;
};

struct ServerErrorSettings {
    bool enabled = true;
    int initial_wait_seconds = 30;
    int max_wait_seconds = 300;
    double backoff_multiplier = 2.0;
    int max_retries_per_call = 10;
};

struct AppSettings {
    McpSettings mcp;
    SkillsSettings skills;
    ServerErrorSettings server_error;
    std::string default_model_name;
    std::unordered_map<std::string, ModelConfig> models;
    int max_agent_iterations = 100;
    bool auto_approve = false;
    size_t max_tool_result_size = 10000;  // 10KB - truncate middle for tool results
    size_t max_request_size = 4000000;   // 4MB max request size
    size_t max_response_size = 5000000;  // 5MB max response size
};

class SettingsManager {
private:
    std::filesystem::path settings_file_path_;
    AppSettings settings_;
    bool loaded_;

    // Process-wide default instance for legacy callers (tests,
    // demos/daedalus-cli, anything not running through Agent).
    // Returns a heap-owned singleton; first call determines the path.
    // Production code (Agent) creates its own instance instead.
    static SettingsManager* default_instance();

    void load_from_json(const json& j);
    void save_to_json(const json& j) const;
    void ensure_settings_directory() const;

    // Get default settings as JSON (for merging)
    json get_default_settings_json() const;

    // Merge user settings with defaults (preserving API keys)
    json merge_with_defaults(const json& user_settings, const json& defaults) const;

public:
    // Construct an instance-scoped SettingsManager.
    // If settings_path is empty, defaults to $HOME/.daedalus/settings/settings.json
    // (or the XDG-compliant location if $XDG_CONFIG_HOME / $DAEDALUS_HOME
    // are set — see Plan.md §3.7).
    //
    // §Plan.md Phase 2: the legacy $DAEDALUSLIB_HOME env var is no longer
    // consulted at this layer. Use $DAEDALUS_HOME (or any $XDG_* variable)
    // to override the user-tier home in tests / CI / sandboxed runs.
    // Pass settings_path via AgentConfig to override at runtime.
    //
    // §Phase 8.8 NOTE: settings_path is NOT affected by
    // AgentConfig::agent_name. `agent_name` namespaces the
    // runtime-state files (messages.json, memory.json) that live
    // inside the *project*, not the global settings.json. Two
    // named agents (bob / alice) read the SAME settings.json
    // because they share the same project-level config. Per-Agent
    // settings isolation is the job of cfg.daedalus_home (Phase
    // 8.5), not cfg.agent_name.
    explicit SettingsManager(std::filesystem::path settings_path = {});

    // Legacy accessor. Returns the process-wide default instance.
    // Created lazily on first call; persists for the process lifetime.
    // Production code (Agent) should construct its own SettingsManager
    // rather than relying on this default.
    static SettingsManager* get_instance() { return default_instance(); }

    // Get or create the default instance, optionally with a custom path.
    // This is the only path the legacy `create_instance(path)` machinery
    // ever supported. New code passes the path to the constructor instead.
    static SettingsManager* get_default(const std::filesystem::path& settings_path = {})
    {
        static SettingsManager* g_default = nullptr;
        if (!g_default) {
            g_default = new SettingsManager(settings_path);
        }
        return g_default;
    }

    // Drop the process-wide default. Tests use this to reset state.
    // Production code (Agent) doesn't touch this — Agent owns its own
    // instance and destroys it on shutdown.
    static void delete_default();

    // No copy / no move: a SettingsManager is unique per location.
    SettingsManager(const SettingsManager&) = delete;
    SettingsManager& operator=(const SettingsManager&) = delete;
    SettingsManager(SettingsManager&&) = delete;
    SettingsManager& operator=(SettingsManager&&) = delete;

    // Load settings from ~/.daedalus/settings/settings.json
    void load();

    // Save current settings to file
    void save() const;

    // Get the settings
    const AppSettings& get_settings() const;

    // Get the settings file path
    const std::filesystem::path& get_settings_file_path() const;

    // Check if settings are loaded
    bool is_loaded() const;

    // Validate settings and print warnings for missing/invalid fields.
    // Returns true if all critical fields are present, false otherwise.
    // Warnings are printed to stderr but do not throw.
    //
    // This also covers the Serper.dev API key (serper-search.api_key) so
    // the [VALIDATION] block at startup is the single source of truth for
    // "is the agent config OK?". A missing or placeholder serper key is
    // treated as a warning (not a hard failure) so that environments
    // that don't use web search can still start.
    bool validate_settings() const;

    // Validate just the Serper.dev API key. Public so it can be called
    // independently (e.g. from a dedicated /serper-status command later),
    // but normally invoked from validate_settings() above.
    daedalus::SerperApiKeyResult validate_serper_api_key() const;

    // Create default settings file if it doesn't exist
    void create_default_settings() const;

    // Wait for any existing lock file to disappear (up to 60 seconds)
    // This prevents reading settings while another process is writing
    bool wait_for_lock_release() const;
    
    // Get just the API config for convenience

    // Get all models
    const std::unordered_map<std::string, ModelConfig>& get_models() const;

    // Get a specific model by name
    std::optional<ModelConfig> get_model(const std::string& name) const;

    // Get the default model
    std::optional<ModelConfig> get_default_model() const;

    // Get the default model name
    std::string get_default_model_name() const;

    // Set the default model name
    void set_default_model(const std::string& model_name);

    // Get the default model's chat endpoint

    // Get agent settings
    int get_max_agent_iterations() const;
    bool get_auto_approve() const;
    size_t get_max_tool_result_size() const;
    int get_max_context_size() const;

    // Get max context size for a specific model (returns user's setting, then built-in default, then 1M fallback)
    int get_max_context_size_for_model(const std::string& model_name) const;

    // Get the built-in default max_context_size for known models
    // (e.g. minimax-m2.7 -> 200K, minimax-m3 -> 1M)
    int get_builtin_max_context_size(const std::string& model_name) const;

    size_t get_max_request_size() const;
    size_t get_max_response_size() const;
    
    // Get Skills settings
    const SkillsSettings& get_skills_settings() const;
    
    // Get MCP settings (soft cap for combined tool schema token count)
    const McpSettings& get_mcp_settings() const;

    // §18.9.C — read the soft cap configured for the combined MCP tool
    // schema. Falls back to the in-struct default (5%) if the settings
    // file omits the key.
    int get_mcp_tool_schema_soft_cap_percent() const {
        return settings_.mcp.tool_schema_soft_cap_percent;
    }
    
    // Get Server Error settings
    const ServerErrorSettings& get_server_error_settings() const;
};
