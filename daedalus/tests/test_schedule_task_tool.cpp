// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/FileIO.h"
#include "core/TaskScheduler.h"
#include "tools/ScheduleTaskTool.h"

#include <daedalus.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace {

std::filesystem::path temporary_project() {
    static std::atomic<unsigned> sequence = 0;
    const auto path = std::filesystem::temp_directory_path()
        / ("daedalus_schedule_tool_"
           + std::to_string(std::chrono::steady_clock::now()
                                .time_since_epoch().count())
           + "_" + std::to_string(sequence++));
    std::filesystem::create_directories(path);
    return path;
}

}  // namespace

TEST_CASE("schedule_task exposes isolated per-agent CRUD and useful errors") {
    const auto root = temporary_project();
    FileIO alpha_io(root, "alpha");
    FileIO beta_io(root, "beta");
    ScheduleTaskTool tool;

    CHECK(tool.get_name() == "schedule_task");
    CHECK(tool.get_definition().at("function").at("parameters")
              .at("required").at(0) == "action");
    CHECK(tool.execute({{"action", "unknown"}}, alpha_io)
              .value("status", "") == "error");
    CHECK(tool.execute({{"action", "add"}, {"name", "missing prompt"}},
                       alpha_io).value("status", "") == "error");

    const auto added = tool.execute({
        {"action", "add"}, {"name", "review"}, {"prompt", "Review the build"},
        {"kind", "once"}, {"run_at", "2030-01-01T00:00:00Z"}
    }, alpha_io);
    REQUIRE(added.value("status", "") == "success");
    const auto id = added.at("task").at("id").get<std::uint64_t>();
    CHECK(tool.execute({{"action", "list"}}, alpha_io).at("total") == 1);
    CHECK(tool.execute({{"action", "list"}}, beta_io).at("total") == 0);
    CHECK(std::filesystem::exists(
        root / ".daedalus" / "daedalus-alpha" / "scheduled_tasks.json"));

    CHECK(tool.execute({
        {"action", "edit"}, {"id", id}, {"prompt", "Updated instructions"}
    }, alpha_io).value("status", "") == "success");
    const auto listed = tool.execute({{"action", "list"}}, alpha_io);
    CHECK(listed.at("tasks").at(0).at("prompt") == "Updated instructions");
    CHECK(tool.execute({{"action", "delete"}, {"id", id}}, alpha_io)
              .value("status", "") == "success");
    CHECK(tool.execute({{"action", "list"}}, alpha_io).at("total") == 0);

    std::filesystem::remove_all(root);
}


namespace {

class ScheduledSink final : public daedalus::AgentSink {
public:
    void on_turn_requested(std::string_view source) noexcept override {
        {
            std::lock_guard lock(mutex_);
            sources_.emplace_back(source);
        }
        cv_.notify_all();
    }

    bool wait_for_wake() {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, std::chrono::seconds(3),
                            [&] { return !sources_.empty(); });
    }

    std::size_t wake_count() {
        std::lock_guard lock(mutex_);
        return sources_.size();
    }

    void block_next_error() {
        std::lock_guard lock(mutex_);
        block_error_ = true;
        error_entered_ = false;
        release_error_ = false;
    }

    void on_error(daedalus::ErrorKind, std::string_view,
                  bool) noexcept override {
        std::unique_lock lock(mutex_);
        if (!block_error_) return;
        error_entered_ = true;
        cv_.notify_all();
        cv_.wait(lock, [&] { return release_error_; });
        block_error_ = false;
    }

    bool wait_for_error() {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, std::chrono::seconds(3),
                            [&] { return error_entered_; });
    }

    void release_error() {
        {
            std::lock_guard lock(mutex_);
            release_error_ = true;
        }
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::string> sources_;
    bool block_error_ = false;
    bool error_entered_ = false;
    bool release_error_ = false;
};

}  // namespace

