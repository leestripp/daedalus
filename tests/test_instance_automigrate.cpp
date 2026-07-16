// SPDX-License-Identifier: BSL-1.0
// tests/test_instance_automigrate.cpp
//
// Verifies that SettingsStore::instance() (the global singleton path)
// auto-migrates legacy JSON on first open. Run as a separate process
// from the rest of the test suite, so the global instance hasn't been
// touched yet.
//
// The test sets XDG_DATA_HOME and XDG_CONFIG_HOME to a temp dir, drops
// a legacy config.json there, then calls instance() and asserts the
// migration happened.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <chrono>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <daedalus/settings.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

int main() {
    // Set up an isolated XDG layout under a temp dir.
    auto tmp = fs::temp_directory_path() /
               ("daedalus-instance-am-" +
                std::to_string(std::chrono::system_clock::now()
                                 .time_since_epoch().count()) + "-" +
                std::to_string(::getpid()));
    fs::remove_all(tmp);
    fs::create_directories(tmp / ".config" / "daedalus");
    fs::create_directories(tmp / ".local" / "share");

    ::setenv("XDG_DATA_HOME",   (tmp / ".local" / "share").string().c_str(), 1);
    ::setenv("XDG_CONFIG_HOME", (tmp / ".config").string().c_str(), 1);

    // Drop a legacy config.json with a real (non-placeholder) api_key.
    json j = {
        {"default_model", "instance-am-model"},
        {"auto_approve", false},
        {"max_agent_iterations", 314},
        {"models", {
            {"instance-am-model", {
                {"host", "http://instance-am.example"},
                {"api_key", "real-key-not-placeholder"},
                {"model", "instance-am-model-id"},
                {"endpoints", {{"chat", "/v1/chat/completions"}}},
                {"max_context_size", 12345}
            }}
        }}
    };
    {
        std::ofstream f(tmp / ".config" / "daedalus" / "config.json");
        f << j.dump(2);
    }

    // Touching instance() for the first time must auto-migrate.
    // instance() returns SettingsStore& (true singleton). Use const ref —
    // the test only reads from the store.
    const auto& store = daedalus::SettingsStore::instance();

    int failures = 0;

    auto m = store.get_model("instance-am-model");
    if (!m.has_value()) {
        std::cerr << "FAIL: instance() didn't auto-migrate model\n";
        ++failures;
    } else {
        if (m->host != "http://instance-am.example") {
            std::cerr << "FAIL: host mismatch: " << m->host << "\n";
            ++failures;
        }
        if (m->api_key != "real-key-not-placeholder") {
            std::cerr << "FAIL: api_key mismatch\n";
            ++failures;
        }
    }
    if (store.get_default_model_name() != "instance-am-model") {
        std::cerr << "FAIL: default_model_name not migrated\n";
        ++failures;
    }
    if (store.get_max_agent_iterations() != 314) {
        std::cerr << "FAIL: max_agent_iterations not migrated\n";
        ++failures;
    }
    if (fs::exists(tmp / ".config" / "daedalus" / "config.json")) {
        std::cerr << "FAIL: legacy config.json not renamed to .migrated\n";
        ++failures;
    } else {
        std::cout << "[ok] instance() renamed config.json → .migrated\n";
    }

    if (failures == 0) {
        std::cout << "[ok] instance() auto-migrated all expected data\n";
    }

    // Clean up
    fs::remove_all(tmp);

    return failures ? 1 : 0;
}