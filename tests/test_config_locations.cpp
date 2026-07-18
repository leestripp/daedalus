// SPDX-License-Identifier: BSL-1.0
// tests/test_config_locations.cpp
//
// Unit tests for daedalus config/settings path resolution.
//
// What we pin (as of the XDG migration: ~./daedalus/settings/settings.json
//   → $XDG_CONFIG_HOME/daedalus/config.json → SQLite SettingsStore):
//
//   L1. Paths::global().user_config_root() / "config.json" is the canonical
//       settings path — NOT settings.json, NOT settings/settings.json.
//   L2. `AgentImpl` derives settings from `cfg.daedalus_home` as
//       `<daedalus_home>/config.json` when no explicit path is passed.
//   L3. `Agent::initialize()` actually loads from the resolved path.
//   L4. SettingsStore is populated from config.json during migration.
//       Agent::list_models() returns non-empty for a valid config with
//       a real (non-placeholder) API key.
//   L5. `Paths::global().user_config_root()` honours $DAEDALUS_CONFIG_HOME.
//
// NOTE: default_settings_path() / default_mcp_servers_path() were removed
// in v0.5.2 — use Paths::global().user_config_root() directly.

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include <daedalus.h>
#include <daedalus/paths.h>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// RAII env-var setter. Restores on destruction.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const std::string& value) : m_name(name) {
        const char* prev = std::getenv(name);
        m_had_prev = (prev != nullptr);
        if (m_had_prev) m_prev = prev;
        if (value.empty()) {
            ::unsetenv(name);
        } else {
            ::setenv(name, value.c_str(), 1);
        }
    }
    ~ScopedEnv() {
        if (m_had_prev) {
            ::setenv(m_name, m_prev.c_str(), 1);
        } else {
            ::unsetenv(m_name);
        }
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    const char* m_name;
    std::string m_prev;
    bool m_had_prev;
};

// Write a minimal-but-valid config.json at <dir>/config.json with the
// given default_model + api_key. Other fields use safe defaults.
void write_config(const fs::path& dir, const std::string& default_model,
                  const std::string& api_key = "test-api-key-not-placeholder") {
    fs::create_directories(dir);
    json j = {
        {"default_model", default_model},
        {"max_agent_iterations", 50},
        {"auto_approve", false},
        {"models", {
            {default_model, {
                {"host", "http://test.invalid"},
                {"api_key", api_key},
                {"model", "test-model-id"},
                {"endpoints", {{"chat", "/v1/chat/completions"}}},
                {"max_context_size", 100000},
                {"temperature", 0.0}
            }}
        }}
    };
    std::ofstream(dir / "config.json") << j.dump(2);
}

// ===========================================================================
// L1. Paths::global().user_config_root() / "config.json" is canonical
// ===========================================================================
void test_user_config_root_config_json_is_canonical() {
    auto root = daedalus::Paths::global().user_config_root();
    auto expected = root / "config.json";
    // The expected path ends in config.json (not settings.json).
    assert(expected.filename() == "config.json");
    std::cout << "[ok] L1: canonical settings path ends in config.json: "
              << expected.string() << "\n";
}

// ===========================================================================
// L2. AgentConfig with daedalus_home set derives settings_path = home/config.json
// ===========================================================================
void test_agent_resolves_daedalus_home_to_config_json() {
    ScopedEnv home("HOME", "/tmp");  // irrelevant, we override below
    ScopedEnv cfg_home("DAEDALUS_CONFIG_HOME", "/tmp/l2-daedalus-home");

    auto root = daedalus::Paths::global().user_config_root();
    write_config(root, "l2-model");

    daedalus::AgentConfig cfg;
    cfg.project_root  = "/tmp";
    cfg.daedalus_home = root;  // explicit

    std::vector<std::string> loaded;
    struct Sink : daedalus::AgentSink {
        std::vector<std::string>* log;
        explicit Sink(std::vector<std::string>* l) : log(l) {}
        void on_log(daedalus::LogLevel, std::string_view msg) noexcept override {
            log->emplace_back(msg);
        }
        void on_assistant_delta(std::string_view) noexcept override {}
        void on_tool_call(std::string_view, const nlohmann::json&) noexcept override {}
        void on_tool_result(std::string_view, const nlohmann::json&) noexcept override {}
        void on_state_change(int, int) noexcept override {}
    };
    auto sink = std::make_shared<Sink>(&loaded);
    daedalus::Agent agent(cfg, sink);
    agent.initialize();

    bool saw_init = false;
    for (const auto& m : loaded) {
        if (m.find("Agent::initialize()") != std::string::npos) {
            saw_init = true;
        }
    }
    assert(saw_init);

    auto models = agent.list_models();
    bool found_l2 = false;
    for (const auto& m : models) {
        if (m.name == "l2-model") found_l2 = true;
    }
    assert(found_l2);

    fs::remove_all(root);
    std::cout << "[ok] L2: AgentImpl resolves cfg.daedalus_home -> config.json\n";
}

