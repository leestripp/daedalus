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
    // §Phase 9.10: deltas stream after on_llm_response_started has already
    // stopped the spinner. Just flush the text; the terminal line is clean
    // because the spinner cleared itself before stopping.
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

void ConsoleAgentSink::on_tool_call(std::string_view name,
                                    const nlohmann::json& args) noexcept {
    // Compact raw output: no box styling imposed by the library.
    std::cerr << "[TOOL CALL] " << name << " " << args.dump() << "\n";
}

void ConsoleAgentSink::on_tool_result(std::string_view name,
                                     const nlohmann::json& result) noexcept {
    // Compact raw output: no box styling imposed by the library.
    // If the output already streamed live via output_chunk progress events,
    // elide it from the dump so it is not printed twice.
    nlohmann::json display = result;
    if (streamed_tool_output_ && display.contains("output")) {
        display["output"] = "<streamed live above>";
    }
    streamed_tool_output_ = false;
    std::cerr << "[TOOL RESULT] " << name << " " << display.dump() << "\n";
}

/// §Phase C1: streaming tool-call progress. Most useful for slow tools
/// like `execute_bash` running a long build. Live output chunks are printed
/// verbatim; other stages get a compact one-liner so the user knows the
/// tool is alive between [TOOL] and [/TOOL]. The webui SSE stream (which
/// renders it per-event) gets the full JSON.
void ConsoleAgentSink::on_tool_progress(std::string_view name,
                                       const nlohmann::json& progress) noexcept {
    if (auto it = progress.find("output_chunk");
        it != progress.end() && it->is_string()) {
        std::cout << it->get_ref<const std::string&>() << std::flush;
        streamed_tool_output_ = true;
        return;
    }
    const std::string stage = progress.value("stage", "");
    const int percent = progress.value("percent", -1);
    if (percent >= 0) {
        std::cerr << "  ↳ " << name << ": " << stage
                  << " (" << percent << "%)\n";
    } else {
        std::cerr << "  ↳ " << name << ": " << stage << "\n";
    }
}

void ConsoleAgentSink::on_state_change(int from, int to) noexcept {
    (void)from;
    // §Phase 9.10: the spinner message reflects the current agent state
    // ("Planning...", "Acting...", etc.). The actual start/stop is driven
    // by the LLM lifecycle hooks below, so this method only updates the
    // label and ensures the spinner stops on terminal states.
    const AgentState s = static_cast<AgentState>(to);
    if (THINKING_STATES.contains(s)) {
        spinner_.message = std::string(daedalus::to_string(s)) + "...";
    } else {
        spinner_.notify_state_change(from, to);
    }
}

void ConsoleAgentSink::on_llm_wait_started() noexcept {
    // The agent is actually waiting for the LLM now, so show the spinner.
    spinner_.start();
}

void ConsoleAgentSink::on_llm_response_started() noexcept {
    // First token / response body arrived — stop the "waiting" indicator
    // before streamed text or tool output appears.
    spinner_.notify_llm_response_started();
}

void ConsoleAgentSink::on_llm_request_finished() noexcept {
    // Safety net: ensure the spinner is stopped for non-streaming paths,
    // cancellations, or any other case where response_started did not fire.
    spinner_.notify_llm_request_finished();
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
