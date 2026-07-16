// SPDX-License-Identifier: BSL-1.0
// tests/test_settings_active_model.cpp
//
// Regression test: verifies that Agent::active_model() works when using
// SettingsStore::instance() (the default). Previously the webui demo set
// cfg.daedalus_home to a legacy path, causing AgentImpl to use
// SettingsStore::for_home() instead, which opened an empty DB.
//
// This test ensures:
//   1. SettingsStore::instance() has the real user models (if any)
//   2. Agent created WITHOUT daedalus_home set can find models via active_model()

#include <cassert>
#include <iostream>
#include <string>
#include <filesystem>

#include <daedalus.h>
#include <daedalus/settings.h>

namespace fs = std::filesystem;

int main() {
    int failures = 0;
    
    // S1: SettingsStore::instance() can list models (sanity check)
    {
        // instance() is a true singleton (returns SettingsStore&) — use const ref.
        const auto& store = daedalus::SettingsStore::instance();
        auto models = store.list_models();
        std::cout << "[S1] SettingsStore::instance() has " << models.size() << " models\n";
        if (models.empty()) {
            std::cerr << "WARN: SettingsStore::instance() has no models (may be fresh DB)\n";
        }
    }
    
    // S2: Agent::active_model() works WITHOUT daedalus_home set
    {
        daedalus::AgentConfig cfg;
        cfg.project_root = "/tmp";
        cfg.model_name = "minimax-m2.7";  // Try to use a known model
        
        struct NullSink : daedalus::AgentSink {
            void on_log(daedalus::LogLevel, std::string_view) noexcept override {}
            void on_assistant_delta(std::string_view) noexcept override {}
            void on_tool_call(std::string_view, const nlohmann::json&) noexcept override {}
            void on_tool_result(std::string_view, const nlohmann::json&) noexcept override {}
            void on_state_change(int, int) noexcept override {}
        };
        
        auto sink = std::make_shared<NullSink>();
        daedalus::Agent agent(cfg, sink);
        
        try {
            agent.initialize();
        } catch (const std::exception& e) {
            std::cerr << "[S2] Agent::initialize() threw: " << e.what() << "\n";
            ++failures;
        }
        
        auto m = agent.active_model();
        std::cout << "[S2] Agent::active_model() returned:\n";
        std::cout << "  name: '" << m.name << "'\n";
        std::cout << "  model_id: '" << m.model_id << "'\n";
        std::cout << "  host: '" << m.host << "'\n";
        std::cout << "  max_context_size: " << m.max_context_size << "\n";
        
        if (m.name.empty()) {
            std::cerr << "[S2] FAIL: active_model() returned empty!\n";
            ++failures;
        }
    }
    
    // S3: Agent::active_model() falls back to default model when model_name is empty
    {
        daedalus::AgentConfig cfg;
        cfg.project_root = "/tmp";
        // No model_name set - should use default from settings
        
        struct NullSink : daedalus::AgentSink {
            void on_log(daedalus::LogLevel, std::string_view) noexcept override {}
            void on_assistant_delta(std::string_view) noexcept override {}
            void on_tool_call(std::string_view, const nlohmann::json&) noexcept override {}
            void on_tool_result(std::string_view, const nlohmann::json&) noexcept override {}
            void on_state_change(int, int) noexcept override {}
        };
        
        auto sink = std::make_shared<NullSink>();
        daedalus::Agent agent(cfg, sink);
        
        try {
            agent.initialize();
        } catch (const std::exception& e) {
            std::cerr << "[S3] Agent::initialize() threw: " << e.what() << "\n";
            ++failures;
        }
        
        auto m = agent.active_model();
        std::cout << "[S3] Agent with no model_name, active_model() returned:\n";
        std::cout << "  name: '" << m.name << "'\n";
        std::cout << "  model_id: '" << m.model_id << "'\n";
        
        // If the store has models but active_model returned empty, that's a bug
        // instance() returns SettingsStore& — use const ref for read-only.
        const auto& store = daedalus::SettingsStore::instance();
        auto models = store.list_models();
        if (!models.empty() && m.name.empty()) {
            std::cerr << "[S3] FAIL: store has " << models.size() << " models but active_model() returned empty!\n";
            ++failures;
        } else if (!m.name.empty()) {
            std::cout << "[S3] OK: active_model() returned a model (default from store)\n";
        } else {
            std::cout << "[S3] OK: no default model set (acceptable for fresh DB)\n";
        }
    }
    
    if (failures) {
        std::cerr << "\ntest_settings_active_model: " << failures << " FAILED\n";
        return 1;
    }
    
    std::cout << "\ntest_settings_active_model: all tests passed\n";
    return 0;
}
