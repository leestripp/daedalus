// SPDX-License-Identifier: BSL-1.0
// demos/daedalus-webui/web_sse_sink.h
//
// WebSseSink: implements AgentSink, writes to the session's SSE handler.
// Extracted from main.cpp to keep main.cpp under 300 lines.

#pragma once

#include <string>
#include <string_view>
#include <nlohmann/json.hpp>

#include "OmniNet.h"
#include "sse/SSEHandler.h"

#include <daedalus.h>

namespace websse {
using namespace OmniNet;

class WebSseSink : public daedalus::AgentSink {
public:
    void bind_sse(SSE::Handler* sse) { sse_ = sse; }

    void on_assistant_delta(std::string_view text) noexcept override {
        if (!sse_) return;
        json j = { {"text", std::string(text)} };
        sse_->broadcast("assistant_delta", j.dump());
    }

    void on_tool_call(std::string_view name,
                      const json& args) noexcept override {
        if (!sse_) return;
        json j = { {"name", std::string(name)}, {"args", args} };
        sse_->broadcast("tool_call", j.dump());
    }

    void on_tool_result(std::string_view name,
                        const json& result) noexcept override {
        if (!sse_) return;
        json j = { {"name", std::string(name)}, {"result", result} };
        sse_->broadcast("tool_result", j.dump());
    }

    void on_state_change(int /*from*/, int to) noexcept override {
        if (!sse_) return;
        const char* names[] = {
            "Idle","Understanding","Planning","Acting","Verifying",
            "AwaitingApproval","Compressing","Done",
            "Cancelled","Error"
        };
        const char* nm = (to >= 0 && to < 10) ? names[to] : "Unknown";
        json j = { {"to", nm}, {"raw", to} };
        sse_->broadcast("state_change", j.dump());
        // Treat terminal states as "end of current assistant message".
        if (to == 7 /* Done */ || to == 8 /* Cancelled */) {
            sse_->broadcast("assistant_end", "{}");
        }
    }

    void on_log(daedalus::LogLevel level,
                std::string_view message) noexcept override {
        if (!sse_) return;
        const char* lname = (level == daedalus::LogLevel::Error) ? "Error"
                          : (level == daedalus::LogLevel::Warn)  ? "Warn"
                          : "Info";
        json j = { {"level", lname}, {"message", std::string(message)} };
        sse_->broadcast("log", j.dump());
    }

    void on_compaction(std::size_t before, std::size_t after) noexcept override {
        if (!sse_) return;
        json j = { {"before", before}, {"after", after} };
        sse_->broadcast("compaction", j.dump());
    }

    void on_bell() noexcept override {
        if (!sse_) return;
        sse_->broadcast("bell", "{}");
    }

    // For this demo we auto-approve all bash commands. A real host would
    // forward the prompt to the browser and wait for user click.
    bool request_command_approval(std::string_view command) override {
        if (sse_) {
            json j = { {"approved", true},
                       {"command", std::string(command)} };
            sse_->broadcast("command_approval", j.dump());
        }
        return true;
    }

private:
    SSE::Handler* sse_{nullptr};
};


} // namespace websse
