#pragma once

#include <filesystem>
#include <ostream>
#include <string>

#include "daedalus/settings.h"

namespace daedalus {

/**
 * @brief Result of looking up the SearXNG instance config from the
 * SettingsStore.
 *
 * Single source of truth for the agent's view of whether the searxng_search
 * tool is properly configured. Populated by load_searxng_config() and
 * consumed by:
 *   - SearxngSearchTool::execute() (to fetch the config at request time)
 *   - tests (to assert configuration correctness)
 *
 * Status values are deliberately explicit (not just bool) so the caller can
 * produce a precise, actionable error message, mirroring SerperApiKeyResult.
 */
struct SearxngConfigResult {
    enum class Status {
        OK,               //!< Config found and looks usable
        MISSING_FILE,     //!< No SettingsStore DB at the expected path
        EMPTY_BASE_URL,   //!< Never configured, or base_url is empty
        INVALID_BASE_URL, //!< base_url is not an http(s) URL
        DISABLED,         //!< Row present but enabled = 0
        IO_ERROR,         //!< Store exists but couldn't be read
    };

    Status status = Status::MISSING_FILE;
    SearxngSettings settings;                  //!< Populated only when status == OK
    std::filesystem::path settings_path;       //!< The store path that produced the result
    std::string error_detail;                  //!< Human-readable detail for non-OK statuses

    bool ok() const { return status == Status::OK; }

    //! Short status tag, suitable for logs and assertion messages.
    const char* status_name() const;
};

//! Stream output for SearxngConfigResult::Status - prints the status_name()
//! so test failures and logs show the enum value rather than an integer.
inline std::ostream& operator<<(std::ostream& os,
                                SearxngConfigResult::Status s) {
    SearxngConfigResult r;
    r.status = s;
    return os << r.status_name();
}

/**
 * @brief Load and validate the SearXNG instance config from the
 * SettingsStore (`searxng_config` table, singleton row id=1).
 *
 * The returned result has `settings_path` set to the SettingsStore path
 * that produced the result.
 */
SearxngConfigResult load_searxng_config();

//! Returns true if `url` looks like a plausible SearXNG base URL:
//! starts with http:// or https://, no whitespace or quotes.
bool looks_like_valid_searxng_url(const std::string& url);

//! Strip trailing '/' characters so callers can append "/search?...".
std::string normalize_searxng_base_url(std::string url);

}  // namespace daedalus
