#include "SettingsManager.h"
#include "core/SerperConfig.h"
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <optional>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <filesystem>

#include "daedalus/paths.h"
#include "core/Logger.h"

// §Phase 5.1: helper macro that converts LOG_CERR(INFO,  ...); style
// statements into LOG_INFO/LOG_WARNING/LOG_ERROR calls via the
// existing Logger. Same formatted text routed through the Logger
// instead of writing to stderr directly.
#define LOG_CERR(level, expr) do {                          \
        std::ostringstream _daedalus_msg;                   \
        _daedalus_msg << expr;                               \
        LOG_##level(_daedalus_msg.str());                    \
    } while (0)
// ----------------------------------------------------------------------------
// Phase 2 (singleton → instance-scoped refactor):
//
// SettingsManager is now a regular heap-owned class. It has a public
// constructor that takes an explicit settings_path. Production code
// (Agent) constructs and owns its own instance.
//
// For legacy callers — tests, demos/daedalus-cli/main.cpp, anything that
// pre-dates the Agent class — we provide `default_instance()` which
// returns a process-wide heap-allocated default. This lets the existing
// CLI and tests keep working without touching them.
// ----------------------------------------------------------------------------

static const int LOCK_WAIT_SECONDS = 5;
static const int LOCK_MAX_RETRIES = 12;  // 60 seconds total wait

namespace {
    // Resolved once at first use; subsequent calls reuse.
    // §Plan.md Phase 2 (2026-07-13): this used to read $DAEDALUSLIB_HOME
    // directly. It now delegates to Paths::global().config_file(), which
    // honours the full resolution chain from Plan.md §3.2:
    //   1. $DAEDALUS_HOME (single-root override)
    //   2. $XDG_CONFIG_HOME/daedalus (XDG-compliant default)
    //   3. $HOME/.daedalus (LEGACY, deprecated; kept for one release cycle)
    //   4. $HOME/.local/share/daedalus (XDG fallback)
    //
    // The legacy $DAEDALUSLIB_HOME env var is no longer consulted — use
    // $DAEDALUS_HOME instead (or any $XDG_* variable).
    //
    // We keep the throw-on-no-HOME behaviour because SettingsManager is
    // a leaf class that should fail loudly rather than silently creating
    // a config under /tmp.
    std::filesystem::path default_settings_path() {
        std::filesystem::path p = daedalus::Paths::global().config_file();
        if (p.empty()) {
            throw SettingsError(
                "Cannot determine daedalus config file path. "
                "Set HOME or DAEDALUS_HOME, or pass settings_path via AgentConfig.");
        }
        // Path resolution succeeded — return whatever Paths picked, which
        // may be a /tmp fallback if HOME was unset at constructor time.
        // The library treats that as an acceptable degraded state.
        return p;
    }
}

SettingsManager::SettingsManager(std::filesystem::path settings_path)
    : settings_file_path_(settings_path.empty()
                            ? default_settings_path()
                            : std::move(settings_path))
    , settings_()
    , loaded_(false) {
}

namespace {
    // The actual static pointer. default_instance() and delete_default()
    // both go through this so reset-after-delete is safe.
    SettingsManager*& default_settings_ref() {
        static SettingsManager* g_default = nullptr;
        return g_default;
    }
}

SettingsManager* SettingsManager::default_instance() {
    auto& p = default_settings_ref();
    if (!p) {
        p = new SettingsManager();
    }
    return p;
}

void SettingsManager::delete_default() {
    auto& p = default_settings_ref();
    delete p;
    p = nullptr;
}

// Wait for any existing lock file to disappear (up to 60 seconds)
// This prevents reading settings while another process is writing
bool SettingsManager::wait_for_lock_release() const {
    std::filesystem::path lock_path = settings_file_path_.parent_path() / ".settings.lock";

    for (int i = 0; i < LOCK_MAX_RETRIES; i++) {
        if (!std::filesystem::exists(lock_path)) {
            return true;  // Lock released
        }
        std::this_thread::sleep_for(std::chrono::seconds(LOCK_WAIT_SECONDS));
    }
    return false;  // Timed out
}

