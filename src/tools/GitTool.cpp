#include "tools/GitTool.h"
#include "core/FileIO.h"

#include <iostream>
#include <string>
#include <stdexcept>
#include <cstring>

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
// ========== Helper Functions (from ToolHandler.cpp) ==========

namespace ToolHandler {
    bool is_cwd_initialized();
    std::filesystem::path get_current_working_directory();
}

// §Fix: single-quote-escape a shell argument (same pattern as
// escape_shell_arg in SearchTool.cpp / BashCommandExecutor.cpp).
// Applied to project_path so paths with spaces or metacharacters
// cannot break the `cd` or inject commands.
static std::string escape_shell_arg(const std::string& input) {
    std::string result;
    result.reserve(input.size() + 16);
    result.push_back('\'');
    for (char c : input) {
        if (c == '\'') {
            result.push_back('\'');
            result.push_back('\\');
            result.push_back('\'');
            result.push_back('\'');
        } else {
            result.push_back(c);
        }
    }
    result.push_back('\'');
    return result;
}

// ========== GitStatusTool ==========

json GitStatusTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", "Show the working tree status. Displays which files have been modified, staged, or are untracked."},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({})}
            })}
        }}
    });
}

json GitStatusTool::execute(const json& args, FileIO& file_io) {
    (void)args;
    try {
        std::string project_path = file_io.get_project_path().string();
        std::string command = "cd " + escape_shell_arg(project_path) + " && git status";

        FILE* pipe = popen(command.c_str(), "r");
        if (!pipe) {
            return json::object({{"status", "error"}, {"message", "Failed to execute git status"}});
        }

        std::string result;
        char buffer[128];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            result += buffer;
        }

        int exit_code = pclose(pipe);

        return json::object({
            {"status", "success"},
            {"command", "git status"},
            {"output", result},
            {"exit_code", WEXITSTATUS(exit_code)}
        });
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string(e.what())}});
    }
}

// ========== GitBranchTool ==========

json GitBranchTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", "List all branches in the repository. Shows both local and remote branches."},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({})}
            })}
        }}
    });
}

json GitBranchTool::execute(const json& args, FileIO& file_io) {
    (void)args;
    try {
        std::string project_path = file_io.get_project_path().string();
        std::string command = "cd " + escape_shell_arg(project_path) + " && git branch -a";

        FILE* pipe = popen(command.c_str(), "r");
        if (!pipe) {
            return json::object({{"status", "error"}, {"message", "Failed to execute git branch"}});
        }

        std::string result;
        char buffer[128];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            result += buffer;
        }

        int exit_code = pclose(pipe);

        return json::object({
            {"status", "success"},
            {"command", "git branch -a"},
            {"output", result},
            {"exit_code", WEXITSTATUS(exit_code)}
        });
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string(e.what())}});
    }
}

// ========== GitCommitTool ==========

json GitCommitTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", "Commit staged changes to the repository. Automatically stages all modified files before committing."},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"message", json::object({
                        {"type", "string"},
                        {"description", "The commit message describing the changes"}
                    })}
                })},
                {"required", json::array({"message"})}
            })}
        }}
    });
}

