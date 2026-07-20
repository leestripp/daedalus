#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "tools/BaseTool.h"

class FileIO;

class BackgroundTool : public BaseTool {
public:
    std::string get_name() const override { return "background_process"; }
    std::string get_description() const override { return "Start, send to, poll, list, and stop background server processes."; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;

private:
    json handle_start(const json& args);
    json handle_send(const json& args);
    json handle_poll(const json& args);
    json handle_stop(const json& args);
    json handle_list(const json& args);
    std::string sanitize_output(const std::string& output);
};