void SettingsManager::ensure_settings_directory() const {
    std::filesystem::path settings_dir = settings_file_path_.parent_path();

    if (!std::filesystem::exists(settings_dir)) {
        if (!std::filesystem::create_directories(settings_dir)) {
            throw SettingsError("Failed to create settings directory: " + settings_dir.string());
        }
    }
}

void SettingsManager::load_from_json(const json& j) {

    // Load MCP settings (the newer multi-server MCP system reads
    // ~/.daedalus/mcp/servers.json directly and ignores the legacy
    // `enabled`/`server_command` fields).
    if (j.contains("mcp") && j["mcp"].is_object()) {
        const json& mcp = j["mcp"];
        if (mcp.contains("tool_schema_soft_cap_percent")) {
            settings_.mcp.tool_schema_soft_cap_percent =
                mcp["tool_schema_soft_cap_percent"].get<int>();
        }
        // Silently ignore legacy `enabled` / `server_command` keys —
        // the field no longer exists in McpSettings and writing it would
        // be a no-op. The whole "mcp" object is now just a soft-cap
        // configuration surface.
    }
    
    // Load default model name
    if (j.contains("default_model")) {
        settings_.default_model_name = j["default_model"].get<std::string>();
    }

    // Load agent settings
    if (j.contains("max_agent_iterations")) {
        settings_.max_agent_iterations = j["max_agent_iterations"].get<int>();
    }
    if (j.contains("auto_approve")) {
        settings_.auto_approve = j["auto_approve"].get<bool>();
    }
    if (j.contains("max_tool_result_size")) {
        settings_.max_tool_result_size = j["max_tool_result_size"].get<size_t>();
    }
    if (j.contains("max_request_size")) {
        settings_.max_request_size = j["max_request_size"].get<size_t>();
    }
    if (j.contains("max_response_size")) {
        settings_.max_response_size = j["max_response_size"].get<size_t>();
    }

    // Load models (new flexible format)
    if (j.contains("models") && j["models"].is_object()) {
        for (auto& [model_name, model_data] : j["models"].items()) {
            ModelConfig config;
            config.name = model_name;

            if (model_data.contains("host")) {
                config.host = model_data["host"].get<std::string>();
            }
            if (model_data.contains("api_key")) {
                config.api_key = model_data["api_key"].get<std::string>();
            }
            if (model_data.contains("model")) {
                config.model = model_data["model"].get<std::string>();
            }
            if (model_data.contains("max_context_size")) {
                config.max_context_size = model_data["max_context_size"].get<int>();
            }
            if (model_data.contains("temperature")) {
                config.temperature = model_data["temperature"].get<double>();
            }

            // Load per-model endpoints (with fallback to default)
            if (model_data.contains("endpoints") && model_data["endpoints"].is_object()) {
                if (model_data["endpoints"].contains("chat")) {
                    config.chat_endpoint = model_data["endpoints"]["chat"].get<std::string>();
                }
            }
            if (config.chat_endpoint.empty()) {
                config.chat_endpoint = "/v1/chat/completions";  // Default endpoint
            }

            settings_.models[model_name] = config;
        }
    }
    
    // Load skills settings (with defaults if not present)
    if (j.contains("skills") && j["skills"].is_object()) {
        const json& skills = j["skills"];
        
        if (skills.contains("enabled")) {
            settings_.skills.enabled = skills["enabled"].get<bool>();
        }
        if (skills.contains("directories") && skills["directories"].is_array()) {
            settings_.skills.directories = skills["directories"].get<std::vector<std::string>>();
        }
        if (skills.contains("max_active_skills")) {
            settings_.skills.max_active_skills = skills["max_active_skills"].get<int>();
        }
        if (skills.contains("auto_discover")) {
            settings_.skills.auto_discover = skills["auto_discover"].get<bool>();
        }
    } else {
        // Set defaults if skills section is missing
        settings_.skills.enabled = true;
        settings_.skills.directories = {".agents/skills/", "~/.daedalus/skills/", "~/.agents/skills/"};
        settings_.skills.max_active_skills = 5;
        settings_.skills.auto_discover = true;
    }
    
    // §Phase 11: removed 'manager' section parser + ManagerSettings
    // struct. The library has no modes; manager settings only mattered
    // to SubAgentTool (deleted in commit b317d1c) which spawned sibling
    // 'daedalus --sub-agent' subprocesses. We accept the section
    // silently for backward-compat with existing settings.json files
    // but do nothing with it. To migrate, users can drop the
    // 'manager' block from settings.json — it's now a no-op.

    // Load server_error settings (with defaults if not present)
    if (j.contains("server_error") && j["server_error"].is_object()) {
        const json& server_error = j["server_error"];
        
        if (server_error.contains("enabled")) {
            settings_.server_error.enabled = server_error["enabled"].get<bool>();
        }
        if (server_error.contains("initial_wait")) {
            settings_.server_error.initial_wait_seconds = server_error["initial_wait"].get<int>();
        }
        if (server_error.contains("max_wait")) {
            settings_.server_error.max_wait_seconds = server_error["max_wait"].get<int>();
        }
        if (server_error.contains("backoff_multiplier")) {
            settings_.server_error.backoff_multiplier = server_error["backoff_multiplier"].get<double>();
        }
        if (server_error.contains("max_retries")) {
            settings_.server_error.max_retries_per_call = server_error["max_retries"].get<int>();
        }
    } else {
        // Set defaults if server_error section is missing
        settings_.server_error.enabled = true;
        settings_.server_error.initial_wait_seconds = 30;
        settings_.server_error.max_wait_seconds = 300;
        settings_.server_error.backoff_multiplier = 2.0;
        settings_.server_error.max_retries_per_call = 10;
    }

    loaded_ = true;
}

