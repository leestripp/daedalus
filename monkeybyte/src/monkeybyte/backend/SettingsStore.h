#pragma once

#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace monkeybyte {

struct InterfaceSettings {
    int font_size = 14;
    int tab_size = 2;
    bool word_wrap = true;
    std::string theme = "dark";
    bool show_hidden = false;             // Explorer: dotfiles visible
    bool show_line_numbers = true;        // Editor
    bool highlight_current_line = true;   // Editor
    std::string browser_home_url;         // Browser: ☰ → Open Browser target
                                          // ("" = blank "New Tab")
    // Chat appearance (§4.5 Chat group). Overrides only — the theme stays
    // the default look; 0 / "" means "use the active theme's value".
    int chat_font_size = 0;               // px, 0 = theme default
    std::string chat_bg;                  // .chat-panel background
    std::string chat_user_bg;             // user bubble background
    std::string chat_user_text;           // user bubble text
    std::string chat_assistant_bg;        // assistant bubble background
    std::string chat_assistant_text;      // assistant bubble text
};

// Row shape of the legacy `models` table (pre-provider-split schema),
// read only by model_bridge::migrate_legacy_models(). New model/provider
// config lives in daedalus's SettingsStore — see backend/ModelBridge.h.
// §CHIMPFLOW — local STT/TTS gateway configuration. The API key follows
// the provider convention: an empty value resolves CHIMPFLOW_KEY at request
// time. Nothing is enabled until the user opts in.
struct ChimpFlowSettings {
    bool enabled = false;
    bool read_assistant = true;
    bool dictation_enabled = true;
    std::string server_url = "http://127.0.0.1"; // loopback origin, no port/path
    int port = 8090;
    std::string api_key;
    std::string tts_model = "pocket-tts";
    std::string stt_model = "whisper";
    std::string voice; // populated from GET /v1/voices, never hardcoded

    bool operator==(const ChimpFlowSettings&) const = default;
};

struct LegacyModelConfig {
    std::string provider_name; // unique id
    std::string base_url;
    std::string endpoint;
    std::string model_name;
    std::string api_key;
    int max_context = 4096;
    double temperature = 0.7;
    bool is_default = false;
    long system_prompt_id = 0; // 0 = default
};

struct SystemPrompt {
    long id = 0;
    std::string name;
    std::string prompt;
    bool is_default = false;
};

// SQLite-backed settings store (design §10.2). Lives at
// $XDG_DATA_HOME/monkeybyte/monkeybyte.db (or ~/.local/share/monkeybyte/).
// Note: MCP servers and model/provider configs are NOT stored here — the
// agent reads them from daedalus's own SettingsStore; see backend/McpBridge.h
// and backend/ModelBridge.h.
class SettingsStore {
public:
    SettingsStore();
    explicit SettingsStore(const std::string& db_path);
    ~SettingsStore();

    SettingsStore(const SettingsStore&) = delete;
    SettingsStore& operator=(const SettingsStore&) = delete;

    // generic key/value (workspace path, etc.)
    std::optional<std::string> get(const std::string& key) const;
    void set(const std::string& key, const std::string& value);

    InterfaceSettings load_interface() const;
    void save_interface(const InterfaceSettings& s);

    ChimpFlowSettings load_chimpflow() const;
    void save_chimpflow(const ChimpFlowSettings& s);

    // Search exclude patterns (§4.3): one glob per entry, matched against
    // file names and workspace-relative paths. Persisted under the
    // "search.excludes" key (newline-separated); seeded with defaults on
    // first load.
    std::vector<std::string> load_search_excludes();
    void save_search_excludes(const std::vector<std::string>& patterns);
    static const std::vector<std::string>& default_search_excludes();

    // Agent host-policy lists (§4.5 Agent tab): names of skills / tools the
    // agent must not use. daedalus does not persist these — the host seeds
    // them via AgentConfig::skills_disabled / tools_disabled. Newline-joined
    // under the "agent.disabled_skills" / "agent.disabled_tools" kv keys.
    std::vector<std::string> load_disabled_skills();
    void save_disabled_skills(const std::vector<std::string>& names);
    std::vector<std::string> load_disabled_tools();
    void save_disabled_tools(const std::vector<std::string>& names);

    // Per-model system-prompt binding (§4.5), keyed by the daedalus model
    // name; 0 = default prompt. The model/provider config itself lives in
    // daedalus's SettingsStore (backend/ModelBridge.h).
    long get_model_system_prompt(const std::string& model_name) const; // 0 if unbound
    void set_model_system_prompt(const std::string& model_name, long prompt_id);

    // Migration-only access to the legacy `models` table (dropped after
    // model_bridge::migrate_legacy_models() runs).
    std::vector<LegacyModelConfig> read_legacy_models() const; // empty if table gone
    void drop_legacy_models();

    std::vector<SystemPrompt> list_system_prompts() const;
    void save_system_prompt(const SystemPrompt& p); // id==0 inserts
    void delete_system_prompt(long id);

    static std::string default_db_path();

private:
    void exec(const std::string& sql) const;
    void migrate();

    sqlite3* db_ = nullptr;
};

} // namespace monkeybyte
