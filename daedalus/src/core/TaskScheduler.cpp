// SPDX-License-Identifier: BSL-1.0
#include "core/TaskScheduler.h"

#include "core/Logger.h"
#include "daedalus/default_paths.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace daedalus::scheduling {
namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

constexpr std::string_view kOnce = "once";
constexpr std::string_view kRecurring = "recurring";
constexpr int kMaxIntervalSeconds = 31'536'000;  // one year
constexpr std::size_t kMaxTasks = 1000;

json success(std::string message, json extra = json::object()) {
    json result{{"status", "success"}, {"message", std::move(message)}};
    result.update(std::move(extra));
    return result;
}

json error(std::string message) {
    return json{{"status", "error"}, {"message", std::move(message)}};
}

std::optional<int> valid_interval(std::optional<int> value) {
    if (!value || *value < 1 || *value > kMaxIntervalSeconds) {
        return std::nullopt;
    }
    return value;
}

json task_from_json(const json& value) {
    if (!value.is_object()) throw std::runtime_error("task is not an object");
    const auto id = value.at("id").get<std::uint64_t>();
    const auto name = value.at("name").get<std::string>();
    const auto prompt = value.at("prompt").get<std::string>();
    const auto kind = value.at("kind").get<std::string>();
    const auto next = parse_utc(value.at("run_at").get<std::string>());
    if (id == 0 || name.empty() || prompt.empty() || !next) {
        throw std::runtime_error("task has invalid identity, text, or run_at");
    }

    ScheduledTask task;
    task.id = id;
    task.name = name;
    task.prompt = prompt;
    task.recurring = kind == kRecurring;
    if (!task.recurring && kind != kOnce) {
        throw std::runtime_error("task kind must be 'once' or 'recurring'");
    }
    if (task.recurring) {
        task.interval_seconds = valid_interval(
            value.at("interval_seconds").get<int>());
        if (!task.interval_seconds) {
            throw std::runtime_error("recurring task has invalid interval");
        }
    }
    task.next_run_at = *next;
    task.created_at = value.value("created_at", "");
    if (value.contains("last_run_at") && value["last_run_at"].is_string()) {
        task.last_run_at = value["last_run_at"].get<std::string>();
    }
    return task.to_json();
}

ScheduledTask decode_task(const json& value) {
    // Reuse the validator above without exposing another serialization path.
    const json checked = task_from_json(value);
    ScheduledTask task;
    task.id = checked.at("id").get<std::uint64_t>();
    task.name = checked.at("name").get<std::string>();
    task.prompt = checked.at("prompt").get<std::string>();
    task.recurring = checked.at("kind").get<std::string>() == kRecurring;
    if (task.recurring) {
        task.interval_seconds = checked.at("interval_seconds").get<int>();
    }
    task.next_run_at = *parse_utc(checked.at("run_at").get<std::string>());
    task.created_at = checked.value("created_at", "");
    if (checked.contains("last_run_at")) {
        task.last_run_at = checked.at("last_run_at").get<std::string>();
    }
    return task;
}

}  // namespace