void SettingsManager::save_to_json(const json& j) const {
    std::filesystem::path lock_path = settings_file_path_.parent_path() / ".settings.lock";
    
    // Create lock file to signal other processes not to read
    std::ofstream lock_file(lock_path);
    lock_file << getpid();  // Write PID for debugging
    lock_file.close();
    
    std::ofstream out_file(settings_file_path_);
    if (!out_file.is_open()) {
        std::filesystem::remove(lock_path);
        throw SettingsError("Failed to open settings file for writing: " + settings_file_path_.string());
    }
    out_file << j.dump(4);  // Pretty print with 4-space indent
    out_file.close();
    
    // Remove lock file after writing is complete
    std::filesystem::remove(lock_path);
}

void SettingsManager::load() {
    // Check if settings file exists
    if (!std::filesystem::exists(settings_file_path_)) {
        // Create default settings file
        create_default_settings();
        
        LOG_CERR(INFO,  "\n===========================================\n"
                  << "Welcome to daedalus!\n"
                  << "===========================================\n"
                  << "A settings file has been created at:\n"
                  << "  " << settings_file_path_.string() << "\n\n"
                  << "Please edit this file and add your API key.\n"
                  << "Then run daedalus again.\n"
                  << "===========================================\n\n");
        
        throw SettingsError("Settings file not found. A default file has been created at: " + settings_file_path_.string() + ". Please configure it and try again.");
    }
    
    // Wait for any existing lock to be released (another process may be writing)
    if (!wait_for_lock_release()) {
        LOG_CERR(WARNING, "[WARNING] Settings file may be in use by another process. Continuing anyway...");
    }
    
    // Read and parse the JSON file
    try {
        std::ifstream in_file(settings_file_path_);
        if (!in_file.is_open()) {
            throw SettingsError("Failed to open settings file: " + settings_file_path_.string());
        }
        
        json user_settings;
        in_file >> user_settings;
        in_file.close();
        

        // Load the user's settings as-is. We do NOT merge with defaults
        // here, because that would silently add models the user never
        // configured. Defaults are only used to create the file the
        // first time (see create_default_settings()).
        load_from_json(user_settings);

        // If using new models format, validate that way
        if (!settings_.models.empty()) {
            auto default_model = get_default_model();
            if (!default_model) {
                throw SettingsError("default_model is set but model not found in models list. Edit: " + settings_file_path_.string());
            }
            if (default_model->api_key.empty() || default_model->api_key == "YOUR_API_KEY_HERE") {
                throw SettingsError("Missing or invalid API key for default model. Edit: " + settings_file_path_.string());
            }
        }
        
    } catch (const json::parse_error& e) {
        throw SettingsError("Failed to parse settings file: " + std::string(e.what()));
    } catch (const json::type_error& e) {
        throw SettingsError("Invalid settings format: " + std::string(e.what()));
    }
}

