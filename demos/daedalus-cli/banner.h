// SPDX-License-Identifier: BSL-1.0
// demos/daedalus-cli/banner.h
//
// Startup banner printer. Extracted from main.cpp to keep it under 300 lines.

#pragma once

#include <daedalus.h>

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
                         const std::string& data_str) {
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

    std::cout << "\n"
        << "╔══════════════════════════════════════════════════════════════════════════╗\n"
        << "║  daedalus CLI — public-API host for libdaedalus.a                          ║\n"
        << "║  Library: " << daedalus::Agent::library_version()
                    << std::string(63 - daedalus::Agent::library_version().size(), ' ')
                    << "║\n"
        << "╠══════════════════════════════════════════════════════════════════════════╣\n"
        << "║  Active model:    " << active_name
                    << std::string(52 - active_name.size(), ' ') << "║\n"
        << "║  Model id:        " << model_id
                    << std::string(52 - model_id.size(), ' ') << "║\n"
        << "║  Base URL:        " << full_url
                    << std::string(54 - full_url.size(), ' ') << "║\n"
        << "║  Chat path:       " << endpoint
                    << std::string(54 - endpoint.size(), ' ') << "║\n"
        << "║  Max context:     " << ctx_str
                    << std::string(52 - ctx_str.size(), ' ') << "║\n"
        << "║  Temperature:     " << active.temperature
                    << std::string(52 - std::to_string(active.temperature).size(), ' ')
                    << "║\n"
        << "╠══════════════════════════════════════════════════════════════════════════╣\n"
        << "║  daedalus_home:    " << home_str
                    << std::string(52 - home_str.size(), ' ') << "║\n"
        << "║  project_root:    " << root_str
                    << std::string(52 - root_str.size(), ' ') << "║\n"
        << "║  session_id:      " << sess_str
                    << std::string(52 - sess_str.size(), ' ') << "║\n"
        << "║  data_dir:        " << data_str
                    << std::string(52 - data_str.size(), ' ') << "║\n"
        << "║  agent_name:      " << (agent_name_str.empty()
                                            ? std::string("(none — legacy .daedalus/)")
                                            : agent_name_str)
                    << std::string(52 - (agent_name_str.empty() ? 22 : agent_name_str.size()), ' ')
                    << "║\n"
        << "║  memory_enabled:  " << (cfg_view.memory_enabled ? "yes" : "no")
                    << std::string(52 - (cfg_view.memory_enabled ? 3 : 2), ' ') << "║\n"
        << "║  auto_approve:   " << (eff_auto ? "yes" : "no")
                    << std::string(52 - (eff_auto ? 3 : 2), ' ') << "║\n"
        << "║  stream:          " << stream_str
                    << std::string(52 - stream_str.size(), ' ') << "║\n"
        << "║  skills loaded:  " << skills_str
                    << std::string(52 - skills_str.size(), ' ') << "║\n"
        << "║  max_iterations:  " << cfg_view.max_agent_iterations
                    << std::string(52 - std::to_string(cfg_view.max_agent_iterations).size(), ' ')
                    << "║\n"
        << "╠══════════════════════════════════════════════════════════════════════════╣\n"
        << "║  Type /help for in-session commands. /quit to exit.                      ║\n"
        << "╚══════════════════════════════════════════════════════════════════════════╝\n";
}
