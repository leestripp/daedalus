// Background execute_bash completion → originating Agent queue + host wake-up.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <daedalus.h>
#include <core/BackgroundProcessManager.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

class CompletionSink final : public daedalus::AgentSink {
public:
    void on_turn_requested(std::string_view source) noexcept override {
        {
            std::lock_guard lock(mutex_);
            sources_.emplace_back(source);
        }
        cv_.notify_all();
    }

    bool wait_for_count(std::size_t count) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, 3s, [&] { return sources_.size() >= count; });
    }

    std::vector<std::string> sources() const {
        std::lock_guard lock(mutex_);
        return sources_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::string> sources_;
};

daedalus::Agent make_agent(const std::string& name,
                           const std::shared_ptr<CompletionSink>& sink) {
    const auto root = std::filesystem::temp_directory_path()
                      / ("daedalus_bg_completion_" + name);
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    daedalus::AgentConfig cfg;
    cfg.project_root = root;
    cfg.daedalus_home = root / "home";
    cfg.agent_name = name;
    cfg.auto_approve = true;
    cfg.memory_enabled = false;
    cfg.mcp_enabled = false;
    return daedalus::Agent(std::move(cfg), sink);
}

nlohmann::json start(daedalus::Agent& agent, const std::string& command) {
    return agent.execute_tool("execute_bash", {
        {"command", command}, {"background", true}
    });
}

} // namespace

TEST_CASE("Agent queues one origin-tagged completion and requests a turn") {
    BackgroundProcessManager::get_instance().cleanup_all();
    auto sink = std::make_shared<CompletionSink>();
    auto agent = make_agent("single", sink);

    const auto result = start(agent, "printf 'agent-final-output'; exit 0");
    REQUIRE(result.value("status", "") == "background");
    REQUIRE(sink->wait_for_count(1));
    REQUIRE(agent.pending_count() == 1);

    const auto pending = agent.pending_messages();
    REQUIRE(pending.size() == 1);
    CHECK(pending[0].source == "trigger:background-process");
    CHECK(pending[0].text.find("completed successfully") != std::string::npos);
    CHECK(pending[0].text.find("agent-final-output") != std::string::npos);
    CHECK(pending[0].text.find("\"exit_code\": 0") != std::string::npos);

    // The wake-up is scheduling-only. Driving the existing queue consumes
    // the completion without a second user submission.
    agent.run_one_turn();
    CHECK(agent.pending_count() == 0);
    std::this_thread::sleep_for(30ms);
    CHECK(sink->sources().size() == 1);
}

TEST_CASE("Concurrent agent tool threads retain their own completion routes") {
    BackgroundProcessManager::get_instance().cleanup_all();
    auto sink_a = std::make_shared<CompletionSink>();
    auto sink_b = std::make_shared<CompletionSink>();
    auto agent_a = make_agent("route_a", sink_a);
    auto agent_b = make_agent("route_b", sink_b);

    nlohmann::json result_a;
    nlohmann::json result_b;
    std::jthread thread_a([&] { result_a = start(agent_a, "printf 'for-agent-a'"); });
    thread_a.join();
    std::jthread thread_b([&] { result_b = start(agent_b, "printf 'for-agent-b'"); });
    thread_b.join();
    REQUIRE(result_a.value("status", "") == "background");
    REQUIRE(result_b.value("status", "") == "background");
    REQUIRE(sink_a->wait_for_count(1));
    REQUIRE(sink_b->wait_for_count(1));

    const auto pending_a = agent_a.pending_messages();
    const auto pending_b = agent_b.pending_messages();
    REQUIRE(pending_a.size() == 1);
    REQUIRE(pending_b.size() == 1);
    CHECK(pending_a[0].text.find("for-agent-a") != std::string::npos);
    CHECK(pending_a[0].text.find("for-agent-b") == std::string::npos);
    CHECK(pending_b[0].text.find("for-agent-b") != std::string::npos);
    CHECK(pending_b[0].text.find("for-agent-a") == std::string::npos);
}

TEST_CASE("Agent destruction invalidates an outstanding completion route") {
    BackgroundProcessManager::get_instance().cleanup_all();
    auto sink = std::make_shared<CompletionSink>();
    std::optional<daedalus::Agent> agent;
    agent.emplace(make_agent("destroyed", sink));
    const auto result = start(*agent, "sleep 0.1");
    REQUIRE(result.value("status", "") == "background");
    agent.reset();
    std::this_thread::sleep_for(250ms);
    CHECK(sink->sources().empty());
}