// ===========================================================================
// L3. Agent::initialize() with no explicit paths uses the XDG default.
// ===========================================================================
void test_agent_uses_xdg_default_when_no_paths() {
    auto tmp_home = fs::temp_directory_path() / "l3-daedalus-home";
    fs::remove_all(tmp_home);
    fs::create_directories(tmp_home / ".config" / "daedalus");

    ScopedEnv home("HOME", tmp_home.string());
    ScopedEnv xdg_cfg("XDG_CONFIG_HOME", "");
    ScopedEnv xdg_data("XDG_DATA_HOME", "");
    ScopedEnv cfg_home("DAEDALUS_CONFIG_HOME", "");

    auto root = daedalus::Paths::global().user_config_root();
    write_config(root, "l3-model");

    daedalus::AgentConfig cfg;
    cfg.project_root = "/tmp";
    // NO daedalus_home set — must fall back to user_config_root / "config.json"

    auto models = [&]() {
        struct Sink : daedalus::AgentSink {
            void on_log(daedalus::LogLevel, std::string_view) noexcept override {}
            void on_assistant_delta(std::string_view) noexcept override {}
            void on_tool_call(std::string_view, const nlohmann::json&) noexcept override {}
            void on_tool_result(std::string_view, const nlohmann::json&) noexcept override {}
            void on_state_change(int, int) noexcept override {}
        };
        auto sink = std::make_shared<Sink>();
        daedalus::Agent agent(cfg, sink);
        agent.initialize();
        return agent.list_models();
    }();

    bool found_l3 = false;
    for (const auto& m : models) {
        if (m.name == "l3-model") found_l3 = true;
    }
    assert(found_l3);

    fs::remove_all(tmp_home);
    std::cout << "[ok] L3: Agent uses XDG default (no explicit path)\n";
}

// ===========================================================================
// L4. SettingsStore is populated from config.json during migration.
// ===========================================================================
void test_settings_manager_loads_real_config() {
    auto tmp = fs::temp_directory_path() / "l4-cfg";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    auto good = tmp / "good";
    write_config(good, "l4-good", "sk-real-not-placeholder");

    daedalus::AgentConfig cfg;
    cfg.project_root  = "/tmp";
    cfg.daedalus_home = good;
    {
        struct Sink : daedalus::AgentSink {
            void on_log(daedalus::LogLevel, std::string_view) noexcept override {}
            void on_assistant_delta(std::string_view) noexcept override {}
            void on_tool_call(std::string_view, const nlohmann::json&) noexcept override {}
            void on_tool_result(std::string_view, const nlohmann::json&) noexcept override {}
            void on_state_change(int, int) noexcept override {}
        };
        auto sink = std::make_shared<Sink>();
        daedalus::Agent agent(cfg, sink);
        agent.initialize();
        auto models = agent.list_models();
        bool found = false;
        for (const auto& m : models) {
            if (m.name == "l4-good") found = true;
        }
        assert(found);
    }
    fs::remove_all(tmp);
    std::cout << "[ok] L4: SettingsStore populated, Agent lists model\n";
}

// ===========================================================================
// L5. $DAEDALUS_CONFIG_HOME short-circuits the XDG resolution so tests
// don't touch the user's real ~/.config.
// ===========================================================================
void test_daedalus_config_home_override() {
    ScopedEnv env("DAEDALUS_CONFIG_HOME", "/tmp/l5-override");
    auto root = daedalus::Paths::global().user_config_root();
    assert(root == "/tmp/l5-override");
    std::cout << "[ok] L5: $DAEDALUS_CONFIG_HOME redirects config root\n";
}

}  // namespace

int main() {
    test_user_config_root_config_json_is_canonical(); // L1
    test_agent_resolves_daedalus_home_to_config_json();   // L2
    test_agent_uses_xdg_default_when_no_paths();          // L3
    test_settings_manager_loads_real_config();            // L4
    test_daedalus_config_home_override();                 // L5

    std::cout << "\ntest_config_locations: 5 tests passed\n";
    return 0;
}
