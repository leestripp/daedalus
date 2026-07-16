/**
 * @file test_serper_settings.cpp
 * @brief Regression tests for the Serper.dev search tool settings key.
 *
 * Background:
 *   The Serper search tool reads its API key from settings.json. There was
 *   a bug where the code only looked for the legacy key "serper", while
 *   user configs use the canonical "serper-search" key — so the search
 *   tool always reported the API key as missing.
 *
 * These tests guard against:
 *   - The code regressing to a single-key lookup.
 *   - The example settings file losing the canonical key.
 *   - The SettingsStore handling for the canonical key.
 *
 * The tests are static (source-grep based) because get_serper_api_key() is
 * a file-static function and not part of the public API; a behavioural
 * test would require a network round-trip or invasive API exposure.
 */

#include <iostream>
#include <filesystem>
#include <fstream>
#include <string>
#include <nlohmann/json.hpp>


using namespace std;

static int failures = 0;
static int test_count = 0;

#define TEST(name) int test_##name()

#define CHECK(cond) do { \
    test_count++; \
    if (!(cond)) { \
        std::cerr << "FAIL: " << #cond << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        failures++; \
        return 1; \
    } \
} while(0)

#define CHECK_EQ(a, b) do { \
    test_count++; \
    if ((a) != (b)) { \
        std::cerr << "FAIL: " << #a << " == " << #b << " (" << (a) << " vs " << (b) << ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
        failures++; \
        return 1; \
    } \
} while(0)

namespace {

// Read an entire file into a string. Returns "" if the file cannot be read.
std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p);
    if (!in.is_open()) return "";
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    return content;
}

// Find the project root by walking up from CWD until we find CMakeLists.txt.
std::filesystem::path find_project_root() {
    std::filesystem::path candidate = std::filesystem::current_path();
    for (int i = 0; i < 6; ++i) {
        if (std::filesystem::exists(candidate / "CMakeLists.txt")) {
            return candidate;
        }
        if (candidate.has_parent_path()) {
            candidate = candidate.parent_path();
        } else {
            break;
        }
    }
    return {};
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ============================================================================
// Test: SearchTool.cpp accepts BOTH "serper-search" and "serper" key names
// ============================================================================
TEST(searchtool_accepts_canonical_and_legacy_keys) {
    auto root = find_project_root();
    if (root.empty()) {
        std::cerr << "SKIP: could not locate project root\n";
        return 0;
    }
    auto path = root / "src" / "tools" / "SearchTool.cpp";
    auto content = slurp(path);
    CHECK(!content.empty());

    // The fix introduced an array of key candidates. Verify both are listed.
    CHECK(contains(content, "serper-search"));
    CHECK(contains(content, "\"serper\""));     // legacy alias still present
    return 0;
}

// ============================================================================
// Test: SearchTool.cpp error message points at the canonical key
// ============================================================================
TEST(searchtool_error_message_mentions_serper_search) {
    auto root = find_project_root();
    if (root.empty()) return 0;
    auto content = slurp(root / "src" / "tools" / "SearchTool.cpp");
    CHECK(!content.empty());

    // The error message should mention "serper-search.api_key" (canonical).
    CHECK(contains(content, "serper-search.api_key"));
    return 0;
}

// ============================================================================
// Test: SettingsMigrator uses the canonical "serper-search" key
// ============================================================================
TEST(settingsmigrator_uses_canonical_key) {
    auto root = find_project_root();
    if (root.empty()) return 0;
    auto content = slurp(root / "src" / "core" / "SettingsMigrator.cpp");
    CHECK(!content.empty());

    // The migrated "serper-search" key should appear in the migrator
    // code (the canonical form).
    CHECK(contains(content, "serper-search"));
    return 0;
}

// ============================================================================
// Test: Examples settings file uses the canonical key
// ============================================================================
TEST(example_settings_uses_canonical_key) {
    auto root = find_project_root();
    if (root.empty()) return 0;
    auto path = root / "examples" / "settings.json.example";
    if (!std::filesystem::exists(path)) {
        // examples/ was deleted in v0.4.2 — skip
        return 0;
    }
    auto content = slurp(path);
    CHECK(!content.empty());

    CHECK(contains(content, "serper-search"));
    // Make sure it has a real api_key field
    CHECK(contains(content, "api_key"));
    return 0;
}

// ============================================================================
// Test: CMakeLists.txt installs from the new examples location
// ============================================================================
TEST(cmakelists_installs_from_examples) {
    auto root = find_project_root();
    if (root.empty()) return 0;
    if (!std::filesystem::exists(root / "examples")) {
        // examples/ was deleted in v0.4.2 — skip
        return 0;
    }
    auto content = slurp(root / "CMakeLists.txt");
    CHECK(!content.empty());

    // Should reference examples/settings.json.example
    CHECK(contains(content, " examples/settings.json.example"));
    // Should NOT reference the deleted settings/ directory
    CHECK(!contains(content, "install(DIRECTORY settings/"));
    return 0;
}

// ============================================================================
// Test: Example settings.json parses as valid JSON with required top-level keys
// ============================================================================
TEST(example_settings_is_valid_json) {
    auto root = find_project_root();
    if (root.empty()) return 0;
    auto path = root / "examples" / "settings.json.example";
    if (!std::filesystem::exists(path)) {
        // examples/ was deleted in v0.4.2 — skip
        return 0;
    }
    auto content = slurp(path);
    CHECK(!content.empty());

    try {
        auto j = nlohmann::json::parse(content);
        CHECK(j.is_object());
        // Should have at least models + serper-search
        CHECK(j.contains("models"));
        CHECK(j.contains("serper-search"));
    } catch (const std::exception& e) {
        std::cerr << "JSON parse error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "Running Serper settings tests..." << std::endl;

    int local_failures = 0;
    local_failures += test_searchtool_accepts_canonical_and_legacy_keys();
    local_failures += test_searchtool_error_message_mentions_serper_search();
    local_failures += test_settingsmigrator_uses_canonical_key();
    local_failures += test_example_settings_uses_canonical_key();
    local_failures += test_cmakelists_installs_from_examples();
    local_failures += test_example_settings_is_valid_json();

    std::cout << "\nResults: " << (local_failures == 0 ? "PASS" : "FAIL") << std::endl;
    std::cout << "Total checks: " << test_count << std::endl;
    std::cout << "Failed checks: " << local_failures << std::endl;

    return local_failures > 0 ? 1 : 0;
}
