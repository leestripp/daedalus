#include "core/SerperConfig.h"

#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

#include "daedalus/paths.h"
#include "daedalus/settings.h"
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
using json = nlohmann::json;

namespace daedalus {

namespace fs = std::filesystem;

namespace {

// Known placeholder values that the example settings file uses. The lookup
// treats any of these as "not configured".
constexpr const char* kPlaceholderValues[] = {
    "YOUR_SERPER_API_KEY",
    "your-serper-api-key",
    "your_serper_api_key",
    "CHANGEME",
    "changeme",
    "<your-api-key>",
};

// The candidate settings.json paths, in priority order. The first existing
// file wins.
//
// §FIX-XDG-STORAGE (2026-07-17): also probe for `config.json` in the
// XDG config root. Pre-v0.4.0 XDG users had `settings.json`; v0.4.0+
// renamed it to `config.json` (see CHANGELOG.md v0.4.0 entry). Both
// names should be probed during the migration window so a v0.4.0 user
// who hasn't yet run --migrate-data still gets their serper key
// loaded.
//
// Returned entries are DIRECTORIES (the caller probes for files inside
// each one), so we drop the trailing file component of config_file().
std::vector<std::filesystem::path> candidate_settings_paths() {
    std::vector<std::filesystem::path> paths;

    const auto& p = daedalus::Paths::global();

    // 1. Legacy $HOME/.daedalus/settings  (Plan.md §3.4 read-fallback).
    //    Only include if Paths::global().user_root() resolved there —
    //    otherwise we're already on the XDG path and the legacy dir
    //    doesn't exist (so probing it would just cost a stat()).
    fs::path legacy_settings_dir = p.user_root() / "settings";
    std::error_code ec;
    if (fs::is_directory(legacy_settings_dir, ec)) {
        paths.emplace_back(std::move(legacy_settings_dir));
    }

    // 2. XDG-compliant $XDG_CONFIG_HOME/daedalus (or $HOME/.config/daedalus).
    paths.emplace_back(p.user_config_root());

    // Local project paths come last (lowest priority).
    auto cwd = fs::current_path(ec);
    if (!ec) {
        paths.push_back(cwd / "settings");
    }
    paths.push_back(fs::absolute("settings"));

    return paths;
}

bool is_placeholder(const std::string& v) {
    for (const auto* ph : kPlaceholderValues) {
        if (v == ph) return true;
    }
    // Also catch anything that still ends with _API_KEY (case-insensitive).
    if (v.size() >= 8) {
        std::string tail = v.substr(v.size() - 8);
        for (auto& c : tail) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (tail == "_api_key") return true;
    }
    return false;
}

}  // namespace

bool looks_like_valid_serper_key(const std::string& key) {
    if (key.empty()) return false;
    if (is_placeholder(key)) return false;
    // Disallow whitespace, quotes, and JSON structural characters - a
    // valid Serper key is just hex.
    for (char c : key) {
        if (std::isspace(static_cast<unsigned char>(c))) return false;
        if (c == '"' || c == '\'' || c == '{' || c == '}' ||
            c == '[' || c == ']' || c == ',' || c == ':') return false;
    }
    // Require at least 32 hex characters (Serper keys are 40, but be
    // forgiving so a future format change doesn't fail this test).
    size_t hex_count = 0;
    for (char c : key) {
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
            ++hex_count;
        } else {
            return false;  // any non-hex char disqualifies
        }
    }
    return hex_count >= 32;
}

const char* SerperApiKeyResult::status_name() const {
    switch (status) {
        case Status::OK:                     return "OK";
        case Status::MISSING_FILE:           return "MISSING_FILE";
        case Status::MISSING_BLOCK:          return "MISSING_BLOCK";
        case Status::MISSING_API_KEY_FIELD:  return "MISSING_API_KEY_FIELD";
        case Status::EMPTY_API_KEY:          return "EMPTY_API_KEY";
        case Status::PLACEHOLDER_API_KEY:    return "PLACEHOLDER_API_KEY";
        case Status::INVALID_API_KEY_FORMAT: return "INVALID_API_KEY_FORMAT";
        case Status::JSON_PARSE_ERROR:       return "JSON_PARSE_ERROR";
        case Status::IO_ERROR:               return "IO_ERROR";
    }
    return "UNKNOWN";
}

