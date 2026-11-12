// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/TaskScheduler.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using daedalus::scheduling::ScheduledTask;
using daedalus::scheduling::TaskScheduler;
using daedalus::scheduling::format_utc;

namespace {

class TempDirectory {
public:
    TempDirectory() {
        static std::atomic<unsigned> sequence = 0;
        path_ = std::filesystem::temp_directory_path()
            / ("daedalus_scheduler_"
               + std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch().count())
               + "_" + std::to_string(sequence++));
        std::filesystem::create_directories(path_);
    }
    ~TempDirectory() { std::filesystem::remove_all(path_); }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

class TriggerRecorder {
public:
    void record(const ScheduledTask& task) {
        {
            std::lock_guard lock(mutex_);
            tasks_.push_back(task);
        }
        cv_.notify_all();
    }

    bool wait_for(std::size_t count, std::chrono::seconds timeout = 3s) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return tasks_.size() >= count; });
    }

    std::size_t size() const {
        std::lock_guard lock(mutex_);
        return tasks_.size();
    }

    std::vector<ScheduledTask> tasks() const {
        std::lock_guard lock(mutex_);
        return tasks_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<ScheduledTask> tasks_;
};

}  // namespace

TEST_CASE("Scheduled task CRUD validates input and persists across restart") {
    TempDirectory temp;
    const auto file = temp.path() / "nested" / "scheduled_tasks.json";
    TaskScheduler scheduler(file);

    CHECK(scheduler.add("bad", "prompt", "once", std::nullopt, std::nullopt)
              .value("status", "") == "error");
    CHECK(scheduler.add("bad", "prompt", "recurring", std::nullopt, std::nullopt)
              .value("status", "") == "error");
    CHECK(scheduler.add("bad", "prompt", "once", "tomorrow", std::nullopt)
              .value("status", "") == "error");
    CHECK(scheduler.add("bad", "prompt", "once", "2030-01-01T00:00:00Z", 5)
              .value("status", "") == "error");

    const auto once = scheduler.add("once", "do once", "once",
                                    "2030-01-01T00:00:00Z", std::nullopt);
    REQUIRE(once.value("status", "") == "success");
    const auto recurring = scheduler.add("repeat", "do again", "recurring",
                                         std::nullopt, 60);
    REQUIRE(recurring.value("status", "") == "success");
    CHECK(recurring.at("task").at("interval_seconds") == 60);
    REQUIRE(std::filesystem::exists(file));

    auto listed = scheduler.list();
    REQUIRE(listed.at("total") == 2);
    const auto first_name = listed.at("tasks").at(0).at("name").get<std::string>();
    const auto second_name = listed.at("tasks").at(1).at("name").get<std::string>();
    CHECK(((first_name == "once" && second_name == "repeat")
           || (first_name == "repeat" && second_name == "once")));
    CHECK(listed.contains("now_utc"));

    const auto id = recurring.at("task").at("id").get<std::uint64_t>();
    auto edited = scheduler.edit(id, "renamed", "new instructions",
                                 std::nullopt, "2031-02-03T04:05:06Z", 120);
    REQUIRE(edited.value("status", "") == "success");
    CHECK(edited.at("task").at("name") == "renamed");
    CHECK(edited.at("task").at("run_at") == "2031-02-03T04:05:06Z");
    CHECK(edited.at("task").at("interval_seconds") == 120);
    CHECK(scheduler.edit(999, "missing", std::nullopt, std::nullopt,
                         std::nullopt, std::nullopt).value("status", "") == "error");

    TaskScheduler restored(file);
    listed = restored.list();
    REQUIRE(listed.at("total") == 2);
    CHECK(restored.add("third", "persist id", "once", "2032-01-01T00:00:00Z",
                       std::nullopt).at("task").at("id") == 3);
    CHECK(restored.remove(id).value("status", "") == "success");
    CHECK(restored.list().at("total") == 2);
    CHECK(restored.remove(id).value("status", "") == "error");
}

TEST_CASE("Malformed task storage is reported and never overwritten") {
    TempDirectory temp;
    const auto file = temp.path() / "scheduled_tasks.json";
    {
        std::ofstream out(file);
        out << "{ definitely not JSON";
    }
    TaskScheduler scheduler(file);
    CHECK(scheduler.list().value("status", "") == "error");
    CHECK(scheduler.add("task", "prompt", "once", "2030-01-01T00:00:00Z",
                        std::nullopt).value("status", "") == "error");
    std::ifstream in(file);
    std::string contents;
    std::getline(in, contents);
    CHECK(contents == "{ definitely not JSON");
}

TEST_CASE("One-shot dispatch consumes persistent state before callback") {
    TempDirectory temp;
    const auto file = temp.path() / "scheduled_tasks.json";
    TaskScheduler scheduler(file);
    const auto past = format_utc(std::chrono::system_clock::now() - 2s);
    REQUIRE(scheduler.add("single", "wake up", "once", past, std::nullopt)
                .value("status", "") == "success");

    TriggerRecorder recorder;
    scheduler.start([&](const ScheduledTask& task) {
        // The worker must not hold its mutex during user/agent code.
        CHECK(scheduler.list().at("total") == 0);
        recorder.record(task);
    });
    REQUIRE(recorder.wait_for(1));
    CHECK(recorder.tasks().front().prompt == "wake up");
    std::this_thread::sleep_for(50ms);
    CHECK(recorder.size() == 1);
    scheduler.stop();

    TaskScheduler restored(file);
    CHECK(restored.list().at("total") == 0);
}

TEST_CASE("Recurring dispatch coalesces overdue slots and supports edit delete stop") {
    TempDirectory temp;
    TaskScheduler scheduler(temp.path() / "scheduled_tasks.json");
    const auto overdue = format_utc(std::chrono::system_clock::now() - 90min);
    const auto added = scheduler.add("repeat", "tick", "recurring", overdue, 1);
    REQUIRE(added.value("status", "") == "success");
    const auto id = added.at("task").at("id").get<std::uint64_t>();

    TriggerRecorder recorder;
    scheduler.start([&](const ScheduledTask& task) { recorder.record(task); });
    REQUIRE(recorder.wait_for(1));
    // Ninety missed minutes must produce one immediate wake-up, not a burst.
    std::this_thread::sleep_for(100ms);
    CHECK(recorder.size() == 1);
    const auto listed = scheduler.list();
    REQUIRE(listed.at("total") == 1);
    CHECK(listed.at("tasks").at(0).contains("last_run_at"));
    CHECK(listed.at("tasks").at(0).at("run_at").get<std::string>()
          > format_utc(std::chrono::system_clock::now() - 1s));

    REQUIRE(recorder.wait_for(2));
    REQUIRE(scheduler.edit(id, std::nullopt, "edited", std::nullopt,
                           std::nullopt, 3600).value("status", "") == "success");
    REQUIRE(scheduler.remove(id).value("status", "") == "success");
    CHECK(scheduler.list().at("total") == 0);
    scheduler.stop();
    const auto count = recorder.size();
    std::this_thread::sleep_for(50ms);
    CHECK(recorder.size() == count);
}
