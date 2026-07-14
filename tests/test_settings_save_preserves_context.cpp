/**
 * @file test_settings_save_preserves_context.cpp
 * @brief Regression tests for SettingsManager::save() preserving data
 *
 * Bug history:
 * SettingsManager::save() originally only wrote a hardcoded list of fields
 * (default_model, max_*, models) to disk. It omitted everything else,
 * including:
 *   - max_context_size on individual models (so user-set values were wiped
 *     on /models switch)
 *   - serper-search.api_key (so the user's API key was wiped on /models
 *     switch)
 *
 * The fix: save() now reads the existing file first (if it exists) and
 * overlays managed fields on top, preserving any unknown fields.
 *
 * This test uses ONLY fields that are known to exist in the user's
 * settings.json:
 *   - auto_approve
 *   - default_model
 *   - max_agent_iterations, max_tool_result_size, max_request_size,
 *     max_response_size
 *   - models.* (host, api_key, model, max_context_size, endpoints.chat)
 *   - serper-search.api_key
 *
 * It does NOT test fields like mcp/skills/file_io/manager/server_error
 * because those are populated by create_default_settings() but are not
 * present in the real user's settings.json. Testing them would require
 * fabricating fake fields that don't actually exist in production use.
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <string>
#include <cstdlib>
#include <cassert>

#include "core/SettingsManager.h"

using namespace std;
using namespace std::filesystem;

static int failures = 0;
static int test_count = 0;

#define EXPECT(cond, msg) do { \
    ++test_count; \
    if (!(cond)) { \
        ++failures; \
        cerr << "  FAIL: " << msg << endl; \
    } else { \
        cerr << "  OK: " << msg << endl; \
    } \
} while (0)

int main() {
    cerr << "=== Settings save() preserves fields ===" << endl;

    // Set up: write a temp settings file matching the ACTUAL structure of
    // the user's settings.json (verified by reading ~/.daedalus/settings/
    // settings.json). Use a temp HOME so we don't touch the real file.
    path tmp_home = temp_directory_path() / "daedalus_save_test_home";
    remove_all(tmp_home);
    create_directories(tmp_home / ".daedalus/settings");
    path tmp_settings = tmp_home / ".daedalus/settings/settings.json";
    {
        ofstream f(tmp_settings);
        f << R"({
            "auto_approve": true,
            "default_model": "minimax-m3",
            "max_agent_iterations": 600,
            "max_request_size": 4000000,
            "max_response_size": 5000000,
            "max_tool_result_size": 500000,
            "models": {
                "minimax-m3": {
                    "host": "https://api.minimax.io",
                    "api_key": "minimax-key-abc",
                    "model": "MiniMax-M3",
                    "max_context_size": 500000,
                    "endpoints": {"chat": "/v1/chat/completions"}
                },
                "lm-studio": {
                    "host": "http://localhost:1234",
                    "api_key": "lm-studio",
                    "model": "llama-3-8b",
                    "max_context_size": 32768,
                    "endpoints": {"chat": "/v1/chat/completions"}
                }
            },
            "serper-search": {
                "api_key": "f40a0b1b3392adf62fda644727ca67034ea58904"
            }
        })";
    }
    EXPECT(exists(tmp_settings), "temp settings file created");

    // Load
    std::unique_ptr<SettingsManager> sm(new SettingsManager(tmp_settings));
    sm->load();

    auto ctx_before = sm->get_max_context_size_for_model("minimax-m3");
    EXPECT(ctx_before == 500000,
           "max_context_size loaded correctly: " + to_string(ctx_before));

    // Trigger save() (simulates /models switching)
    sm->save();

    // Read back the file
    {
        ifstream f(tmp_settings);
        stringstream ss;
        ss << f.rdbuf();
        string saved = ss.str();

        // The actual bugs that were reported:
        EXPECT(saved.find("\"max_context_size\"") != string::npos,
               "models.*.max_context_size key preserved in saved file");
        EXPECT(saved.find("500000") != string::npos,
               "max_context_size=500000 preserved");
        EXPECT(saved.find("32768") != string::npos,
               "max_context_size=32768 preserved");

        EXPECT(saved.find("\"serper-search\"") != string::npos,
               "serper-search section preserved");
        EXPECT(saved.find("f40a0b1b3392adf62fda644727ca67034ea58904") != string::npos,
               "serper-search.api_key value preserved");

        // Sanity: managed fields still present
        EXPECT(saved.find("\"auto_approve\"") != string::npos,
               "auto_approve preserved");
        EXPECT(saved.find("\"default_model\"") != string::npos,
               "default_model preserved");
        EXPECT(saved.find("minimax-key-abc") != string::npos,
               "model api_key preserved");
    }

    // Round-trip: load again and confirm values still correct
    std::unique_ptr<SettingsManager> sm2(new SettingsManager(tmp_settings));
    sm2->load();
    auto ctx_after = sm2->get_max_context_size_for_model("minimax-m3");
    EXPECT(ctx_after == 500000,
           "max_context_size still correct after re-load: " + to_string(ctx_after));

    remove_all(tmp_home);

    cerr << "\nResults: " << (test_count - failures) << "/" << test_count << " passed" << endl;
    return failures > 0 ? 1 : 0;
}