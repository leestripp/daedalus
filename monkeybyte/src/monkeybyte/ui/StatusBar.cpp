#include "monkeybyte/ui/StatusBar.h"

#include <cstdio>

namespace monkeybyte {

namespace {

// 950 → "950", 12800 → "12.8k", 250000 → "250k"
std::string kformat(int n) {
    char buf[32];
    if (n < 1000) {
        std::snprintf(buf, sizeof(buf), "%d", n);
    } else if (n < 10000) {
        std::snprintf(buf, sizeof(buf), "%.1fk", n / 1000.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%dk", (n + 500) / 1000);
    }
    return buf;
}

} // namespace

StatusBar::StatusBar() : Gtk::Box(Gtk::Orientation::HORIZONTAL) {
    add_css_class("status-bar");
    set_size_request(-1, 24);

    branch_label_.set_halign(Gtk::Align::START);
    branch_label_.set_hexpand(true);
    branch_label_.set_visible(false);
    model_label_.set_halign(Gtk::Align::END);

    append(branch_label_);
    append(model_label_);
}

void StatusBar::set_branch_info(const std::string& text) {
    branch_label_.set_text(text);
    branch_label_.set_visible(!text.empty());
}

void StatusBar::set_model(const std::string& model) {
    model_ = model;
    refresh_model_label();
}

void StatusBar::set_agent_stats(const daedalus::AgentStats& stats) {
    stats_ = stats;
    refresh_model_label();
}

// §8 — right side: model + live agent stats. With no session (zeroed
// stats) only the model name shows.
void StatusBar::refresh_model_label() {
    std::string text = model_.empty() ? "No model" : model_;
    if (stats_.iteration > 0 || stats_.total_tokens > 0) {
        text += " · " + kformat(stats_.total_tokens);
        if (stats_.max_context_size > 0) {
            text += "/" + kformat(stats_.max_context_size) + " ctx";
            const int pct = static_cast<int>(
                100.0 * stats_.total_tokens / stats_.max_context_size + 0.5);
            text += " (" + std::to_string(pct) + "%)";
        } else {
            text += " tok";
        }
        if (stats_.cached_tokens > 0)
            // §ContextCache — per LAST response (daedalus resets it from
            // each response's usage); show cached/prompt so a partial hit
            // reads as such instead of looking like a reset.
            text += " · cached " + kformat(stats_.cached_tokens) + "/" +
                    kformat(stats_.prompt_tokens);
        text += " · iter " + std::to_string(stats_.iteration);
        if (stats_.completed_tool_calls > 0 || stats_.failed_tool_calls > 0) {
            text += " · tools " + std::to_string(stats_.completed_tool_calls);
            if (stats_.failed_tool_calls > 0)
                text += " +" + std::to_string(stats_.failed_tool_calls) + " failed";
        }
    }
    model_label_.set_text(text);
}

} // namespace monkeybyte
