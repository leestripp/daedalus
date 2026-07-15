#include "tools/ToolHandler.h"
#include "tools/ToolFactory.h"
#include "tools/GitTool.h"
#include "tools/MemoryTool.h"
#include "core/FileIO.h"
#include "core/SessionMemory.h"
#include "core/CommonUtils.h"
#include "core/Logger.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <set>
#include <cctype>


// §Phase 5.1: helper macro that converts LOG_CERR(INFO,  ...); style
// statements into LOG_INFO/LOG_WARNING/LOG_ERROR calls via the
// existing Logger. Same formatted text routed through the Logger
// instead of writing to stderr directly.
#define LOG_CERR(level, expr) do {                          \
        std::ostringstream _daedalus_msg;                   \
        _daedalus_msg << expr;                               \
        LOG_##level(_daedalus_msg.str());                    \
    } while (0)
namespace ToolHandler {
    std::shared_ptr<ToolFactory> g_tool_factory;

    // Session-level cache for approved commands (in-memory only)
    std::set<std::string> session_approved_commands;

    // Persisted approvals (loaded from .daedalus/approvals.json)
    std::set<std::string> persisted_approved_commands;

    // Current working directory tracker
    std::filesystem::path current_working_directory;
    bool cwd_initialized = false;

    // Auto mode - automatically approve tool executions
    bool auto_mode_enabled = false;

    // §Phase 3 — sink set by LLMClient::set_sink(). Tools use this for
    // blocking I/O like execute_bash approval. weak_ptr because the host
    // owns the sink's lifetime.
    std::weak_ptr<daedalus::AgentSink> g_sink;

    // Path to the approvals JSON file
    std::filesystem::path get_approvals_file_path(const std::filesystem::path& project_path) {
        return project_path / ".daedalus" / "approvals.json";
    }

    // Initialize the tool factory
    void init_tool_factory() {
        if (!g_tool_factory) {
            g_tool_factory = std::make_shared<ToolFactory>();
        }
    }

    // Get the tool factory instance
    std::shared_ptr<ToolFactory> get_tool_factory() {
        if (!g_tool_factory) {
            init_tool_factory();
        }
        return g_tool_factory;
    }
    
    // Initialize memory tools with SessionMemory pointer
    void init_memory_tools(SessionMemory* session_memory) {
        RememberTool::set_session_memory(session_memory);
        ForgetTool::set_session_memory(session_memory);
        MemoriesTool::set_session_memory(session_memory);
        MemoryStatsTool::set_session_memory(session_memory);
    }

    // Load persisted approvals from JSON file
    void load_persisted_approvals(const std::filesystem::path& project_path) {
        std::filesystem::path approvals_file = get_approvals_file_path(project_path);
        if (std::filesystem::exists(approvals_file)) {
            try {
                std::ifstream infile(approvals_file);
                json data = json::parse(infile);
                infile.close();
                if (data.contains("approved_commands") && data["approved_commands"].is_array()) {
                    for (const auto& cmd : data["approved_commands"]) {
                        persisted_approved_commands.insert(cmd.get<std::string>());
                    }
                }
            } catch (const std::exception& e) {
                LOG_CERR(WARNING, "[WARNING] Failed to load persisted approvals: " << e.what());
            }
        }
    }

    // Save persisted approvals to JSON file
    void save_persisted_approvals(const std::filesystem::path& project_path) {
        std::filesystem::path approvals_file = get_approvals_file_path(project_path);
        try {
            std::filesystem::path daedalus_dir = approvals_file.parent_path();
            if (!std::filesystem::exists(daedalus_dir)) {
                std::filesystem::create_directories(daedalus_dir);
            }
            json data;
            data["approved_commands"] = json::array();
            for (const auto& cmd : persisted_approved_commands) {
                data["approved_commands"].push_back(cmd);
            }
            std::ofstream outfile(approvals_file);
            outfile << data.dump(2);
            outfile.close();
        } catch (const std::exception& e) {
            LOG_CERR(ERROR, "[APPROVALS] Failed to save approvals: " << e.what());
        }
    }