json GitCommitTool::execute(const json& args, FileIO& file_io) {
    std::string message;

    try {
        if (args.is_object() && args.contains("message")) {
            message = args["message"].get<std::string>();
        } else if (args.is_string()) {
            json parsed_args = json::parse(args.get<std::string>());
            if (parsed_args.contains("message")) {
                message = parsed_args["message"].get<std::string>();
            } else {
                return json::object({{"status", "error"}, {"message", "Missing 'message' in arguments"}});
            }
        } else {
            return json::object({{"status", "error"}, {"message", "Invalid arguments format for git_commit"}});
        }

        if (message.empty()) {
            return json::object({{"status", "error"}, {"message", "Missing or empty 'message' argument"}});
        }
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string("Error parsing arguments: ") + e.what()}});
    }

    try {
        std::string project_path = file_io.get_project_path().string();

        // Check if there are any staged changes
        std::string check_command = "cd " + escape_shell_arg(project_path) + " && git status --porcelain";
        FILE* check_pipe = popen(check_command.c_str(), "r");
        if (!check_pipe) {
            return json::object({{"status", "error"}, {"message", "Failed to check git status"}});
        }

        std::string status_output;
        char buffer[128];
        while (fgets(buffer, sizeof(buffer), check_pipe) != nullptr) {
            status_output += buffer;
        }
        pclose(check_pipe);

        if (status_output.empty()) {
            return json::object({
                {"status", "error"},
                {"message", "No changes to commit. Stage files first with 'git add' using execute_bash."}
            });
        }

        // Escape single quotes in message
        std::string escaped_message = message;
        size_t pos = 0;
        while ((pos = escaped_message.find('\'', pos)) != std::string::npos) {
            escaped_message.replace(pos, 1, "'\\''");
            pos += 4;
        }

        // Stage all changes and commit
        std::string add_command = "cd " + escape_shell_arg(project_path) + " && git add -A";
        FILE* add_pipe = popen(add_command.c_str(), "r");
        if (add_pipe) {
            pclose(add_pipe);
        }

        std::string commit_command = "cd " + escape_shell_arg(project_path) + " && git commit -m '" + escaped_message + "'";
        FILE* commit_pipe = popen(commit_command.c_str(), "r");
        if (!commit_pipe) {
            return json::object({{"status", "error"}, {"message", "Failed to execute git commit"}});
        }

        std::string result;
        while (fgets(buffer, sizeof(buffer), commit_pipe) != nullptr) {
            result += buffer;
        }

        int exit_code = pclose(commit_pipe);

        // §Fix: a failed commit (hook rejection, no identity, ...)
        // must surface as an error, not success.
        if (WEXITSTATUS(exit_code) != 0) {
            return json::object({
                {"status", "error"},
                {"message", "Failed to commit. Error: " + result},
                {"exit_code", WEXITSTATUS(exit_code)}
            });
        }

        return json::object({
            {"status", "success"},
            {"command", "git commit -m '" + message + "'"},
            {"output", result},
            {"exit_code", WEXITSTATUS(exit_code)}
        });
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string(e.what())}});
    }
}

// ========== GitAddTool ==========

json GitAddTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", "Stage files for commit. Add files to the Git staging area. Use '-A' or 'all' to stage all files, or specify a specific file or directory."},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"files", json::object({
                        {"type", "string"},
                        {"description", "The files to stage. Use '-A', 'all', or '.' to stage all files, or specify a specific file/directory path"}
                    })}
                })},
                {"required", json::array({"files"})}
            })}
        }}
    });
}

json GitAddTool::execute(const json& args, FileIO& file_io) {
    std::string files;

    try {
        if (args.is_object() && args.contains("files")) {
            files = args["files"].get<std::string>();
        } else if (args.is_string()) {
            json parsed_args = json::parse(args.get<std::string>());
            if (parsed_args.contains("files")) {
                files = parsed_args["files"].get<std::string>();
            } else {
                return json::object({{"status", "error"}, {"message", "Missing 'files' in arguments"}});
            }
        } else {
            return json::object({{"status", "error"}, {"message", "Invalid arguments format for git_add"}});
        }
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string("Error parsing arguments: ") + e.what()}});
    }

    try {
        std::string project_path = file_io.get_project_path().string();

        // Escape single quotes
        std::string escaped_files = files;
        size_t pos = 0;
        while ((pos = escaped_files.find('\'', pos)) != std::string::npos) {
            escaped_files.replace(pos, 1, "'\\''");
            pos += 4;
        }

        std::string add_command;
        if (files == "-A" || files == "all" || files == ".") {
            add_command = "cd " + escape_shell_arg(project_path) + " && git add -A";
        } else {
            // §Fix: single-quote the argument — inside double quotes
            // $(...), backticks and " stay live (injection), and the
            // '\'' escape is garbage in a double-quoted context.
            add_command = "cd " + escape_shell_arg(project_path) + " && git add '" + escaped_files + "'";
        }

        FILE* pipe = popen(add_command.c_str(), "r");
        if (!pipe) {
            return json::object({{"status", "error"}, {"message", "Failed to execute git add"}});
        }

        std::string result;
        char buffer[128];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            result += buffer;
        }

        int exit_code = pclose(pipe);

        if (result.empty() && WEXITSTATUS(exit_code) == 0) {
            result = "Files staged successfully: " + files;
        }

        return json::object({
            {"status", "success"},
            {"command", "git add " + files},
            {"output", result},
            {"exit_code", WEXITSTATUS(exit_code)}
        });
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string(e.what())}});
    }
}

// ========== GitSwitchTool ==========

json GitSwitchTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", "Switch to a different branch. Use to switch to feature branches, main, or other working branches. Can also create and switch to a new branch."},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"branch", json::object({
                        {"type", "string"},
                        {"description", "The name of the branch to switch to"}
                    })},
                    {"create", json::object({
                        {"type", "boolean"},
                        {"description", "Create a new branch before switching. Uses 'git switch -c'."}
                    })}
                })},
                {"required", json::array({"branch"})}
            })}
        }}
    });
}

