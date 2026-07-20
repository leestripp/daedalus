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
#include <atomic>

#include <daedalus.h>
#include <daedalus/default_paths.h>
#include <daedalus/paths.h>

#include "tools/ToolFactory.h"
#include "tools/ToolHandler.h"
#include "example_tools.h"
#include "ConsoleAgentSink.h"
#include "util.h"
#include "RawStdin.h"
#include "banner.h"
#include "migrate.h"

namespace fs = std::filesystem;

// §TOOL-TRIM (prompt.txt): the CLI opts in to individual example tools.
// File / git / search / pdf / system-info / background / network tools are
// no longer registered by libdaedalus by default; the host registers exactly
// the ones it wants through the internal ToolFactory. This is the same
// factory path used by Agent::register_tool() for public custom tools, but
// the example tools are internal FileIO-based tools so they are registered
// directly with ToolHandler::get_tool_factory().
static void register_cli_tools()
{
    auto factory = ToolHandler::get_tool_factory();
    if (!factory) return;

    // Search tools
    factory->register_tool(std::make_shared<GrepSearchTool>());
    factory->register_tool(std::make_shared<WikiSearchTool>());
    factory->register_tool(std::make_shared<SerperSearchTool>());

    // PDF tool
    factory->register_tool(std::make_shared<ReadPDFTool>());

    // Git tools
    factory->register_tool(std::make_shared<GitStatusTool>());
    factory->register_tool(std::make_shared<GitBranchTool>());
    factory->register_tool(std::make_shared<GitCommitTool>());
    factory->register_tool(std::make_shared<GitAddTool>());
    factory->register_tool(std::make_shared<GitSwitchTool>());
    factory->register_tool(std::make_shared<GitDiffTool>());
    factory->register_tool(std::make_shared<GitPushTool>());
    factory->register_tool(std::make_shared<GitMergeTool>());

    // System / background / network tools
    factory->register_tool(std::make_shared<SystemInfoTool>());
    factory->register_tool(std::make_shared<BackgroundTool>());
    factory->register_tool(std::make_shared<HttpRequestTool>());
    factory->register_tool(std::make_shared<DnsLookupTool>());
    factory->register_tool(std::make_shared<TcpCheckTool>());
    factory->register_tool(std::make_shared<TlsInspectTool>());
    factory->register_tool(std::make_shared<WebSocketConnectTool>());
    factory->register_tool(std::make_shared<WebSocketSendTool>());
    factory->register_tool(std::make_shared<WebSocketRecvTool>());
    factory->register_tool(std::make_shared<WebSocketCloseTool>());
}

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

    // §MODEL-DISCOVERY: short-circuit before constructing an Agent.
    if (args.discover_models_only) {
        fs::path home = args.daedalus_home;
        if (home.empty()) {
            home = daedalus::Paths::global().user_data_root();
        }
        auto result = daedalus::Agent::discover_models(args.discover_model_name, home);
        if (!result.success) {
            std::cerr << "Error: " << result.error_message << "\n";
            return 1;
        }
        for (const auto& m : result.models) {
            std::cout << m.id << "\n";
        }
        return 0;
    }

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
        // Use user_data_root() (XDG $XDG_DATA_HOME/daedalus/,
        // default $HOME/.local/share/daedalus/) — NOT user_config_root().
        // The SettingsStore DB lives at <daedalus_home>/daedalus.db,
        // and that DB is rooted at the XDG *data* hierarchy per Plan.md
        // §3.2 (and `default_daedalus_home()` in default_paths.cpp uses
        // `user_root()` which also resolves here when no legacy
        // override exists).
        //
        // Bug (2026-07-16): previously this used `user_config_root()`
        // which returned $HOME/.config/daedalus. That path never holds
        // the DB — the user's models live under the *data* path, so
        // `daedalus-cli --model-list` reported "(none configured)"
        // unless the user passed `--home /home/ubuntu/.local/share/daedalus`
        // explicitly.
        cfg.daedalus_home = daedalus::Paths::global().user_data_root();
    }

    // §Phase 14: load the demo's system_prompt.txt (or override file)
    // and pass to AgentConfig as cfg.system_prompt. The library treats
    // this as opaque text and prepends it verbatim to the composed
    // system prompt (see Agent.md Phase 14).
    cfg.system_prompt       = resolve_system_prompt(
                                  args.system_prompt_file, target_path);

    auto sink  = std::make_shared<daedalus::ConsoleAgentSink>();
    daedalus::Agent agent(std::move(cfg), sink);

    // §TOOL-TRIM (prompt.txt): register the non-core example tools
    // (file, git, search, pdf, system-info, background, network) so the
    // CLI has the full feature set it had before the trim.
    register_cli_tools();

    // §Demo fix 2026-07-14: register the agent with the SIGINT
    // handler so Ctrl+C (and SIGTERM) cancel any in-flight turn.
    // Cleared again before `agent.shutdown()` below so the handler
    // can't fire on a destroyed agent.
    g_agent_for_signal.store(&agent, std::memory_order_release);

    try {
        agent.initialize();
        // §CRASH-4 (2026-07-16): install async-signal-safe crash
        // handlers now that Messages has been initialised. From here
        // on, SIGSEGV/SIGABRT/SIGFPE/SIGILL will write the current
        // messages to messages.json.crash before re-raising.
        agent.install_crash_handlers();

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

        // §Demo fix: flush banner + any initialization logs before the
        // raw-mode watcher takes over, so the first "\n> " prompt is not
        // visually interleaved with startup output.
        std::cout << "\n" << std::flush;
        std::cerr << std::flush;

        // §Demo fix 2026-07-14: raw-mode stdin watcher.
        //
        // Replaces the old `std::getline` loop. Two improvements:
        //   1. Reads in raw mode, so bare ESC is delivered to us
        //      (std::getline in cooked mode can't see lone ESC).
        //      On ESC we cancel the current turn and break the
        //      loop — "ESC exits the Agent Loop".
        //   2. Reads from a separate thread, so even mid-turn ESC
        //      is detected. While `agent.run_one_turn()` is
        //      blocking on the network, the watcher thread is
        //      reading stdin; pressing ESC fires `agent.cancel()`
        //      from that thread (LLMClient::cancel() is async-
        //      signal-safe in practice — see LLMClient.h:80).
        //
        // The watcher is RAII: its destructor restores terminal
        // mode and joins the thread.
        daedalus_cli::RawStdinWatcher watcher(agent, "\n> ");

        while (!g_shutting_down.load() && !watcher.esc_seen()) {
            // Single canonical site for the prompt — printed just before
            // reading, never after. The watcher handles its own input
            // echoing and does not print the prompt. The leading '\n'
            // ensures the prompt never appends to the previous line of
            // tool/agent output.
            std::cout << "\n> " << std::flush;
            auto line_opt = watcher.next_line();
            if (!line_opt) break;          // EOF / Ctrl+D / shutdown

            // §Demo fix 2026-07-14: a bare ESC arrived while we were
            // blocked in next_line(). esc_seen() is set BEFORE the
            // sentinel is pushed, so by the time we get here the
            // outer loop guard will also fire on the next iteration —
            // but we exit now to honour the user's intent immediately.
            if (watcher.esc_seen()) break;

            std::string line = std::move(*line_opt);

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
            if (line == "/providers") {
                auto providers = agent.list_providers();
                std::cout << "Configured providers:\n";
                if (providers.empty()) std::cout << "  (none)\n";
                else for (const auto& p : providers)
                    std::cout << "  " << p << "\n";
                continue;
            }
            if (line.rfind("/provider ", 0) == 0) {
                std::string provider = line.substr(10);
                if (provider.empty()) {
                    std::cout << "Usage: /provider <name>\n";
                } else if (agent.switch_provider(provider)) {
                    std::cout << "[PROVIDER] Switched to " << provider << "\n";
                } else {
                    std::cout << "[PROVIDER] Failed to switch to " << provider
                              << " (no configured models for this provider)\n";
                }
                continue;
            }
            if (line == "/models") {
                auto result = agent.discover_provider_models();
                if (!result.success) {
                    std::cout << "[MODELS] Discovery failed: " << result.error_message << "\n";
                } else if (result.models.empty()) {
                    std::cout << "[MODELS] No models returned by provider\n";
                } else {
                    std::cout << "Provider models:\n";
                    for (const auto& m : result.models) {
                        std::cout << "  " << m.id;
                        if (m.context_length > 0)
                            std::cout << " [ctx=" << m.context_length << "]";
                        if (m.supports_vision) std::cout << " [vision]";
                        if (m.supports_reasoning) std::cout << " [reasoning]";
                        std::cout << "\n";
                    }
                }
                continue;
            }
            if (line.rfind("/model ", 0) == 0) {
                std::string name = line.substr(7);
                if (name.empty()) {
                    std::cout << "Usage: /model <name>\n";
                } else if (agent.switch_model(name)) {
                    std::cout << "[MODEL] Switched to " << name << "\n";
                } else {
                    std::cout << "[MODEL] Failed to switch to " << name
                              << " (not a configured model)\n";
                }
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
            if (line == "/usage") {
                print_usage(std::cout, agent.stats());
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
            // The next iteration prints the prompt before next_line().
        }

        // Distinguish "user pressed ESC" from "user typed /quit" so
        // we can show a friendlier goodbye in the ESC case.
        if (watcher.esc_seen()) {
            std::cout << "Cancelled. Goodbye!\n";
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
        g_agent_for_signal.store(nullptr, std::memory_order_release);
        return 1;
    }

    // Clear the signal-handler's pointer before tearing down the
    // agent so it can't fire cancel() on a half-destroyed object.
    g_agent_for_signal.store(nullptr, std::memory_order_release);
    agent.shutdown();
    return 0;
}
