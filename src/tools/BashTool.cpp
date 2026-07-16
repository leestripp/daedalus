#include "tools/BashTool.h"
#include "tools/ToolHandler.h"
#include "tools/BashCommandExecutor.h"
#include "tools/BashPathChecker.h"
#include "core/FileIO.h"
#include "core/utils.h"
#include "core/Subprocess.h"

#include <iostream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Build event callback implementation
BuildEventCallback BashTool::build_callback_ = nullptr;

// Trigger build callback if command looks like a build command
static void trigger_build_callback(const std::string& command, int exit_code) {
    auto callback = BashTool::get_build_callback();
    if (!callback) return;
    
    std::vector<std::string> build_patterns = {
        "cmake", "make", "g++", "gcc", "clang++", "clang",
        "build", "compile", "ninja",
        "npm build", "npm install", "npm test",
        "cargo build", "cargo test", "cargo run",
        "go build", "go test",
        "python -m pytest", "pytest", "ctest"
    };
    
    std::string cmd_lower = command;
    std::transform(cmd_lower.begin(), cmd_lower.end(), cmd_lower.begin(), ::tolower);
    
    for (const auto& pattern : build_patterns) {
        if (cmd_lower.find(pattern) != std::string::npos) {
            callback(command, exit_code == 0);
            return;
        }
    }
}

// ========== BashTool ==========

json BashTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", json::object({
            {"name", get_name()},
            {"description", get_description()},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"command", json::object({
                        {"type", "string"},
                        {"description", "The shell command to execute"}
                    })},
                    {"timeout", json::object({
                        {"type", "integer"},
                        {"description", "Optional timeout in seconds for this command. Default: 60. Range: 1-3600."}
                    })}
                })},
                {"required", json::array({"command"})}
            })}
        })}
    });
}

json BashTool::execute(const json& args, FileIO& file_io) {
    return execute(args, file_io, nullptr);
}

json BashTool::execute(const json& args, FileIO& file_io, ToolProgressFn progress) {
    auto emit_progress = [&progress](const std::string& stage,
                                     int percent,
                                     const std::string& message) {
        if (!progress) return;
        json p = {
            {"stage",   stage},
            {"percent", percent},
            {"message", message},
        };
        progress(p);
    };

    // ===== Parse Arguments =====
    std::string command;
    int timeout_seconds = BashTool::get_default_timeout_seconds();
    
    try {
        if (args.is_object() && args.contains("command")) {
            command = args["command"].get<std::string>();
        } else if (args.is_string()) {
            json parsed_args = json::parse(args.get<std::string>());
            if (parsed_args.contains("command")) {
                command = parsed_args["command"].get<std::string>();
            } else {
                return json::object({{"status", "error"}, {"message", "Missing 'command' in arguments"}});
            }
        } else {
            command = args.get<std::string>();
        }

        if (command.empty()) {
            return json::object({{"status", "error"}, {"message", "Missing or empty 'command' argument"}});
        }
        
        // Parse optional timeout parameter
        if (args.is_object() && args.contains("timeout")) {
            try {
                timeout_seconds = args["timeout"].get<int>();
                if (timeout_seconds < BashTool::get_min_timeout_seconds() || 
                    timeout_seconds > BashTool::get_max_timeout_seconds()) {
                    return json::object({{"status", "error"}, 
                        {"message", "Timeout must be between " + std::to_string(BashTool::get_min_timeout_seconds()) + 
                                    " and " + std::to_string(BashTool::get_max_timeout_seconds()) + " seconds."}});
                }
            } catch (const std::exception&) {
                return json::object({{"status", "error"}, {"message", "Invalid timeout value"}});
            }
        }
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string("Error parsing arguments: ") + e.what()}});
    }

    try {
        std::string project_path = file_io.get_project_path().string();
        
        // ===== Rule 2: Block privilege escalation (sudo, su, pkexec, etc.) =====
        // EXCEPT: Allow if auto_approve is enabled (user has configured this)
        if (BashPathChecker::contains_privilege_escalation(command) && !ToolHandler::is_auto_mode_enabled()) {
            return json::object({
                {"status", "error"},
                {"message", BashPathChecker::privilege_escalation_message(command)}
            });
        }
        
        // ===== Rule 3: Block standalone & (background operator) but allow &&
        // ===== §B3 fix: use BashPathChecker::contains_standalone_ampersand
        // which understands single/double quotes, heredocs, and fd
        // redirections like 2>&1 or &>. The pre-fix inline parser
        // rejected `&` everywhere, including safe quoted contexts.
        if (BashPathChecker::contains_standalone_ampersand(command)) {
            return json::object({
                {"status", "error"},
                {"message", "Security: Background operator (&) — popen() would hang forever waiting on the child. Use `&&` to chain commands instead, or run them sequentially."}
            });
        }

        // ===== Rule 3.5: Ask host for approval (Phase 3) =====
        // If auto-mode is on, this is a no-op. Otherwise, route through
        // the sink so the host can show a y/n prompt (CLI), dialog (TUI),
        // etc. Successful approval adds the command to the session set so
        // chained approvals don't repeat the prompt.
        if (!ToolHandler::request_command_approval(command)) {
            return json::object({
                {"status", "denied"},
                {"message", "Command denied by host."}
            });
        }
        ToolHandler::add_session_approved_command_internal(command);

        // ===== Phase C1: streaming progress =====
        // "starting" fires as soon as the command is parsed + approved —
        // the host can show a spinner / "Running…" indicator before any
        // subprocess I/O has happened. Useful for slow bash commands
        // (cmake builds, package installs, etc.) where the user otherwise
        // sees a frozen UI for tens of seconds.
        emit_progress("starting", 0,
                      "Starting command: " + command.substr(
                          0, std::min<size_t>(80, command.size())));
        emit_progress("executing", 50, "Running command...");

        // ===== Rule 4: Execute single command =====
        return execute_single_command(command, project_path, timeout_seconds);
        
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string(e.what())}});
    }
}

// Execute a single command
json BashTool::execute_single_command(const std::string& command,
                                       const std::string& project_path,
                                       int timeout_seconds) {
    auto result = BashCommandExecutor::execute(command, project_path, timeout_seconds);
    
    trigger_build_callback(command, result.exit_code);
    
    return json::object({
        {"status", result.success ? "success" : "error"},
        {"command", command},
        {"output", result.output},
        {"exit_code", result.exit_code}
    });
}

// ========== Bash_Approvals Delegation (Backward Compatibility for Tests) ==========
// Note: Most delegation functions are now inline in the header.
// Only non-inline implementations go here.

// Safe diagnostic prefixes for syntax checking (allowed without path restrictions)
const std::vector<std::string>& BashTool::get_safe_diagnostic_prefixes() {
    static const std::vector<std::string> prefixes = {
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
    return prefixes;
}

