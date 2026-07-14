/**
 * @file test_serper_startup.cpp
 * @brief Startup-load tests for the Serper.dev API key.
 *
 * These tests verify the *behaviour* of the SerperConfig helper - i.e. that
 * it actually loads ~/.daedalus/settings/settings.json at startup, finds
 * the "serper-search" key, validates it, and reports a precise error if
 * anything is wrong. This is the regression guard for the recurring bug
 * where the serper-search key was silently being dropped from settings.json.
 *
 * Test design:
 *
 *   1. HERMETIC tests (no env, no $HOME): create a temp directory, write
 *      a synthetic settings.json into it, call the helper with that dir
 *      as the candidate, and assert the result. These cover every error
 *      path and every happy path without touching the user's real config.
 *
 *   2. REAL-CONFIG test: call the real production entry point
 *      load_serper_api_key() (which uses $HOME) and assert the result
 *      is OK and the key looks like a valid Serper key. This is the
 *      actual startup scenario the user cares about.
 *
 * The real-config test is skippable in two ways:
 *   - Set DAEDALUS_SKIP_SERPER_STARTUP_TEST=1 to skip unconditionally.
 *   - If the real config is missing/malformed, the test is reported as
 *     FAILED (with a precise, actionable message) - never silently
 *     skipped - because a missing serper key is exactly the bug this
 *     test was written to catch.
 */

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>  // getpid()

#include "core/SerperConfig.h"

using namespace daedalus;
using namespace std;

static int failures = 0;
static int test_count = 0;

#define TEST(name) int test_##name()

#define CHECK(cond) do { \
    test_count++; \
    if (!(cond)) { \
        std::cerr << "  FAIL: " << #cond << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        failures++; \
        return 1; \
    } \
} while(0)

#define CHECK_EQ(a, b) do { \
    test_count++; \
    auto _av = (a); \
    auto _bv = (b); \
    if (!(_av == _bv)) { \
        std::cerr << "  FAIL: " << #a << " == " << #b << " (" << _av << " vs " << _bv \
                  << ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
        failures++; \
        return 1; \
    } \
} while(0)

namespace {

// RAII tempdir: creates a unique temp directory on construction, removes
// it (recursively, best-effort) on destruction. Used to hermetically test
// the loader without touching $HOME.
struct TempDir {
    std::filesystem::path path;

    TempDir() {
        // Use a per-process unique subdir to avoid collisions when ctest
        // runs multiple test binaries in parallel.
        static int counter = 0;
        std::ostringstream name;
        name << "daedalus_serper_test_" << ::getpid() << "_" << ++counter;
        auto base = std::filesystem::temp_directory_path() / name.str();
        std::error_code ec;
        std::filesystem::remove_all(base, ec);  // in case of stale leftover
        std::filesystem::create_directories(base, ec);
        if (ec) {
            std::cerr << "Failed to create tempdir: " << ec.message() << std::endl;
        }
        path = base;
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    // Write a file relative to the tempdir, creating parent dirs as needed.
    void write_file(const std::filesystem::path& rel, const std::string& content) {
        auto full = path / rel;
        std::filesystem::create_directories(full.parent_path());
        std::ofstream f(full, std::ios::binary | std::ios::trunc);
        f << content;
    }
};

// A FAKE, clearly-non-secret 40-char hex string used as the "good" key
// fixture in happy-path tests. This is intentionally an obvious test
// pattern (all 0-f ascending) so it can never be confused with a real
// Serper.dev key and never accidentally be promoted to a default. It
// MUST stay in this file as a fixture - it is not a secret and has no
// value on the Serper.dev service.
//
// IMPORTANT: never paste a real Serper key here. A real key would be
// leaked into git history the moment this file is committed. The lookup
// logic only needs *any* 40-char hex string to exercise the happy path.
const char* kGoodKey = "0123456789abcdef0123456789abcdef01234567";

}  // namespace

// ============================================================================
// HERMETIC TESTS - synthetic settings.json in a tempdir
// ============================================================================

// Happy path: "serper-search" block with a valid key should load.
TEST(load_happy_path_canonical_key) {
    TempDir tmp;
    tmp.write_file("settings/settings.json", std::string(R"({
        "default_model": "minimax-m3",
        "models": {},
        "serper-search": {
            "api_key": ")") + kGoodKey + R"("
        }
    })");