    // Clear the session-approved commands cache
    void clear_session_approved_commands() {
        session_approved_commands.clear();
    }

    // Add a command to session approved commands (internal use by BashTool)
    void add_session_approved_command_internal(const std::string& command) {
        session_approved_commands.insert(command);
    }

    // Initialize current working directory with project root
    void init_current_working_directory(const std::filesystem::path& project_root) {
        current_working_directory = project_root;
        cwd_initialized = true;
    }

    // Get current working directory (must be initialized first)
    std::filesystem::path get_current_working_directory() {
        return current_working_directory;
    }

    // Check if cwd has been initialized
    bool is_cwd_initialized() {
        return cwd_initialized;
    }

    // Set auto mode (automatically approve tool executions)
    void set_auto_mode(bool enabled) {
        auto_mode_enabled = enabled;
    }

    // Check if auto mode is enabled
    bool is_auto_mode_enabled() {
        return auto_mode_enabled;
    }

    // §Phase 3 — sink storage. Tools (e.g. execute_bash approval) use this
    // to call back into the host. weak_ptr because the sink lifetime is
    // owned by the host (LLMClient keeps a shared_ptr internally).

    void set_sink(std::shared_ptr<daedalus::AgentSink> sink) {
        g_sink = sink;
    }

    bool request_command_approval(const std::string& command) {
        // Auto mode always approves.
        if (auto_mode_enabled) return true;

        // If the command is already in the session or persisted approved
        // set, approve without prompting. (Backward-compatible behaviour.)
        if (is_command_session_approved(command)) return true;

        // Otherwise, ask the host via the sink.
        if (auto s = g_sink.lock()) {
            try {
                return s->request_command_approval(command);
            } catch (...) {
                // Host threw — abort the approval.
                return false;
            }
        }

        // No sink attached and not auto-mode → deny (preserves the legacy
        // "banned by default" semantics that were the default before Phase 3).
        return false;
    }

    // Clear persisted approvals file
    void clear_persisted_approvals(const std::filesystem::path& project_path) {
        persisted_approved_commands.clear();
        std::filesystem::path approvals_file = get_approvals_file_path(project_path);
        if (std::filesystem::exists(approvals_file)) {
            std::filesystem::remove(approvals_file);
        }
        std::filesystem::path daedalus_dir = approvals_file.parent_path();
        if (std::filesystem::exists(daedalus_dir) && std::filesystem::is_empty(daedalus_dir)) {
            std::filesystem::remove(daedalus_dir);
        }
    }

    // Add a command to persisted approvals and save to file
    void add_persisted_approval(const std::filesystem::path& project_path, const std::string& command) {
        // Add to persisted set
        persisted_approved_commands.insert(command);
        // Save to file
        save_persisted_approvals(project_path);
    }

