#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "BaseTool.h"

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
    std::string get_description() const override { return "Show working tree status"; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

// ========== GitBranchTool ==========
class GitBranchTool : public BaseTool {
public:
    std::string get_name() const override { return "git_branch"; }
    std::string get_description() const override { return "List all branches"; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

// ========== GitCommitTool ==========
class GitCommitTool : public BaseTool {
public:
    std::string get_name() const override { return "git_commit"; }
    std::string get_description() const override { return "Commit staged changes"; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

// ========== GitAddTool ==========
class GitAddTool : public BaseTool {
public:
    std::string get_name() const override { return "git_add"; }
    std::string get_description() const override { return "Stage files for commit"; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

// ========== GitSwitchTool ==========
class GitSwitchTool : public BaseTool {
public:
    std::string get_name() const override { return "git_switch"; }
    std::string get_description() const override { return "Switch to a different branch"; }
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
    std::string get_description() const override { return "Push commits to remote repository"; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

// ========== GitMergeTool ==========
class GitMergeTool : public BaseTool {
public:
    std::string get_name() const override { return "git_merge"; }
    std::string get_description() const override { return "Merge a branch into the current branch"; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};
