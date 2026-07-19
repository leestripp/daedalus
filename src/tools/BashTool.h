#pragma once

#include <string>
#include <set>
#include <vector>
#include <functional>
#include <nlohmann/json.hpp>
#include "BaseTool.h"
#include "BashChainer.h"
#include "BashPathChecker.h"

class FileIO;

using json = nlohmann::json;

/**
 * BashTool.h - Contains shell command execution tool
 */

// Build event callback type
using BuildEventCallback = std::function<void(const std::string& command, bool success)>;

// ========== BashTool ==========
class BashTool : public BaseTool {
public:
    [[nodiscard]] std::string get_name() const override { return "execute_bash"; }
    [[nodiscard]] std::string get_description() const override { return "Execute shell commands in the project root (build, test, git, package installs, etc.)."; }
    [[nodiscard]] json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
    json execute(const json& args, FileIO& file_io, ToolProgressFn progress) override;
    
    // Set callback for build events (called from MiniMaxClient)
    static void set_build_callback(BuildEventCallback callback) { build_callback_ = callback; }
    
    // Get callback for use in helper functions
    static BuildEventCallback get_build_callback() { return build_callback_; }

    // ====== Public Helper Functions for Testing ======
    // These delegate to BashChainer or BashPathChecker static methods
    static bool is_dangerous_command(const std::string& command) { 
        return BashPathChecker::is_dangerous_command(command); 
    }
    static bool contains_privilege_escalation(const std::string& command) { 
        return BashPathChecker::contains_privilege_escalation(command); 
    }
    static std::string extract_path_from_command(const std::string& command) { 
        return BashPathChecker::extract_path_from_command(command); 
    }
    static bool contains_command_chaining(const std::string& command) { 
        return BashChainer::contains_command_chaining(command); 
    }
    static std::vector<std::pair<std::string, std::string>> parse_chained_commands(const std::string& command) { 
        return BashChainer::parse_chained_commands(command); 
    }
    static bool contains_background_operator(const std::string& command) { 
        return BashChainer::contains_background_operator(command); 
    }
    
    // Convert command to approval pattern (for session approval)
    static std::string command_to_pattern(const std::string& command);

    // Get default timeout in seconds (60 seconds)
    static constexpr int get_default_timeout_seconds() { return 60; }

    // Get min/max allowed timeout values
    static constexpr int get_min_timeout_seconds() { return 1; }
    static constexpr int get_max_timeout_seconds() { return 3600; }  // 1 hour max

private:
    // Private helper functions
    static std::string escape_shell_arg(const std::string& input);
    static std::string extract_command_prefix(const std::string& command);
    static json handle_chained_commands(const std::string& command, const std::string& project_path, int timeout_seconds);
    static json execute_single_command(const std::string& command, const std::string& project_path, int timeout_seconds);
    static void add_session_approved_command(const std::string& command);

    // Safe diagnostic prefixes that can be auto-approved
    static const std::vector<std::string>& get_safe_diagnostic_prefixes();
    
    // Static callback for build events
    static BuildEventCallback build_callback_;
};