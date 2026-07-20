#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "tools/BaseTool.h"

class FileIO;

using json = nlohmann::json;

// Forward declarations
class GrepSearchTool;

/**
 * SearchTool.h - Search tool classes (no file-I/O helpers; use execute_bash).
 */

// ========== GrepSearchTool ==========
class GrepSearchTool : public BaseTool {
public:
    std::string get_name() const override { return "grep_search"; }
    std::string get_description() const override { return "Search for a string or regex across project files. Use include_pattern to limit file types."; }
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
