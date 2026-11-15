// SPDX-License-Identifier: BSL-1.0
// tests/test_config_locations.cpp
//
// Unit tests for daedalus config/settings path resolution.
//
// As of v0.5.0 settings live in a SQLite database, not config.json:
//   - Global DB:  $XDG_DATA_HOME/daedalus/daedalus.db
//                 (fallback ~/.local/share/daedalus/daedalus.db)
//   - Per-agent DB when cfg.daedalus_home is set: <daedalus_home>/daedalus.db
//
// What we pin:
//
//   L1. Paths::global().user_data_root() / "daedalus.db" is the canonical
//       settings database path — NOT config.json.
//   L2. AgentConfig with daedalus_home set reads from <daedalus_home>/daedalus.db.
//   L3. Agent with no explicit daedalus_home uses the global SettingsStore DB.
//   L4. SettingsStore populated directly (upsert_model + set_default_model_name)
//       is visible to Agent::list_models().
//   L5. Paths::global().user_data_root() honours $DAEDALUS_DATA_HOME.

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include <daedalus.h>
#include <daedalus/paths.h>
#include <daedalus/settings.h>

namespace fs = std::filesystem;

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

// Build a minimal ModelConfig for testing.
daedalus::ModelConfig make_test_model(const std::string& name) {
    daedalus::ModelConfig m;
    m.name             = name;
    m.provider         = "test-provider";
    m.model_id         = "model-" + name;
    m.max_context_size = 100000;
    m.temperature      = 0.0;
    return m;
}

void ensure_test_provider(daedalus::SettingsStore& store) {
    if (!store.get_provider("test-provider").has_value()) {
        daedalus::ProviderConfig p;
        p.name          = "test-provider";
        p.host          = "http://test.invalid";
        p.api_key       = "sk-test-not-placeholder";
        p.chat_endpoint = "/v1/chat/completions";
        store.upsert_provider(p);
    }
}

struct NullSink : daedalus::AgentSink {
    void on_log(daedalus::LogLevel, std::string_view) noexcept override {}
    void on_assistant_delta(std::string_view) noexcept override {}
    void on_tool_call(std::string_view, const nlohmann::json&) noexcept override {}
    void on_tool_result(std::string_view, const nlohmann::json&) noexcept override {}
    void on_state_change(int, int) noexcept override {}
};

// ===========================================================================
// L1. user_data_root() / "daedalus.db" is the canonical settings DB path.
// ===========================================================================
bool test_user_data_root_db_is_canonical() {
    auto root = daedalus::Paths::global().user_data_root();
    auto expected = root / "daedalus.db";
    if (expected.filename() != "daedalus.db") {
        std::cerr << "FAIL: L1 expected daedalus.db, got " << expected << "\n";
        return false;
    }
    std::cout << "[ok] L1: canonical settings DB is daedalus.db: "
              << expected.string() << "\n";
    return true;
}

// ===========================================================================
// L2. AgentConfig with daedalus_home set reads from <home>/daedalus.db.
// ===========================================================================
bool test_agent_uses_daedalus_home_db() {
    auto home = fs::temp_directory_path() / "l2-daedalus-home";
    fs::remove_all(home);
    fs::create_directories(home);

    {
        auto store = daedalus::SettingsStore::for_home(home);
        ensure_test_provider(store);
        store.upsert_model(make_test_model("l2-model"));
        store.set_default_model_name("l2-model");
    }

    daedalus::AgentConfig cfg;
    cfg.mcp_enabled = false;  // unrelated to this test; never start real MCP servers
    cfg.project_root  = "/tmp";
    cfg.daedalus_home = home;

    auto sink = std::make_shared<NullSink>();
    daedalus::Agent agent(cfg, sink);
    agent.initialize();

    auto models = agent.list_models();
    bool found_l2 = false;
    for (const auto& m : models) {
        if (m.name == "l2-model") found_l2 = true;
    }
    if (!found_l2) {
        std::cerr << "FAIL: L2 did not find l2-model\n";
        return false;
    }

    fs::remove_all(home);
    std::cout << "[ok] L2: AgentImpl resolves cfg.daedalus_home -> daedalus.db\n";
    return true;
}