namespace {

// Core loader. Given a path to a settings.json, attempt to read & parse it
// and pull out the serper key. Returns IO_ERROR for open failures,
// JSON_PARSE_ERROR for bad JSON, MISSING_BLOCK if no serper key at all,
// etc.
SerperApiKeyResult load_from_path(const std::filesystem::path& settings_file) {
    SerperApiKeyResult r;
    r.settings_path = settings_file;

    std::ifstream f(settings_file);
    if (!f.is_open()) {
        r.status = SerperApiKeyResult::Status::IO_ERROR;
        r.error_detail = "Failed to open: " + settings_file.string();
        return r;
    }

    json j;
    try {
        f >> j;
    } catch (const json::parse_error& e) {
        r.status = SerperApiKeyResult::Status::JSON_PARSE_ERROR;
        r.error_detail = std::string(e.what());
        return r;
    }

    if (!j.is_object()) {
        r.status = SerperApiKeyResult::Status::JSON_PARSE_ERROR;
        r.error_detail = "Top-level JSON is not an object";
        return r;
    }

    // Accept either "serper-search" (canonical) or "serper" (legacy).
    static const std::array<const char*, 2> kKeyCandidates = {"serper-search", "serper"};

    const json* block = nullptr;
    const char* used_key_name = nullptr;
    for (const char* k : kKeyCandidates) {
        auto it = j.find(k);
        if (it != j.end() && it->is_object()) {
            block = &(*it);
            used_key_name = k;
            break;
        }
    }

    if (block == nullptr) {
        r.status = SerperApiKeyResult::Status::MISSING_BLOCK;
        r.error_detail =
            "No \"serper-search\" or \"serper\" object in " + settings_file.string();
        return r;
    }

    auto ait = block->find("api_key");
    if (ait == block->end()) {
        r.status = SerperApiKeyResult::Status::MISSING_API_KEY_FIELD;
        r.error_detail =
            "Block \"" + std::string(used_key_name) +
            "\" exists but has no \"api_key\" field in " + settings_file.string();
        return r;
    }

    if (!ait->is_string()) {
        r.status = SerperApiKeyResult::Status::INVALID_API_KEY_FORMAT;
        r.error_detail = "\"api_key\" is not a string";
        return r;
    }

    std::string key = ait->get<std::string>();
    if (key.empty()) {
        r.status = SerperApiKeyResult::Status::EMPTY_API_KEY;
        r.error_detail = "\"api_key\" is an empty string";
        return r;
    }

    if (is_placeholder(key)) {
        r.status = SerperApiKeyResult::Status::PLACEHOLDER_API_KEY;
        r.error_detail =
            "\"api_key\" is still the placeholder value: \"" + key + "\"";
        return r;
    }

    if (!looks_like_valid_serper_key(key)) {
        r.status = SerperApiKeyResult::Status::INVALID_API_KEY_FORMAT;
        r.error_detail = "\"api_key\" does not look like a valid Serper API key";
        return r;
    }

    r.status = SerperApiKeyResult::Status::OK;
    r.api_key = std::move(key);
    return r;
}

// §Plan.md Phase 4 (v0.5.0+): prefer the SQLite-backed SettingsStore
// (which is migrated from JSON via SettingsMigrator on first run).
// Only fall back to reading a legacy settings.json file if the store
// doesn't have a key. The store path is the source of truth for v0.5.0+.
SerperApiKeyResult load_from_settings_store(const std::filesystem::path& db_path) {
    SerperApiKeyResult r;
    r.settings_path = db_path;  // point at the DB so error messages are useful
    try {
        // §B2 fix (v0.5.1): for_home() returns by value. The local copy
        // shares the DbPool connection — safe to move around.
        auto store = daedalus::SettingsStore::for_home(db_path.parent_path());
        std::string key = store.get_serper_api_key();
        if (key.empty()) {
            r.status = SerperApiKeyResult::Status::MISSING_BLOCK;
            r.error_detail = "No Serper API key in SettingsStore (" + db_path.string() + ")";
            return r;
        }
        if (is_placeholder(key)) {
            r.status = SerperApiKeyResult::Status::PLACEHOLDER_API_KEY;
            r.error_detail = "Serper API key in SettingsStore is still a placeholder";
            return r;
        }
        if (!looks_like_valid_serper_key(key)) {
            r.status = SerperApiKeyResult::Status::INVALID_API_KEY_FORMAT;
            r.error_detail = "Serper API key in SettingsStore does not look valid";
            return r;
        }
        r.status = SerperApiKeyResult::Status::OK;
        r.api_key = std::move(key);
        return r;
    } catch (const std::exception& e) {
        r.status = SerperApiKeyResult::Status::IO_ERROR;
        r.error_detail = std::string("SettingsStore read failed: ") + e.what();
        return r;
    }
}

}  // namespace

// Forward declaration so load_serper_api_key() can delegate to the
// test-friendly variant defined below.
SerperApiKeyResult load_serper_api_key_from_dirs(
    const std::vector<std::filesystem::path>& candidate_dirs);