TEST_CASE("Due task queues its origin-tagged prompt and wakes an idle Agent host") {
    const auto root = temporary_project();
    auto sink = std::make_shared<ScheduledSink>();
    daedalus::AgentConfig config;
    config.project_root = root;
    config.daedalus_home = root / "home";
    config.agent_name = "scheduled-agent";
    config.auto_approve = true;
    config.memory_enabled = false;
    config.mcp_enabled = false;
    daedalus::Agent agent(std::move(config), sink);

    const auto past = daedalus::scheduling::format_utc(
        std::chrono::system_clock::now() - std::chrono::seconds(2));
    const auto added = agent.execute_tool("schedule_task", {
        {"action", "add"}, {"name", "host-wake"}, {"kind", "once"},
        {"run_at", past}, {"prompt", "Perform the scheduled host-wake check"}
    });
    REQUIRE(added.value("status", "") == "success");
    REQUIRE(sink->wait_for_wake());
    REQUIRE(agent.pending_count() == 1);
    const auto pending = agent.pending_messages();
    REQUIRE(pending.size() == 1);
    CHECK(pending.front().source == "trigger:task-1");
    CHECK(pending.front().text.find("Perform the scheduled host-wake check")
          != std::string::npos);
    CHECK(agent.execute_tool("schedule_task", {{"action", "list"}})
              .at("total") == 0);
    agent.shutdown();
    std::filesystem::remove_all(root);
}


TEST_CASE("Scheduled occurrences are dropped while an Agent turn is active") {
    const auto root = temporary_project();
    auto sink = std::make_shared<ScheduledSink>();
    daedalus::AgentConfig config;
    config.project_root = root;
    config.daedalus_home = root / "home";
    config.agent_name = "busy-scheduled-agent";
    config.auto_approve = true;
    config.memory_enabled = false;
    config.mcp_enabled = false;
    daedalus::Agent agent(std::move(config), sink);
    agent.initialize();

    // With no LLM configured, run_one_turn reports on_error synchronously.
    // Blocking that callback gives the test a real active turn without network.
    sink->block_next_error();
    agent.submit_user_message("hold the turn open");
    std::jthread turn([&] { agent.run_one_turn(); });
    REQUIRE(sink->wait_for_error());

    const auto past = daedalus::scheduling::format_utc(
        std::chrono::system_clock::now() - std::chrono::seconds(2));
    REQUIRE(agent.execute_tool("schedule_task", {
        {"action", "add"}, {"name", "busy-once"}, {"kind", "once"},
        {"run_at", past}, {"prompt", "must not queue"}
    }).value("status", "") == "success");
    REQUIRE(agent.execute_tool("schedule_task", {
        {"action", "add"}, {"name", "busy-repeat"}, {"kind", "recurring"},
        {"run_at", past}, {"interval_seconds", 3600},
        {"prompt", "drop this occurrence only"}
    }).value("status", "") == "success");

    // Wait until persistent consumption is observable: the one-shot is gone,
    // while the recurring task remains with a future deadline.
    bool consumed = false;
    for (int attempt = 0; attempt < 300; ++attempt) {
        const auto listed = agent.execute_tool("schedule_task", {{"action", "list"}});
        if (listed.at("total") == 1
            && listed.at("tasks").at(0).at("name") == "busy-repeat"
            && listed.at("tasks").at(0).at("run_at").get<std::string>() > past) {
            consumed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(consumed);
    CHECK(agent.pending_count() == 0);
    CHECK(sink->wake_count() == 0);

    sink->release_error();
    turn.join();

    // Error is a terminal state, not an active turn. The next deadline queues
    // normally, proving that a dropped occurrence does not disable scheduling.
    REQUIRE(agent.execute_tool("schedule_task", {
        {"action", "add"}, {"name", "after-busy"}, {"kind", "once"},
        {"run_at", past}, {"prompt", "queue after the turn"}
    }).value("status", "") == "success");
    REQUIRE(sink->wait_for_wake());
    REQUIRE(agent.pending_count() == 1);
    CHECK(agent.pending_messages().front().text.find("queue after the turn")
          != std::string::npos);

    agent.shutdown();
    std::filesystem::remove_all(root);
}
