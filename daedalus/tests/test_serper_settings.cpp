/**
 * @file test_serper_settings.cpp
 * @brief Regression tests for Serper.dev search tool settings key.
 *
 * The Serper search tool reads its API key from the SQLite SettingsStore
 * (via daedalus::load_serper_api_key() in core/SerperConfig.cpp, the single
 * source of truth). These tests guard against the tool reimplementing its
 * own key lookup.
 *
 * §SERPER-CORE (2026-08-02): SerperSearchTool moved from
 * demos/example-tools/SearchTool.cpp to src/tools/SerperSearchTool.cpp;
 * the grep targets were updated accordingly.
 *
 * The tests are static (source-grep based) because the key lookup is
 * exercised end-to-end by test_serper_startup.cpp.
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
// Test: SerperSearchTool delegates key lookup to the shared SerperConfig
// helper (no reimplemented lookup logic in the tool itself)
// ============================================================================
TEST(searchtool_delegates_to_serper_config) {
    auto root = find_project_root();
    if (root.empty()) {
        std::cerr << "SKIP: could not locate project root\n";
        return 0;
    }
    auto path = root / "src" / "tools" / "SerperSearchTool.cpp";
    if (!std::filesystem::exists(path)) {
        std::cerr << "FAIL: " << path << " not found\n";
        return 1;
    }
    auto content = slurp(path);
    CHECK(!content.empty());
    CHECK(contains(content, "core/SerperConfig.h"));
    CHECK(contains(content, "daedalus::load_serper_api_key()"));
    return 0;
}

// ============================================================================
// Test: SerperSearchTool error message guides the user when the key is missing
// ============================================================================
TEST(searchtool_error_message_mentions_missing_key) {
    auto root = find_project_root();
    if (root.empty()) return 0;
    auto path = root / "src" / "tools" / "SerperSearchTool.cpp";
    if (!std::filesystem::exists(path)) {
        std::cerr << "FAIL: " << path << " not found\n";
        return 1;
    }
    auto content = slurp(path);
    CHECK(!content.empty());
    CHECK(contains(content, "API key not configured"));
    return 0;
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "Running Serper settings tests..." << std::endl;

    int local_failures = 0;
    local_failures += test_searchtool_delegates_to_serper_config();
    local_failures += test_searchtool_error_message_mentions_missing_key();

    std::cout << "\nResults: " << (local_failures == 0 ? "PASS" : "FAIL") << std::endl;
    std::cout << "Total checks: " << test_count << std::endl;
    std::cout << "Failed checks: " << local_failures << std::endl;

    return local_failures > 0 ? 1 : 0;
}