std::string format_utc(TimePoint value) {
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(value);
    const std::time_t time = std::chrono::system_clock::to_time_t(seconds);
    std::tm utc{};
    gmtime_r(&time, &utc);
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::optional<TimePoint> parse_utc(std::string_view value) {
    const std::string input(value);
    std::istringstream in(input);
    std::chrono::sys_seconds parsed;
    in >> std::chrono::parse("%Y-%m-%dT%H:%M:%SZ", parsed);
    if (in.fail()) return std::nullopt;
    in >> std::ws;
    if (!in.eof()) return std::nullopt;
    return parsed;
}

nlohmann::json ScheduledTask::to_json() const {
    json result{
        {"id", id},
        {"name", name},
        {"prompt", prompt},
        {"kind", recurring ? kRecurring : kOnce},
        {"run_at", format_utc(next_run_at)},
        {"created_at", created_at}
    };
    if (recurring) result["interval_seconds"] = interval_seconds.value_or(0);
    if (last_run_at) result["last_run_at"] = *last_run_at;
    return result;
}

TaskScheduler::TaskScheduler(fs::path file_path)
    : file_path_(std::move(file_path)) {
    load();
}

TaskScheduler::~TaskScheduler() {
    stop();
}

nlohmann::json TaskScheduler::mutation_error(std::string message) const {
    if (!load_error_.empty()) {
        return error("Scheduled-task store cannot be used: " + load_error_);
    }
    return error(std::move(message));
}

json TaskScheduler::add(std::string name,
                        std::string prompt,
                        std::string_view kind,
                        const std::optional<std::string>& run_at,
                        std::optional<int> interval_seconds) {
    std::lock_guard lock(mutex_);
    if (!load_error_.empty()) return mutation_error({});
    if (name.empty()) return error("Task name cannot be empty");
    if (prompt.empty()) return error("Task prompt cannot be empty");
    if (kind != kOnce && kind != kRecurring) {
        return error("Task kind must be 'once' or 'recurring'");
    }
    if (tasks_.size() >= kMaxTasks) {
        return error("Scheduled-task limit reached (1000); delete a task first");
    }

    const bool recurring = kind == kRecurring;
    if (recurring && !valid_interval(interval_seconds)) {
        return error("Recurring tasks require interval_seconds between 1 and 31536000");
    }
    if (!recurring && interval_seconds) {
        return error("One-shot tasks must not specify interval_seconds");
    }
    if (!recurring && !run_at) {
        return error("One-shot tasks require run_at in UTC (YYYY-MM-DDTHH:MM:SSZ)");
    }

    const auto now = std::chrono::system_clock::now();
    TimePoint next = now + std::chrono::seconds(interval_seconds.value_or(0));
    if (run_at) {
        const auto parsed = parse_utc(*run_at);
        if (!parsed) {
            return error("Invalid run_at; use UTC YYYY-MM-DDTHH:MM:SSZ (for example 2026-11-12T09:30:00Z)");
        }
        next = *parsed;
    }

    ScheduledTask task;
    task.id = next_id_++;
    task.name = std::move(name);
    task.prompt = std::move(prompt);
    task.recurring = recurring;
    task.interval_seconds = recurring ? interval_seconds : std::nullopt;
    task.next_run_at = next;
    task.created_at = format_utc(now);

    tasks_.push_back(std::move(task));
    try {
        save_locked();
    } catch (const std::exception& e) {
        tasks_.pop_back();
        --next_id_;
        return error(std::string("Could not persist scheduled task: ") + e.what());
    }
    wake_.notify_all();
    return success("Scheduled task added", {{"task", tasks_.back().to_json()}});
}

json TaskScheduler::edit(std::uint64_t id,
                         const std::optional<std::string>& name,
                         const std::optional<std::string>& prompt,
                         const std::optional<std::string>& kind,
                         const std::optional<std::string>& run_at,
                         std::optional<int> interval_seconds) {
    std::lock_guard lock(mutex_);
    if (!load_error_.empty()) return mutation_error({});
    if (!name && !prompt && !kind && !run_at && !interval_seconds) {
        return error("Provide at least one field to edit");
    }
    if (name && name->empty()) return error("Task name cannot be empty");
    if (prompt && prompt->empty()) return error("Task prompt cannot be empty");
    if (kind && *kind != kOnce && *kind != kRecurring) {
        return error("Task kind must be 'once' or 'recurring'");
    }

    const auto found = std::ranges::find_if(tasks_, [id](const auto& task) {
        return task.id == id;
    });
    if (found == tasks_.end()) {
        return error("Scheduled task not found: id " + std::to_string(id));
    }

    ScheduledTask updated = *found;
    const bool recurring = kind ? *kind == kRecurring : updated.recurring;
    if (recurring) {
        const auto effective_interval = interval_seconds
            ? interval_seconds : updated.interval_seconds;
        if (!valid_interval(effective_interval)) {
            return error("Recurring tasks require interval_seconds between 1 and 31536000");
        }
        updated.interval_seconds = effective_interval;
    } else {
        if (interval_seconds) {
            return error("One-shot tasks must not specify interval_seconds");
        }
        updated.interval_seconds.reset();
    }

    if (run_at) {
        const auto parsed = parse_utc(*run_at);
        if (!parsed) {
            return error("Invalid run_at; use UTC YYYY-MM-DDTHH:MM:SSZ");
        }
        updated.next_run_at = *parsed;
    } else if (interval_seconds || (kind && recurring != updated.recurring)) {
        updated.next_run_at = std::chrono::system_clock::now()
            + std::chrono::seconds(updated.interval_seconds.value_or(0));
    }
    if (name) updated.name = *name;
    if (prompt) updated.prompt = *prompt;
    updated.recurring = recurring;

    const ScheduledTask original = *found;
    *found = std::move(updated);
    try {
        save_locked();
    } catch (const std::exception& e) {
        *found = original;
        return error(std::string("Could not persist scheduled task: ") + e.what());
    }
    wake_.notify_all();
    return success("Scheduled task updated", {{"task", found->to_json()}});
}

json TaskScheduler::remove(std::uint64_t id) {
    std::lock_guard lock(mutex_);
    if (!load_error_.empty()) return mutation_error({});
    const auto found = std::ranges::find_if(tasks_, [id](const auto& task) {
        return task.id == id;
    });
    if (found == tasks_.end()) {
        return error("Scheduled task not found: id " + std::to_string(id));
    }
    const json removed = found->to_json();
    const auto original = tasks_;
    tasks_.erase(found);
    try {
        save_locked();
    } catch (const std::exception& e) {
        tasks_ = original;
        return error(std::string("Could not persist scheduled-task deletion: ") + e.what());
    }
    wake_.notify_all();
    return success("Scheduled task deleted", {{"task", removed}});
}

json TaskScheduler::list() const {
    std::lock_guard lock(mutex_);
    std::vector<json> ordered;
    ordered.reserve(tasks_.size());
    for (const auto& task : tasks_) ordered.push_back(task.to_json());
    std::ranges::sort(ordered, [](const json& left, const json& right) {
        if (left.at("run_at") == right.at("run_at")) {
            return left.at("id") < right.at("id");
        }
        return left.at("run_at") < right.at("run_at");
    });
    json tasks = std::move(ordered);
    json result{
        {"status", load_error_.empty() ? "success" : "error"},
        {"tasks", std::move(tasks)},
        {"total", tasks_.size()},
        {"now_utc", format_utc(std::chrono::system_clock::now())},
        {"storage_file", file_path_.string()}
    };
    if (!load_error_.empty()) result["message"] = load_error_;
    return result;
}

void TaskScheduler::start(TriggerCallback callback) {
    if (!callback) throw std::invalid_argument("TaskScheduler requires a callback");
    std::lock_guard lock(mutex_);
    callback_ = std::move(callback);
    if (worker_.joinable()) {
        wake_.notify_all();
        return;
    }
    worker_ = std::jthread([this](std::stop_token stop_token) {
        run(stop_token);
    });
}

void TaskScheduler::stop() noexcept {
    if (worker_.joinable()) {
        {
            // Match the worker's stop-check/wait mutex. Requesting and
            // notifying without it allows the worker to check false, then
            // miss our notification and sleep forever before join().
            std::lock_guard lock(mutex_);
            worker_.request_stop();
            wake_.notify_all();
        }
        worker_.join();
    }
    std::lock_guard lock(mutex_);
    callback_ = {};
}

void TaskScheduler::run(std::stop_token stop_token) {
    std::unique_lock lock(mutex_);
    while (!stop_token.stop_requested()) {
        const auto now = std::chrono::system_clock::now();
        const auto due = std::ranges::find_if(tasks_, [now](const auto& task) {
            return task.next_run_at <= now;
        });
        if (due != tasks_.end()) {
            const ScheduledTask task = *due;
            const auto original = tasks_;
            if (task.recurring) {
                auto next = task.next_run_at;
                const auto interval = std::chrono::seconds(*task.interval_seconds);
                // Coalesce downtime arithmetically: one wake-up, then the
                // first future slot, without looping once per missed second.
                const auto elapsed_intervals = (now - next) / interval + 1;
                next += elapsed_intervals * interval;
                due->next_run_at = next;
                due->last_run_at = format_utc(now);
            } else {
                tasks_.erase(due);
            }

            try {
                // §SCHEDULED-TASKS (2026-11-11): consume before dispatch. A
                // crash or callback failure can therefore never replay a task.
                save_locked();
            } catch (const std::exception& e) {
                tasks_ = original;
                LOG_ERROR(std::string("[SCHEDULE] Could not consume due task: ") + e.what());
                wake_.wait_for(lock, 1s);
                continue;
            }

            auto callback = callback_;
            lock.unlock();
            if (callback) {
                try {
                    callback(task);
                } catch (const std::exception& e) {
                    LOG_ERROR(std::string("[SCHEDULE] Task callback failed: ") + e.what());
                } catch (...) {
                    LOG_ERROR("[SCHEDULE] Task callback failed with an unknown exception");
                }
            }
            lock.lock();
            continue;
        }

        if (tasks_.empty()) {
            // stop() requests the jthread stop and notifies this same CV.
            // Plain waits are deliberate: stop-token waits on this GCC build
            // did not wake for add/edit notifications in the Agent routing
            // regression test.
            wake_.wait(lock);
            continue;
        }
        const auto next = std::ranges::min_element(tasks_,
            [](const auto& left, const auto& right) {
                return left.next_run_at < right.next_run_at;
            })->next_run_at;
        wake_.wait_until(lock, next);
    }
}

void TaskScheduler::load() {
    std::lock_guard lock(mutex_);
    tasks_.clear();
    next_id_ = 1;
    load_error_.clear();
    std::error_code ec;
    if (!fs::exists(file_path_, ec)) return;

    try {
        std::ifstream in(file_path_);
        if (!in) throw std::runtime_error("could not open " + file_path_.string());
        const json root = json::parse(in);
        if (!root.is_object() || !root.at("tasks").is_array()) {
            throw std::runtime_error("root must be an object with a tasks array");
        }
        for (const auto& value : root.at("tasks")) {
            const auto task = decode_task(value);
            if (std::ranges::any_of(tasks_, [&](const auto& existing) {
                    return existing.id == task.id;
                })) {
                throw std::runtime_error("duplicate task id " + std::to_string(task.id));
            }
            next_id_ = std::max(next_id_, task.id + 1);
            tasks_.push_back(task);
        }
        if (root.contains("next_id")) {
            next_id_ = std::max(next_id_, root.at("next_id").get<std::uint64_t>());
        }
    } catch (const std::exception& e) {
        tasks_.clear();
        next_id_ = 1;
        load_error_ = file_path_.string() + ": " + e.what();
        LOG_ERROR("[SCHEDULE] Failed to load scheduled tasks: " + load_error_);
    }
}

void TaskScheduler::save_locked() const {
    json tasks = json::array();
    for (const auto& task : tasks_) tasks.push_back(task.to_json());
    const json root{{"version", 1}, {"next_id", next_id_}, {"tasks", tasks}};

    const fs::path parent = file_path_.parent_path();
    fs::create_directories(parent);
    const fs::path temporary = file_path_.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("could not open " + temporary.string());
        out << root.dump(2, ' ', false, json::error_handler_t::replace) << '\n';
        out.flush();
        if (!out) throw std::runtime_error("could not write " + temporary.string());
    }
    std::error_code ec;
    fs::rename(temporary, file_path_, ec);
    if (ec) {
        fs::remove(temporary);
        throw std::runtime_error("could not replace " + file_path_.string()
                                 + ": " + ec.message());
    }
}

TaskSchedulerService& TaskSchedulerService::instance() {
    static TaskSchedulerService service;
    return service;
}

TaskScheduler& TaskSchedulerService::get(const fs::path& project_root,
                                         std::string_view agent_name) {
    // FileIO canonicalizes its root. Do the same here so the Agent's worker
    // and schedule_task always share one instance even for relative/symlinked
    // project paths.
    const auto normalized_root = fs::weakly_canonical(project_root);
    const std::string root = normalized_root.string();
    const std::string name(agent_name);
    std::lock_guard lock(mutex_);
    auto& scheduler = schedulers_[{root, name}];
    if (!scheduler) {
        scheduler = std::make_unique<TaskScheduler>(
            daedalus::resolve_agent_dir(normalized_root, agent_name)
            / "scheduled_tasks.json");
    }
    return *scheduler;
}

}  // namespace daedalus::scheduling
