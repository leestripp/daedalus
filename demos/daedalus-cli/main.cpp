// SPDX-License-Identifier: BSL-1.0
// demos/daedalus-cli/main.cpp
//
// daedalus CLI demo — the simplest possible host for libdaedalus.
//
// This demo now uses ONLY the public <daedalus.h> API. Hosts that embed
// the library do not need to include any internal headers.
// Everything is reached through daedalus::Agent and daedalus::AgentSink.
//
// Pattern:
//   AgentConfig cfg = ... (from CLI args) ...
//   Agent agent(cfg, ConsoleAgentSink);
//   agent.initialize();
//   while (read input) {
//       agent.submit_user_message(input);
//       agent.run_one_turn();          // one user msg → stop/cancel/error
//   }
//   // dtor → cleanup() automatically

#include <iostream>
#include <string>
#include <filesystem>
#include <memory>

#include <daedalus.h>
#include <daedalus/default_paths.h>
#include <daedalus/paths.h>

#include "ConsoleAgentSink.h"
#include "util.h"
#include "banner.h"
#include "migrate.h"

namespace fs = std::filesystem;

int main(int argc, char* argv[]) {
    install_signal_handlers();

    // §Plan.md Phase 5: short-circuit `--migrate-data` / `--migrate-data-apply`
    // before any agent init. The CLI host is allowed to use stdio; this
    // path is the only place we write to stdout from the migration
    // machinery.
    {
        auto mig = daedalus_cli::detect_migrate_data_args(argc, argv);
        if (mig.present) {
            return daedalus_cli::run_migrate_data(mig.options);
        }
    }

    // --help / --version short-circuit (no full init needed).
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") { print_help(argv[0]); return 0; }
        if (a == "--version" || a == "-v") {
            std::cout << "daedalus-cli (libdaedalus)\n";
            return 0;
        }
    }

    CliArgs args = parse_cli_args(argc, argv);
    fs::path target_path = resolve_target_path(args.target_path);

    daedalus::AgentConfig cfg;
    cfg.project_root         = target_path;
    cfg.model_name          = args.model_override;
    cfg.auto_approve        = args.auto_mode;
    cfg.memory_enabled       = !args.no_memory;
    cfg.clear_memory         = args.clear_memory_flag;
    cfg.worker_threads       = args.thread_count_override;
    cfg.max_agent_iterations = 600;
    cfg.stream              = args.stream_enabled;
    cfg.daedalus_home        = args.daedalus_home;
    cfg.agent_name          = args.agent_name;

    // §Plan.md migration (2026-07-13): when the host doesn't pass
    // explicit paths, default to the XDG-compliant locations (NOT the
    // legacy `/home/ubuntu/.daedalus/` flat dir). The library's
    // `Paths::global()` resolves the same env vars as the rest of the
    // library, so this stays consistent with `--migrate-data`, the
    // deprecation warning, and `docs/storage.md`.
    if (args.skills_dirs.empty()) {
        cfg.skills_dirs = std::vector<fs::path>{
            daedalus::Paths::global().global_skills_dir() };
    }

    if (args.daedalus_home.empty()) {
        cfg.daedalus_home = daedalus::Paths::global().user_config_root();
    }
    if (args.daedalus_home.empty())
        cfg.settings_path   = daedalus::default_settings_path();

    // §Phase 14: load the demo's system_prompt.txt (or override file)
    // and pass to AgentConfig as cfg.system_prompt. The library treats
    // this as opaque text and prepends it verbatim to the composed
    // system prompt (see Agent.md Phase 14).
    cfg.system_prompt       = resolve_system_prompt(
                                  args.system_prompt_file, target_path);

    auto sink  = std::make_shared<daedalus::ConsoleAgentSink>();
    daedalus::Agent agent(std::move(cfg), sink);

    try {
        agent.initialize();

        if (args.model_list_only) {
            auto models = agent.list_models();
            std::cout << "Available models:\n";
            if (models.empty()) std::cout << "  (none configured)\n";
            else for (const auto& m : models)
                std::cout << "  " << m.name << " (model=" << m.model_id
                          << ", temperature=" << m.temperature << ")\n";
            agent.shutdown();
            return 0;
        }

        if (args.clear_memory_flag) {
            agent.shutdown();
            return 0;
        }

        auto cfg_view   = agent.config();
        auto data_dir   = daedalus::resolve_agent_dir(cfg_view.project_root, args.agent_name);
        print_banner(agent, cfg_view,
                     args.daedalus_home.empty() ? cfg_view.daedalus_home.string() : args.daedalus_home.string(),
                     target_path.string(),
                     cfg_view.session_id,
                     args.agent_name,
                     data_dir.string());

        while (!g_shutting_down.load()) {
            std::cout << "\n> " << std::flush;
            std::string line;
            if (!std::getline(std::cin, line)) break;

            while (!line.empty() && std::isspace((unsigned char)line.front())) line.erase(line.begin());
            while (!line.empty() && std::isspace((unsigned char)line.back()))  line.pop_back();
            if (line.empty()) continue;

            // -------- In-session commands --------
            if (line == "/quit" || line == "/exit") {
                std::cout << "Exiting. Goodbye!\n";
                break;
            }
            if (line == "/help")   { print_help(argv[0]); continue; }
            if (line == "/clear") {
                std::cout << "[CLEAR] Session reset\n";
                continue;
            }
            if (line == "/models") {
                auto models = agent.list_models();
                std::cout << "Available models:\n";
                if (models.empty()) std::cout << "  (none)\n";
                else for (const auto& m : models)
                    std::cout << "  " << m.name << " (model=" << m.model_id
                              << ", temperature=" << m.temperature << ")\n";
                continue;
            }
            if (line == "/skills") {
                auto skills = agent.list_skills();
                std::cout << "Loaded skills (" << skills.size() << "):\n";
                if (skills.empty()) std::cout << "  (none)\n";
                else for (const auto& s : skills)
                    std::cout << "  - " << s.name << ": " << s.description << "\n";
                continue;
            }
            if (line == "/tools") {
                auto tools = agent.list_tools();
                std::cout << "Registered tools (" << tools.size() << "):\n";
                if (tools.empty()) std::cout << "  (none)\n";
                else for (const auto& t : tools)
                    std::cout << "  - " << t.name << ": " << t.description << "\n";
                continue;
            }
            if (line == "/stats") {
                auto st = agent.stats();
                std::cout << "Stats: iteration=" << st.iteration
                          << " / " << st.max_iterations
                          << " | tokens=" << st.total_tokens
                          << " (prompt=" << st.prompt_tokens
                          << ", completion=" << st.completion_tokens << ")"
                          << " | ctx=" << st.total_tokens << "/" << st.max_context_size << "\n";
                continue;
            }
            if (line == "/state") {
                std::cout << "State: " << daedalus::to_string(agent.state()) << "\n";
                continue;
            }

            // Actual user input — drive one agent turn.
            agent.submit_user_message(std::move(line));
            agent.run_one_turn();
        }
    } catch (const std::exception& e) {
        // §Demo fix 2026-07-14: restore cursor + SGR before printing
        // the FATAL line. The catch block is the last chance to do
        // regular stdio writes (the signal handler only uses the
        // async-signal-safe `write(2)`). Try both so a console that
        // was previously hidden / colour-painted reads cleanly.
        std::cerr << "\x1b[?25h\x1b[0m" << std::flush;
        daedalus_cli_restore_terminal_state();
        std::cerr << "\n[FATAL] " << e.what() << "\n";
        return 1;
    }

    agent.shutdown();
    return 0;
}
