// SPDX-License-Identifier: BSL-1.0
// demos/daedalus-cli/banner.h
//
// Startup banner printer. Extracted from main.cpp to keep it under 300 lines.
//
// §PT-fix 2026-07-16: replaced the inline `std::string(WIDTH - s.size(), ' ')`
// pad with a single `pad_to` helper that clamps to 0 — the original code
// called `std::string(NEGATIVE, ' ')` and threw `std::length_error` whenever
// any field (typically `data_dir` or `project_root` on a long path) exceeded
// its hard-coded column width. Symptom: `[FATAL] basic_string::_M_create`
// followed by `std::terminate` before the LLM was even contacted.
//
// The visual column widths are unchanged (still 66-char inner box), so
// short values still pad nicely; long values simply truncate-with-ellipsis
// rather than crashing the demo. Use pad_to() everywhere below.

#pragma once

#include <daedalus.h>

#include <algorithm>
#include <string>

namespace {

// Pad `s` on the right with spaces so the total visible width is at
// most `width`. Returns `s + " …<truncated>"` if `s` is already wider
// than `width - 4`. Never throws.
inline std::string pad_to(const std::string& s, std::size_t width)
{
    if (s.size() <= width) {
        // width is a small constant (≤ 66), so the cast is safe.
        return s + std::string(static_cast<std::size_t>(width) - s.size(), ' ');
    }
    if (width < 6) {
        return s.substr(0, width);
    }
    // Truncate with ellipsis so the box border still aligns.
    return s.substr(0, width - 4) + " …";
}

}  // namespace

// =============================================================================
// Banner printer — shows the resolved AgentConfig so the user can verify
// the right model / paths are loaded.
// =============================================================================

inline void print_banner(const daedalus::Agent& agent,
                         const daedalus::AgentConfig& cfg_view,
                         const std::string& home_str,
                         const std::string& root_str,
                         const std::string& sess_str,
                         const std::string& agent_name_str,
                         const std::string& data_str)
{
    auto active = agent.active_model();
    std::string active_name = active.name.empty() ? "(none)" : active.name;
    std::string model_id    = active.model_id.empty() ? "(none)" : active.model_id;
    std::string host       = active.host.empty() ? "(none)" : active.host;
    std::string endpoint   = active.chat_endpoint.empty()
                             ? "/v1/chat/completions" : active.chat_endpoint;
    std::string full_url   = host;
    if (!host.empty() && host.find("://") == std::string::npos)
        full_url = "https://" + host;
    std::string ctx_str = (active.max_context_size > 0)
                          ? std::to_string(active.max_context_size)
                          : "(default)";

    const bool eff_auto = agent.auto_approve();
    std::string stream_str = cfg_view.stream ? "yes (SSE tokens)" : "no (buffered)";
    auto skills = agent.list_skills();
    std::string skills_str = std::to_string(skills.size());

    // Inner box is 66 chars wide (between the two `║` borders). Each
    // row has a label + value. pad_to(width) keeps the right border
    // aligned and never throws.
    constexpr std::size_t kBox = 66;

    std::cout << "\n"
        << "╔══════════════════════════════════════════════════════════════════════════╗\n"
        << "║  daedalus CLI — public-API host for libdaedalus.a                          ║\n"
        << "║  Library: " << pad_to(daedalus::Agent::library_version(), kBox - 12) << "║\n"
        << "╠══════════════════════════════════════════════════════════════════════════╣\n"
        << "║  Active model:    " << pad_to(active_name,  kBox - 19) << "║\n"
        << "║  Model id:        " << pad_to(model_id,     kBox - 19) << "║\n"
        << "║  Base URL:        " << pad_to(full_url,     kBox - 19) << "║\n"
        << "║  Chat path:       " << pad_to(endpoint,     kBox - 19) << "║\n"
        << "║  Max context:     " << pad_to(ctx_str,      kBox - 19) << "║\n"
        << "║  Temperature:     " << pad_to(std::to_string(active.temperature), kBox - 19) << "║\n"
        << "╠══════════════════════════════════════════════════════════════════════════╣\n"
        << "║  daedalus_home:    " << pad_to(home_str,      kBox - 19) << "║\n"
        << "║  project_root:    " << pad_to(root_str,      kBox - 19) << "║\n"
        << "║  session_id:      " << pad_to(sess_str,      kBox - 19) << "║\n"
        << "║  data_dir:        " << pad_to(data_str,      kBox - 19) << "║\n"
        << "║  agent_name:      " << pad_to(agent_name_str.empty()
                                              ? std::string("(none — legacy .daedalus/)")
                                              : agent_name_str,
                                          kBox - 19)
                                << "║\n"
        << "║  memory_enabled:  " << pad_to(cfg_view.memory_enabled ? "yes" : "no", kBox - 19) << "║\n"
        << "║  auto_approve:   "  << pad_to(eff_auto ? "yes" : "no",               kBox - 19) << "║\n"
        << "║  stream:          " << pad_to(stream_str,                        kBox - 19) << "║\n"
        << "║  skills loaded:  "  << pad_to(skills_str,                        kBox - 19) << "║\n"
        << "║  max_iterations:  " << pad_to(std::to_string(cfg_view.max_agent_iterations),
                                          kBox - 19)
                                << "║\n"
        << "╠══════════════════════════════════════════════════════════════════════════╣\n"
        << "║  Type /help for in-session commands. /quit to exit.                      ║\n"
        << "╚══════════════════════════════════════════════════════════════════════════╝\n";
}