    auto r = load_serper_api_key_from_dirs({tmp.path / "settings"});
    CHECK(r.ok());
    CHECK_EQ(r.status, SerperApiKeyResult::Status::OK);
    CHECK_EQ(r.api_key, std::string(kGoodKey));
    CHECK_EQ(r.status_name(), "OK");
    return 0;
}

// Legacy "serper" key (no hyphen) should still be accepted.
TEST(load_happy_path_legacy_key) {
    TempDir tmp;
    tmp.write_file("settings/settings.json", std::string(R"({
        "serper": {
            "api_key": ")") + kGoodKey + R"("
        }
    })");

    auto r = load_serper_api_key_from_dirs({tmp.path / "settings"});
    CHECK(r.ok());
    CHECK_EQ(r.api_key, std::string(kGoodKey));
    return 0;
}

// MISSING_FILE: when no settings.json exists in the candidate dirs.
TEST(load_missing_file) {
    TempDir tmp;
    // Don't write anything.
    auto r = load_serper_api_key_from_dirs({tmp.path / "settings"});
    CHECK(!r.ok());
    CHECK_EQ(r.status, SerperApiKeyResult::Status::MISSING_FILE);
    CHECK_EQ(r.status_name(), "MISSING_FILE");
    // settings_path should point at the first candidate so the user knows
    // where to create the file.
    CHECK_EQ(r.settings_path, (tmp.path / "settings" / "settings.json"));
    return 0;
}

// MISSING_BLOCK: settings.json exists but has no serper key at all.
TEST(load_missing_serper_block) {
    TempDir tmp;
    tmp.write_file("settings/settings.json", R"({
        "default_model": "minimax-m3",
        "models": {}
    })");
    auto r = load_serper_api_key_from_dirs({tmp.path / "settings"});
    CHECK(!r.ok());
    CHECK_EQ(r.status, SerperApiKeyResult::Status::MISSING_BLOCK);
    CHECK_EQ(r.status_name(), "MISSING_BLOCK");
    return 0;
}

// MISSING_API_KEY_FIELD: block exists but no "api_key" inside.
TEST(load_missing_api_key_field) {
    TempDir tmp;
    tmp.write_file("settings/settings.json", R"({
        "serper-search": {
            "timeout_seconds": 30
        }
    })");
    auto r = load_serper_api_key_from_dirs({tmp.path / "settings"});
    CHECK(!r.ok());
    CHECK_EQ(r.status, SerperApiKeyResult::Status::MISSING_API_KEY_FIELD);
    return 0;
}

// EMPTY_API_KEY: api_key is present but is the empty string.
TEST(load_empty_api_key) {
    TempDir tmp;
    tmp.write_file("settings/settings.json", R"({
        "serper-search": {
            "api_key": ""
        }
    })");
    auto r = load_serper_api_key_from_dirs({tmp.path / "settings"});
    CHECK(!r.ok());
    CHECK_EQ(r.status, SerperApiKeyResult::Status::EMPTY_API_KEY);
    return 0;
}

// PLACEHOLDER_API_KEY: api_key is still the default placeholder.
TEST(load_placeholder_api_key) {
    TempDir tmp;
    tmp.write_file("settings/settings.json", R"({
        "serper-search": {
            "api_key": "YOUR_SERPER_API_KEY"
        }
    })");
    auto r = load_serper_api_key_from_dirs({tmp.path / "settings"});
    CHECK(!r.ok());
    CHECK_EQ(r.status, SerperApiKeyResult::Status::PLACEHOLDER_API_KEY);
    return 0;
}