    // Check if a command matches a pattern (supports multiple wildcards for files)
    bool matches_pattern(const std::string& command, const std::string& pattern) {
        // Exact match
        if (command == pattern) {
            return true;
        }

        // Check if pattern has wildcards
        if (pattern.find('*') != std::string::npos) {
            // Split pattern by '*' to get multiple parts
            std::vector<std::string> parts;
            std::string current;
            for (char c : pattern) {
                if (c == '*') {
                    if (!current.empty()) {
                        parts.push_back(current);
                    }
                    current.clear();
                } else {
                    current += c;
                }
            }
            if (!current.empty()) {
                parts.push_back(current);
            }

            // Handle patterns ending with * (like "rm -rf *", "ls *")
            if (parts.size() == 1 && pattern.back() == '*') {
                // Pattern is like "cmd *" - check if command starts with the prefix
                std::string prefix = parts[0];
                if (command.size() >= prefix.size() && 
                    command.compare(0, prefix.size(), prefix) == 0) {
                    return true;
                }
                return false;
            }

            // Need at least 2 parts (before and after wildcard) to check
            if (parts.size() >= 2) {
                // Check if command starts with first part
                if (command.compare(0, parts[0].size(), parts[0]) != 0) {
                    return false;
                }

                // Only check if command ends with last part if the pattern does NOT end with '*'
                // If pattern ends with '*', the last part is just a literal string in the middle
                // that must appear somewhere between wildcards, not at the end
                bool pattern_ends_with_wildcard = (pattern.back() == '*');
                if (!pattern_ends_with_wildcard) {
                    if (command.size() < parts.back().size()) {
                        return false;
                    }
                    if (command.compare(command.size() - parts.back().size(), parts.back().size(), parts.back()) != 0) {
                        return false;
                    }
                }

                // Check if all middle parts appear in order
                size_t search_start = parts[0].size();
                // When pattern ends with wildcard, check all parts after the first as middle parts
                // (the ending is handled by the trailing wildcard, not as a literal suffix)
                size_t end_idx = pattern_ends_with_wildcard ? parts.size() : parts.size() - 1;
                for (size_t i = 1; i < end_idx; i++) {
                    size_t pos = command.find(parts[i], search_start);
                    if (pos == std::string::npos) {
                        return false;
                    }
                    search_start = pos + parts[i].size();
                }

                return true;
            }
        }

        // Also check prefix match (original behavior)
        if (command.compare(0, pattern.size(), pattern) == 0) {
            return true;
        }

        return false;
    }

    // List of safe read-only diagnostic tools
    const std::vector<std::string> safe_diagnostic_prefixes = {
        "node --check", "node -c",
        "python -m py_compile", "python -m pyflakes", "python -m mypy",
        "python -m pylint", "python -m flake8", "python -m ruff",
        "python -m black --check", "python -m isort --check-only",
        "python -m pytest --collect-only", "python -m pytest --dry-run",
        "rustc --check", "cargo check", "cargo clippy",
        "cargo test --no-run", "cargo build --dry-run",
        "g++ -fsyntax-only", "gcc -fsyntax-only",
        "clang++ -fsyntax-only", "clang -fsyntax-only",
        "cmake -N", "cmake --build . --target help",
        "go build -n", "go vet", "go test -c", "go mod verify",
        "npx tsc --noEmit", "npx eslint --no-eslintrc --quiet",
        "ruby -c", "perl -c", "shellcheck",
    };

    // Check if a command contains command chaining operators that require splitting

    // Check if a command matches any approved session or persisted command (supports wildcards)
    bool is_command_session_approved(const std::string& command) {
        // Note: Chained commands are handled specially in BashTool::execute
        // This function checks individual commands only

        for (const auto& approved : session_approved_commands) {
            if (matches_pattern(command, approved)) {
                return true;
            }
        }
        for (const auto& approved : persisted_approved_commands) {
            if (matches_pattern(command, approved)) {
                return true;
            }
        }
        return false;
    }

    // Escape shell argument

    // Execute a tool call - delegates to appropriate tool
    json execute_tool_call(const json& tool_call, FileIO& file_io) {
        return execute_tool_call(tool_call, file_io, nullptr);
    }

