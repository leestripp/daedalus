#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "BaseTool.h"

class FileIO;

using json = nlohmann::json;

// Forward declarations
class ReadFileTool;
class ListDirectoryTool;
class CreateFileTool;
class CreateDirectoryTool;
class ModifyFileTool;
class EditFilePatchTool;
class DeleteFileTool;
class DeleteDirectoryTool;

/**
 * FileTool.h - Contains all file operation tool classes
 */

// ========== ReadFileTool ==========
class ReadFileTool : public BaseTool {
public:
    std::string get_name() const override { return "read_file"; }
    std::string get_description() const override { return "Read the full contents of a file. Set line_numbers=true to include line numbers in the output."; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
    // §Phase C2: 3-arg overload for streaming tool-call progress.
    // The 2-arg version above delegates here with `progress=nullptr`.
    // Reads >1MB take noticeable wall time on slow disks / network
    // filesystems, so we emit a single "reading" event just before
    // the slurp. FileIO::read_file_content caps at 10MB; we don't try
    // to mid-read percent updates because the rdbuf slurp is opaque.
    json execute(const json& args, FileIO& file_io, ToolProgressFn progress) override;
};

// ========== ListDirectoryTool ==========
class ListDirectoryTool : public BaseTool {
public:
    std::string get_name() const override { return "list_directory"; }
    std::string get_description() const override { return "List files and directories in a path, including size, modified time, permissions, and line counts."; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

// ========== CreateFileTool ==========
class CreateFileTool : public BaseTool {
public:
    std::string get_name() const override { return "create_file"; }
    std::string get_description() const override { return "Create a new file with specified content"; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

// ========== CreateDirectoryTool ==========
class CreateDirectoryTool : public BaseTool {
public:
    std::string get_name() const override { return "create_directory"; }
    std::string get_description() const override { return "Create a new directory in the project"; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

// ========== ModifyFileTool ==========
class ModifyFileTool : public BaseTool {
public:
    std::string get_name() const override { return "modify_file"; }
    std::string get_description() const override { return "Overwrite an existing file with new content. Use edit_file_patch for small, targeted changes."; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

// ========== EditFilePatchTool ==========
class EditFilePatchTool : public BaseTool {
public:
    std::string get_name() const override { return "edit_file_patch"; }
    std::string get_description() const override { return "Apply a patch to a file by replacing content. Whitespace is normalized for flexible matching."; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

// ========== DeleteFileTool ==========
class DeleteFileTool : public BaseTool {
public:
    std::string get_name() const override { return "delete_file"; }
    std::string get_description() const override { return "Delete a file at the specified path"; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

// ========== DeleteDirectoryTool ==========
class DeleteDirectoryTool : public BaseTool {
public:
    std::string get_name() const override { return "delete_directory"; }
    std::string get_description() const override { return "Delete a directory at the specified path"; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};
