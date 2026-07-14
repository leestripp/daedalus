/**
 * @file demos/daedalus-cli/ConsoleAgentSink.h
 * @brief Default AgentSink implementation for the CLI demo.
 *
 * Wires every sink event to the console:
 *   - on_assistant_delta → cout (with buffering and final newline)
 *   - on_log            → cerr
 *   - on_tool_call      → cerr (info only)
 *   - on_tool_result    → cerr (info only)
 *   - on_state_change   → cerr (Phase 9.6)
 *   - on_compaction     → cerr (Phase 9.6)
 *   - on_bell           → play_system_bell()
 *   - request_command_approval → y/n prompt on cin
 *
 * Hosts that need different I/O (TUI, web, GTK4) implement their own
 * AgentSink subclass and pass it to LLMClient::set_sink().
 */
#pragma once

#include <iostream>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "daedalus/AgentSink.h"

// §Phase 9.10: demos/daedalus-cli/CMakeLists.txt adds demos/common/ to
// the include path, so the header resolves as a bare filename.
#include "ThinkingSpinner.h"

namespace daedalus {

class ConsoleAgentSink final : public AgentSink {
public:
    ConsoleAgentSink();

    // §Phase 9.10: expose the spinner so the main loop can override
    // the message ("Thinking...", "Planning...", etc.) before calling
    // start(). The sink itself manages the auto-stop on assistant_delta
    // and the start/stop on state changes.
    ThinkingSpinner& spinner() noexcept { return spinner_; }
    const ThinkingSpinner& spinner() const noexcept { return spinner_; }

    // Streaming output — buffer deltas, flush on final newline or buffer limit.
    void on_assistant_delta(std::string_view text) noexcept override;

    // Tool lifecycle — informational only.
    void on_tool_call(std::string_view name,
                      const nlohmann::json& args) noexcept override;
    void on_tool_result(std::string_view name,
                        const nlohmann::json& result) noexcept override;

    // State / lifecycle — §Phase 9.6 observability.
    void on_state_change(int from, int to) noexcept override;

    // Context compaction — §Phase 9.6 observability.
    void on_compaction(std::size_t before_tokens,
                       std::size_t after_tokens) noexcept override;

    // Log messages.
    void on_log(daedalus::LogLevel level,
                std::string_view message) noexcept override;

    // Bell.
    void on_bell() noexcept override;

    // Blocking I/O.
    bool request_command_approval(std::string_view command) override;

private:
    /// Flush any buffered assistant text to stdout.
    void flush_delta_buffer();

    std::string delta_buffer_;
    static constexpr std::size_t DELTA_FLUSH_THRESHOLD = 2048;

    // §Phase 9.10: spinner shown on stderr between user submit and the
    // first streamed assistant_delta. See demos/common/ThinkingSpinner.h
    // for the lifecycle / auto-stop contract.
    ThinkingSpinner spinner_;
};

}  // namespace daedalus
