// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace daedalus::scheduling {

using TimePoint = std::chrono::system_clock::time_point;

struct ScheduledTask {
    std::uint64_t id = 0;
    std::string name;
    std::string prompt;
    bool recurring = false;
    std::optional<int> interval_seconds;
    TimePoint next_run_at{};
    std::string created_at;
    std::optional<std::string> last_run_at;

    [[nodiscard]] nlohmann::json to_json() const;
};

/// Persistent time-based task queue. Mutations and the worker share one mutex;
/// callbacks always run after the due state has been saved and the mutex freed.
class TaskScheduler {
public:
    using TriggerCallback = std::function<void(const ScheduledTask&)>;

    explicit TaskScheduler(std::filesystem::path file_path);
    ~TaskScheduler();

    TaskScheduler(const TaskScheduler&) = delete;
    TaskScheduler& operator=(const TaskScheduler&) = delete;

    nlohmann::json add(std::string name,
                       std::string prompt,
                       std::string_view kind,
                       const std::optional<std::string>& run_at,
                       std::optional<int> interval_seconds);
    nlohmann::json edit(std::uint64_t id,
                        const std::optional<std::string>& name,
                        const std::optional<std::string>& prompt,
                        const std::optional<std::string>& kind,
                        const std::optional<std::string>& run_at,
                        std::optional<int> interval_seconds);
    nlohmann::json remove(std::uint64_t id);
    [[nodiscard]] nlohmann::json list() const;

    void start(TriggerCallback callback);
    void stop() noexcept;

private:
    std::filesystem::path file_path_;
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::jthread worker_;
    TriggerCallback callback_;
    std::vector<ScheduledTask> tasks_;
    std::uint64_t next_id_ = 1;
    std::string load_error_;

    void load();
    void save_locked() const;
    void run(std::stop_token stop_token);
    [[nodiscard]] nlohmann::json mutation_error(std::string message) const;
};

/// Process-wide lookup matches the todo service: tools resolve the same
/// per-agent scheduler from FileIO without another global tool binding.
class TaskSchedulerService {
public:
    static TaskSchedulerService& instance();
    TaskScheduler& get(const std::filesystem::path& project_root,
                       std::string_view agent_name);

private:
    TaskSchedulerService() = default;
    ~TaskSchedulerService() = default;

    std::mutex mutex_;
    std::map<std::pair<std::string, std::string>,
             std::unique_ptr<TaskScheduler>> schedulers_;
};

[[nodiscard]] std::string format_utc(TimePoint value);
[[nodiscard]] std::optional<TimePoint> parse_utc(std::string_view value);

}  // namespace daedalus::scheduling