json GitSwitchTool::execute(const json& args, FileIO& file_io) {
    std::string branch;
    bool create = false;

    try {
        if (args.is_object() && args.contains("branch")) {
            branch = args["branch"].get<std::string>();
        } else if (args.is_string()) {
            json parsed_args = json::parse(args.get<std::string>());
            if (parsed_args.contains("branch")) {
                branch = parsed_args["branch"].get<std::string>();
            } else {
                return json::object({{"status", "error"}, {"message", "Missing 'branch' in arguments"}});
            }
        } else {
            return json::object({{"status", "error"}, {"message", "Invalid arguments format for git_switch"}});
        }

        // Parse create parameter
        if (args.is_object() && args.contains("create")) {
            create = args["create"].get<bool>();
        } else if (args.is_string()) {
            json parsed_args = json::parse(args.get<std::string>());
            if (parsed_args.contains("create")) {
                create = parsed_args["create"].get<bool>();
            }
        }

        if (branch.empty()) {
            return json::object({{"status", "error"}, {"message", "Missing or empty 'branch' argument"}});
        }
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string("Error parsing arguments: ") + e.what()}});
    }

    try {
        std::string project_path = file_io.get_project_path().string();

        // First check the current branch
        std::string branch_command = "cd " + escape_shell_arg(project_path) + " && git branch --show-current";
        FILE* branch_pipe = popen(branch_command.c_str(), "r");
        if (!branch_pipe) {
            return json::object({{"status", "error"}, {"message", "Failed to get current branch"}});
        }

        std::string current_branch;
        char buffer[128];
        while (fgets(buffer, sizeof(buffer), branch_pipe != nullptr ? branch_pipe : nullptr) != nullptr) {
            current_branch += buffer;
        }
        if (branch_pipe) pclose(branch_pipe);

        // Trim whitespace
        current_branch.erase(current_branch.find_last_not_of(" \n\r\t") + 1);

        // We are now giving the Agent more freedom with git_switch
        // Allow switching to any branch from any branch

        // Escape single quotes in branch name
        std::string escaped_branch = branch;
        size_t pos = 0;
        while ((pos = escaped_branch.find('\'', pos)) != std::string::npos) {
            escaped_branch.replace(pos, 1, "'\\''");
            pos += 4;
        }

        // Execute git switch
        std::string switch_command = "cd " + escape_shell_arg(project_path) + " && git switch";
        if (create) {
            switch_command += " -c";  // Create and switch
        }
        switch_command += " '" + escaped_branch + "'";
        FILE* pipe = popen(switch_command.c_str(), "r");
        if (!pipe) {
            return json::object({{"status", "error"}, {"message", "Failed to execute git switch"}});
        }

        std::string result;
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            result += buffer;
        }

        int exit_code = pclose(pipe);

        if (WEXITSTATUS(exit_code) != 0) {
            return json::object({
                {"status", "error"},
                {"message", "Failed to switch branch. Error: " + result}
            });
        }

        return json::object({
            {"status", "success"},
            {"command", "git switch " + branch},
            {"output", result},
            {"exit_code", WEXITSTATUS(exit_code)}
        });
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string(e.what())}});
    }
}

// ========== GitDiffTool ==========

json GitDiffTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", json::object({
            {"name", get_name()},
            {"description", "Show changes between the working tree and the staging area. Displays differences in files."},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"file", json::object({
                        {"type", "string"},
                        {"description", "Optional: The specific file to show diff for. If not provided, shows all changes."}
                    })}
                })}
            })}
        })}
    });
}

