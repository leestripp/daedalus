#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "BaseTool.h"

class FileIO;

using json = nlohmann::json;

// Forward declarations
class GrepSearchTool;
class ReadFileLinesTool;
class ReadFileHeadTool;
class ReadFileTailTool;
class ListDirectoryTreeTool;

/**
 * SearchTool.h - Contains all search and read tool classes
 */

// ========== GrepSearchTool ==========
class GrepSearchTool : public BaseTool {
public:
    std::string get_name() const override { return "grep_search"; }
    std::string get_description() const override { return "Search string/regex across project"; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

// ========== ReadFileLinesTool ==========
class ReadFileLinesTool : public BaseTool {
public:
    std::string get_name() const override { return "read_file_lines"; }
    std::string get_description() const override { return "Read specific line range from file"; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

// ========== ReadFileHeadTool ==========
class ReadFileHeadTool : public BaseTool {
public:
    std::string get_name() const override { return "read_file_head"; }
    std::string get_description() const override { return "Read first N lines of file"; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

// ========== ReadFileTailTool ==========
class ReadFileTailTool : public BaseTool {
public:
    std::string get_name() const override { return "read_file_tail"; }
    std::string get_description() const override { return "Read last N lines of file"; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

// ========== ListDirectoryTreeTool ==========
class ListDirectoryTreeTool : public BaseTool {
public:
    std::string get_name() const override { return "list_directory_tree"; }
    std::string get_description() const override { return "Tree view of directory with optional depth limit"; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

// ========== WikiSearchTool ==========
class WikiSearchTool : public BaseTool {
public:
    std::string get_name() const override { return "wiki_search"; }
    std::string get_description() const override { return "Search Wikipedia for articles"; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

// ========== SerperSearchTool ==========
class SerperSearchTool : public BaseTool {
public:
    std::string get_name() const override { return "serper_search"; }
    std::string get_description() const override { return "Perform web search using Google via Serper.dev API"; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};
