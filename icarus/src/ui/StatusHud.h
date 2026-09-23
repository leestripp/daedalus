// SPDX-License-Identifier: BSL-1.0
// src/ui/StatusHud.h
//
// Lightweight status HUD for the icarus interactive session. It does not
// take over the terminal screen; instead it:
//   1. Updates the ThinkingSpinner message with the current agent state only
//      (model/token/iteration/todo details live in the sticky footer).
//   2. Provides a compact summary line that can be printed after a turn.
//
// A true persistent sticky bar is intentionally deferred: the agent streams
// text and subprocess output, which fights with full-screen redraws. The
// spinner line is the ideal single-line status surface in a scrollback-first
// CLI.

#pragma once

#include <cstddef>
#include <string>
#include <utility>

#include "ui/AnsiRenderer.h"

// Forward declaration to avoid pulling in ThinkingSpinner.h here.
class ThinkingSpinner;

namespace daedalus {
class Agent;
struct AgentStats;
} // namespace daedalus

namespace icarus::ui {

class StatusHud {
public:
    explicit StatusHud(const RenderConfig& cfg);

    // Refresh the HUD from the agent's current state and stats.
    void update(const daedalus::Agent& agent);

    // Set the displayed state directly. The sink uses this with the `to`
    // value from on_state_change: daedalus's Agent::state() only ever
    // reports Understanding/terminal states — the intermediate states are
    // driven by the LLM's set_status tool and only reach the host through
    // the sink callback.
    void set_state(std::string state) { state_ = std::move(state); }

    // Apply the current status string to the spinner message.
    void apply_to_spinner(ThinkingSpinner& spinner) const;

    // Render a compact summary line suitable for printing after a turn.
    // Includes reset escape so styles do not leak.
    [[nodiscard]] std::string summary_line() const;

    // Render a compact one-line footer for the sticky panel.
    // Includes reset escape so styles do not leak.
    [[nodiscard]] std::string footer_line() const;

    // Number of pending todos to show in the status. Updated by caller.
    void set_pending_todos(std::size_t n) { pending_todos_ = n; }

private:
    RenderConfig cfg_;
    Style style_;

    std::string model_name_;
    std::string state_;
    std::size_t total_tokens_ = 0;
    std::size_t max_tokens_ = 0;
    std::size_t iteration_ = 0;
    std::size_t max_iterations_ = 0;
    std::size_t pending_todos_ = 0;
    bool has_model_ = false;

    [[nodiscard]] std::string format_spinner_message() const;
    [[nodiscard]] std::string format_token_bar(std::size_t width) const;
};

} // namespace icarus::ui