void SettingsManager::save() const {
    if (!loaded_) {
        throw SettingsError("No settings loaded to save");
    }

    ensure_settings_directory();

    // Preserve all unknown fields (e.g. serper-search, or any custom user
    // field added by the user or a newer daedalus version) by reading the
    // existing file first and overlaying our managed fields on top. This
    // avoids wiping out parts of the user's settings that this function
    // does not know about.
    json j;
    if (std::filesystem::exists(settings_file_path_)) {
        try {
            std::ifstream in(settings_file_path_);
            if (in.is_open()) {
                try {
                    in >> j;
                } catch (const json::exception&) {
                    // Existing file is malformed - fall back to a fresh
                    // object so we don't lose our managed fields.
                    j = json::object();
                }
            }
        } catch (...) {
            // Don't let a read failure block saving managed fields
        }
    }

    // Save default model name
    j["default_model"] = settings_.default_model_name;

    // Save agent settings
    j["max_agent_iterations"] = settings_.max_agent_iterations;
    j["auto_approve"] = settings_.auto_approve;
    j["max_tool_result_size"] = settings_.max_tool_result_size;
    j["max_request_size"] = settings_.max_request_size;
    j["max_response_size"] = settings_.max_response_size;

    // Save models. We rebuild the model entries from the in-memory state
    // because partial updates would leave stale fields on a model that
    // was edited through the UI.
    for (const auto& [name, model] : settings_.models) {
        j["models"][name]["host"] = model.host;
        j["models"][name]["api_key"] = model.api_key;
        j["models"][name]["model"] = model.model;
        if (model.max_context_size > 0) {
            // Only save explicit values; let 0 mean "use built-in default"
            j["models"][name]["max_context_size"] = model.max_context_size;
        } else {
            // Remove an old explicit value so the built-in default applies
            j["models"][name].erase("max_context_size");
        }
        if (!model.chat_endpoint.empty()) {
            j["models"][name]["endpoints"]["chat"] = model.chat_endpoint;
        }
        // §Phase 8.7: temperature is a valid explicit value at 0.0
        // (deterministic), so we always round-trip it. Skip only the
        // case where the struct is uninitialised — actually a non-issue
        // since ModelConfig defaults temperature to 0.0.
        j["models"][name]["temperature"] = model.temperature;
    }

    save_to_json(j);
}

const AppSettings& SettingsManager::get_settings() const {
    if (!loaded_) {
        throw SettingsError("Settings have not been loaded yet");
    }
    return settings_;
}

const std::filesystem::path& SettingsManager::get_settings_file_path() const {
    return settings_file_path_;
}

bool SettingsManager::is_loaded() const {
    return loaded_;
}

void SettingsManager::create_default_settings() const {
    ensure_settings_directory();

    // Create default settings JSON with flexible models format
    json default_settings = {
        {"default_model", "minimax"},
        {"max_agent_iterations", 100},
        {"auto_approve", false},
        {"max_tool_result_size", 200000},
        {"serper-search", {
            {"api_key", "YOUR_SERPER_API_KEY"}
        }},
        {"models", {
            {"lm-studio", {
                {"host", "http://localhost:1234"},
                {"api_key", "lm-studio"},
                {"model", "llama-3-8b"},
                {"max_context_size", 8000},
                {"endpoints", {
                    {"chat", "/v1/chat/completions"}
                }}
            }},
            {"minimax", {
                {"host", "https://api.minimax.io"},
                {"api_key", "YOUR_API_KEY_HERE"},
                {"model", "MiniMax-M3"},
{"max_context_size", 500000},
                {"endpoints", {
                    {"chat", "/v1/chat/completions"}
                }}
            }}
        }},
        {"version", "2.0"}
    };

    save_to_json(default_settings);
}

