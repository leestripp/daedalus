#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "BaseTool.h"

using json = nlohmann::json;

// ========== DebugLogTool ==========
// §DEBUG-LOG (2026-11-14): shared append/tail debug log. The agent writes
// with an [agent] tag via this tool; the host app (MonkeyByte backend,
// POST /v1/debug/log) appends UI/SSE events with an [app] tag. One file,
// timestamped lines — the agent tails it to correlate app-side behaviour
// with its own actions instead of guessing from source. Default path:
// <project root>/.daedalus/app-debug.log (overridable per call).
class DebugLogTool : public BaseTool {
public:
    std::string get_name() const override { return "debug_log"; }
    std::string get_description() const override {
        return "Append to, or tail, the shared agent/app debug log. "
               "The host app logs UI and event-stream activity here; use this "
               "to diagnose app behaviour with timestamps instead of guessing.";
    }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};
