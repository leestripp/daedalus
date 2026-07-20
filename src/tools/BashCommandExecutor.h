#pragma once

#include <string>

class FileIO;

/**
 * BashCommandExecutor - Pure command execution without UI or approval logic
 * 
 * Handles:
 * - Command execution with timeout via Subprocess
 * - Shell argument escaping
 * - Output sanitization (binary detection, truncation)
 * - Spinner animation for user feedback
 * - pkill command sanitization (-f flag removal)
 */

class BashCommandExecutor {
public:
    /**
     * Result of command execution
     */
    struct Result {
        bool success;
        int exit_code;
        std::string output;
        std::string error;  // Non-empty if execution failed
        
        Result() : success(false), exit_code(-1) {}
        Result(bool ok, int code, const std::string& out, const std::string& err = "")
            : success(ok), exit_code(code), output(out), error(err) {}
    };
    
    /**
     * Execute a shell command with timeout
     * 
     * @param command The shell command to execute
     * @param base_dir Working directory for the command
     * @param timeout_seconds Maximum time to wait (default 60)
     * @return Result with success/failure, exit code, and output
     */
    static Result execute(const std::string& command, 
                          const std::string& base_dir,
                          int timeout_seconds = 60);
    
    // Utility functions
    static std::string escape_shell_arg(const std::string& input);
    static std::string sanitize_output(const std::string& output);
    static std::string sanitize_pkill_command(const std::string& command);
    
private:
    // Internal execution with spinner
    static Result execute_internal(const std::string& full_command,
                                   const std::string& display_command,
                                   int timeout_seconds);
    
    // Build spinner message
    static std::string build_spinner_context(const std::string& command);
};