json SettingsManager::get_default_settings_json() const {
    // Returns the default settings as a JSON object (for merging)
    // Simplified: no more extension-based restrictions, write/delete whitelist model
    return json{
        {"default_model", "minimax"},
        {"max_agent_iterations", 300},
        {"auto_approve", false},
        {"max_tool_result_size", 200000},
        {"max_request_size", 4000000},
        {"max_response_size", 5000000},
        {"mcp", {
            // §18.9.C — soft cap on combined tool schema tokens, as a
            // percent of the model's max_context_size. Above this, the
            // LLMClient logs a startup warning. Set higher if you have a
            // large-context model.
            {"tool_schema_soft_cap_percent", 5}
        }},
        {"skills", {
            {"enabled", true},
            {"directories", {".agents/skills/", "~/.daedalus/skills/", "~/.agents/skills/"}},
            {"max_active_skills", 5},
            {"auto_discover", true}
        }},
        {"serper-search", {
            {"api_key", "YOUR_SERPER_API_KEY"}
        }},
        {"models", {
            {"minimax", {
                {"host", "https://api.minimax.io"},
                {"api_key", "YOUR_API_KEY_HERE"},
                {"model", "MiniMax-M2.7"},
                {"endpoints", {{"chat", "/v1/chat/completions"}}},
{"max_context_size", 500000},
            }},
            {"lm-studio", {
                {"host", "http://localhost:1234"},
                {"api_key", "lm-studio"},
                {"model", "llama-3-8b"},
                {"endpoints", {{"chat", "/v1/chat/completions"}}},
                {"max_context_size", 8000},
            }}
        }}
    };
}

json SettingsManager::merge_with_defaults(const json& user_settings, const json& defaults) const {
    json result = defaults;
    
    // Helper to check if a string ends with another string
    auto ends_with = [](const std::string& str, const std::string& suffix) {
        if (str.length() >= suffix.length()) {
            return str.compare(str.length() - suffix.length(), suffix.length(), suffix) == 0;
        }
        return false;
    };
    
    // Merge top-level keys
    for (auto& [key, value] : user_settings.items()) {
        // Special handling for models - merge each model but preserve API keys
        if (key == "models" && value.is_object()) {
            json models_result = json::object();
            
            // Start with default models
            if (defaults.contains("models") && defaults["models"].is_object()) {
                for (auto& [model_name, model_config] : defaults["models"].items()) {
                    models_result[model_name] = model_config;
                }
            }
            
            // Override with user models, preserving API keys
            for (auto& [model_name, model_config] : value.items()) {
                if (model_config.is_object()) {
                    json merged_model = model_config;
                    
                    // If model exists in defaults, merge it but preserve API key
                    if (models_result.contains(model_name) && models_result[model_name].is_object()) {
                        const json& default_model = models_result[model_name];
                        
                        // Merge each key in the model config
                        for (auto& [cfg_key, cfg_value] : default_model.items()) {
                            // Don't overwrite if user has a value AND it's not the default placeholder
                            if (!model_config.contains(cfg_key) || 
                                (cfg_key == "api_key" && 
                                 (model_config[cfg_key].get<std::string>().empty() || 
                                  ends_with(model_config[cfg_key].get<std::string>(), "_API_KEY") ||
                                  model_config[cfg_key].get<std::string>() == "YOUR_API_KEY_HERE" ||
                                  model_config[cfg_key].get<std::string>() == "lm-studio"))) {
                                // Use default value
                                merged_model[cfg_key] = cfg_value;
                            }
                        }
                    }
                    models_result[model_name] = merged_model;
                } else {
                    models_result[model_name] = model_config;
                }
            }
            result["models"] = models_result;
        }
        // Special handling for image - merge nested objects
        else if (key == "image" && value.is_object()) {
            json image_result = json::object();
            
            // Start with defaults
            if (defaults.contains("image") && defaults["image"].is_object()) {
                image_result = defaults["image"];
            }
            
            // Override with user values (but preserve API keys if any)
            for (auto& [img_key, img_value] : value.items()) {
                image_result[img_key] = img_value;
            }
            result["image"] = image_result;
        }
        // Special handling for serper-search - preserve API key
        // Accepts both "serper-search" (canonical) and "serper" (legacy) keys
        else if ((key == "serper-search" || key == "serper") && value.is_object()) {
            json serper_result = json::object();

            // Start with defaults (canonical key)
            const char* default_key = "serper-search";
            if (defaults.contains(default_key) && defaults[default_key].is_object()) {
                serper_result = defaults[default_key];
            }

            // Override with user values, but preserve API key if user didn't provide one
            for (auto& [serper_key, serper_value] : value.items()) {
                if (serper_key == "api_key" && !serper_value.get<std::string>().empty() &&
                    !ends_with(serper_value.get<std::string>(), "_API_KEY")) {
                    // User provided a real API key, use it
                    serper_result[serper_key] = serper_value;
                }
                // If empty or placeholder, keep default
            }
            // Always store under the canonical key
            result["serper-search"] = serper_result;
        }
        // Special handling for legacy "api" section - preserve API key
        else if (key == "api" && value.is_object()) {
            json api_result = json::object();
            
            // Start with defaults
            if (defaults.contains("api") && defaults["api"].is_object()) {
                api_result = defaults["api"];
            }
            
            // Override with user values, but preserve API key if user didn't provide one
            for (auto& [api_key, api_value] : value.items()) {
                if (api_key == "api_key" && !api_value.get<std::string>().empty() && 
                    api_value.get<std::string>() != "YOUR_API_KEY_HERE") {
                    // User provided a real API key, use it
                    api_result[api_key] = api_value;
                }
                // If empty or placeholder, keep default
                else if (api_key != "api_key") {
                    api_result[api_key] = api_value;
                }
            }
            result["api"] = api_result;
        }
        // For all other keys, use user value (which adds missing keys automatically)
        else {
            result[key] = value;
        }
    }
    
    return result;
}
int SettingsManager::get_max_agent_iterations() const {
    if (!loaded_) {
        throw SettingsError("Settings have not been loaded yet");
    }
    return settings_.max_agent_iterations;
}

