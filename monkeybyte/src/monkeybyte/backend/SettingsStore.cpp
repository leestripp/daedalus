#include "monkeybyte/backend/SettingsStore.h"

#include <sqlite3.h>

#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>

namespace monkeybyte {

namespace {

std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : fallback;
}

} // namespace

std::string SettingsStore::default_db_path() {
    const std::string data_home =
        env_or("XDG_DATA_HOME", env_or("HOME", ".") + "/.local/share");
    const std::filesystem::path dir = std::filesystem::path(data_home) / "monkeybyte";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec); // best effort; sqlite3_open
    return (dir / "monkeybyte.db").string();      // reports a real error below
}

SettingsStore::SettingsStore() : SettingsStore(default_db_path()) {}

SettingsStore::SettingsStore(const std::string& db_path) {
    if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
        const std::string msg = db_ ? sqlite3_errmsg(db_) : "unknown";
        throw std::runtime_error("SettingsStore: cannot open " + db_path + ": " + msg);
    }
    migrate();
}

SettingsStore::~SettingsStore() {
    if (db_) sqlite3_close(db_);
}

void SettingsStore::exec(const std::string& sql) const {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        const std::string msg = err ? err : "unknown";
        sqlite3_free(err);
        throw std::runtime_error("SettingsStore SQL error: " + msg);
    }
}

void SettingsStore::migrate() {
    exec(R"sql(
PRAGMA journal_mode=WAL;
PRAGMA busy_timeout=5000;  -- multi-instance: block-and-retry on concurrent writers
CREATE TABLE IF NOT EXISTS settings(
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS model_prompts(
    model_name TEXT PRIMARY KEY,
    system_prompt_id INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS system_prompts(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL UNIQUE,
    prompt TEXT NOT NULL,
    is_default INTEGER NOT NULL DEFAULT 0
);
)sql");
}

namespace {

struct Stmt {
    sqlite3_stmt* p = nullptr;
    ~Stmt() { if (p) sqlite3_finalize(p); }
};

Stmt prepare(sqlite3* db, const std::string& sql) {
    Stmt s;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s.p, nullptr) != SQLITE_OK)
        throw std::runtime_error(std::string("SettingsStore prepare: ") + sqlite3_errmsg(db));
    return s;
}

void bind_text(sqlite3_stmt* st, int idx, const std::string& v) {
    sqlite3_bind_text(st, idx, v.c_str(), -1, SQLITE_TRANSIENT);
}

} // namespace

std::optional<std::string> SettingsStore::get(const std::string& key) const {
    auto st = prepare(db_, "SELECT value FROM settings WHERE key=?1");
    bind_text(st.p, 1, key);
    if (sqlite3_step(st.p) == SQLITE_ROW) {
        const auto* txt = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 0));
        return txt ? std::string(txt) : std::string();
    }
    return std::nullopt;
}