json GitDiffTool::execute(const json& args, FileIO& file_io) {
    std::string file;

    try {
        if (args.is_object() && args.contains("file")) {
            file = args["file"].get<std::string>();
        } else if (args.is_string()) {
            json parsed_args = json::parse(args.get<std::string>());
            if (parsed_args.contains("file")) {
                file = parsed_args["file"].get<std::string>();
            }
        }
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string("Error parsing arguments: ") + e.what()}});
    }

    try {
        std::string project_path = file_io.get_project_path().string();

        std::string diff_command;
        if (file.empty()) {
            diff_command = "cd " + escape_shell_arg(project_path) + " && git diff";
        } else {
            // Escape single quotes in file path
            std::string escaped_file = file;
            size_t pos = 0;
            while ((pos = escaped_file.find('\'', pos)) != std::string::npos) {
                escaped_file.replace(pos, 1, "'\\''");
                pos += 4;
            }
            // §Fix: single-quote the argument (see GitAddTool).
            diff_command = "cd " + escape_shell_arg(project_path) + " && git diff '" + escaped_file + "'";
        }

        FILE* pipe = popen(diff_command.c_str(), "r");
        if (!pipe) {
            return json::object({{"status", "error"}, {"message", "Failed to execute git diff"}});
        }

        std::string result;
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            result += buffer;
        }

        int exit_code = pclose(pipe);

        // If no changes, result will be empty
        if (result.empty()) {
            if (file.empty()) {
                result = "No changes in working tree";
            } else {
                result = "No changes in file: " + file;
            }
        }

        return json::object({
            {"status", "success"},
            {"command", file.empty() ? "git diff" : "git diff " + file},
            {"output", result},
            {"exit_code", WEXITSTATUS(exit_code)}
        });
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string(e.what())}});
    }
}

// ========== GitPushTool ==========

json GitPushTool::get_definition() const {
    json definition = json::object();
    definition["type"] = "function";
    definition["function"]["name"] = "git_push";
    definition["function"]["description"] = "Push commits to remote repository. Use to push the current branch to its remote tracking branch. Supports setting upstream with -u flag.";
    definition["function"]["parameters"]["type"] = "object";
    definition["function"]["parameters"]["properties"]["force"]["type"] = "boolean";
    definition["function"]["parameters"]["properties"]["force"]["description"] = "Use --force flag. WARNING: This can overwrite remote history. Requires user approval.";
    definition["function"]["parameters"]["properties"]["set_upstream"]["type"] = "boolean";
    definition["function"]["parameters"]["properties"]["set_upstream"]["description"] = "Use -u flag to set upstream tracking. Use this when pushing a new branch for the first time.";
    return definition;
}

json GitPushTool::execute(const json& args, FileIO& file_io) {
    bool force = false;
    bool set_upstream = false;

    try {
        // Parse arguments
        if (args.is_object()) {
            if (args.contains("force")) {
                force = args["force"].get<bool>();
            }
            if (args.contains("set_upstream")) {
                set_upstream = args["set_upstream"].get<bool>();
            }
        } else if (args.is_string()) {
            json parsed_args = json::parse(args.get<std::string>());
            if (parsed_args.contains("force")) {
                force = parsed_args["force"].get<bool>();
            }
            if (parsed_args.contains("set_upstream")) {
                set_upstream = parsed_args["set_upstream"].get<bool>();
            }
        }
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string("Error parsing arguments: ") + e.what()}});
    }

    // Force push requires user approval - check via environment
    if (force) {
        const char* approved = std::getenv("GIT_FORCE_APPROVED");
        if (!approved || std::string(approved) != "1") {
            // §Phase 5.1: the library never reaches for std::cout.
            // Log the warning so the host can see what happened;
            // the host is expected to gate this tool via its own
            // approval flow (sink + request_command_approval).
            LOG_WARNING("DANGEROUS COMMAND: git push --force — "
                        "this will overwrite remote history with --force flag. "
                        "This is potentially destructive! "
                        "Set GIT_FORCE_APPROVED=1 to bypass, or use "
                        "agent.request_command_approval() to gate this tool.");

            return json::object({
                {"status", "approval_required"},
                {"message", "git push --force requires user approval. Set GIT_FORCE_APPROVED=1 environment variable to bypass (use with caution)."}
            });
        }
    }

    try {
        std::string project_path = file_io.get_project_path().string();

        // Build push command
        std::string push_command = "cd " + escape_shell_arg(project_path) + " && git push";
        
        if (set_upstream) {
            push_command += " -u";
        }
        
        if (force) {
            push_command += " --force";
        }

        FILE* pipe = popen(push_command.c_str(), "r");
        if (!pipe) {
            return json::object({{"status", "error"}, {"message", "Failed to execute git push"}});
        }

        std::string result;
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            result += buffer;
        }

        int exit_code = pclose(pipe);

        // §Fix: a rejected/failed push must surface as an error,
        // not success.
        if (WEXITSTATUS(exit_code) != 0) {
            return json::object({
                {"status", "error"},
                {"message", "Failed to push. Error: " + result},
                {"exit_code", WEXITSTATUS(exit_code)}
            });
        }

        // Provide meaningful output even when result is empty
        if (result.empty()) {
            result = "Successfully pushed to remote repository.\n";
        }

        return json::object({
            {"status", "success"},
            {"command", "git push" + std::string(set_upstream ? " -u" : "") + std::string(force ? " --force" : "")},
            {"output", result},
            {"exit_code", WEXITSTATUS(exit_code)}
        });
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string(e.what())}});
    }
}