bool SettingsManager::get_auto_approve() const {
    if (!loaded_) {
        throw SettingsError("Settings have not been loaded yet");
    }
    return settings_.auto_approve;
}

int SettingsManager::get_max_context_size() const {
    if (!loaded_) {
        throw SettingsError("Settings have not been loaded yet");
    }
    // Use the current default model's max_context_size if available,
    // otherwise fall back to the global setting.
    return get_max_context_size_for_model(settings_.default_model_name);
}

int SettingsManager::get_max_context_size_for_model(const std::string& model_name) const {
    if (!loaded_) {
        throw SettingsError("Settings have not been loaded yet");
    }
    // Look up the model's per-model context size first (user's explicit value)
    auto it = settings_.models.find(model_name);
    if (it != settings_.models.end() && it->second.max_context_size > 0) {
        return it->second.max_context_size;
    }
    // Fall back to built-in defaults for known models
    // This is critical because settings.json often doesn't include max_context_size,
    // and we need to know the correct context size for each model.
    return get_builtin_max_context_size(model_name);
}

bool SettingsManager::validate_settings() const {
    bool valid = true;
    LOG_CERR(INFO,  "\n[VALIDATION] Checking settings.json for missing fields...\n");

    // Check default_model is set
    if (settings_.default_model_name.empty()) {
        LOG_CERR(WARNING,  "  [WARNING] No 'default_model' set in settings.json.\n"
                  << "            Add: \"default_model\": \"<model_name>\"\n");
        valid = false;
    } else {
        LOG_CERR(INFO,  "  [OK] default_model: " << settings_.default_model_name);
    }

    // Check at least one model is configured
    if (settings_.models.empty()) {
        LOG_CERR(ERROR,  "  [ERROR] No models configured in settings.json.\n"
                  << "            Add a 'models' object with at least one model.\n");
        valid = false;
    } else {
        LOG_CERR(INFO,  "  [OK] models configured: " << settings_.models.size());
    }

    // Validate each model
    for (const auto& [name, model] : settings_.models) {
        LOG_CERR(INFO,  "  [Model: " << name << "]\n");

        // Check host
        if (model.host.empty()) {
            LOG_CERR(WARNING,  "    [WARNING] Missing 'host'. Add: \"host\": \"https://api.example.com\"\n");
            valid = false;
        } else {
            LOG_CERR(INFO,  "    [OK] host: " << model.host);
        }

        // Check api_key
        if (model.api_key.empty()) {
            LOG_CERR(ERROR,  "    [ERROR] Missing 'api_key'.\n");
            valid = false;
        } else if (model.api_key == "YOUR_API_KEY_HERE" ||
                   model.api_key == "YOUR_OPENAI_API_KEY" ||
                   model.api_key == "YOUR_ANTHROPIC_API_KEY" ||
                   model.api_key == "YOUR_DEEPSEEK_API_KEY") {
            LOG_CERR(ERROR,  "    [ERROR] api_key is still the placeholder. Replace with your real API key.\n");
            valid = false;
        } else {
            // Do NOT print any portion of the api_key here, not even a
            // short prefix - the first 7 characters of a key are still
            // 17% of the credential material. Report only that a key is
            // configured, plus its length so the user can sanity-check
            // (e.g. "40 chars" for a typical Serper or sk-cp key).
            LOG_CERR(INFO,  "    [OK] api_key: <configured, "
                      << model.api_key.size() << " chars>\n");
        }

        // Check model name
        if (model.model.empty()) {
            LOG_CERR(WARNING,  "    [WARNING] Missing 'model' name. Add: \"model\": \"<model_name>\"\n");
            valid = false;
        } else {
            LOG_CERR(INFO,  "    [OK] model: " << model.model);
        }

        // Check max_context_size - use fallback if not set
        if (model.max_context_size == 0) {
            int fallback = get_builtin_max_context_size(name);
            LOG_CERR(WARNING,  "    [WARNING] Missing 'max_context_size'. Using fallback: "
                      << fallback << " tokens.\n"
                      << "              Add: \"max_context_size\": " << fallback);
            // Not setting valid=false since fallback works, but warn the user
        } else {
            LOG_CERR(INFO,  "    [OK] max_context_size: " << model.max_context_size);
        }

        // Check chat endpoint
        if (model.chat_endpoint.empty()) {
            LOG_CERR(WARNING,  "    [WARNING] Missing 'endpoints.chat'. Using default: /v1/chat/completions\n");
        } else {
            LOG_CERR(INFO,  "    [OK] endpoints.chat: " << model.chat_endpoint);
        }
    }

    // Check that default_model points to an actual configured model
    if (!settings_.default_model_name.empty() &&
        settings_.models.find(settings_.default_model_name) == settings_.models.end()) {
        LOG_CERR(ERROR,  "  [ERROR] default_model '" << settings_.default_model_name
                  << "' is not in the models list.\n");
        valid = false;
    }

    // Check max_agent_iterations
    if (settings_.max_agent_iterations == 0) {
        LOG_CERR(WARNING,  "  [WARNING] max_agent_iterations is 0. Using default of 100.\n");
    }

    // ------------------------------------------------------------------------
    // Serper.dev API key check.
    //
    // This is the integration point: the Serper key check now lives inside
    // validate_settings() so the [VALIDATION] block at startup reports it.
    // Previously a missing serper key only surfaced at the first web search,
    // many minutes into a session - now it is reported immediately, alongside
    // the rest of the config validation.
    //
    // A missing or invalid serper key is a WARNING (not a hard failure)
    // because not every environment needs web search.
    // ------------------------------------------------------------------------
    {
        auto serper = validate_serper_api_key();
        if (serper.ok()) {
            // No key material is printed - only the length so the user
            // can sanity-check.
            LOG_CERR(INFO,  "  [OK] serper-search.api_key: <configured, "
                      << serper.api_key.size() << " chars>\n");
            // (no effect on `valid` - serper is optional)
        } else {
            LOG_CERR(WARNING,  "  [WARNING] serper-search: " << serper.status_name()
                      << " - " << serper.error_detail << "\n"
                      << "              (serper_search tool will fail at request time)\n");
            // Also a soft warning: do not set valid=false because the agent
            // can still start and work without web search.
        }
    }

    LOG_CERR(INFO,  "[VALIDATION] " << (valid ? "PASSED" : "COMPLETED WITH WARNINGS") << "\n\n");
    return valid;
}

