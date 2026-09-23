// SPDX-License-Identifier: BSL-1.0
// src/ui/StatusHud.cpp

#include "ui/StatusHud.h"

#include <daedalus.h>

#include <sstream>

#include "ThinkingSpinner.h"

namespace icarus::ui {

StatusHud::StatusHud(const RenderConfig& cfg) : cfg_(cfg), style_(cfg) {}

void StatusHud::update(const daedalus::Agent& agent) {
    auto model = agent.active_model();
    has_model_ = !model.name.empty();
    model_name_ = has_model_ ? model.name : "no model";

    state_ = std::string(daedalus::to_string(agent.state()));

    auto stats = agent.stats();
    total_tokens_ = stats.total_tokens;
    max_tokens_ = stats.max_context_size > 0 ? stats.max_context_size : 1;
    iteration_ = stats.iteration;
    max_iterations_ = stats.max_iterations > 0 ? stats.max_iterations : 1;
}

void StatusHud::apply_to_spinner(ThinkingSpinner& spinner) const {
    spinner.set_message(format_spinner_message());
}

std::string StatusHud::summary_line() const {
    std::ostringstream oss;
    oss << style_.dim("[") << style_.primary("icarus");
    if (has_model_) {
        oss << style_.dim(" · ") << style_.muted(model_name_);
    }
    oss << style_.dim(" · ") << style_.info(state_);

    oss << style_.dim(" · ") << format_token_bar(12);
    oss << style_.dim(" · iter ") << iteration_ << "/" << max_iterations_;
    if (pending_todos_ > 0) {
        oss << style_.dim(" · ") << style_.warning(std::to_string(pending_todos_) + " todo" +
                                                   (pending_todos_ == 1 ? "" : "s"));
    }
    oss << style_.dim("]") << "\x1b[0m";
    return oss.str();
}

std::string StatusHud::footer_line() const {
    int pct = 0;
    if (max_tokens_ > 0) {
        pct = static_cast<int>((static_cast<double>(total_tokens_) /
                                static_cast<double>(max_tokens_)) * 100.0);
        if (pct > 100) pct = 100;
    }

    std::ostringstream oss;
    if (has_model_) {
        oss << style_.primary(model_name_);
    } else {
        oss << style_.muted("no model");
    }

    oss << style_.dim(" · ");

    if (state_ == "Done" || state_ == "Idle") {
        oss << style_.success(state_);
    } else if (state_ == "Error" || state_ == "Cancelled") {
        oss << style_.error(state_);
    } else if (state_ == "AwaitingApproval") {
        oss << style_.warning(state_);
    } else {
        oss << style_.info(state_);
    }

    oss << style_.dim(" · tok ") << pct << "%";

    if (max_iterations_ > 1) {
        oss << style_.dim(" · iter ") << iteration_ << "/" << max_iterations_;
    }

    if (pending_todos_ > 0) {
        oss << style_.dim(" · ")
            << style_.warning(std::to_string(pending_todos_) + " todo" +
                              (pending_todos_ == 1 ? "" : "s"));
    }

    oss << "\x1b[0m";
    return oss.str();
}

std::string StatusHud::format_spinner_message() const {
    // The spinner only shows the current agent state (plus the glyph).
    // Model, token, iteration and todo details already live in the sticky
    // footer, so duplicating them here just adds noise. Before the first
    // state change of a turn the agent is still Idle — show a generic
    // "Thinking..." placeholder instead of the misleading "Idle" label.
    Style s(cfg_);
    if (state_.empty() || state_ == "Idle") {
        return cfg_.color_enabled ? s.primary("Thinking...") : "Thinking...";
    }
    if (!cfg_.color_enabled) return state_;

    if (state_ == "Done") {
        return s.success(state_);
    }
    if (state_ == "Error" || state_ == "Cancelled") {
        return s.error(state_);
    }
    if (state_ == "AwaitingApproval") {
        return s.warning(state_);
    }
    return s.info(state_);
}

std::string StatusHud::format_token_bar(std::size_t width) const {
    double frac = static_cast<double>(total_tokens_) / static_cast<double>(max_tokens_);
    std::ostringstream oss;
    oss << progress_bar(frac, width, cfg_) << " " << static_cast<int>(frac * 100.0) << "%";
    return oss.str();
}

} // namespace icarus::ui
