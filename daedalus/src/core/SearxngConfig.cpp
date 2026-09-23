// SPDX-License-Identifier: BSL-1.0
// src/core/SearxngConfig.cpp
//
// §SEARXNG-CORE (2026-09-21): SearXNG instance config is stored in the
// SQLite SettingsStore (searxng_config table, schema v7). This file reads
// ONLY from the store — there is no JSON file fallback at runtime.

#include "core/SearxngConfig.h"

#include <cctype>
#include <filesystem>
#include <string>

#include "daedalus/paths.h"
#include "daedalus/settings.h"

namespace daedalus {

namespace fs = std::filesystem;

bool looks_like_valid_searxng_url(const std::string& url) {
    if (url.empty()) return false;
    const bool http  = url.rfind("http://", 0) == 0;
    const bool https = url.rfind("https://", 0) == 0;
    if (!http && !https) return false;
    // Must have something after the scheme.
    if (url.size() <= (https ? 8 : 7)) return false;
    // Disallow whitespace and quotes — a base URL never contains them.
    for (char c : url) {
        if (std::isspace(static_cast<unsigned char>(c))) return false;
        if (c == '"' || c == '\'') return false;
    }
    return true;
}

std::string normalize_searxng_base_url(std::string url) {
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

const char* SearxngConfigResult::status_name() const {
    switch (status) {
        case Status::OK:               return "OK";
        case Status::MISSING_FILE:     return "MISSING_FILE";
        case Status::EMPTY_BASE_URL:   return "EMPTY_BASE_URL";
        case Status::INVALID_BASE_URL: return "INVALID_BASE_URL";
        case Status::DISABLED:         return "DISABLED";
        case Status::IO_ERROR:         return "IO_ERROR";
    }
    return "UNKNOWN";
}

SearxngConfigResult load_searxng_config() {
    // SettingsStore is the sole source of truth (same contract as
    // SerperConfig): searxng_config table, singleton row id=1.
    const auto& p = daedalus::Paths::global();
    fs::path db_dir = p.user_data_root();
    fs::path db_path = db_dir / "daedalus.db";

    SearxngConfigResult r;
    r.settings_path = db_path;

    std::error_code ec;
    if (!fs::exists(db_path, ec)) {
        r.status = SearxngConfigResult::Status::MISSING_FILE;
        r.error_detail = "SettingsStore not found at " + db_path.string();
        return r;
    }

    try {
        auto store = daedalus::SettingsStore::for_home(db_dir);
        SearxngSettings s = store.get_searxng_settings();

        if (s.base_url.empty()) {
            r.status = SearxngConfigResult::Status::EMPTY_BASE_URL;
            r.error_detail = "No SearXNG base_url in SettingsStore at "
                             + db_path.string()
                             + ". Set it via SettingsStore::set_searxng_settings() "
                               "or the host's settings UI.";
            return r;
        }
        if (!looks_like_valid_searxng_url(s.base_url)) {
            r.status = SearxngConfigResult::Status::INVALID_BASE_URL;
            r.error_detail = "SearXNG base_url is not an http(s) URL: "
                             + s.base_url;
            return r;
        }
        if (!s.enabled) {
            r.status = SearxngConfigResult::Status::DISABLED;
            r.error_detail = "SearXNG search is disabled in SettingsStore "
                             "(enabled = 0).";
            return r;
        }
        s.base_url = normalize_searxng_base_url(std::move(s.base_url));
        r.status = SearxngConfigResult::Status::OK;
        r.settings = std::move(s);
        return r;
    } catch (const std::exception& e) {
        r.status = SearxngConfigResult::Status::IO_ERROR;
        r.error_detail = std::string("SettingsStore read failed: ") + e.what();
        return r;
    }
}

}  // namespace daedalus
