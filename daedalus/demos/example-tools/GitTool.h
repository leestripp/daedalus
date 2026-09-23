#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "tools/BaseTool.h"

class FileIO;

using json = nlohmann::json;

// Forward declarations
class GitStatusTool;
class GitBranchTool;
class GitCommitTool;
class GitAddTool;
class GitSwitchTool;
class GitDiffTool;
class GitPushTool;
class GitMergeTool;

/**
 * GitTool.h - Contains Git and shell execution tool classes
 */

// ========== GitStatusTool ==========
class GitStatusTool : public BaseTool {
public:
    std::string get_name() const override { return "git_status"; }
    std::string get_description() const override { return "Show working tree status: modified, staged, and untracked files."; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

// ========== GitBranchTool ==========
class GitBranchTool : public BaseTool {
public:
    std::string get_name() const override { return "git_branch"; }
    std::string get_description() const override { return "List local and remote branches in the repository."; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

// ========== GitCommitTool ==========
class GitCommitTool : public BaseTool {
public:
    std::string get_name() const override { return "git_commit"; }
    std::string get_description() const override { return "Commit staged changes. Stage files first with git_add or execute_bash."; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

// ========== GitAddTool ==========
class GitAddTool : public BaseTool {
public:
    std::string get_name() const override { return "git_add"; }
    std::string get_description() const override { return "Stage files for commit. Use '-A' or 'all' to stage all changes."; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

// ========== GitSwitchTool ==========
class GitSwitchTool : public BaseTool {
public:
    std::string get_name() const override { return "git_switch"; }
    std::string get_description() const override { return "Switch to an existing branch or create and switch to a new one."; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

// ========== GitDiffTool ==========
class GitDiffTool : public BaseTool {
public:
    std::string get_name() const override { return "git_diff"; }
    std::string get_description() const override { return "Show changes between working tree and staging area"; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

// ========== GitPushTool ==========
class GitPushTool : public BaseTool {
public:
    std::string get_name() const override { return "git_push"; }
    std::string get_description() const override { return "Push commits to the remote. Use set_upstream=true for new branches; force=true requires approval."; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

// ========== GitMergeTool ==========
class GitMergeTool : public BaseTool {
public:
    std::string get_name() const override { return "git_merge"; }
    std::string get_description() const override { return "Merge a branch into the current branch. Supports --no-ff, --squash, and --abort."; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};
