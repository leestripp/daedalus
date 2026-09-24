// SPDX-License-Identifier: BSL-1.0
// src/banner.h
//
// Startup banner printer. Uses icarus::ui::AnsiRenderer for colors and
// box drawing while keeping the crash-safe truncation behavior from the
// original implementation.

#pragma once

#include <daedalus.h>

#include <algorithm>
#include <iostream>
#include <string>

#include "ui/AnsiRenderer.h"

namespace {

// Pad `s` on the right with spaces so the total visible width is at most
// `width`. Truncates with ellipsis if needed. Never throws.
inline std::string pad_to(const std::string& s, std::size_t width) {
    std::size_t vw = icarus::ui::visible_width(s);
    if (vw <= width) {
        return s + std::string(static_cast<std::size_t>(width) - vw, ' ');
    }
    if (width < 6) {
        return icarus::ui::truncate(s, width, "");
    }
    return icarus::ui::truncate(s, width, " …");
}

} // namespace

inline void print_banner(const daedalus::Agent& agent,
                         const daedalus::AgentConfig& cfg_view,
                         const std::string& home_str,
                         const std::string& root_str,
                         const std::string& sess_str,
                         const std::string& agent_name_str,
                         const std::string& data_str,
                         const icarus::ui::RenderConfig& render_cfg = icarus::ui::RenderConfig::detect()) {
    // Box-drawing characters selected by Unicode capability. Defined inside
    // the function so the lambdas below do not capture an anonymous-namespace
    // type (avoids GCC -Wsubobject-linkage warnings).
    struct BannerBox {
        std::string horizontal;
        std::string vertical;
        std::string top_left;
        std::string top_right;
        std::string bottom_left;
        std::string bottom_right;
        std::string tee_left;
        std::string tee_right;
    };
    auto make_box_chars = [](bool unicode) -> BannerBox {
        if (unicode) {
            return {"═", "║", "╔", "╗", "╚", "╝", "╣", "╠"};
        }
        return {"-", "|", "+", "+", "+", "+", "+", "+"};
    };

    icarus::ui::Style s(render_cfg);
    auto box = make_box_chars(render_cfg.unicode_enabled);

    auto active = agent.active_model();
    std::string active_name = active.name.empty() ? "(none)" : active.name;
    std::string model_id = active.model_id.empty() ? "(none)" : active.model_id;
    std::string host = active.host.empty() ? "(none)" : active.host;
    std::string endpoint = active.chat_endpoint.empty()
                           ? "/v1/chat/completions" : active.chat_endpoint;
    std::string full_url = host;
    if (!host.empty() && host.find("://") == std::string::npos)
        full_url = "https://" + host;
    std::string ctx_str = (active.max_context_size > 0)
                          ? std::to_string(active.max_context_size)
                          : "(default)";

    // Trim the trailing-zero noise std::to_string(double) produces
    // ("0.000000" → "0", "0.700000" → "0.7").
    auto format_number = [](double v) {
        std::string str = std::to_string(v);
        while (str.size() > 1 && str.back() == '0') str.pop_back();
        if (!str.empty() && str.back() == '.') str.pop_back();
        return str;
    };

    const bool eff_auto = agent.auto_approve();
    std::string stream_str = cfg_view.stream ? "yes (SSE tokens)" : "no (buffered)";
    auto skills = agent.list_skills();
    std::string skills_str = std::to_string(skills.size());

    constexpr std::size_t kMaxBox = 72;
    // Fit the banner to the live terminal width (resize/narrow-term aware).
    // A full row is box_width + 6 visible columns (2 borders + 2×2 padding).
    std::size_t box_width = kMaxBox;
    const int live_width = icarus::ui::effective_terminal_width(render_cfg);
    if (live_width > 0 && static_cast<std::size_t>(live_width) < kMaxBox + 6) {
        box_width = live_width > 30 ? static_cast<std::size_t>(live_width) - 6 : 24;
    }
    // Inner width = box_width + 2 side spaces on each side = box_width + 4.
    const std::string hline(box_width + 4, '-');

    auto row = [&](const std::string& content) -> std::string {
        return box.vertical + "  " + pad_to(content, box_width) + "  " + box.vertical;
    };

    auto sep = [&]() -> std::string {
        return box.tee_right + hline + box.tee_left;
    };

    auto border = [&](const std::string& left, const std::string& right) -> std::string {
        return left + hline + right;
    };

    const std::string title = s.bold("icarus") + s.dim(" — AI coding agent powered by daedalus");

    std::cout << "\n"
        << s.primary(border(box.top_left, box.top_right)) << "\n"
        << s.primary(row(title)) << "\n"
        // §Monorepo 2026-09-24: was "Library: <daedalus version>"; one
        // unified tag stream means icarus's version IS the library's.
        << s.primary(row(s.muted("Version:      ") + ICARUS_VERSION)) << "\n"
        << s.primary(sep()) << "\n"
        << s.primary(row(s.muted("Active model: ") + s.bold(active_name))) << "\n"
        << s.primary(row(s.muted("Model id:     ") + model_id)) << "\n"
        << s.primary(row(s.muted("Base URL:     ") + full_url)) << "\n"
        << s.primary(row(s.muted("Chat path:    ") + endpoint)) << "\n"
        << s.primary(row(s.muted("Max context:  ") + ctx_str)) << "\n"
        << s.primary(row(s.muted("Temperature:  ") + format_number(active.temperature))) << "\n"
        << s.primary(sep()) << "\n"
        << s.primary(row(s.muted("daedalus_home: ") + home_str)) << "\n"
        << s.primary(row(s.muted("project_root: ") + root_str)) << "\n"
        << s.primary(row(s.muted("session_id:   ") + sess_str)) << "\n"
        << s.primary(row(s.muted("data_dir:     ") + data_str)) << "\n"
        << s.primary(row(s.muted("agent_name:   ") +
            (agent_name_str.empty() ? std::string("(none — legacy .daedalus/)") : agent_name_str))) << "\n"
        << s.primary(row(s.muted("memory:       ") +
            std::string(cfg_view.memory_enabled ? "enabled" : "disabled"))) << "\n"
        << s.primary(row(s.muted("auto_approve: ") +
            std::string(eff_auto ? "yes" : "no"))) << "\n"
        << s.primary(row(s.muted("stream:       ") + stream_str)) << "\n"
        << s.primary(row(s.muted("skills:       ") + skills_str + " loaded")) << "\n"
        << s.primary(row(s.muted("iterations:   ") +
            std::to_string(cfg_view.max_agent_iterations))) << "\n"
        << s.primary(sep()) << "\n"
        << s.primary(row("Type /help for commands. /quit to exit.")) << "\n"
        << s.primary(border(box.bottom_left, box.bottom_right)) << "\n";
}