    json execute_tool_call(const json& tool_call,
                           FileIO& file_io,
                           ToolProgressFn progress) {
        // Initialize tool factory if not done
        init_tool_factory();
        
        std::string tool_name;
        json args;
        
        try {
            if (tool_call.is_object()) {
                // Support both formats:
                // 1. {"function": {"name": "tool_name", "arguments": "{\"param\": \"value\"}"}} (from LLM - arguments is STRING!)
                // 2. {"function": {"name": "tool_name", "arguments": {"param": "value"}}} (parsed format)
                // 3. {"name": "tool_name", "arguments": {...}} (legacy format)
                if (tool_call.contains("function") && tool_call["function"].is_object()) {
                    tool_name = tool_call["function"].value("name", "");
                    
                    // Get arguments - could be a string or object
                    auto& func_obj = tool_call["function"];
                    if (func_obj.contains("arguments")) {
                        auto& arg_val = func_obj["arguments"];
                        if (arg_val.is_string()) {
                            // Arguments is a JSON string - parse it!
                            std::string args_str = arg_val.get<std::string>();
                            try {
                                args = json::parse(args_str);
                            } catch (const json::parse_error& e) {
                                std::string err_msg = "[ERROR] Failed to parse arguments JSON string: " + std::string(e.what());
                                LOG_CERR(INFO, err_msg);
                                LOG_ERROR("ToolHandler: " + err_msg);
                                
                                // Try to recover truncated JSON by completing unclosed braces
                                std::string recovered = daedalus::try_recover_json_string(args_str);
                                bool recovery_succeeded = false;
                                if (recovered != args_str) {
                                    // Recovery attempted, try parsing again
                                    try {
                                        args = json::parse(recovered);
                                        LOG_CERR(INFO, "[INFO] Successfully recovered JSON arguments");
                                        recovery_succeeded = true;
                                    } catch (...) {
                                        // Recovery didn't work, continue to error handling
                                        LOG_CERR(INFO, "[INFO] JSON recovery failed, using escaped raw args");
                                    }
                                }
                                
                                // If recovery succeeded, skip the error return
                                if (recovery_succeeded) {
                                    // args is now set to the recovered JSON, continue with tool execution
                                } else {
                                    // Escape problematic characters in the raw arguments to prevent
                                    // JSON serialization issues. The original string may contain
                                    // unescaped quotes or special characters that break JSON parsing.
                                    std::string escaped_args = args_str;
                                    for (size_t i = 0; i < escaped_args.size(); i++) {
                                        char c = escaped_args[i];
                                        if (c == '"' || c == '\\' || c < 0x20) {
                                            escaped_args[i] = '?';
                                        }
                                    }
                                    
                                    return json::object({
                                        {"status", "error"},
                                        {"message", "Invalid arguments JSON - check raw_arguments for details"},
                                        {"error_type", "parse_error"},
                                        {"error_details", std::string(e.what())},
                                        {"raw_arguments", escaped_args}
                                    });
                                }
                            }
                        } else {
                            args = arg_val;
                        }
                    } else {
                        args = json::object();
                    }
                } else {
                    tool_name = tool_call.value("name", "");
                    args = tool_call.value("arguments", json::object());
                }
            } else {
                LOG_ERROR("Tool call is not an object: " + tool_call.dump());
                return json::object({{"status", "error"}, {"message", "Invalid tool call format"}});
            }
            
            if (tool_name.empty()) {
                LOG_ERROR("Empty tool name in tool call");
                return json::object({{"status", "error"}, {"message", "Missing tool name"}});
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Exception parsing tool call: " + std::string(e.what()));
            return json::object({{"status", "error"}, {"message", std::string("Error parsing tool call: ") + e.what()}});
        }
        
        // Delegate to tool factory for all tools (including execute_bash)
        return g_tool_factory->execute_tool(tool_name, args, file_io, progress);
    }

    // Get tool definitions as JSON string
    std::string get_tool_definitions() {
        // Initialize tool factory if not done
        init_tool_factory();
        
        json definitions = g_tool_factory->get_all_definitions();
        return definitions.dump();
    }
    
    // Get tool definitions excluding specified tools (for sub-agent mode)
    std::string get_tool_definitions_filtered(const std::vector<std::string>& exclude_tools) {
        // Initialize tool factory if not done
        init_tool_factory();
        
        json definitions = g_tool_factory->get_all_definitions();
        
        // Filter out excluded tools
        json filtered = json::array();
        for (const auto& def : definitions) {
            if (def.contains("function") && def["function"].contains("name")) {
                std::string tool_name = def["function"]["name"];
                bool excluded = false;
                for (const auto& excluded_name : exclude_tools) {
                    if (tool_name == excluded_name) {
                        excluded = true;
                        break;
                    }
                }
                if (!excluded) {
                    filtered.push_back(def);
                }
            }
        }
        return filtered.dump();
    }
}
