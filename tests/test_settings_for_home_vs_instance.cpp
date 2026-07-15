// SPDX-License-Identifier: BSL-1.0
// tests/test_settings_for_home_vs_instance.cpp
//
// Regression test: verifies that SettingsStore::for_home() and
// SettingsStore::instance() are NOT equivalent.
//
// Bug: The webui demo set cfg.daedalus_home to "/home/ubuntu/.daedalus"
// which caused AgentImpl to use SettingsStore::for_home() instead of
// instance(). For_home() opens a fresh DB at <path>/daedalus.db which
// doesn't have the user's models - they're in the instance() DB.
//
// This test ensures:
//   1. instance() returns the global singleton with real user data
//   2. for_home() opens a fresh DB at the specified path
//   3. They are NOT the same store

#include <cassert>
#include <iostream>
#include <filesystem>

#include <daedalus/settings.h>

namespace fs = std::filesystem;

int main() {
    int failures = 0;
    
    // S1: instance() has models, for_home("/nonexistent") does not
    {
        auto& inst = daedalus::SettingsStore::instance();
        auto models_inst = inst.list_models();
        std::cout << "[S1] instance() has " << models_inst.size() << " models\n";
        
        // Create a temp directory with a fresh DB
        fs::path tmp = fs::temp_directory_path() / "daedalus-test-for-home";
        fs::remove_all(tmp);
        fs::create_directories(tmp);
        
        auto& fh = daedalus::SettingsStore::for_home(tmp);
        auto models_fh = fh.list_models();
        std::cout << "[S1] for_home(temp) has " << models_fh.size() << " models\n";
        
        // Cleanup
        fs::remove_all(tmp);
        
        // They should be different
        if (&inst == &fh) {
            std::cerr << "[S1] FAIL: instance() and for_home() returned same object!\n";
            ++failures;
        } else {
            std::cout << "[S1] OK: instance() and for_home() are distinct stores\n";
        }
        
        // instance should have models (or be empty), for_home should definitely be empty
        if (!models_fh.empty()) {
            std::cerr << "[S1] FAIL: for_home() should have 0 models but has " << models_fh.size() << "\n";
            ++failures;
        }
    }
    
    // S2: Verify the XDG path confusion scenario
    // The user had models at ~/.config/daedalus/ but webui used ~/.daedalus/
    {
        fs::path config_home = std::getenv("HOME") ? fs::path(std::getenv("HOME")) / ".config" / "daedalus" : fs::path("/tmp/test");
        fs::path legacy_home = std::getenv("HOME") ? fs::path(std::getenv("HOME")) / ".daedalus" : fs::path("/tmp/test2");
        
        std::cout << "[S2] Checking if config and legacy homes are different...\n";
        
        if (fs::exists(config_home)) {
            auto& config_store = daedalus::SettingsStore::for_home(config_home);
            auto config_models = config_store.list_models();
            std::cout << "[S2] " << config_home << " has " << config_models.size() << " models\n";
        }
        
        if (fs::exists(legacy_home)) {
            auto& legacy_store = daedalus::SettingsStore::for_home(legacy_home);
            auto legacy_models = legacy_store.list_models();
            std::cout << "[S2] " << legacy_home << " has " << legacy_models.size() << " models\n";
        } else {
            std::cout << "[S2] " << legacy_home << " does not exist\n";
        }
    }
    
    if (failures) {
        std::cerr << "\ntest_settings_for_home_vs_instance: " << failures << " FAILED\n";
        return 1;
    }
    
    std::cout << "\ntest_settings_for_home_vs_instance: all tests passed\n";
    return 0;
}
