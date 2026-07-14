#pragma once

#include <string>
#include <vector>

/**
 * BashPathChecker - Simplified path validation for bash commands
 *
 * NEW MODEL: Trust Linux permissions
 * - Path checking is simplified since OS handles permissions
 * - Only check for privilege escalation (sudo, su, pkexec)
 * - Dangerous command detection kept for future use
 */

class BashPathChecker {
public:
    // ========== Privilege Escalation Detection ==========

    /**
     * Check if a command contains privilege escalation attempts
     * Commands like sudo, su -, pkexec, doas, dzdo, run0 are blocked
     *
     * @param command The shell command to check
     * @return true if command contains privilege escalation, false otherwise
     */
    static bool contains_privilege_escalation(const std::string& command);

    /**
     * Get error message for blocked privilege escalation
     *
     * @param command The command that was blocked
     * @return Error message explaining why command was blocked
     */
    static std::string privilege_escalation_message(const std::string& command);

    // ========== Dangerous Command Detection ==========

    /**
     * Check if a command is considered dangerous
     * Dangerous commands like rm -rf, del, etc.
     * Note: With trust OS permissions model, these are allowed but logged
     */
    static bool is_dangerous_command(const std::string& command);

    /**
     * Extract path argument from command (e.g., "rm -rf build" -> "build")
     * Returns empty string if no path found
     */
    static std::string extract_path_from_command(const std::string& command);

    /**
     * Extract all paths from a command (arguments that don't start with -)
     *
     * @param command The shell command to parse
     * @return Vector of path arguments
     */
    static std::vector<std::string> extract_paths(const std::string& command);

    /**
     * Extract the first path from a command
     *
     * @param command The shell command to parse
     * @return First path argument, or empty string if none
     */
    static std::string extract_first_path(const std::string& command);
};