SerperApiKeyResult load_serper_api_key() {
    // §Plan.md Phase 4 (v0.5.0+): the SettingsStore is the source of truth.
    // SettingsMigrator runs on first DB open and migrates the serper key
    // from the legacy settings.json into the store. So the production code
    // path here is: try the store first, then fall back to JSON for
    // pre-migration users.
    const auto& p = daedalus::Paths::global();
    fs::path db_dir = p.user_data_root();
    fs::path db_path = db_dir / "daedalus.db";
    std::error_code ec;
    if (fs::exists(db_path, ec)) {
        auto r = load_from_settings_store(db_path);
        if (r.ok()) {
            return r;
        }
        // OK/MISSING_BLOCK/etc — fall through to JSON. Only IO_ERROR
        // (DB is unreachable) should skip the JSON fallback; for any
        // other status the JSON file may have a fresher copy.
        if (r.status != SerperApiKeyResult::Status::IO_ERROR) {
            auto json_r = load_serper_api_key_from_dirs({});
            if (json_r.ok()) {
                // Promote the JSON key into the store so future loads
                // don't need the fallback path.
                try {
                    // §B2 fix (v0.5.1): for_home() returns by value — use auto.
                auto store = daedalus::SettingsStore::for_home(db_dir);
                    store.set_serper_api_key(json_r.api_key);
                } catch (...) {
                    // Non-fatal: next start will try the same fallback.
                }
                return json_r;
            }
            // If JSON also fails, prefer the store's status (more specific).
            return r.status == SerperApiKeyResult::Status::MISSING_BLOCK
                       ? json_r  // MISSING_FILE from JSON if no JSON either
                       : r;
        }
    }

    return load_serper_api_key_from_dirs({});
}

SerperApiKeyResult load_serper_api_key_from_dirs(
    const std::vector<std::filesystem::path>& candidate_dirs) {

    // Fall back to the production candidate list when the caller passes
    // an empty vector (used by load_serper_api_key()).
    std::vector<std::filesystem::path> dirs = candidate_dirs;
    if (dirs.empty()) {
        dirs = candidate_settings_paths();
    }

    // First, report where we looked. If nothing exists, return MISSING_FILE
    // with settings_path set to the first candidate (the user's primary
    // config location) so the error message tells them where to create it.
    //
    // §FIX-XDG-STORAGE (2026-07-17): probe BOTH `settings.json` (legacy,
    // pre-v0.4.0 filename) AND `config.json` (v0.4.0+ canonical name) in
    // each candidate directory. The legacy name lived in `~/.daedalus/settings/`;
    // post-v0.4.0 the canonical XDG file is `<user_config_root>/config.json`
    // (see SettingsMigrator and docs/storage.md §1.2).
    std::filesystem::path first_path;
    bool any_existing = false;
    for (const auto& dir : dirs) {
        // Probe both names. `settings.json` is checked first because it's
        // the more specific filename in legacy paths and matches what
        // SerperConfig always used historically. `config.json` is checked
        // second so v0.4.0+ users with the canonical layout get a match.
        for (const char* name : {"settings.json", "config.json"}) {
            auto p = dir / name;
            if (first_path.empty()) first_path = p;
            std::error_code ec;
            if (std::filesystem::exists(p, ec)) {
                any_existing = true;
                auto r = load_from_path(p);
                // If the file is present but missing the serper block, treat
                // that as the authoritative answer for that file (don't fall
                // through to a lower-priority file with a valid key). The
                // user's primary config is the source of truth - silently
                // falling through to a secondary file would be confusing.
                return r;
            }
        }
    }

    SerperApiKeyResult r;
    r.status = any_existing
                   ? SerperApiKeyResult::Status::IO_ERROR
                   : SerperApiKeyResult::Status::MISSING_FILE;
    r.settings_path = first_path;
    if (r.status == SerperApiKeyResult::Status::MISSING_FILE) {
        r.error_detail = "No settings.json or config.json found. Checked:";
        for (const auto& dir : dirs) {
            for (const char* name : {"settings.json", "config.json"}) {
                r.error_detail += "\n    - " + (dir / name).string();
            }
        }
    }
    return r;
}

bool check_serper_api_key_at_startup() {
    SerperApiKeyResult r = load_serper_api_key();

    if (r.ok()) {
        // Do NOT print any portion of the key - not even a prefix. Even
        // a partial key in a log line is a credential leak. The path is
        // sufficient for the user to verify which config was loaded.
        // §Phase 5.1: route via Logger.
        LOG_INFO("[SERPER] API key loaded (path: "
                 + r.settings_path.string() + ")");
        return true;
    }

    LOG_CERR(INFO, "\n[SERPER] WARNING: " << r.status_name() << ": "
              << r.error_detail);
    LOG_CERR(INFO, "[SERPER] The serper_search tool will fail at request time "
              << "until this is fixed.");

    if (r.status == SerperApiKeyResult::Status::MISSING_FILE) {
        LOG_CERR(INFO, "[SERPER] Create or edit: " << r.settings_path);
        LOG_CERR(INFO,  "[SERPER] And add a top-level block like:\n"
                  << "    \"serper-search\": {\n"
                  << "        \"api_key\": \"<your-40-char-hex-key>\"\n"
                  << "    }\n");
    } else {
        LOG_CERR(INFO, "[SERPER] Edit: " << r.settings_path);
        LOG_CERR(INFO,  "[SERPER] And ensure it contains:\n"
                  << "    \"serper-search\": {\n"
                  << "        \"api_key\": \"<your-40-char-hex-key>\"\n"
                  << "    }\n");
    }
    LOG_CERR(INFO,  '\n');
    return false;
}

}  // namespace daedalus
