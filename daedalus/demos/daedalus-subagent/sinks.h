// SPDX-License-Identifier: BSL-1.0
// demos/daedalus-subagent/sinks.h
//
// Sink implementations for the sub-agent demo.
// Extracted from main.cpp to keep it under 300 lines.

#pragma once

#include <daedalus.h>
#include "ThinkingSpinner.h"

// =============================================================================
// StdoutSink — parent agent's sink
// =============================================================================

class StdoutSink : public daedalus::AgentSink {
public:
    StdoutSink() {
        spinner_.message = "[parent] Thinking...";
        spinner_.output = &std::cout;
    }

    void on_assistant_delta(std::string_view t) noexcept override {
        if (spinner_.is_running()) {
            spinner_.stop();
            std::cout << "\n" << std::flush;
        } else {
            spinner_.notify_assistant_delta();
        }
        std::cout << "[parent] " << t;
        std::cout.flush();
    }

    void on_tool_call(std::string_view n,
                      const nlohmann::json& a) noexcept override {
        if (spinner_.is_running()) {
            spinner_.stop();
            std::cout << "\n" << std::flush;
        }
        std::cout << "\n[parent tool] " << n << " args=" << a.dump() << "\n";
    }

    void on_tool_result(std::string_view n,
                        const nlohmann::json& r) noexcept override {
        if (spinner_.is_running()) {
            spinner_.stop();
            std::cout << "\n" << std::flush;
        }
        std::cout << "[parent tool result] " << n
                  << " status=" << r.value("status", "?") << "\n";
    }

    void on_state_change(int, int to) noexcept override {
        spinner_.stop();
        const auto state_name =
            daedalus::to_string(static_cast<daedalus::AgentState>(to));
        constexpr int kUnderstanding = 1;
        constexpr int kPlanning      = 2;
        constexpr int kActing        = 3;
        constexpr int kVerifying     = 4;
        constexpr int kCompressing   = 7;
        if (to == kUnderstanding || to == kPlanning ||
            to == kActing       || to == kVerifying ||
            to == kCompressing) {
            spinner_.message = std::string("[parent] ") + state_name + "...";
            spinner_.start();
        }
    }

    void on_compaction(std::size_t before, std::size_t after) noexcept override {
        if (spinner_.is_running()) {
            spinner_.stop();
            std::cout << "\n" << std::flush;
        }
        std::cout << "\n[parent compact] " << before << " -> " << after
                  << " tokens\n";
    }

    void on_log(daedalus::LogLevel lvl, std::string_view m) noexcept override {
        if (spinner_.is_running()) {
            spinner_.stop();
            std::cout << "\n" << std::flush;
        }
        std::cerr << "[" << daedalus::to_string(lvl) << "] " << m << "\n";
    }

private:
    ThinkingSpinner spinner_;
};

// =============================================================================
// ChildOnlySink — child agent's sink (no tool logging; just thinking spinner)
// =============================================================================

class ChildOnlySink : public daedalus::AgentSink {
public:
    ChildOnlySink() {
        spinner_.message = "  [child] Thinking...";
        spinner_.output = &std::cout;
    }

    void on_assistant_delta(std::string_view t) noexcept override {
        if (spinner_.is_running()) {
            spinner_.stop();
            std::cout << "\n" << std::flush;
        } else {
            spinner_.notify_assistant_delta();
        }
        std::cout << "  [child] " << t;
        std::cout.flush();
    }

    void on_state_change(int, int to) noexcept override {
        spinner_.stop();
        const auto state_name =
            daedalus::to_string(static_cast<daedalus::AgentState>(to));
        constexpr int kUnderstanding = 1;
        constexpr int kPlanning      = 2;
        constexpr int kActing        = 3;
        constexpr int kVerifying     = 4;
        constexpr int kCompressing   = 7;
        if (to == kUnderstanding || to == kPlanning ||
            to == kActing       || to == kVerifying ||
            to == kCompressing) {
            spinner_.message = std::string("  [child] ") + state_name + "...";
            spinner_.start();
        }
    }

    void on_compaction(std::size_t before, std::size_t after) noexcept override {
        if (spinner_.is_running()) {
            spinner_.stop();
            std::cout << "\n" << std::flush;
        }
        std::cout << "\n  [child compact] " << before << " -> " << after
                  << " tokens\n";
    }

    void on_log(daedalus::LogLevel lvl, std::string_view m) noexcept override {
        if (spinner_.is_running()) {
            spinner_.stop();
            std::cout << "\n" << std::flush;
        }
        std::cerr << "  [child " << daedalus::to_string(lvl) << "] " << m << "\n";
    }

    void on_tool_call(std::string_view, const nlohmann::json&) noexcept override {
        if (spinner_.is_running()) {
            spinner_.stop();
            std::cout << "\n" << std::flush;
        }
    }

    void on_tool_result(std::string_view, const nlohmann::json&) noexcept override {
        if (spinner_.is_running()) {
            spinner_.stop();
            std::cout << "\n" << std::flush;
        }
    }

private:
    ThinkingSpinner spinner_;
};
