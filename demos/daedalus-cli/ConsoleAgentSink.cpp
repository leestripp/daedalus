/**
 * @file demos/daedalus-cli/ConsoleAgentSink.cpp
 * @brief ConsoleAgentSink implementation.
 */

#include "ConsoleAgentSink.h"

#include <iostream>
#include <sstream>
#include <unordered_set>

#include "daedalus/types/AgentState.h"

namespace daedalus {

// §Phase 9.10: set of AgentStates that warrant showing the spinner.
// Anything outside this set (Done, Error, Cancelled, ...)
// hides both the spinner and the spinner line.
static const std::unordered_set<AgentState> THINKING_STATES = {
    AgentState::Understanding, AgentState::Planning,
    AgentState::Acting,        AgentState::Verifying,
    AgentState::Compressing,
};

ConsoleAgentSink::ConsoleAgentSink() {
    // §Phase 9.10: spinner's background thread writes to stderr by
    // default — same stream the [STATE]/[TOOL]/[Log] lines use. The
    // spinner's first frame starts with `\r` so it overwrites the
    // current line instead of advancing, and on auto-stop writes
    // `\r\x1b[K` to clear the same line. Net result: a single
    // always-at-bottom-of-line "Thinking…" indicator.
    spinner_.message = "Thinking...";
}

void ConsoleAgentSink::on_assistant_delta(std::string_view text) noexcept {
    // §Phase 9.10: if the spinner is currently animating, stop it
    // BEFORE flushing any text so we leave a clean line below the
    // spinner's last position. The flush goes to stdout (cout), the
    // spinner was on stderr; they share the same terminal so we need
    // the explicit `\n` to push past the cleared spinner line.
    const bool spinner_was_running = spinner_.is_running();
    if (spinner_was_running) {
        spinner_.stop();   // joins the worker thread (max ~100ms)
        std::cerr << "\n" << std::flush;
    } else {
        // Even without a spinner, notify so the spinner's own state
        // is consistent (idempotent no-op if already stopped).
        spinner_.notify_assistant_delta();
    }

    delta_buffer_ += text;
    if (delta_buffer_.size() >= DELTA_FLUSH_THRESHOLD ||
        delta_buffer_.find('\n') != std::string::npos) {
        flush_delta_buffer();
    }
}

void ConsoleAgentSink::flush_delta_buffer() {
    if (!delta_buffer_.empty()) {
        std::cout << delta_buffer_ << std::flush;
        delta_buffer_.clear();
    }
}

void ConsoleAgentSink::on_tool_call(std::string_view /*name*/,
                                    const nlohmann::json& /*args*/) noexcept {
    // Intentionally empty: formatted tool display is handled by on_tool_display
    // so the CLI does not emit redundant [TOOL] tags around the block.
}

void ConsoleAgentSink::on_tool_result(std::string_view /*name*/,
                                     const nlohmann::json& /*result*/) noexcept {
    // Intentionally empty: formatted tool display is handled by on_tool_display.
}

/// §Phase C1: streaming tool-call progress. Most useful for slow tools
/// like `execute_bash` running a long build. We just print a compact
/// one-liner so the user knows the tool is alive between [TOOL] and
/// [/TOOL]. The webui SSE stream (which renders it per-event) gets the
/// full JSON; the CLI only needs this compact marker.
void ConsoleAgentSink::on_tool_progress(std::string_view name,
                                       const nlohmann::json& progress) noexcept {
    const std::string stage = progress.value("stage", "");
    const int percent = progress.value("percent", -1);
    if (percent >= 0) {
        std::cerr << "  ↳ " << name << ": " << stage
                  << " (" << percent << "%)\n";
    } else {
        std::cerr << "  ↳ " << name << ": " << stage << "\n";
    }
}

void ConsoleAgentSink::on_tool_display(std::string_view text) noexcept {
    // Print the library's formatted tool block directly without a log-level
    // prefix so the CLI output stays clean.
    std::cerr << text << std::flush;
}

void ConsoleAgentSink::on_state_change(int from, int to) noexcept {
    (void)from;
    // §Phase 9.10: stop the spinner first so the new state's label
    // appears on its own line. The spinner label ("Planning...",
    // "Acting...") IS the state indicator now — the old
    // "[STATE] X -> Y" log line is intentionally omitted because
    // it would be redundant with the spinner's message.
    spinner_.stop();
    const AgentState s = static_cast<AgentState>(to);
    if (THINKING_STATES.contains(s)) {
        spinner_.message = std::string(daedalus::to_string(s)) + "...";
        spinner_.start();
    }
}

void ConsoleAgentSink::on_compaction(std::size_t before_tokens,
                                     std::size_t after_tokens) noexcept {
    // §Phase 9.6: notify the user when context was just compacted so
    // they understand why old messages were dropped.  Print before → after.
    std::cerr << "[COMPACT] " << before_tokens << " -> " << after_tokens
              << " tokens\n";
}

void ConsoleAgentSink::on_log(daedalus::LogLevel level,
                              std::string_view message) noexcept {
    // Print directly to cerr (same as AgentSink default, but explicit).
    std::cerr << "[" << daedalus::to_string(level) << "] " << message << "\n";
}

void ConsoleAgentSink::on_bell() noexcept {
    std::cout << '\a' << std::flush;
}

bool ConsoleAgentSink::request_command_approval(std::string_view command) {
    std::cout << "\n⚠  Execute the following command?\n  " << command << "\n\n  y / n  " << std::flush;
    std::string line;
    std::getline(std::cin, line);
    if (!std::cin || line.empty()) return false;
    return line[0] == 'y' || line[0] == 'Y';
}

}  // namespace daedalus