// ===========================================================================
// L3. Agent with no explicit paths uses the global SettingsStore DB.
// ===========================================================================
bool test_agent_uses_global_db_when_no_home() {
    auto tmp_home = fs::temp_directory_path() / "l3-daedalus-home";
    fs::remove_all(tmp_home);
    fs::create_directories(tmp_home / ".local" / "share" / "daedalus");

    ScopedEnv home("HOME", tmp_home.string());
    ScopedEnv xdg_data("XDG_DATA_HOME", "");
    ScopedEnv daedalus_data("DAEDALUS_DATA_HOME", "");

    {
        auto& store = daedalus::SettingsStore::instance();
        ensure_test_provider(store);
        store.upsert_model(make_test_model("l3-model"));
        store.set_default_model_name("l3-model");
    }

    daedalus::AgentConfig cfg;
    cfg.mcp_enabled = false;  // unrelated to this test; never start real MCP servers
    cfg.project_root = "/tmp";
    // NO daedalus_home set — must fall back to the global SettingsStore.

    auto sink = std::make_shared<NullSink>();
    daedalus::Agent agent(cfg, sink);
    agent.initialize();

    auto models = agent.list_models();
    bool found_l3 = false;
    for (const auto& m : models) {
        if (m.name == "l3-model") found_l3 = true;
    }
    if (!found_l3) {
        std::cerr << "FAIL: L3 did not find l3-model\n";
        return false;
    }

    fs::remove_all(tmp_home);
    std::cout << "[ok] L3: Agent uses global SettingsStore (no explicit home)\n";
    return true;
}

// ===========================================================================
// L4. SettingsStore populated directly is visible through Agent::list_models().
// ===========================================================================
bool test_settings_store_direct_population() {
    auto home = fs::temp_directory_path() / "l4-cfg";
    fs::remove_all(home);
    fs::create_directories(home);

    {
        auto store = daedalus::SettingsStore::for_home(home);
        ensure_test_provider(store);
        store.upsert_model(make_test_model("l4-good"));
        store.set_default_model_name("l4-good");
    }

    daedalus::AgentConfig cfg;
    cfg.mcp_enabled = false;  // unrelated to this test; never start real MCP servers
    cfg.project_root  = "/tmp";
    cfg.daedalus_home = home;

    auto sink = std::make_shared<NullSink>();
    daedalus::Agent agent(cfg, sink);
    agent.initialize();

    auto models = agent.list_models();
    bool found_l4 = false;
    for (const auto& m : models) {
        if (m.name == "l4-good") found_l4 = true;
    }
    if (!found_l4) {
        std::cerr << "FAIL: L4 did not find l4-good\n";
        return false;
    }

    fs::remove_all(home);
    std::cout << "[ok] L4: SettingsStore populated, Agent lists model\n";
    return true;
}

// ===========================================================================
// L5. $DAEDALUS_DATA_HOME short-circuits the global data root resolution.
// ===========================================================================
bool test_daedalus_data_home_override() {
    ScopedEnv env("DAEDALUS_DATA_HOME", "/tmp/l5-override");
    auto root = daedalus::Paths::global().user_data_root();
    if (root != "/tmp/l5-override") {
        std::cerr << "FAIL: L5 expected /tmp/l5-override, got " << root << "\n";
        return false;
    }
    std::cout << "[ok] L5: $DAEDALUS_DATA_HOME redirects data root\n";
    return true;
}

}  // namespace

int main() {
    bool ok = true;
    ok &= test_user_data_root_db_is_canonical();     // L1
    ok &= test_agent_uses_daedalus_home_db();        // L2
    ok &= test_agent_uses_global_db_when_no_home();  // L3
    ok &= test_settings_store_direct_population();   // L4
    ok &= test_daedalus_data_home_override();        // L5

    if (ok) {
        std::cout << "\ntest_config_locations: 5 tests passed\n";
        return 0;
    }
    std::cerr << "\ntest_config_locations: FAILED\n";
    return 1;
}