void SettingsStore::set(const std::string& key, const std::string& value) {
    auto st = prepare(db_, "INSERT INTO settings(key,value) VALUES(?1,?2) "
                           "ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    bind_text(st.p, 1, key);
    bind_text(st.p, 2, value);
    if (sqlite3_step(st.p) != SQLITE_DONE)
        throw std::runtime_error("SettingsStore::set failed");
}

const std::vector<std::string>& SettingsStore::default_search_excludes() {
    static const std::vector<std::string> defaults = {
        "build",        "_build",   "cmake-build-*", "node_modules",
        "__pycache__",  "target",   "dist",          "*.o",
        "*.a",          "*.so",     "*.dylib",       "*.exe",
        "*.dll",        "*.class",  "*.pyc",         "*.min.js",
        "*.min.css",
    };
    return defaults;
}

std::vector<std::string> SettingsStore::load_search_excludes() {
    auto stored = get("search.excludes");
    if (!stored) { // first run: seed the saved settings with the defaults
        save_search_excludes(default_search_excludes());
        stored = get("search.excludes");
    }
    std::vector<std::string> out;
    std::string line;
    std::istringstream in(*stored);
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

void SettingsStore::save_search_excludes(const std::vector<std::string>& patterns) {
    std::string joined;
    for (const auto& p : patterns) {
        if (!joined.empty()) joined += '\n';
        joined += p;
    }
    set("search.excludes", joined);
}

namespace {

std::vector<std::string> split_lines(const std::string& blob) {
    std::vector<std::string> out;
    std::string line;
    std::istringstream in(blob);
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

std::string join_lines(const std::vector<std::string>& items) {
    std::string out;
    for (const auto& s : items) {
        if (!out.empty()) out += '\n';
        out += s;
    }
    return out;
}

} // namespace

std::vector<std::string> SettingsStore::load_disabled_skills() {
    return split_lines(get("agent.disabled_skills").value_or(""));
}

void SettingsStore::save_disabled_skills(const std::vector<std::string>& names) {
    set("agent.disabled_skills", join_lines(names));
}

std::vector<std::string> SettingsStore::load_disabled_tools() {
    return split_lines(get("agent.disabled_tools").value_or(""));
}

void SettingsStore::save_disabled_tools(const std::vector<std::string>& names) {
    set("agent.disabled_tools", join_lines(names));
}

InterfaceSettings SettingsStore::load_interface() const {
    // Corrupt values must not crash callers (~15 GTK callbacks reach this):
    // fall back to the struct default on unparseable numbers.
    auto to_int = [](const std::string& v, int fallback) {
        try {
            return std::stoi(v);
        } catch (...) {
            return fallback;
        }
    };
    InterfaceSettings s;
    if (auto v = get("font_size")) s.font_size = to_int(*v, s.font_size);
    if (auto v = get("tab_size")) s.tab_size = to_int(*v, s.tab_size);
    if (auto v = get("word_wrap")) s.word_wrap = (*v == "1" || *v == "true");
    if (auto v = get("theme")) s.theme = *v;
    if (auto v = get("show_hidden")) s.show_hidden = (*v == "1" || *v == "true");
    if (auto v = get("show_line_numbers"))
        s.show_line_numbers = (*v == "1" || *v == "true");
    if (auto v = get("highlight_current_line"))
        s.highlight_current_line = (*v == "1" || *v == "true");
    if (auto v = get("browser_home_url")) s.browser_home_url = *v;
    if (auto v = get("chat_font_size")) s.chat_font_size = to_int(*v, s.chat_font_size);
    if (auto v = get("chat.bg")) s.chat_bg = *v;
    if (auto v = get("chat.user_bg")) s.chat_user_bg = *v;
    if (auto v = get("chat.user_text")) s.chat_user_text = *v;
    if (auto v = get("chat.assistant_bg")) s.chat_assistant_bg = *v;
    if (auto v = get("chat.assistant_text")) s.chat_assistant_text = *v;
    return s;
}

void SettingsStore::save_interface(const InterfaceSettings& s) {
    set("font_size", std::to_string(s.font_size));
    set("tab_size", std::to_string(s.tab_size));
    set("word_wrap", s.word_wrap ? "1" : "0");
    set("theme", s.theme);
    set("show_hidden", s.show_hidden ? "1" : "0");
    set("show_line_numbers", s.show_line_numbers ? "1" : "0");
    set("highlight_current_line", s.highlight_current_line ? "1" : "0");
    set("browser_home_url", s.browser_home_url);
    set("chat_font_size", std::to_string(s.chat_font_size));
    set("chat.bg", s.chat_bg);
    set("chat.user_bg", s.chat_user_bg);
    set("chat.user_text", s.chat_user_text);
    set("chat.assistant_bg", s.chat_assistant_bg);
    set("chat.assistant_text", s.chat_assistant_text);
}

ChimpFlowSettings SettingsStore::load_chimpflow() const {
    ChimpFlowSettings s;
    const auto boolean = [](const std::string& value, bool fallback) {
        if (value == "1" || value == "true") return true;
        if (value == "0" || value == "false") return false;
        return fallback;
    };
    if (auto v = get("chimpflow.enabled")) s.enabled = boolean(*v, s.enabled);
    if (auto v = get("chimpflow.read_assistant"))
        s.read_assistant = boolean(*v, s.read_assistant);
    if (auto v = get("chimpflow.dictation_enabled"))
        s.dictation_enabled = boolean(*v, s.dictation_enabled);
    if (auto v = get("chimpflow.server_url")) s.server_url = *v;
    if (auto v = get("chimpflow.port")) {
        try {
            s.port = std::stoi(*v);
        } catch (...) {
            // Keep the safe default on corrupt values, matching load_interface().
        }
    }
    if (auto v = get("chimpflow.api_key")) s.api_key = *v;
    if (auto v = get("chimpflow.tts_model")) s.tts_model = *v;
    if (auto v = get("chimpflow.stt_model")) s.stt_model = *v;
    if (auto v = get("chimpflow.voice")) s.voice = *v;
    return s;
}

void SettingsStore::save_chimpflow(const ChimpFlowSettings& s) {
    set("chimpflow.enabled", s.enabled ? "1" : "0");
    set("chimpflow.read_assistant", s.read_assistant ? "1" : "0");
    set("chimpflow.dictation_enabled", s.dictation_enabled ? "1" : "0");
    set("chimpflow.server_url", s.server_url);
    set("chimpflow.port", std::to_string(s.port));
    set("chimpflow.api_key", s.api_key);
    set("chimpflow.tts_model", s.tts_model);
    set("chimpflow.stt_model", s.stt_model);
    set("chimpflow.voice", s.voice);
}

std::vector<LegacyModelConfig> SettingsStore::read_legacy_models() const {
    std::vector<LegacyModelConfig> out;
    {
        auto st = prepare(db_, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='models'");
        if (sqlite3_step(st.p) != SQLITE_ROW) return out; // table already dropped
    }
    auto st = prepare(db_, "SELECT provider_name,base_url,endpoint,model_name,api_key,"
                           "max_context,temperature,is_default,system_prompt_id FROM models "
                           "ORDER BY provider_name");
    while (sqlite3_step(st.p) == SQLITE_ROW) {
        LegacyModelConfig m;
        const auto col = [&](int i) {
            const auto* t = reinterpret_cast<const char*>(sqlite3_column_text(st.p, i));
            return t ? std::string(t) : std::string();
        };
        m.provider_name = col(0);
        m.base_url = col(1);
        m.endpoint = col(2);
        m.model_name = col(3);
        m.api_key = col(4);
        m.max_context = sqlite3_column_int(st.p, 5);
        m.temperature = sqlite3_column_double(st.p, 6);
        m.is_default = sqlite3_column_int(st.p, 7) != 0;
        m.system_prompt_id = sqlite3_column_int64(st.p, 8);
        out.push_back(std::move(m));
    }
    return out;
}

void SettingsStore::drop_legacy_models() {
    exec("DROP TABLE IF EXISTS models");
}

long SettingsStore::get_model_system_prompt(const std::string& model_name) const {
    auto st = prepare(db_, "SELECT system_prompt_id FROM model_prompts WHERE model_name=?1");
    bind_text(st.p, 1, model_name);
    if (sqlite3_step(st.p) == SQLITE_ROW)
        return sqlite3_column_int64(st.p, 0);
    return 0;
}

void SettingsStore::set_model_system_prompt(const std::string& model_name, long prompt_id) {
    auto st = prepare(db_,
        "INSERT INTO model_prompts(model_name,system_prompt_id) VALUES(?1,?2) "
        "ON CONFLICT(model_name) DO UPDATE SET system_prompt_id=excluded.system_prompt_id");
    bind_text(st.p, 1, model_name);
    sqlite3_bind_int64(st.p, 2, prompt_id);
    if (sqlite3_step(st.p) != SQLITE_DONE)
        throw std::runtime_error("SettingsStore::set_model_system_prompt failed");
}

std::vector<SystemPrompt> SettingsStore::list_system_prompts() const {
    auto st = prepare(db_, "SELECT id,name,prompt,is_default FROM system_prompts ORDER BY id");
    std::vector<SystemPrompt> out;
    while (sqlite3_step(st.p) == SQLITE_ROW) {
        SystemPrompt p;
        p.id = sqlite3_column_int64(st.p, 0);
        const auto col = [&](int i) {
            const auto* t = reinterpret_cast<const char*>(sqlite3_column_text(st.p, i));
            return t ? std::string(t) : std::string();
        };
        p.name = col(1);
        p.prompt = col(2);
        p.is_default = sqlite3_column_int(st.p, 3) != 0;
        out.push_back(std::move(p));
    }
    return out;
}

void SettingsStore::save_system_prompt(const SystemPrompt& p) {
    if (p.is_default) exec("UPDATE system_prompts SET is_default=0");
    if (p.id == 0) {
        auto st = prepare(db_,
            "INSERT INTO system_prompts(name,prompt,is_default) VALUES(?1,?2,?3) "
            "ON CONFLICT(name) DO UPDATE SET prompt=excluded.prompt,"
            "is_default=excluded.is_default");
        bind_text(st.p, 1, p.name);
        bind_text(st.p, 2, p.prompt);
        sqlite3_bind_int(st.p, 3, p.is_default ? 1 : 0);
        if (sqlite3_step(st.p) != SQLITE_DONE)
            throw std::runtime_error("SettingsStore::save_system_prompt failed");
    } else {
        auto st = prepare(db_,
            "UPDATE system_prompts SET name=?1,prompt=?2,is_default=?3 WHERE id=?4");
        bind_text(st.p, 1, p.name);
        bind_text(st.p, 2, p.prompt);
        sqlite3_bind_int(st.p, 3, p.is_default ? 1 : 0);
        sqlite3_bind_int64(st.p, 4, p.id);
        sqlite3_step(st.p);
    }
}

void SettingsStore::delete_system_prompt(long id) {
    auto st = prepare(db_, "DELETE FROM system_prompts WHERE id=?1");
    sqlite3_bind_int64(st.p, 1, id);
    sqlite3_step(st.p);
}

} // namespace monkeybyte
