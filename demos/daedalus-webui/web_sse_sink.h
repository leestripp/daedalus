// SPDX-License-Identifier: BSL-1.0
// demos/daedalus-webui/web_sse_sink.h
//
// WebSseSink: implements AgentSink, writes to the session's SSE handler.
// Extracted from main.cpp to keep main.cpp under 300 lines.

#pragma once

#include <string>
#include <string_view>
#include <condition_variable>
#include <mutex>
#include <nlohmann/json.hpp>

#include "OmniNet.h"
#include "sse/SSEHandler.h"

#include <daedalus.h>

namespace websse {
using namespace OmniNet;

class WebSseSink : public daedalus::AgentSink {
public:
    explicit WebSseSink(bool auto_approve = false) : auto_approve_(auto_approve) {}

    void bind_sse(SSE::Handler* sse) { sse_ = sse; }

    void set_auto_approve(bool v) { auto_approve_ = v; }

    void on_assistant_delta(std::string_view text) noexcept override {
        if (!sse_) return;
        json j = { {"text", std::string(text)} };
        sse_->broadcast("assistant_delta", j.dump());
    }

    // §Phase D1: 3-arg overloads keyed by OpenAI-style call_id so the
    // browser can group tool_call + tool_progress + tool_result events
    // for the same invocation into a single card. Sinks that don't
    // care about correlation (ConsoleAgentSink, test stubs) keep
    // overriding only the 2-arg version — the 3-arg default in
    // AgentSink discards the id and forwards.
    void on_tool_call(std::string_view name,
                      std::string_view call_id,
                      const json& args) noexcept override {
        if (!sse_) return;
        json j = { {"id", std::string(call_id)},
                   {"name", std::string(name)},
                   {"args", args} };
        sse_->broadcast("tool_call", j.dump());
    }

    void on_tool_result(std::string_view name,
                        std::string_view call_id,
                        const json& result) noexcept override {
        if (!sse_) return;
        json j = { {"id", std::string(call_id)},
                   {"name", std::string(name)},
                   {"result", result} };
        sse_->broadcast("tool_result", j.dump());
    }

    // §webui-tool-display: human-readable tool header/result/separator
    // produced by the library. The structured tool_call/tool_result events
    // already drive the side panel; this text stream is rendered inline in
    // the transcript so a turn that only executes tools is not empty.
    void on_tool_display(std::string_view text) noexcept override {
        if (!sse_) return;
        json j = { {"text", std::string(text)} };
        sse_->broadcast("tool_display", j.dump());
    }

    /// §Phase C1: streaming tool-call progress — fires between
    /// tool_call and tool_result. The webui can show a live progress
    /// bar / status text per tool, instead of waiting in silence.
    void on_tool_progress(std::string_view name,
                         std::string_view call_id,
                         const json& progress) noexcept override {
        if (!sse_) return;
        json j = { {"id", std::string(call_id)},
                   {"name", std::string(name)},
                   {"progress", progress} };
        sse_->broadcast("tool_progress", j.dump());
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

    // §webui-approval: if --auto is set, approve immediately. Otherwise
    // emit an approval_required SSE event and block the agent's worker
    // thread until the browser POSTs a decision to /sessions/<id>/approve.
    bool request_command_approval(std::string_view command) override {
        if (auto_approve_) {
            if (sse_) {
                json j = { {"auto_approved", true},
                           {"command", std::string(command)} };
                sse_->broadcast("command_approval", j.dump());
            }
            return true;
        }

        {
            std::lock_guard<std::mutex> lock(approval_mu_);
            pending_command_ = std::string(command);
            approval_ready_  = false;
        }

        if (sse_) {
            json j = { {"command", std::string(command)} };
            sse_->broadcast("approval_required", j.dump());
        }

        std::unique_lock<std::mutex> lock(approval_mu_);
        approval_cv_.wait(lock, [this] {
            return approval_ready_ || shutdown_;
        });

        // If the session is shutting down while we wait, deny so the
        // agent can unwind cleanly.
        if (shutdown_) return false;

        return approval_approved_;
    }

    // Called from the HTTP handler thread when the browser decides.
    // sticky is accepted for API symmetry but not persisted in this demo.
    void resolve_approval(bool approved, bool /*sticky*/) {
        {
            std::lock_guard<std::mutex> lock(approval_mu_);
            approval_ready_    = true;
            approval_approved_ = approved;
        }
        approval_cv_.notify_one();
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(approval_mu_);
            shutdown_ = true;
        }
        approval_cv_.notify_one();
    }

private:
    SSE::Handler* sse_{nullptr};
    bool auto_approve_ = false;

    std::mutex              approval_mu_;
    std::condition_variable approval_cv_;
    std::string             pending_command_;
    bool                    approval_ready_  = false;
    bool                    approval_approved_ = false;
    bool                    shutdown_        = false;
};


} // namespace websse
