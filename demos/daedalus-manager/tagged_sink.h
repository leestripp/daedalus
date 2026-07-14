// SPDX-License-Identifier: BSL-1.0
// demos/daedalus-manager/tagged_sink.h
//
// TaggedSink — sink that prefixes all events with an agent role tag.
// Extracted from main.cpp to keep it under 300 lines.

#pragma once

#include <daedalus.h>
#include "ThinkingSpinner.h"

class TaggedSink : public daedalus::AgentSink {
public:
    explicit TaggedSink(std::string tag) : tag_(std::move(tag)) {
        spinner_.message = "[" + tag_ + "] Thinking...";
        spinner_.output = &std::cout;
    }

    void on_assistant_delta(std::string_view t) noexcept override {
        if (spinner_.is_running()) {
            spinner_.stop();
            std::cout << "\n" << std::flush;
        } else {
            spinner_.notify_assistant_delta();
        }
        std::cout << "[" << tag_ << "] " << t;
        std::cout.flush();
    }

    void on_tool_call(std::string_view n,
                      const nlohmann::json&) noexcept override {
        if (spinner_.is_running()) {
            spinner_.stop();
            std::cout << "\n" << std::flush;
        }
        std::cout << "\n[" << tag_ << " tool] " << n << "\n";
    }

    void on_tool_result(std::string_view n,
                        const nlohmann::json& r) noexcept override {
        if (spinner_.is_running()) {
            spinner_.stop();
            std::cout << "\n" << std::flush;
        }
        std::cout << "[" << tag_ << " result] " << n
                  << " status=" << r.value("status", "?") << "\n";
    }

    void on_state_change(int /*from*/, int to) noexcept override {
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
            spinner_.message = "[" + tag_ + "] " + state_name + "...";
            spinner_.start();
        }
    }

    void on_compaction(std::size_t before, std::size_t after) noexcept override {
        if (spinner_.is_running()) {
            spinner_.stop();
            std::cout << "\n" << std::flush;
        }
        std::cout << "\n[" << tag_ << " compact] " << before << " -> "
                  << after << " tokens\n";
    }

    void on_log(daedalus::LogLevel lvl, std::string_view m) noexcept override {
        if (spinner_.is_running()) {
            spinner_.stop();
            std::cout << "\n" << std::flush;
        }
        std::cerr << "[" << tag_ << " " << daedalus::to_string(lvl) << "] "
                  << m << "\n";
    }

private:
    std::string tag_;
    ThinkingSpinner spinner_;
};