// INVALID_API_KEY_FORMAT: api_key has whitespace / non-hex / too short.
TEST(load_invalid_format) {
    TempDir tmp;
    tmp.write_file("settings/settings.json", R"({
        "serper-search": {
            "api_key": "not a hex key!"
        }
    })");
    auto r = load_serper_api_key_from_dirs({tmp.path / "settings"});
    CHECK(!r.ok());
    CHECK_EQ(r.status, SerperApiKeyResult::Status::INVALID_API_KEY_FORMAT);
    return 0;
}

// JSON_PARSE_ERROR: settings.json is not valid JSON.
TEST(load_json_parse_error) {
    TempDir tmp;
    tmp.write_file("settings/settings.json", "{ this is not json");
    auto r = load_serper_api_key_from_dirs({tmp.path / "settings"});
    CHECK(!r.ok());
    CHECK_EQ(r.status, SerperApiKeyResult::Status::JSON_PARSE_ERROR);
    return 0;
}

// Priority: the FIRST existing file wins, even if a later one has a valid
// key. This protects against the surprise of the user editing ~/.daedalus/
// settings.json but the agent using a stale ./settings/settings.json.
TEST(load_first_existing_file_wins) {
    TempDir tmp;
    // First dir: present but missing the serper block -> must be authoritative.
    tmp.write_file("primary/settings.json", R"({"default_model": "x"})");
    // Second dir: has a valid key - should be IGNORED.
    tmp.write_file("secondary/settings.json", std::string(R"({
        "serper-search": {"api_key": ")") + kGoodKey + R"("}
    })");

    auto r = load_serper_api_key_from_dirs(
        {tmp.path / "primary", tmp.path / "secondary"});
    CHECK(!r.ok());
    CHECK_EQ(r.status, SerperApiKeyResult::Status::MISSING_BLOCK);
    return 0;
}

// ============================================================================
// REAL CONFIG TEST - exercises the production code path on the user's
// actual ~/.daedalus/settings/settings.json. This is the test the user
// actually cares about: "at startup, the agent's real config must contain
// a valid serper-search key".
// ============================================================================

TEST(real_config_at_startup) {
    // Honor explicit opt-out for CI environments without a configured key.
    if (const char* skip = std::getenv("DAEDALUS_SKIP_SERPER_STARTUP_TEST")) {
        if (skip[0] != '\0' && skip[0] != '0') {
            std::cout << "  [SKIP] DAEDALUS_SKIP_SERPER_STARTUP_TEST is set" << std::endl;
            return 0;
        }
    }

    auto r = load_serper_api_key();

    if (!r.ok()) {
        std::cerr << "\n[SERPER STARTUP TEST FAILURE]\n"
                  << "  Status:   " << r.status_name() << "\n"
                  << "  Detail:   " << r.error_detail << "\n"
                  << "  Path:     " << r.settings_path.string() << "\n"
                  << "\n"
                  << "The serper_search tool will fail at request time until\n"
                  << "this is fixed. To skip this test in CI, set the env var\n"
                  << "DAEDALUS_SKIP_SERPER_STARTUP_TEST=1.\n"
                  << std::endl;
    }

    CHECK(r.ok());
    CHECK_EQ(r.status, SerperApiKeyResult::Status::OK);
    CHECK(looks_like_valid_serper_key(r.api_key));
    return 0;
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "Running Serper startup-load tests..." << std::endl;

    int local_failures = 0;
    local_failures += test_load_happy_path_canonical_key();
    local_failures += test_load_happy_path_legacy_key();
    local_failures += test_load_missing_file();
    local_failures += test_load_missing_serper_block();
    local_failures += test_load_missing_api_key_field();
    local_failures += test_load_empty_api_key();
    local_failures += test_load_placeholder_api_key();
    local_failures += test_load_invalid_format();
    local_failures += test_load_json_parse_error();
    local_failures += test_load_first_existing_file_wins();
    local_failures += test_real_config_at_startup();

    std::cout << "\nResults: " << (local_failures == 0 ? "PASS" : "FAIL") << std::endl;
    std::cout << "Total checks: " << test_count << std::endl;
    std::cout << "Failed checks: " << local_failures << std::endl;

    return local_failures > 0 ? 1 : 0;
}
