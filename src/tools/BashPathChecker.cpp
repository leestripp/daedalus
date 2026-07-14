#include "tools/BashPathChecker.h"
#include <algorithm>
#include <cstring>
#include <sstream>

// ========== Privilege Escalation Detection ==========

bool BashPathChecker::contains_privilege_escalation(const std::string& command) {
    // Check for privilege escalation patterns
    const char* patterns[] = {
        "sudo ", "sudo -", "sudo\n", "sudo\t",
        "su -", "su\n", "su\t",
        "pkexec",
        "doas",
        "dzdo",
        "run0"
    };

    std::string cmd_lower = command;
    std::transform(cmd_lower.begin(), cmd_lower.end(), cmd_lower.begin(), ::tolower);

    for (const auto& pattern : patterns) {
        if (cmd_lower.find(pattern) != std::string::npos) {
            return true;
        }
    }

    return false;
}

std::string BashPathChecker::privilege_escalation_message([[maybe_unused]] const std::string& command) {
    return "Security: Privilege escalation commands (sudo, su, pkexec, doas, etc.) are not allowed. "
           "The Agent runs as a normal user and cannot use sudo.";
}

// ========== Dangerous Command Detection ==========

bool BashPathChecker::is_dangerous_command(const std::string& command) {
    size_t start = command.find_first_not_of(" \t\n");
    if (start == std::string::npos) {
        return false;
    }

    std::string trimmed = command.substr(start);

    std::istringstream iss(trimmed);
    std::string cmd;
    iss >> cmd;

    const std::vector<std::string> dangerous_commands = {
        "rm", "del", "rmdir", "shred", "mkfs", "dd", "fdformat",
        "mkboot", "parted", "fdisk", "sfdisk", "cfdisk"
    };

    for (const auto& dangerous : dangerous_commands) {
        if (cmd == dangerous) {
            return true;
        }
    }

    if (cmd == "rm" || cmd == "del") {
        std::string flags;
        while (iss >> flags) {
            if (flags.find('-') == 0) {
                std::string flag = flags.substr(1);
                if (flag.find('r') != std::string::npos ||
                    flag.find('f') != std::string::npos) {
                    return true;
                }
            }
        }
    }

    return false;
}

std::string BashPathChecker::extract_path_from_command(const std::string& command) {
    std::istringstream iss(command);
    std::string cmd;
    iss >> cmd;

    std::string arg;
    while (iss >> arg) {
        if (arg.empty()) continue;
        if (arg[0] == '-') continue;
        return arg;
    }
    return "";
}

std::vector<std::string> BashPathChecker::extract_paths(const std::string& command) {
    std::vector<std::string> paths;
    std::istringstream iss(command);
    std::string cmd;
    iss >> cmd;  // skip command name

    std::string arg;
    while (iss >> arg) {
        if (arg.empty()) continue;
        if (arg[0] == '-') continue;  // skip flags
        paths.push_back(arg);
    }
    return paths;
}

std::string BashPathChecker::extract_first_path(const std::string& command) {
    std::vector<std::string> paths = extract_paths(command);
    if (paths.empty()) {
        return "";
    }
    return paths[0];
}