daedalus::SerperApiKeyResult SettingsManager::validate_serper_api_key() const {
    // Delegate to the shared loader in core/SerperConfig. The loader does
    // its own file I/O (independent of the SettingsManager's own copy of
    // the in-memory AppSettings) so it reads the file as it actually is
    // on disk, including any serper-search block that hasn't been parsed
    // into AppSettings. This is intentional: the AppSettings struct does
    // not (yet) model the serper key, and we don't want to add it just
    // for validation.
    return daedalus::load_serper_api_key();
}

int SettingsManager::get_builtin_max_context_size(const std::string& model_name) const {
    // Built-in defaults for known models
    // Each model MUST have its OWN max_context_size setting.
    // Map is keyed by the setting name (e.g. "minimax-m2.7") so the user
    // can also match by the model name (e.g. "MiniMax-M2.7") if needed.
    static const std::unordered_map<std::string, int> known_models = {
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
        {"deepseek-reasoner",   128000},
        // Meta Llama models
        {"llama-3-8b",           8000},
        {"llama-3-70b",          8000},
        {"llama-3.1-8b",       128000},
        {"llama-3.1-70b",      128000},
        {"llama-3.1-405b",     128000},
        // Local models (small contexts)
        {"lm-studio",            8000},
    };
    auto it = known_models.find(model_name);
    if (it != known_models.end()) {
        return it->second;
    }
    // Final fallback: 32K tokens (conservative default for unknown models)
    // Better to under-estimate and trigger compression early than overflow.
    return 32000;
}

