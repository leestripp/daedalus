/**
 * @file test_edit_file_patch.cpp
 * @brief Unit tests for the rewritten EditFilePatchTool
 *
 * Tests cover all matching strategies and error handling:
 * - Exact substring match
 * - Line-normalized match (whitespace flexible)
 * - Fuzzy match (typo tolerance)
 * - Regex match
 * - Line-based replacement
 * - replace_all
 * - Error context (shows file lines around failed match)
 * - Whitespace handling
 */

#include <iostream>
#include <filesystem>
#include <fstream>
#include <string>
#include <cstdlib>
#include <nlohmann/json.hpp>

#include "tools/FileTool.h"
#include "core/FileIO.h"
#include "core/Logger.h"

using json = nlohmann::json;
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

// Helper: write a test file
static void write_file(const std::string& path, const std::string& content) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream f(path);
    f << content;
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

// Create a test fixture
class PatchTest {
public:
    std::string test_dir;
    std::string test_file;
    std::unique_ptr<FileIO> file_io;
    std::unique_ptr<EditFilePatchTool> tool;

    PatchTest() {
        // Create unique test directory
        test_dir = "/tmp/daedalus_test_" + std::to_string(std::rand()) + "_" + std::to_string(::getpid());
        test_file = test_dir + "/test.txt";
        write_file(test_file, "// initial content\nline2\nline3\n");
        file_io = std::make_unique<FileIO>(std::filesystem::path(test_dir), nullptr);
        tool = std::make_unique<EditFilePatchTool>();
    }

    ~PatchTest() {
        std::error_code ec;
        std::filesystem::remove_all(test_dir, ec);
    }

    json patch(const std::string& old_str, const std::string& new_str, bool replace_all = false) {
        json args = {
            {"path", "test.txt"},
            {"old_str", old_str},
            {"new_str", new_str},
            {"replace_all", replace_all}
        };
        return tool->execute(args, *file_io);
    }

    json patch_regex(const std::string& pattern, const std::string& new_str) {
        json args = {
            {"path", "test.txt"},
            {"old_str", pattern},
            {"new_str", new_str},
            {"regex", true}
        };
        return tool->execute(args, *file_io);
    }

    json patch_lines(int start, int end, const std::string& new_str) {
        json args = {
            {"path", "test.txt"},
            {"line_start", start},
            {"line_end", end},
            {"new_str", new_str}
        };
        return tool->execute(args, *file_io);
    }
};

// ============================================================================
// Test: exact substring match
// ============================================================================
TEST(exact_match) {
    PatchTest t;
    auto result = t.patch("line2", "REPLACED");
    CHECK(result["status"] == "success");
    CHECK_EQ(result["replacements"], 1);
    CHECK_EQ(result["first_match_line"], 2);
    auto content = read_file(t.test_file);
    CHECK(content.find("REPLACED") != std::string::npos);
    CHECK(content.find("line2") == std::string::npos);
    return 0;
}

// ============================================================================
// Test: line-normalized match (whitespace flexible)
// ============================================================================
TEST(line_normalized_match) {
    PatchTest t;
    // File has trailing spaces on each line, pattern does not - should still match
    write_file(t.test_file, "alpha   \nbeta\t\ngamma\n");
    auto result = t.patch("alpha\nbeta\ngamma", "REPLACED");
    CHECK(result["status"] == "success");
    return 0;
}

// ============================================================================
// Test: trailing whitespace normalized
// ============================================================================
TEST(trailing_whitespace_normalized) {
    PatchTest t;
    write_file(t.test_file, "line1   \nline2\t\t\nline3\n");
    // Our pattern has no trailing whitespace
    auto result = t.patch("line1", "REPLACED");
    CHECK(result["status"] == "success");
    return 0;
}

// ============================================================================
// Test: multi-line old_str exact match
// ============================================================================
TEST(multiline_exact_match) {
    PatchTest t;
    write_file(t.test_file, "int main() {\n    return 0;\n}\n");
    auto result = t.patch("int main() {\n    return 0;\n}", "int main() { return 42; }");
    CHECK(result["status"] == "success");
    CHECK(read_file(t.test_file).find("return 42") != std::string::npos);
    return 0;
}

// ============================================================================
// Test: replace_all
// ============================================================================
TEST(replace_all) {
    PatchTest t;
    write_file(t.test_file, "foo bar foo baz foo\n");
    auto result = t.patch("foo", "X", true);
    CHECK(result["status"] == "success");
    CHECK_EQ(result["replacements"], 3);
    auto content = read_file(t.test_file);
    CHECK(content.find("foo") == std::string::npos);
    CHECK_EQ(content, "X bar X baz X\n");
    return 0;
}

// ============================================================================
// Test: regex match
// ============================================================================
TEST(regex_match) {
    PatchTest t;
    write_file(t.test_file, "var1 = 10\nvar2 = 20\nvar3 = 30\n");
    // Pass replace_all=true to replace all 3 var declarations
    json args = {
        {"path", "test.txt"},
        {"old_str", R"(var\d+ = \d+)"},
        {"new_str", "var = 0"},
        {"regex", true},
        {"replace_all", true}
    };
    auto result = t.tool->execute(args, *t.file_io);
    CHECK(result["status"] == "success");
    CHECK_EQ(result["replacements"], 3);
    return 0;
}

