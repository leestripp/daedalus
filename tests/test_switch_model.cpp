// SPDX-License-Identifier: BSL-1.0
// tests/test_switch_model.cpp
//
// §MODEL-SWITCH acceptance: Agent::switch_model() changes the active
// model, updates the SettingsStore default, reconfigures the internal
// LLMClient, and clears the conversation history so the new model
// starts with a fresh context.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <daedalus.h>
#include <daedalus/settings.h>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct NullSink : public daedalus::AgentSink {
    void on_log(daedalus::LogLevel, std::string_view) noexcept override {}
};

fs::path make_temp_home(const std::string& tag) {
    fs::path p = fs::temp_directory_path() /
        ("daedalus_test_switch_model_" + tag + "_" +
         std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

daedalus::ModelConfig make_model(const std::string& name,
                                 const std::string& host,
                                 int max_context_size) {
    daedalus::ModelConfig m;
    m.name             = name;
    m.host             = host;
    m.api_key          = name + "-key";
    m.model_id         = name + "-model";
    m.chat_endpoint    = "/v1/chat/completions";
    m.max_context_size = max_context_size;
    m.temperature      = 0.0;
    return m;
}

void write_settings(const fs::path& daedalus_home) {
    auto store = daedalus::SettingsStore::for_home(daedalus_home);
    store.upsert_model(make_model("small", "http://small.example", 10000));
    store.upsert_model(make_model("large", "http://large.example", 100000));
    store.set_default_model_name("small");
}

}  // namespace

TEST_CASE("switch_model rejects unknown model and leaves state intact") {
    fs::path home = make_temp_home("reject");
    write_settings(home);

    fs::path project = home / "project";
    fs::create_directories(project);

    daedalus::AgentConfig cfg;
    cfg.project_root  = project;
    cfg.daedalus_home = home;
    cfg.session_id    = "switch-reject";

    auto sink = std::make_shared<NullSink>();
    daedalus::Agent agent(std::move(cfg), sink);
    agent.initialize();

    CHECK(agent.active_model().name == "small");

    // Unknown model must fail.
    bool switched = agent.switch_model("nonexistent");
    CHECK_FALSE(switched);

    // Active model and default must be unchanged.
    CHECK(agent.active_model().name == "small");
    auto store = daedalus::SettingsStore::for_home(home);
    CHECK(store.get_default_model_name() == "small");

    agent.shutdown();
    fs::remove_all(home);
}

TEST_CASE("switch_model clears conversation history") {
    fs::path home = make_temp_home("clear");
    write_settings(home);

    fs::path project = home / "project";
    fs::create_directories(project);

    // Pre-seed a messages.json in the new layout path.
    fs::path agent_dir = project / ".daedalus" / "daedalus";
    fs::create_directories(agent_dir);
    fs::path messages_file = agent_dir / "messages.json";
    {
        json history = json::array();
        history.push_back({{"role", "system"}, {"content", "you are a test assistant"}});
        history.push_back({{"role", "user"}, {"content", "hello"}});
        history.push_back({{"role", "assistant"}, {"content", "hi"}});
        std::ofstream f(messages_file);
        f << history.dump(2);
    }
    REQUIRE(fs::is_regular_file(messages_file));

    daedalus::AgentConfig cfg;
    cfg.project_root  = project;
    cfg.daedalus_home = home;
    cfg.session_id    = "switch-clear";

    auto sink = std::make_shared<NullSink>();
    daedalus::Agent agent(std::move(cfg), sink);
    agent.initialize();

    // The pre-seeded history was loaded.
    CHECK(agent.stats().message_count == 3);
    CHECK(agent.last_assistant_text() == "hi");

    // Switching to a valid model clears history.
    bool switched = agent.switch_model("large");
    CHECK(switched);
    CHECK(agent.active_model().name == "large");
    CHECK(agent.stats().message_count == 0);
    CHECK(agent.last_assistant_text().empty());

    // The on-disk transcript was removed.
    CHECK_FALSE(fs::exists(messages_file));

    // Default model in the store is updated.
    auto store = daedalus::SettingsStore::for_home(home);
    CHECK(store.get_default_model_name() == "large");

    agent.shutdown();
    fs::remove_all(home);
}

TEST_CASE("switch_model updates compression config for new context size") {
    fs::path home = make_temp_home("ctx");
    write_settings(home);

    fs::path project = home / "project";
    fs::create_directories(project);

    daedalus::AgentConfig cfg;
    cfg.project_root  = project;
    cfg.daedalus_home = home;
    cfg.session_id    = "switch-ctx";

    auto sink = std::make_shared<NullSink>();
    daedalus::Agent agent(std::move(cfg), sink);
    agent.initialize();

    CHECK(agent.active_model().max_context_size == 10000);

    bool switched = agent.switch_model("large");
    CHECK(switched);
    CHECK(agent.active_model().max_context_size == 100000);

    agent.shutdown();
    fs::remove_all(home);
}
