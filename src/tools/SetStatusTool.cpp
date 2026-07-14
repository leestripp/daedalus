#include "tools/SetStatusTool.h"
#include "core/FileIO.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

namespace {

// Public AgentState names — keep in sync with
// include/daedalus/types/AgentState.h and
// demos/daedalus-webui/web_sse_sink.h.
constexpr std::array<std::string_view, 11> kValidStates = {
    "Idle",
    "Understanding",
    "Planning",
    "Acting",
    "Verifying",
    "AwaitingUser",
    "AwaitingApproval",
    "Compressing",
    "Done",
    "Cancelled",
    "Error",
};

// Title-case helper: lowercase input, then upper-case the first letter.
// e.g. "planning" -> "Planning", "ACTING" -> "Acting".

// Return the canonical PascalCase form of the matched state, or ""
// if no match (case-insensitive).
std::string canonicalise(std::string_view name) {
    auto eq_ci = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
            char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
            if (ca != cb) return false;
        }
        return true;
    };
    for (auto v : kValidStates) {
        if (eq_ci(name, v)) return std::string(v);
    }
    return {};
}

std::string valid_states_list() {
    std::string out;
    for (size_t i = 0; i < kValidStates.size(); ++i) {
        if (i) out += ", ";
        out += std::string(kValidStates[i]);
    }
    return out;
}

}  // namespace

json SetStatusTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", json::object({
            {"name", get_name()},
            {"description", get_description()},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"state", json::object({
                        {"type", "string"},
                        {"description",
                         "The state to transition to. One of: Idle, Understanding, Planning, "
                         "Acting, Verifying, Compressing, Done, Cancelled, Error. "
                         "Case-insensitive; normalised to title-case in the response."}
                    })}
                })},
                {"required", json::array({"state"})}
            })}
        })}
    });
}

json SetStatusTool::execute(const json& args, FileIO& /*file_io*/) {
    try {
        // §Robustness: accept either {"state": "..."} object form, or a
        // JSON-string form that some legacy LLM clients emit.
        std::string raw_state;
        if (args.is_object()) {
            if (!args.contains("state")) {
                return json::object({
                    {"status", "error"},
                    {"message", "Missing 'state' in arguments"},
                });
            }
            if (!args["state"].is_string()) {
                return json::object({
                    {"status", "error"},
                    {"message", "'state' must be a string"},
                });
            }
            raw_state = args["state"].get<std::string>();
        } else if (args.is_string()) {
            try {
                auto parsed = json::parse(args.get<std::string>());
                if (!parsed.contains("state") || !parsed["state"].is_string()) {
                    return json::object({
                        {"status", "error"},
                        {"message", "Missing 'state' in arguments"},
                    });
                }
                raw_state = parsed["state"].get<std::string>();
            } catch (...) {
                raw_state = args.get<std::string>();
            }
        } else {
            return json::object({
                {"status", "error"},
                {"message", "Invalid argument format for set_status"},
            });
        }

        // Trim whitespace.
        auto first = raw_state.find_first_not_of(" \t\r\n");
        auto last  = raw_state.find_last_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return json::object({
                {"status", "error"},
                {"message", "'state' is empty"},
            });
        }
        std::string trimmed = raw_state.substr(first, last - first + 1);

        // Validate (case-insensitive match against the canonical list).
        std::string normalised = canonicalise(trimmed);
        if (normalised.empty()) {
            return json::object({
                {"status", "error"},
                {"message",
                 "Unknown state '" + trimmed + "'. Valid: " + valid_states_list() + "."},
            });
        }

        // The host-side state-change side-effect is triggered by
        // LLMClient::run_agent_cycle() when it sees status == "set_state".
        return json::object({
            {"status", "set_state"},
            {"state", normalised},
        });
    } catch (const std::exception& e) {
        return json::object({
            {"status", "error"},
            {"message", std::string(e.what())},
        });
    }
}