// ============================================================================
// Test: line-based replacement
// ============================================================================
TEST(line_based_replacement) {
    PatchTest t;
    write_file(t.test_file, "line1\nline2\nline3\nline4\nline5\n");
    auto result = t.patch_lines(2, 3, "NEW_LINE_2\nNEW_LINE_3");
    CHECK(result["status"] == "success");
    auto content = read_file(t.test_file);
    CHECK(content.find("line1") != std::string::npos);
    CHECK(content.find("line2") == std::string::npos);
    CHECK(content.find("line3") == std::string::npos);
    CHECK(content.find("NEW_LINE_2") != std::string::npos);
    CHECK(content.find("line4") != std::string::npos);
    return 0;
}

// ============================================================================
// Test: line out of bounds
// ============================================================================
TEST(line_out_of_bounds) {
    PatchTest t;
    auto result = t.patch_lines(100, 101, "X");
    CHECK(result["status"] == "error");
    CHECK(result["permanent_failure"] == true);
    CHECK(result["message"].get<std::string>().find("out of bounds") != std::string::npos);
    return 0;
}

// ============================================================================
// Test: not found error includes context
// ============================================================================
TEST(not_found_with_context) {
    PatchTest t;
    write_file(t.test_file, "alpha\nbeta\ngamma\ndelta\nepsilon\n");
    auto result = t.patch("NONEXISTENT_PATTERN", "X");
    CHECK(result["status"] == "error");
    CHECK(result["permanent_failure"] == true);
    // Error should include hint about reading the file
    CHECK(result["hint"].get<std::string>().find("read_file") != std::string::npos);
    // Error should include context
    CHECK(result.contains("context"));
    CHECK(result["context"].get<std::string>().find("alpha") != std::string::npos);
    return 0;
}

// ============================================================================
// Test: invalid regex gives clear error
// ============================================================================
TEST(invalid_regex) {
    PatchTest t;
    auto result = t.patch_regex("[invalid(regex", "X");
    CHECK(result["status"] == "error");
    CHECK(result["message"].get<std::string>().find("regex") != std::string::npos);
    return 0;
}

// ============================================================================
// Test: empty old_str fails
// ============================================================================
TEST(empty_old_str) {
    PatchTest t;
    auto result = t.patch("", "X");
    CHECK(result["status"] == "error");
    CHECK(result["permanent_failure"] == true);
    return 0;
}

// ============================================================================
// Test: preserves leading whitespace (Python, YAML)
// ============================================================================
TEST(preserves_leading_whitespace) {
    PatchTest t;
    write_file(t.test_file, "def foo():\n    if True:\n        return 1\n    return 0\n");
    // Pattern has correct indentation
    auto result = t.patch("    if True:\n        return 1", "    if False:\n        return 1");
    CHECK(result["status"] == "success");
    auto content = read_file(t.test_file);
    CHECK(content.find("    if False:") != std::string::npos);
    CHECK(content.find("        return 1") != std::string::npos);
    return 0;
}

// ============================================================================
// Test: fuzzy match with typos (if supported)
// ============================================================================
TEST(fuzzy_match_with_typo) {
    PatchTest t;
    write_file(t.test_file, "Hello World\n");
    json args = {
        {"path", "test.txt"},
        {"old_str", "Hello Wrold"},  // typo: Wrold instead of World
        {"new_str", "Goodbye World"},
        {"fuzzy", true}
    };
    auto result = t.tool->execute(args, *t.file_io);
    CHECK(result["status"] == "success");
    auto content = read_file(t.test_file);
    CHECK(content.find("Goodbye World") != std::string::npos);
    return 0;
}

// ============================================================================
// Test: file does not exist
// ============================================================================
TEST(file_not_found) {
    PatchTest t;
    json args = {{"path", "nonexistent.txt"}, {"old_str", "x"}, {"new_str", "y"}};
    auto result = t.tool->execute(args, *t.file_io);
    CHECK(result["status"] == "error");
    CHECK(result["permanent_failure"] == true);
    CHECK(result["message"].get<std::string>().find("does not exist") != std::string::npos);
    return 0;
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "Running EditFilePatchTool tests..." << std::endl;

    int local_failures = 0;
    local_failures += test_exact_match();
    local_failures += test_line_normalized_match();
    local_failures += test_trailing_whitespace_normalized();
    local_failures += test_multiline_exact_match();
    local_failures += test_replace_all();
    local_failures += test_regex_match();
    local_failures += test_line_based_replacement();
    local_failures += test_line_out_of_bounds();
    local_failures += test_not_found_with_context();
    local_failures += test_invalid_regex();
    local_failures += test_empty_old_str();
    local_failures += test_preserves_leading_whitespace();
    local_failures += test_fuzzy_match_with_typo();
    local_failures += test_file_not_found();

    std::cout << "\nResults: " << (local_failures == 0 ? "PASS" : "FAIL") << std::endl;
    std::cout << "Total checks: " << test_count << std::endl;
    std::cout << "Failed checks: " << local_failures << std::endl;

    return local_failures > 0 ? 1 : 0;
}
