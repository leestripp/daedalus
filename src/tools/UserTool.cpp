#include "tools/UserTool.h"
#include "core/FileIO.h"

#include <iostream>
#include <string>
#include <stdexcept>

#include "core/Logger.h"

// §Phase 5.1: helper macro that converts LOG_CERR(INFO,  ...); style
// statements into LOG_INFO/LOG_WARNING/LOG_ERROR calls via the
// existing Logger. Same formatted text routed through the Logger
// instead of writing to stderr directly.
#define LOG_CERR(level, expr) do {                          \
        std::ostringstream _daedalus_msg;                   \
        _daedalus_msg << expr;                               \
        LOG_##level(_daedalus_msg.str());                    \
    } while (0)
// ========== TellUserTool ==========

json TellUserTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", "Send a message to the user. Use this to communicate results, status updates, or ask the user to review something. The message will be displayed to the user."},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"content", json::object({
                        {"type", "string"},
                        {"description", "The message to send to the user"}
                    })}
                })},
                {"required", json::array({"content"})}
            })}
        }}
    });
}

json TellUserTool::execute(const json& args, FileIO&) {
    (void)args;
    try {
        std::string content;

        if (args.is_object()) {
            if (args.contains("content")) {
                content = args["content"].get<std::string>();
            } else {
                return json::object({{"status", "error"}, {"message", "Missing 'content' in arguments"}});
            }
        } else if (args.is_string()) {
            try {
                json parsed_args = json::parse(args.get<std::string>());
                if (parsed_args.contains("content")) {
                    content = parsed_args["content"].get<std::string>();
                } else {
                    return json::object({{"status", "error"}, {"message", "Missing 'content' in arguments"}});
                }
            } catch (...) {
                content = args.get<std::string>();
            }
        } else {
            try {
                content = args.get<std::string>();
            } catch (...) {
                return json::object({{"status", "error"}, {"message", "Invalid argument format for tell_user"}});
            }
        }

        // §Phase 5.1: removed the std::cout call. The library
        // doesn't reach for the host's terminal directly. The
        // content is returned in the JSON result; the agent loop
        // forwards it to the sink elsewhere, and the host chooses
        // how to display it (terminal / web UI / log).
        return json::object({
            {"status", "success"},
            {"message", "Message sent to user."},
            {"content", content}
        });
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string(e.what())}});
    }
}

// ========== StopTool ==========

json StopTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", "Signal that the task is complete. Call this when you have finished the user's request - all research, planning, edits, and verification are done. This will end the agent loop."},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"summary", json::object({
                        {"type", "string"},
                        {"description", "Brief summary of what was accomplished"}
                    })}
                })},
                {"required", json::array({"summary"})}
            })}
        }}
    });
}

json StopTool::execute(const json& args, FileIO&) {
    try {
        std::string summary;

        if (args.is_object() && args.contains("summary")) {
            summary = args["summary"].get<std::string>();
        } else if (args.is_string()) {
            try {
                json parsed_args = json::parse(args.get<std::string>());
                summary = parsed_args.value("summary", "");
            } catch (...) {
                summary = args.get<std::string>();
            }
        } else {
            summary = args.value("summary", "");
        }

        if (summary.empty()) {
            return json::object({{"status", "error"}, {"message", "Missing 'summary' in arguments"}});
        }

        return json::object({
            {"status", "stop"},
            {"summary", summary}
        });
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string(e.what())}});
    }
}
