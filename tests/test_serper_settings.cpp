/**
 * @file test_serper_settings.cpp
 * @brief Regression tests for Serper.dev search tool settings key.
 *
 * The Serper search tool reads its API key from the SQLite SettingsStore
 * (via SettingsStore::get_serper_api_key()). These tests guard against
 * the code regressing to a single-key lookup.
 *
 * The tests are static (source-grep based) because get_serper_api_key() is
 * a file-static function and not part of the public API.
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

namespace {

// Read an entire file into a string.
std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p);
    if (!in.is_open()) return "";
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    return content;
}

// Find the project root by walking up from CWD.
std::filesystem::path find_project_root() {
    constexpr int kMaxLevels = 8;
    constexpr const char* kSentinel = "demos/example-tools/SearchTool.cpp";
    std::filesystem::path candidate = std::filesystem::current_path();
    for (int i = 0; i < kMaxLevels; ++i) {
        if (std::filesystem::exists(candidate / kSentinel)) {
            return candidate;
        }
        if (!candidate.has_parent_path()) break;
        candidate = candidate.parent_path();
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
    auto path = root / "demos" / "example-tools" / "SearchTool.cpp";
    if (!std::filesystem::exists(path)) {
        std::cerr << "SKIP: " << path << " not found\n";
        return 0;
    }
    auto content = slurp(path);
    CHECK(!content.empty());
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
    auto path = root / "demos" / "example-tools" / "SearchTool.cpp";
    if (!std::filesystem::exists(path)) {
        std::cerr << "SKIP: " << path << " not found\n";
        return 0;
    }
    auto content = slurp(path);
    CHECK(!content.empty());
    CHECK(contains(content, "serper-search.api_key"));
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

    std::cout << "\nResults: " << (local_failures == 0 ? "PASS" : "FAIL") << std::endl;
    std::cout << "Total checks: " << test_count << std::endl;
    std::cout << "Failed checks: " << local_failures << std::endl;

    return local_failures > 0 ? 1 : 0;
}
