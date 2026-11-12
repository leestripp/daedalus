// SPDX-License-Identifier: BSL-1.0
#include "tools/ScheduleTaskTool.h"

#include "core/FileIO.h"
#include "core/Logger.h"
#include "core/TaskScheduler.h"

#include <cstdint>
#include <optional>
#include <string>

using json = nlohmann::json;

namespace {

std::optional<std::string> optional_string(const json& args,
                                           std::string_view key) {
    if (!args.contains(key)) return std::nullopt;
    if (!args.at(key).is_string()) {
        throw std::invalid_argument("'" + std::string(key) + "' must be a string");
    }
    return args.at(key).get<std::string>();
}

std::optional<int> optional_interval(const json& args) {
    if (!args.contains("interval_seconds")) return std::nullopt;
    if (!args.at("interval_seconds").is_number_integer()) {
        throw std::invalid_argument("'interval_seconds' must be an integer");
    }
    return args.at("interval_seconds").get<int>();
}

std::uint64_t required_id(const json& args) {
    if (!args.contains("id") || !args.at("id").is_number_unsigned()) {
        if (args.contains("id") && args.at("id").is_number_integer()
            && args.at("id").get<std::int64_t>() > 0) {
            return args.at("id").get<std::uint64_t>();
        }
        throw std::invalid_argument("A positive integer 'id' is required");
    }
    const auto id = args.at("id").get<std::uint64_t>();
    if (id == 0) throw std::invalid_argument("Task 'id' must be positive");
    return id;
}

}  // namespace

json ScheduleTaskTool::get_definition() const {
    return json{
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description",
             "Manage persistent scheduled tasks. action='add' creates a task; "
             "'list' returns all tasks and current UTC time; 'edit' changes any "
             "supplied fields of id; 'delete' removes id. For kind='once', run_at "
             "is required and the task deletes itself when fired. For "
             "kind='recurring', interval_seconds is required when adding, and "
             "run_at optionally sets the first run (default: now + interval). "
             "Times are UTC in YYYY-MM-DDTHH:MM:SSZ form. Overdue tasks fire once "
             "on startup; missed recurring intervals coalesce. If the agent is "
             "busy when a task fires, that occurrence is dropped permanently "
             "instead of queued. Tasks only run while an agent host is active."},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"action", {
                        {"type", "string"},
                        {"enum", {"add", "list", "edit", "delete"}},
                        {"description", "Operation to perform."}
                    }},
                    {"id", {
                        {"type", "integer"},
                        {"minimum", 1},
                        {"description", "Task id returned by add/list; required for edit/delete."}
                    }},
                    {"name", {
                        {"type", "string"},
                        {"description", "Short task name; required for add."}
                    }},
                    {"prompt", {
                        {"type", "string"},
                        {"description", "Instructions submitted to the agent when due; required for add."}
                    }},
                    {"kind", {
                        {"type", "string"},
                        {"enum", {"once", "recurring"}},
                        {"description", "Schedule type. Add defaults to 'once'."}
                    }},
                    {"run_at", {
                        {"type", "string"},
                        {"description", "UTC deadline YYYY-MM-DDTHH:MM:SSZ. Required for one-shot add; optional first/next run otherwise."}
                    }},
                    {"interval_seconds", {
                        {"type", "integer"},
                        {"minimum", 1},
                        {"maximum", 31536000},
                        {"description", "Fixed recurrence interval (1 second to 1 year)."}
                    }}
                }},
                {"required", {"action"}}
            }}
        }}
    };
}

json ScheduleTaskTool::execute(const json& args, FileIO& file_io) {
    try {
        const std::string action = args.at("action").get<std::string>();
        auto& scheduler = daedalus::scheduling::TaskSchedulerService::instance().get(
            file_io.get_project_path(), file_io.get_agent_name());
        if (action == "list") return scheduler.list();

        if (action == "add") {
            return scheduler.add(
                args.at("name").get<std::string>(),
                args.at("prompt").get<std::string>(),
                args.value("kind", "once"),
                optional_string(args, "run_at"),
                optional_interval(args));
        }
        if (action == "edit") {
            return scheduler.edit(
                required_id(args),
                optional_string(args, "name"),
                optional_string(args, "prompt"),
                optional_string(args, "kind"),
                optional_string(args, "run_at"),
                optional_interval(args));
        }
        if (action == "delete") return scheduler.remove(required_id(args));
        return json{{"status", "error"},
                    {"message", "Unknown action '" + action
                                + "'; use add, list, edit, or delete"}};
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("[SCHEDULE] schedule_task failed: ") + e.what());
        return json{{"status", "error"}, {"message", e.what()}};
    }
}