size_t SettingsManager::get_max_tool_result_size() const {
    if (!loaded_) {
        throw SettingsError("Settings have not been loaded yet");
    }
    return settings_.max_tool_result_size;
}

size_t SettingsManager::get_max_request_size() const {
    if (!loaded_) {
        throw SettingsError("Settings have not been loaded yet");
    }
    return settings_.max_request_size;
}

size_t SettingsManager::get_max_response_size() const {
    if (!loaded_) {
        throw SettingsError("Settings have not been loaded yet");
    }
    return settings_.max_response_size;
}

const std::unordered_map<std::string, ModelConfig>& SettingsManager::get_models() const {
    if (!loaded_) {
        throw SettingsError("Settings have not been loaded yet");
    }
    return settings_.models;
}

std::optional<ModelConfig> SettingsManager::get_model(const std::string& name) const {
    if (!loaded_) {
        throw SettingsError("Settings have not been loaded yet");
    }
    auto it = settings_.models.find(name);
    if (it != settings_.models.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::optional<ModelConfig> SettingsManager::get_default_model() const {
    if (!loaded_) {
        throw SettingsError("Settings have not been loaded yet");
    }
    return get_model(settings_.default_model_name);
}

std::string SettingsManager::get_default_model_name() const {
    if (!loaded_) {
        throw SettingsError("Settings have not been loaded yet");
    }
    return settings_.default_model_name;
}

void SettingsManager::set_default_model(const std::string& model_name) {
    if (!loaded_) {
        throw SettingsError("Settings have not been loaded yet");
    }

    // Verify the model exists
    if (settings_.models.find(model_name) == settings_.models.end()) {
        throw SettingsError("Model not found: " + model_name);
    }

    settings_.default_model_name = model_name;
}

const SkillsSettings& SettingsManager::get_skills_settings() const {
    if (!loaded_) {
        throw SettingsError("Settings have not been loaded yet");
    }
    return settings_.skills;
}

const McpSettings& SettingsManager::get_mcp_settings() const {
    if (!loaded_) {
        throw SettingsError("Settings have not been loaded yet");
    }
    return settings_.mcp;
}

// §Phase 11: removed get_manager_settings() — ManagerSettings struct
// was deleted (commit pending) since the only consumer was SubAgentTool.

const ServerErrorSettings& SettingsManager::get_server_error_settings() const {
    if (!loaded_) {
        throw SettingsError("Settings have not been loaded yet");
    }
    return settings_.server_error;
}