// ========== GitMergeTool ==========

json GitMergeTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", json::object({
            {"name", get_name()},
            {"description", "Merge a branch into the current branch. Supports regular merges, --no-ff merges, squash merges, and aborting a merge in progress."},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"branch", json::object({
                        {"type", "string"},
                        {"description", "The name of the branch to merge into the current branch"}
                    })},
                    {"no_fast_forward", json::object({
                        {"type", "boolean"},
                        {"description", "Use --no-ff flag to create a merge commit even when merge resolves as fast-forward"}
                    })},
                    {"squash", json::object({
                        {"type", "boolean"},
                        {"description", "Use --squash to squash all commits from the branch into a single commit"}
                    })},
                    {"abort", json::object({
                        {"type", "boolean"},
                        {"description", "Abort the current merge in progress and restore the pre-merge state"}
                    })}
                })},
                {"required", json::array({"branch"})}
            })}
        })}
    });
}

json GitMergeTool::execute(const json& args, FileIO& file_io) {
    std::string branch;
    bool no_fast_forward = false;
    bool squash = false;
    bool abort = false;

    try {
        if (args.is_object()) {
            if (args.contains("branch")) {
                branch = args["branch"].get<std::string>();
            }
            if (args.contains("no_fast_forward")) {
                no_fast_forward = args["no_fast_forward"].get<bool>();
            }
            if (args.contains("squash")) {
                squash = args["squash"].get<bool>();
            }
            if (args.contains("abort")) {
                abort = args["abort"].get<bool>();
            }
        } else if (args.is_string()) {
            json parsed_args = json::parse(args.get<std::string>());
            if (parsed_args.contains("branch")) {
                branch = parsed_args["branch"].get<std::string>();
            }
            if (parsed_args.contains("no_fast_forward")) {
                no_fast_forward = parsed_args["no_fast_forward"].get<bool>();
            }
            if (parsed_args.contains("squash")) {
                squash = parsed_args["squash"].get<bool>();
            }
            if (parsed_args.contains("abort")) {
                abort = parsed_args["abort"].get<bool>();
            }
        } else {
            return json::object({{"status", "error"}, {"message", "Invalid arguments format for git_merge"}});
        }
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string("Error parsing arguments: ") + e.what()}});
    }

    try {
        std::string project_path = file_io.get_project_path().string();

        std::string merge_command = "cd " + escape_shell_arg(project_path) + " && git merge";

        if (abort) {
            merge_command = "cd " + escape_shell_arg(project_path) + " && git merge --abort";
        } else {
            if (branch.empty()) {
                return json::object({{"status", "error"}, {"message", "Missing or empty 'branch' argument"}});
            }

            // Escape single quotes in branch name
            std::string escaped_branch = branch;
            size_t pos = 0;
            while ((pos = escaped_branch.find('\'', pos)) != std::string::npos) {
                escaped_branch.replace(pos, 1, "'\\''");
                pos += 4;
            }

            merge_command += " '" + escaped_branch + "'";

            if (no_fast_forward) {
                merge_command += " --no-ff";
            }
            if (squash) {
                merge_command += " --squash";
            }
        }

        FILE* pipe = popen(merge_command.c_str(), "r");
        if (!pipe) {
            return json::object({{"status", "error"}, {"message", "Failed to execute git merge"}});
        }

        std::string result;
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            result += buffer;
        }

        int exit_code = pclose(pipe);

        // Check for merge conflicts
        bool has_conflicts = (result.find("CONFLICT") != std::string::npos ||
                              result.find("conflict") != std::string::npos);

        if (has_conflicts || WEXITSTATUS(exit_code) != 0) {
            return json::object({
                {"status", "error"},
                {"command", merge_command},
                {"output", result},
                {"exit_code", WEXITSTATUS(exit_code)},
                {"has_conflicts", has_conflicts},
                {"message", has_conflicts ? "Merge conflict detected. Resolve conflicts manually and commit, or use git_merge with abort=true to cancel." : "Merge failed"}
            });
        }

        return json::object({
            {"status", "success"},
            {"command", merge_command},
            {"output", result},
            {"exit_code", WEXITSTATUS(exit_code)}
        });
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string(e.what())}});
    }
}
