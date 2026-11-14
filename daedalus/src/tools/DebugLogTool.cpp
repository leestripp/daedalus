// SPDX-License-Identifier: BSL-1.0
// tools/DebugLogTool.cpp — §DEBUG-LOG shared agent/app debug log.

#include "tools/DebugLogTool.h"

#include "core/FileIO.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace {

std::string utc_now() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&time, &tm);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

std::filesystem::path default_path(const FileIO& file_io) {
    return file_io.get_project_path() / ".daedalus" / "app-debug.log";
}

void append_line(const std::filesystem::path& path, const std::string& tag,
                 const std::string& message) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream stream(path, std::ios::app);
    if (!stream) throw std::runtime_error("debug_log: cannot open " + path.string());
    stream << utc_now() << " [" << tag << "] " << message << '\n';
}

}  // namespace

json DebugLogTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", "Append a line to the shared debug log, or tail its most recent lines. "
                            "The host app logs UI and event-stream events to the same file with an "
                            "[app] tag; agent lines carry an [agent] tag."},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"action", json::object({
                        {"type", "string"},
                        {"enum", json::array({"append", "tail"})},
                        {"description", "append (default) writes message; tail reads the last lines"}
                    })},
                    {"message", json::object({
                        {"type", "string"},
                        {"description", "Line to append (required for append)"}
                    })},
                    {"lines", json::object({
                        {"type", "integer"},
                        {"description", "Number of lines for tail (default 100)"}
                    })},
                    {"path", json::object({
                        {"type", "string"},
                        {"description", "Explicit log file (default <project>/.daedalus/app-debug.log)"}
                    })}
                })},
                {"required", json::array()}
            })}
        }}
    });
}

json DebugLogTool::execute(const json& args, FileIO& file_io) {
    const std::string action = args.value("action", "append");
    const std::filesystem::path path =
        args.contains("path") && !args.at("path").is_null()
            ? std::filesystem::path(args.at("path").get<std::string>())
            : default_path(file_io);

    if (action == "append") {
        if (!args.contains("message") || !args.at("message").is_string())
            return json{{"status", "error"}, {"message", "debug_log append requires a string 'message' argument."}};
        append_line(path, "agent", args.at("message").get<std::string>());
        return json{{"status", "ok"}, {"action", "append"}, {"path", path.string()}};
    }

    if (action == "tail") {
        const std::size_t wanted = args.value("lines", 100);
        std::error_code error;
        if (!std::filesystem::exists(path, error))
            return json{{"status", "ok"}, {"action", "tail"}, {"path", path.string()},
                        {"lines", json::array()}, {"total", 0}};
        std::ifstream stream(path);
        if (!stream) return json{{"status", "error"}, {"message", "debug_log: cannot read " + path.string()}};
        std::vector<std::string> all;
        std::string line;
        while (std::getline(stream, line)) all.push_back(line);
        const std::size_t total = all.size();
        const std::size_t start = total > wanted ? total - wanted : 0;
        json lines = json::array();
        for (std::size_t index = start; index < total; ++index) lines.push_back(all[index]);
        return json{{"status", "ok"}, {"action", "tail"}, {"path", path.string()},
                    {"lines", std::move(lines)}, {"total", total}};
    }

    return json{{"status", "error"}, {"message", "debug_log: unknown action '" + action + "' (append|tail)"}};
}
