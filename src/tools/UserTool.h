#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "BaseTool.h"

class FileIO;

using json = nlohmann::json;

// Forward declarations
class TellUserTool;
class CompressContextTool;
class StopTool;

/**
 * UserTool.h - Contains user interaction tool classes
 */

// ========== TellUserTool ==========
class TellUserTool : public BaseTool {
public:
    std::string get_name() const override { return "tell_user"; }
    std::string get_description() const override { return "Send a message to the user"; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

// ========== CompressContextTool ==========
class CompressContextTool : public BaseTool {
public:
    std::string get_name() const override { return "compress_context"; }
    std::string get_description() const override { return "Request context compression when you no longer need older parts"; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

// ========== StopTool ==========
class StopTool : public BaseTool {
public:
    std::string get_name() const override { return "stop"; }
    std::string get_description() const override { return "Signal that the task is complete"; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};
