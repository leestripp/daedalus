// SPDX-License-Identifier: BSL-1.0
// src/main.cpp
//
// icarus — CLI AI coding agent built on the daedalus library.
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
#include <cctype>
#include <filesystem>
#include <memory>
#include <atomic>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <chrono>
#include <ctime>
#include <algorithm>
#include <tuple>
#include <iterator>
#include <thread>

#include <daedalus.h>
#include <daedalus/default_paths.h>
#include <daedalus/paths.h>
#include <daedalus/project_paths.h>
#include <daedalus/settings.h>

// §Settings TUI (2026-07-30): the daedalus settings-demo sources are
// compiled into this binary (see CMakeLists.txt); RunApp() drives the
// fullscreen FTXUI editor for the bare /settings command.
#include "SettingsApp.h"

#include <optional>

#include "ConsoleAgentSink.h"
#include "util.h"
#include "banner.h"
#include "migrate.h"
#include "a2a_tools.h"
#include "a2a_agent.h"
#include "ui/AnsiRenderer.h"
#include "ui/InputQueue.h"
#include "ui/InputReader.h"
#include "ui/InputThread.h"

#include <nlohmann/json.hpp>

// Defined by CMakeLists.txt from the latest git tag; fallback for
// non-CMake compilation of this TU.
#ifndef ICARUS_VERSION
#define ICARUS_VERSION "dev"
#endif

namespace fs = std::filesystem;

// Mask an API key for display: show first/last 2 chars if long enough.
static std::string mask_api_key(std::string_view key) {
    if (key.empty()) return "(none)";
    if (key.size() <= 6) return "****";
    return std::string(key.substr(0, 2)) + "****" + std::string(key.substr(key.size() - 2));
}

// All tables are built through icarus::ui::DrawTable (the fluent front end
// for make_table) so rendering behaviour stays consistent everywhere.

static void print_providers_table(const std::vector<daedalus::ProviderConfig>& providers,
                                  const std::string& current_provider,
                                  const icarus::ui::RenderConfig& cfg) {
    icarus::ui::DrawTable t(cfg);
    t.column("#", 3, 5)
     .column("Name", 8, 20)
     .column("Host", 10, 40)
     .column("Plugin", 8, 24)
     .column("Timeout", 7, 10)
     .column("Key", 6, 16);
    for (std::size_t i = 0; i < providers.size(); ++i) {
        const auto& p = providers[i];
        std::string name = p.name;
        if (p.name == current_provider) name += " *";
        // §ProviderPlugins (2026-07-31): show the effective plugin — the
        // explicit row value, or the auto-detected match for name/host.
        std::string plugin = p.plugin;
        if (plugin.empty()) {
            if (auto resolved = daedalus::ProviderRegistry::resolve(p.name, p.host)) {
                plugin = "(auto: " + resolved->name() + ")";
            }
        }
        t.row({std::to_string(i + 1), name, p.host, plugin, std::to_string(p.timeout_seconds), mask_api_key(p.api_key)});
    }
    t.print();
}

static void print_configured_models_table(const daedalus::Agent& agent,
                                          const std::vector<daedalus::ModelInfo>& models,
                                          const icarus::ui::RenderConfig& cfg) {
    (void)agent;
    icarus::ui::DrawTable t(cfg);
    t.column("#", 3, 5)
     .column("Name", 8, 20)
     .column("Provider", 8, 16)
     .column("Model ID", 10, 28)
     .column("Ctx", 5, 10)
     .column("Temp", 6, 8);
    for (std::size_t i = 0; i < models.size(); ++i) {
        const auto& m = models[i];
        // Trim trailing-zero noise ("0.700000" → "0.7").
        std::string temp = std::to_string(m.temperature);
        while (temp.size() > 1 && temp.back() == '0') temp.pop_back();
        if (!temp.empty() && temp.back() == '.') temp.pop_back();
        t.row({
            std::to_string(i + 1),
            m.name,
            m.provider,
            m.model_id,
            m.max_context_size > 0 ? std::to_string(m.max_context_size) : "(default)",
            temp
        });
    }
    t.print();
}

// Helper to join strings with a delimiter.
static std::string join_strings(const std::vector<std::string>& parts, std::string_view sep) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out += sep;
        out += parts[i];
    }
    return out;
}

static void print_discovered_models_table(const std::vector<daedalus::DiscoveredModel>& models,
                                          const daedalus::ModelInfo& active,
                                          const std::string& current_provider,
                                          const icarus::ui::RenderConfig& cfg) {
    icarus::ui::DrawTable t(cfg);
    t.column("#", 3, 5)
     .column("Model ID", 10, 32)
     .column("Context", 7, 10)
     .column("Features", 10, 24);
    for (std::size_t i = 0; i < models.size(); ++i) {
        const auto& m = models[i];
        std::vector<std::string> feats;
        if (m.supports_vision) feats.push_back("vision");
        if (m.supports_reasoning) feats.push_back("reasoning");
        std::string features = feats.empty() ? "-" : join_strings(feats, ",");
        bool is_current = (m.id == active.model_id && active.provider == current_provider);
        if (is_current) features += " [current]";
        t.row({
            std::to_string(i + 1),
            m.id,
            m.context_length > 0 ? std::to_string(m.context_length) : "-",
            features
        });
    }
    t.print();
}

static void print_skills_table(const std::vector<daedalus::SkillInfo>& enabled_skills,
                               const std::vector<std::string>& disabled_names,
                               const icarus::ui::RenderConfig& cfg) {
    icarus::ui::DrawTable t(cfg);
    t.column("#", 2, 4)
     .column("Name", 8, 20)
     .column("Enabled", 7, 9)
     .column("Description", 12, 40, true)
     .column("Tags", 6, 24);
    std::size_t idx = 1;
    for (const auto& s : enabled_skills) {
        t.row({std::to_string(idx++), s.name, "yes", s.description,
               join_strings(s.tags, ",")});
    }
    for (const auto& name : disabled_names) {
        t.row({std::to_string(idx++), name, "no", "", ""});
    }
    t.print();
}

static void print_tools_table(const std::vector<daedalus::ToolInfo>& tools,
                              const daedalus::Agent& agent,
                              const icarus::ui::RenderConfig& cfg) {
    icarus::ui::DrawTable t(cfg);
    t.column("#", 2, 4)
     .column("Name", 8, 24)
     .column("Enabled", 7, 9)
     .column("Description", 12, 48, true);
    for (std::size_t i = 0; i < tools.size(); ++i) {
        const auto& tool = tools[i];
        t.row({
            std::to_string(i + 1),
            tool.name,
            agent.is_tool_enabled(tool.name) ? "yes" : "no",
            tool.description});
    }
    t.print();
}

static void print_mcp_servers_table(const std::vector<std::string>& servers,
                                    const daedalus::Agent& agent,
                                    const icarus::ui::RenderConfig& cfg) {
    icarus::ui::DrawTable t(cfg);
    t.column("#", 2, 4)
     .column("Name", 8, 40)
     .column("Enabled", 7, 9);
    for (std::size_t i = 0; i < servers.size(); ++i) {
        t.row({
            std::to_string(i + 1),
            servers[i],
            agent.is_mcp_server_enabled(servers[i]) ? "yes" : "no"});
    }
    t.print();
}

// Print every filesystem path icarus/daedalus/OmniNet uses: user-tier XDG
// locations, OmniNet config/data, icarus input history, and the per-project
// (local) agent directories.
static void print_paths_table(const daedalus::AgentConfig& cfg_view,
                              const std::string& agent_name,
                              const fs::path& history_file,
                              const icarus::ui::RenderConfig& cfg) {
    const daedalus::Paths& P = daedalus::Paths::global();
    daedalus::ProjectPaths pp(cfg_view.project_root, agent_name);
    const fs::path agent_dir = daedalus::resolve_agent_dir(cfg_view.project_root, agent_name);

    icarus::ui::DrawTable t(cfg);
    t.column("Scope", 7, 10)
     .column("Name", 10, 20)
     .column("Path", 20, 80, true);
    t.row({"daedalus", "config root",    P.user_config_root().string()});
    t.row({"daedalus", "config file",    P.config_file().string()});
    t.row({"daedalus", "data root",      cfg_view.daedalus_home.string()});
    t.row({"daedalus", "database",       (cfg_view.daedalus_home / "daedalus.db").string()});
    t.row({"daedalus", "state root",     P.user_state_root().string()});
    t.row({"daedalus", "cache root",     P.user_cache_root().string()});
    t.row({"daedalus", "log file",       P.log_file().string()});
    t.row({"daedalus", "skills (global)", P.global_skills_dir().string()});
    t.row({"daedalus", "mcp servers",    P.mcp_servers_file().string()});
    t.row({"daedalus", "mcp env",        P.mcp_env_file().string()});
    t.row({"daedalus", "response cache", P.response_cache_dir().string()});
    t.row({"omninet",  "config file",    P.omninet_config_file().string()});
    t.row({"omninet",  "data dir",       P.omninet_data_dir().string()});
    t.row({"icarus",   "input history",  history_file.empty() ? "(not persisted)" : history_file.string()});
    t.row({"project",  "project root",   cfg_view.project_root.string()});
    t.row({"project",  "agent dir",      agent_dir.string()});
    t.row({"project",  "messages",       (agent_dir / "messages.json").string()});
    t.row({"project",  "memory dir",     pp.memory_dir().string()});
    t.row({"project",  "memory file",    pp.memory_file().string()});
    t.row({"project",  "sessions dir",   pp.sessions_dir().string()});
    t.row({"project",  "skills (active)", pp.skills_active_dir().string()});
    t.row({"project",  "session log",    (agent_dir / "logs.txt").string()});
    t.print();
}

// Resolve a /skills, /tools, or /mcp-servers argument that may be either a
// name or a 1-based table index (with optional '#' prefix).
static std::optional<std::string> resolve_skill_name(const daedalus::Agent& agent, std::string_view token) {
    if (token.empty()) return std::nullopt;
    std::string t(token);
    if (t.front() == '#') t = t.substr(1);
    if (!t.empty() && std::all_of(t.begin(), t.end(),
                                  [](unsigned char c) { return std::isdigit(c); })) {
        std::size_t idx = 0;
        try {
            idx = std::stoull(t);
        } catch (...) {
            return std::nullopt;
        }
        if (idx == 0) return std::nullopt;
        auto skills = agent.list_skills();
        if (idx <= skills.size()) return skills[idx - 1].name;
        auto disabled = agent.disabled_skills();
        std::size_t disabled_idx = idx - skills.size() - 1;
        if (disabled_idx < disabled.size()) return disabled[disabled_idx];
        return std::nullopt;
    }
    return std::string(token);
}

static std::optional<std::string> resolve_tool_name(const daedalus::Agent& agent, std::string_view token) {
    if (token.empty()) return std::nullopt;
    std::string t(token);
    if (t.front() == '#') t = t.substr(1);
    if (!t.empty() && std::all_of(t.begin(), t.end(),
                                  [](unsigned char c) { return std::isdigit(c); })) {
        std::size_t idx = 0;
        try {
            idx = std::stoull(t);
        } catch (...) {
            return std::nullopt;
        }
        if (idx == 0) return std::nullopt;
        auto tools = agent.list_tools();
        if (idx > tools.size()) return std::nullopt;
        return tools[idx - 1].name;
    }
    return std::string(token);
}

static std::optional<std::string> resolve_mcp_server_name(const daedalus::Agent& agent, std::string_view token) {
    if (token.empty()) return std::nullopt;
    std::string t(token);
    if (t.front() == '#') t = t.substr(1);
    if (!t.empty() && std::all_of(t.begin(), t.end(),
                                  [](unsigned char c) { return std::isdigit(c); })) {
        std::size_t idx = 0;
        try {
            idx = std::stoull(t);
        } catch (...) {
            return std::nullopt;
        }
        if (idx == 0) return std::nullopt;
        auto servers = agent.mcp_configured_servers();
        if (idx > servers.size()) return std::nullopt;
        return servers[idx - 1];
    }
    return std::string(token);
}

// Format a JSON todo result into a one-line status message.
static std::string todo_status(const nlohmann::json& result, std::string_view verb) {
    if (result.contains("error")) {
        return std::string("[TODO] Error ") + std::string(verb) + ": " + result["error"].get<std::string>();
    }
    if (result.contains("id")) {
        return std::string("[TODO] ") + std::string(verb) + " #" + std::to_string(result["id"].get<int>());
    }
    return std::string("[TODO] ") + std::string(verb);
}

// ---------------------------------------------------------------------------

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
            // §version 2026-07-31 / §Monorepo 2026-09-24: print icarus's
            // version (latest git tag via the ICARUS_VERSION compile
            // definition). Since the monorepo consolidation every component
            // shares ONE tag stream, so the library version is always
            // identical and no longer printed separately.
            std::cout << "icarus " << ICARUS_VERSION << "\n";
            return 0;
        }
    }

    CliArgs args = parse_cli_args(argc, argv);

    // --a2a-agent mode: run as an A2A agent HTTP server (for IDE integration).
    // Handled before the normal REPL path so the daemon never touches the
    // interactive terminal.
    if (args.a2a_agent_mode) {
        if (args.acp_agent_deprecated) {
            std::cerr << "[warn] --acp-agent is deprecated; use --a2a-agent\n";
        }
        return a2a::run_a2a_agent(args);
    }

    if (args.acp_url_deprecated) {
        std::cerr << "[warn] --acp-url is deprecated; use --a2a-url\n";
    }
    if (args.acp_agent_deprecated) {
        std::cerr << "[warn] --acp-agent is deprecated; use --a2a-agent\n";
    }

    // §Task B4 (CodeMax.md §16): --discover-models NAME short-circuit
    // before any Agent construction (mirrors the --model-list pattern).
    // Queries the provider's /v1/models endpoint via the configured
    // model's host/key.
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
    // §BUG-006 (daedalus 0.8.7): auto_approve is std::optional<bool> —
    // setting it (even to false) makes the host authoritative and ignores
    // the SettingsStore row. Only set it when --auto was actually passed,
    // so a persisted store setting still applies otherwise.
    if (args.auto_mode) {
        cfg.auto_approve = true;
    }
    cfg.memory_enabled       = !args.no_memory;
    cfg.clear_memory         = args.clear_memory_flag;
    cfg.worker_threads       = args.thread_count_override;
    cfg.max_agent_iterations = 600;
    // Experimental worker → verifier double agent loop (shared library).
    cfg.verification_enabled = true;
    cfg.stream              = args.stream_enabled;
    cfg.daedalus_home        = args.daedalus_home;
    cfg.agent_name          = args.agent_name;
    // §UA 2026-07-29: identify ourselves to providers. daedalus sends this
    // as the single User-Agent on every LLM request (empty would fall back
    // to its "daedalus/1.0" default). ICARUS_VERSION comes from the latest
    // git tag via CMakeLists.txt ("dev" when untagged).
    cfg.user_agent          = "daedalus-icarus/" ICARUS_VERSION;

    // §ContextCache (2026-07-31): stable per-project session id. daedalus
    // uses it as the server-side prompt-cache key (e.g. Moonshot
    // prompt_cache_key) whenever no agent_name is set; a value that
    // survives restarts keeps the cache warm across sessions.
    {
        std::ostringstream sess;
        sess << "icarus-" << std::hex
             << std::hash<std::string>{}(fs::absolute(target_path).string());
        cfg.session_id = sess.str();
    }

    // §Plan.md migration (2026-07-13): when the host doesn't pass
    // explicit paths, default to the XDG-compliant locations (NOT the
    // legacy `/home/ubuntu/.daedalus/` flat dir). The library's
    // `Paths::global()` resolves the same env vars as the rest of the
    // library, so this stays consistent with `--migrate-data`, the
    // deprecation warning, and `docs/storage.md`.
    if (args.skills_dirs.empty()) {
        cfg.skills_dirs = std::vector<fs::path>{
            daedalus::Paths::global().global_skills_dir() };
    } else {
        cfg.skills_dirs = args.skills_dirs;
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

    // §Phase 14: load the system prompt from --system-prompt or AGENTS.md
    // and pass it to AgentConfig as cfg.system_prompt. The library treats
    // this as opaque text and prepends it verbatim to the composed
    // system prompt (see Agent.md Phase 14).
    cfg.system_prompt       = resolve_system_prompt(
                                  args.system_prompt_file, target_path);

    icarus::ui::RenderConfig render_cfg = icarus::ui::RenderConfig::detect(
        args.no_color, args.no_unicode, args.no_hud);
    auto sink  = std::make_shared<daedalus::ConsoleAgentSink>(render_cfg);

    // Compute the per-session data directory as early as possible so
    // set_log_path() is called before agent.initialize() fires any
    // on_log messages (e.g. the daedalus startup banner).
    auto data_dir = daedalus::resolve_agent_dir(target_path, args.agent_name);
    sink->set_log_path((data_dir / "logs.txt").string());

    // §Settings TUI (2026-07-30): capture the resolved home before cfg is
    // moved into the Agent — bare /settings builds a SettingsStore against
    // the same DB for the embedded settings TUI.
    const fs::path settings_home = cfg.daedalus_home;

    // §ThinkingLevel (2026-08-01): daedalus does not persist the thinking
    // level — icarus keeps it in a kv_settings row (written by /level and
    // the settings TUI Agent tab) and applies it at startup.
    {
        auto store = daedalus::SettingsStore::for_home(settings_home);
        const std::string lvl = store.get_string("thinking_level");
        if (!lvl.empty())
            cfg.thinking_level = daedalus::thinking_level_from_string(lvl);
    }

    daedalus::Agent agent(std::move(cfg), sink);

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
        // Chain our terminal-restoring handler in front of daedalus's
        // crash handlers so a SIGSEGV/abort/fatal-fp/illegal-instruction
        // does not leave the user's TTY in raw mode.
        install_terminal_restoring_crash_handlers();

        // Register the A2A tool so the agent can interact with remote A2A agents.
        {
            std::string a2a_url = args.a2a_url.empty()
                ? "http://localhost:8000"
                : args.a2a_url;
            agent.register_tool(std::make_unique<a2a::A2aTool>(a2a_url));
        }

        if (args.model_list_only) {
            auto models = agent.list_models();
            std::cout << "Available models:\n";
            if (models.empty()) std::cout << "  (none configured)\n";
            else for (const auto& m : models)
                std::cout << "  " << m.name << " (provider=" << m.provider
                          << ", model_id=" << m.model_id << ")\n";
            agent.shutdown();
            return 0;
        }

        if (args.clear_memory_flag) {
            agent.shutdown();
            return 0;
        }

        auto cfg_view       = agent.config();
        auto agent_data_dir = daedalus::resolve_agent_dir(cfg_view.project_root, args.agent_name);
        // §Task C3 (CodeMax.md §16): prefer agent.agent_name() (the
        // validated/effective per-agent namespace) over the raw arg.
        std::string effective_agent_name(agent.agent_name());
        if (effective_agent_name.empty()) effective_agent_name = args.agent_name;
        print_banner(agent, cfg_view,
                     args.daedalus_home.empty() ? cfg_view.daedalus_home.string() : args.daedalus_home.string(),
                     target_path.string(),
                     cfg_view.session_id,
                     effective_agent_name,
                     agent_data_dir.string(),
                     render_cfg);

        // Flush banner + any initialization logs before the input loop
        // takes over, so the first prompt is not visually interleaved with
        // startup output.
        std::cout << std::flush;
        std::cerr << std::flush;

        // Provider / model selection state for interactive indexed commands.
        std::string current_provider = agent.active_model().provider;
        std::vector<daedalus::ProviderConfig> last_providers;
        std::vector<daedalus::DiscoveredModel> last_discovered_models;
        std::vector<daedalus::ModelInfo> last_configured_models;
        std::string last_user_message; // For /continue.

        // §Task 3-4 (CodeMax.md §16.5): install the linenoise-backed
        // InputReader. Its approval reader is injected into the sink
        // below, after input_thread exists — the wrapper pauses the
        // background reader so stdin has exactly one reader while the
        // approval prompt is up.
        icarus::ui::InputReader::Config icfg;
        if (args.daedalus_home.empty()) {
            // No explicit --home → default to XDG, falling back to
            // ~/.local/share. Matches the convention used for
            // daedalus_home itself (see default_paths.cpp).
            const char* xdg  = std::getenv("XDG_DATA_HOME");
            const char* home = std::getenv("HOME");
            if (xdg && *xdg) {
                icfg.history_file = fs::path(xdg) / "icarus"
                                                / "input_history.txt";
            } else if (home && *home) {
                icfg.history_file = fs::path(home) / ".local" / "share"
                                                / "icarus"
                                                / "input_history.txt";
            }
            // Both env vars unset → empty path → no persistence.
        } else {
            icfg.history_file = fs::path(args.daedalus_home)
                                     / "input_history.txt";
        }
        icfg.history_max  = 1000;
        icfg.multiline    = true;
        icfg.header_render = render_cfg;   // share the runtime-detected capabilities
        icfg.header_hint   = "Type /help for available commands";
        icfg.show_header   = true;
        icarus::ui::InputReader input(icfg);

        // §prompt.txt: the visual frame (hint line + horizontal rule) is
        // rendered above the input line by InputReader::print_header().
        // The background input thread uses the plain "> " prompt itself.

        // Run the input reader on a dedicated thread. User input is cached
        // in InputQueue and pulled by the Agent Loop below. ESC (empty line)
        // invokes the cancel callback so an in-flight agent turn returns
        // promptly without killing the process.
        icarus::ui::InputQueue input_queue;
        icarus::ui::InputThread input_thread(
            input, input_queue, g_shutting_down,
            [&agent]() {
                // Mirror the SIGINT handler: cancel any in-flight turn.
                // This is safe to call from the input thread while the main
                // thread is blocked inside agent.run_one_turn().
                agent.cancel();
            },
            /*start_paused=*/true);

        // §Approval fix 2026-07-29: inject the approval reader into the
        // sink (it uses this std::function in request_command_approval()
        // instead of calling std::getline directly). The approval prompt
        // fires mid-turn while the InputThread is resumed (so ESC can
        // cancel the turn) and its pump thread is actively read()ing
        // stdin — a concurrent linenoise() call here would race it for
        // the user's answer and lose, making every approval come back
        // empty (refused). Park the reader thread first (pause() blocks
        // until parked, which also joins the pump), read the answer
        // single-line, then resume. The /settings handler uses the same
        // park/resume pattern around the settings TUI.
        sink->set_approval_reader(
            [&input, &input_thread](std::string_view p)
                -> std::optional<std::string> {
                input_thread.pause();
                struct ResumeGuard {
                    icarus::ui::InputThread* t;
                    ~ResumeGuard() { if (t) t->resume(); }
                } resume_guard{&input_thread};
                return input.read_line_single(p);
            });

        // §BUG-008 (daedalus 0.8.7): let the sink's 'a' answer drive
        // Agent::set_auto_approve() — the library's authoritative runtime
        // auto-approve. Bypassed commands then surface to the user via the
        // sink's on_command_auto_approved() instead of silently running.
        sink->set_auto_approve_setter(
            [&agent](bool on) { agent.set_auto_approve(on); });

        sink->set_turn_request_handler([&input_queue] {
            // The completion is already in the Agent's normal or steer
            // queue. Wake pop() only; main consumes it without a duplicate.
            input_queue.wake();
        });

        bool header_printed = false;
        while (!g_shutting_down.load()) {
            if (agent.pending_count() > 0 || agent.pending_steer_count() > 0) {
                header_printed = false;
                input_thread.resume();
                sink->start_agent_turn();
                agent.run_one_turn();
                sink->stop_agent_turn();
                continue;
            }

            // Print the input header only once when we are about to block for
            // input. The queue pop uses a short timeout so the loop remains
            // responsive to g_shutting_down; without the guard that timeout
            // would re-print the header every 50 ms and flood the terminal.
            if (!header_printed && input_queue.empty()) {
                // Park the reader so the header and the next "> " prompt are
                // drawn as one coherent block after any output has finished.
                input_thread.pause();
                input.print_header();
                // Bell: the REPL is about to block for user input, so an
                // away user knows their attention is needed. TTY only —
                // never leak BEL into piped output.
                if (::isatty(STDOUT_FILENO)) {
                    std::cout << '\a' << std::flush;
                }
                input_thread.resume();
                header_printed = true;
            }

            auto item = input_queue.pop(std::chrono::milliseconds(50));
            if (!item) {
                // Timeout: loop so we still respect g_shutting_down.
                continue;
            }

            // Input arrived; allow the next idle cycle to draw a fresh header.
            header_printed = false;

            if (item->type == icarus::ui::InputItemType::Eof) {
                // EOF / Ctrl+D / signal-interrupted read.
                std::cout << "\n";
                break;
            }

            if (item->type == icarus::ui::InputItemType::Cancel) {
                // Cancel callback was already invoked by the input thread.
                // When idle this is a no-op; when a turn was active the
                // agent has already been asked to stop.
                continue;
            }

            std::string line = std::move(item->text);
            while (!line.empty() && std::isspace((unsigned char)line.front())) line.erase(line.begin());
            while (!line.empty() && std::isspace((unsigned char)line.back()))  line.pop_back();
            if (line.empty()) continue;

            // Record every submitted line for up/down history recall.
            // Skip non-TTY (piped) input so unattended runs don't pollute
            // the interactive history file.
            if (::isatty(STDIN_FILENO)) {
                icarus::ui::InputReader::add_history(line);
            }

            // -------- In-session commands --------
            if (line == "/quit" || line == "/exit") {
                std::cout << "Exiting. Goodbye!\n";
                break;
            }
            if (line == "/help")   { print_help(argv[0]); continue; }
            if (line == "/init") {
                // Enqueue the instruction so the Agent Loop treats it like
                // normal user input. This keeps /init cancellable via ESC and
                // avoids duplicating the submit/run logic here.
                input_queue.push({icarus::ui::InputItemType::Message,
                    "Please analyze this project and create or update the "
                    "AGENTS.md file. The file should include: project overview, "
                    "tech stack, build and test commands, project structure, "
                    "code style and conventions, and any other relevant "
                    "information for AI coding agents. First explore the "
                    "codebase (look at CMakeLists.txt, README, source files, "
                    "etc.), then write a comprehensive AGENTS.md using the "
                    "execute_bash tool."});
                continue;
            }
            if (line == "/clear") {
                agent.clear_memory();
                last_user_message.clear();
                std::cout << "[CLEAR] Session memory cleared\n";
                continue;
            }
            if (line == "/compress") {
                // §AutoCompress (2026-08-01): daedalus compress_context() is
                // synchronous when no turn is running — on_compaction()
                // fires on this thread and renders the Compact box. With a
                // turn running it defers to the end of the agent cycle.
                std::cout << "[COMPRESS] Compressing conversation context...\n";
                sink->start_agent_turn();  // spinner during the summary LLM call
                (void)agent.compress_context();  // result unused: fire-and-forget
                sink->stop_agent_turn();
                continue;
            }
            // Provider management commands.
            auto parse_one_based_index = [](const std::string& s, std::size_t max) -> std::optional<std::size_t> {
                if (s.empty()) return std::nullopt;
                try {
                    std::size_t pos = 0;
                    int idx = std::stoi(s, &pos);
                    if (pos == s.size() && idx > 0 && static_cast<std::size_t>(idx) <= max) {
                        return static_cast<std::size_t>(idx - 1);  // 1-based -> 0-based
                    }
                } catch (...) {}
                return std::nullopt;
            };
            if (line == "/providers") {
                last_providers = agent.list_providers_detailed();
                if (last_providers.empty()) {
                    std::cout << "No providers configured.\n";
                } else {
                    print_providers_table(last_providers,
                        current_provider.empty() ? agent.active_model().provider : current_provider,
                        render_cfg);
                }
                continue;
            }
            if (line.rfind("/provider-add ", 0) == 0) {
                // §ProviderPlugins (2026-07-31): /provider-add <name> <host> <api_key|-> [plugin]
                // "-" = no API key (auth-less providers like LM Studio);
                // plugin is optional — auto-detected from the host when omitted.
                std::istringstream iss(line.substr(14));
                std::string name, host, api_key, plugin;
                iss >> name >> host >> api_key >> plugin;
                if (api_key == "-") api_key.clear();
                if (name.empty() || host.empty()) {
                    std::cout << "Usage: /provider-add <name> <host> <api_key|-> [plugin]\n";
                } else if (!plugin.empty() &&
                           !daedalus::ProviderRegistry::plugin_for_name(plugin)) {
                    std::cout << "[PROVIDER] Unknown plugin '" << plugin
                              << "' (registered: ";
                    auto all = daedalus::ProviderRegistry::all();
                    for (std::size_t i = 0; i < all.size(); ++i) {
                        std::cout << (i ? ", " : "") << all[i]->name();
                    }
                    std::cout << ")\n";
                } else {
                    daedalus::ProviderConfig p;
                    p.name = name;
                    p.host = host;
                    p.api_key = api_key;
                    p.plugin = plugin;
                    if (agent.upsert_provider(p)) {
                        auto resolved = daedalus::ProviderRegistry::resolve(p.name, p.host);
                        const std::string effective =
                            plugin.empty()
                                ? (resolved ? resolved->name() + " [auto]" : "?")
                                : plugin;
                        std::cout << "[PROVIDER] Added/updated " << name
                                  << " (plugin: " << effective << ")\n";
                    } else {
                        std::cout << "[PROVIDER] Failed to add " << name << "\n";
                    }
                }
                continue;
            }
            if (line.rfind("/provider-delete ", 0) == 0) {
                std::string arg = line.substr(17);
                if (arg.empty()) {
                    std::cout << "Usage: /provider-delete <name|#>\n";
                    continue;
                }
                if (last_providers.empty()) {
                    last_providers = agent.list_providers_detailed();
                }
                std::string name = arg;
                if (auto idx = parse_one_based_index(arg, last_providers.size())) {
                    name = last_providers[*idx].name;
                }
                if (agent.delete_provider(name)) {
                    if (current_provider == name) current_provider.clear();
                    std::cout << "[PROVIDER] Deleted " << name << "\n";
                } else {
                    std::cout << "[PROVIDER] Failed to delete " << name << "\n";
                }
                continue;
            }
            if (line.rfind("/provider ", 0) == 0) {
                std::string arg = line.substr(10);
                if (arg.empty()) {
                    std::cout << "Usage: /provider <name|#>\n";
                    continue;
                }
                if (last_providers.empty()) {
                    last_providers = agent.list_providers_detailed();
                }
                std::string provider_name = arg;
                if (auto idx = parse_one_based_index(arg, last_providers.size())) {
                    provider_name = last_providers[*idx].name;
                }
                if (agent.switch_provider(provider_name)) {
                    current_provider = provider_name;
                    last_discovered_models.clear();
                    std::cout << "[PROVIDER] Switched to " << provider_name << "\n";
                } else {
                    std::cout << "[PROVIDER] Failed to switch to " << provider_name
                              << " (no configured models for this provider)\n";
                }
                continue;
            }
            // Model management commands.
            auto refresh_discovered_models = [&]() {
                std::string provider_for_discovery = current_provider;
                if (provider_for_discovery.empty()) {
                    provider_for_discovery = agent.active_model().provider;
                }
                // Only switch when the provider actually changes —
                // switch_provider() logs "Switched to ..." even for a
                // no-op switch, which is confusing during /models.
                if (!provider_for_discovery.empty() &&
                    provider_for_discovery != agent.active_model().provider &&
                    agent.switch_provider(provider_for_discovery)) {
                    current_provider = provider_for_discovery;
                }
                auto result = agent.discover_provider_models();
                last_discovered_models = result.models;
                return result;
            };
            if (line == "/models --configured" || line.rfind("/models --configured ", 0) == 0) {
                // §Provider-scoped models (2026-08-01): optional provider
                // filter — /models --configured <provider> lists only that
                // provider's models; bare lists all providers.
                std::string filter = line.size() > 20 ? line.substr(21) : "";
                while (!filter.empty() && std::isspace(static_cast<unsigned char>(filter.back()))) filter.pop_back();
                while (!filter.empty() && std::isspace(static_cast<unsigned char>(filter.front()))) filter.erase(filter.begin());
                last_configured_models = agent.list_models();
                if (!filter.empty()) {
                    std::erase_if(last_configured_models,
                        [&filter](const daedalus::ModelInfo& m) { return m.provider != filter; });
                }
                if (last_configured_models.empty()) {
                    if (filter.empty()) {
                        std::cout << "No models configured.\n";
                    } else {
                        std::cout << "No models configured for provider '"
                                  << filter << "'.\n";
                    }
                } else {
                    print_configured_models_table(agent, last_configured_models, render_cfg);
                }
                continue;
            }
            if (line == "/models") {
                auto result = refresh_discovered_models();
                if (!result.success) {
                    std::cout << "[MODELS] Discovery failed: " << result.error_message << "\n";
                    // §Provider-scoped models (2026-08-01): keep the fallback
                    // scoped to the provider discovery just ran against —
                    // dumping every configured model regardless of provider
                    // made the current-provider concept meaningless here.
                    const std::string fallback_provider = current_provider.empty()
                        ? agent.active_model().provider : current_provider;
                    last_configured_models = agent.list_models();
                    if (!fallback_provider.empty()) {
                        std::erase_if(last_configured_models,
                            [&fallback_provider](const daedalus::ModelInfo& m) {
                                return m.provider != fallback_provider;
                            });
                    }
                    if (!last_configured_models.empty()) {
                        std::cout << "(Showing configured models for provider '"
                                  << fallback_provider << "' instead.)\n";
                        print_configured_models_table(agent, last_configured_models, render_cfg);
                    } else {
                        std::cout << "(No configured models for provider '"
                                  << fallback_provider
                                  << "'. Tip: /models --configured lists all providers.)\n";
                    }
                } else if (result.models.empty()) {
                    std::cout << "[MODELS] No models returned by provider\n";
                } else {
                    print_discovered_models_table(result.models, agent.active_model(),
                                                  current_provider, render_cfg);
                }
                continue;
            }
            if (line.rfind("/model-add ", 0) == 0) {
                // /model-add <name> <provider> <model_id>
                std::istringstream iss(line.substr(11));
                std::string name, provider, model_id;
                iss >> name >> provider >> model_id;
                if (name.empty() || provider.empty() || model_id.empty()) {
                    std::cout << "Usage: /model-add <name> <provider> <model_id>\n";
                } else {
                    daedalus::ModelConfig m;
                    m.name = name;
                    m.provider = provider;
                    m.model_id = model_id;
                    if (agent.upsert_model(m)) {
                        std::cout << "[MODEL] Added/updated " << name << "\n";
                    } else {
                        std::cout << "[MODEL] Failed to add " << name
                                  << " (provider must exist)\n";
                    }
                }
                continue;
            }

            if (line.rfind("/model-delete ", 0) == 0) {
                std::string arg = line.substr(14);
                if (arg.empty()) {
                    std::cout << "Usage: /model-delete <name|#>\n";
                    continue;
                }
                if (last_configured_models.empty()) {
                    last_configured_models = agent.list_models();
                }
                std::string name = arg;
                if (auto idx = parse_one_based_index(arg, last_configured_models.size())) {
                    name = last_configured_models[*idx].name;
                }
                // daedalus's delete_model() returns true even when nothing
                // was deleted, so verify the name exists first.
                const auto& configured = agent.list_models();
                const bool exists = std::any_of(configured.begin(), configured.end(),
                                                [&](const auto& m) { return m.name == name; });
                if (!exists) {
                    std::cout << "[MODEL] Failed to delete " << name << " (not configured)\n";
                } else if (agent.delete_model(name)) {
                    std::cout << "[MODEL] Deleted " << name << "\n";
                    last_configured_models.clear();
                } else {
                    std::cout << "[MODEL] Failed to delete " << name << "\n";
                }
                continue;
            }

            if (line.rfind("/model ", 0) == 0) {
                std::string arg = line.substr(7);
                if (arg.empty()) {
                    std::cout << "Usage: /model <name|#>\n";
                    continue;
                }

                const bool numeric_arg = !arg.empty() &&
                    std::all_of(arg.begin(), arg.end(),
                                [](unsigned char c) { return std::isdigit(c); });

                // /model NAME switches to a configured model by name or
                // model_id — no provider discovery needed.
                if (!numeric_arg) {
                    if (agent.switch_model(arg)) {
                        std::cout << "[MODEL] Switched to " << arg << "\n";
                    } else {
                        const auto& models = agent.list_models();
                        auto it = std::find_if(models.begin(), models.end(),
                                               [&](const auto& cm) { return cm.model_id == arg; });
                        std::string matched_name = (it != models.end()) ? it->name : "";
                        if (!matched_name.empty() && agent.switch_model(matched_name)) {
                            std::cout << "[MODEL] Switched to " << matched_name << "\n";
                        } else {
                            std::cout << "[MODEL] Failed to switch to " << arg
                                      << " (not a configured model; run /models to discover)\n";
                        }
                    }
                    continue;
                }

                // /model # picks from the last discovered list (auto-discovers
                // if needed); falls back to configured models when the
                // provider has no discovery endpoint.
                if (last_discovered_models.empty()) {
                    refresh_discovered_models();
                }
                if (auto idx = parse_one_based_index(arg, last_discovered_models.size())) {
                    const auto& discovered = last_discovered_models[*idx];
                    std::string provider = current_provider.empty()
                        ? agent.active_model().provider
                        : current_provider;
                    if (provider.empty()) {
                        std::cout << "[MODEL] No provider selected; switch provider first with /provider\n";
                        continue;
                    }

                    // Reuse an existing configured entry with the same provider+model_id if one exists.
                    const auto& models = agent.list_models();
                    auto it = std::find_if(models.begin(), models.end(),
                                           [&](const auto& cm) {
                                               return cm.provider == provider && cm.model_id == discovered.id;
                                           });
                    std::string existing_name = (it != models.end()) ? it->name : "";

                    if (!existing_name.empty()) {
                        if (agent.switch_model(existing_name)) {
                            std::cout << "[MODEL] Switched to " << existing_name << "\n";
                        } else {
                            std::cout << "[MODEL] Failed to switch to " << existing_name << "\n";
                        }
                    } else {
                        daedalus::ModelConfig m;
                        m.name = discovered.id;
                        m.provider = provider;
                        m.model_id = discovered.id;
                        if (agent.upsert_model(m) && agent.switch_model(m.name)) {
                            std::cout << "[MODEL] Added + switched to " << m.name << "\n";
                        } else {
                            std::cout << "[MODEL] Failed to add/switch to " << discovered.id << "\n";
                        }
                    }
                    continue;
                }

                // Discovery unavailable (or index out of its range) — index
                // into the configured models instead.
                if (last_configured_models.empty()) {
                    last_configured_models = agent.list_models();
                }
                if (auto idx = parse_one_based_index(arg, last_configured_models.size())) {
                    const std::string& name = last_configured_models[*idx].name;
                    if (agent.switch_model(name)) {
                        std::cout << "[MODEL] Switched to " << name << "\n";
                    } else {
                        std::cout << "[MODEL] Failed to switch to " << name << "\n";
                    }
                } else {
                    std::cout << "[MODEL] No model at index " << arg
                              << " (discovery unavailable and no configured model at that index)\n";
                }
                continue;
            }
            if (line == "/skills") {
                auto skills = agent.list_skills();
                auto disabled = agent.disabled_skills();
                if (skills.empty() && disabled.empty()) {
                    std::cout << "No skills loaded.\n";
                } else {
                    print_skills_table(skills, disabled, render_cfg);
                }
                continue;
            }
            if (line.rfind("/skills ", 0) == 0) {
                std::istringstream iss(line.substr(8));
                std::string sub, token;
                iss >> sub >> token;
                if ((sub == "enable" || sub == "disable") && !token.empty()) {
                    bool enabled = (sub == "enable");
                    auto resolved = resolve_skill_name(agent, token);
                    if (!resolved) {
                        std::cout << "[SKILL] Unknown index '" << token << "'\n";
                    } else if (agent.set_skill_enabled(*resolved, enabled)) {
                        std::cout << "[SKILL] " << *resolved << " " << sub << "d\n";
                    } else {
                        std::cout << "[SKILL] Could not " << sub << " '" << *resolved << "'\n";
                    }
                } else {
                    std::cout << "Usage: /skills enable|disable <name|#>\n";
                }
                continue;
            }
            if (line == "/tools") {
                auto tools = agent.list_tools();
                if (tools.empty()) {
                    std::cout << "No tools registered.\n";
                } else {
                    print_tools_table(tools, agent, render_cfg);
                }
                continue;
            }
            if (line.rfind("/tools ", 0) == 0) {
                std::istringstream iss(line.substr(7));
                std::string sub, token;
                iss >> sub >> token;
                if ((sub == "enable" || sub == "disable") && !token.empty()) {
                    bool enabled = (sub == "enable");
                    auto resolved = resolve_tool_name(agent, token);
                    if (!resolved) {
                        std::cout << "[TOOL] Unknown index '" << token << "'\n";
                    } else if (agent.set_tool_enabled(*resolved, enabled)) {
                        std::cout << "[TOOL] " << *resolved << " " << sub << "d\n";
                    } else {
                        std::cout << "[TOOL] Could not " << sub << " '" << *resolved << "'\n";
                    }
                } else {
                    std::cout << "Usage: /tools enable|disable <name|#>\n";
                }
                continue;
            }
            // §Task C4 (CodeMax.md §16): /mcp-servers command.
            if (line == "/mcp-servers") {
                auto servers = agent.mcp_configured_servers();
                if (servers.empty()) {
                    std::cout << "No MCP servers configured.\n";
                } else {
                    print_mcp_servers_table(servers, agent, render_cfg);
                }
                continue;
            }
            if (line.rfind("/mcp-servers ", 0) == 0) {
                std::istringstream iss(line.substr(13));
                std::string sub, token;
                iss >> sub >> token;
                if ((sub == "enable" || sub == "disable") && !token.empty()) {
                    bool enabled = (sub == "enable");
                    auto resolved = resolve_mcp_server_name(agent, token);
                    if (!resolved) {
                        std::cout << "[MCP] Unknown index '" << token << "'\n";
                    } else if (agent.set_mcp_server_enabled(*resolved, enabled)) {
                        std::cout << "[MCP] " << *resolved << " " << sub << "d\n";
                    } else {
                        std::cout << "[MCP] Could not " << sub << " '" << *resolved << "'\n";
                    }
                } else {
                    std::cout << "Usage: /mcp-servers enable|disable <name|#>\n";
                }
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
                          << ", completion=" << st.completion_tokens;
                // §ContextCache (2026-07-31): prompt tokens served from the
                // provider's server-side context cache on the last response.
                if (st.cached_tokens > 0)
                    std::cout << ", cached=" << st.cached_tokens;
                std::cout << ")"
                          << " | ctx=" << st.total_tokens << "/" << st.max_context_size << "\n";
                continue;
            }
            if (line == "/state") {
                std::cout << "State: " << daedalus::to_string(agent.state()) << "\n";
                // §ProviderPlugins (2026-07-31): surface the resolved plugin
                // for the active provider (empty before the first LLM client).
                const std::string plugin = agent.active_provider_plugin();
                if (!plugin.empty())
                    std::cout << "Provider plugin: " << plugin << "\n";
                continue;
            }
            // §ThinkingLevel (2026-08-01): /levels lists the thinking levels
            // the active model honors; /level <name|#> selects one at runtime
            // and persists it (kv_settings row, re-applied at startup).
            if (line == "/levels") {
                const auto levels = agent.supported_thinking_levels();
                if (levels.empty()) {
                    std::cout << "The current model has no thinking-level "
                                 "control (provider default applies).\n";
                } else {
                    const auto active = agent.config().thinking_level;
                    icarus::ui::DrawTable t(render_cfg);
                    t.column("#", 3, 5)
                     .column("Level", 6, 12)
                     .column("Status", 6, 12);
                    // Default is always a valid selection (daedalus sends
                    // nothing, provider/model decides) — show it as row 0
                    // so on/off models like MiniMax-M3 display both states.
                    t.row({"0", "Default",
                           active == daedalus::ThinkingLevel::Default ? "[active]" : "-"});
                    for (std::size_t i = 0; i < levels.size(); ++i) {
                        t.row({
                            std::to_string(i + 1),
                            daedalus::to_string(levels[i]),
                            levels[i] == active ? "[active]" : "-"
                        });
                    }
                    t.print();
                    std::cout << "(Default = send nothing; the provider/model "
                                 "decides)\n";
                }
                continue;
            }
            if (line == "/level" || line.rfind("/level ", 0) == 0) {
                const auto levels = agent.supported_thinking_levels();
                const auto active = agent.config().thinking_level;
                std::string arg = line.size() > 6 ? line.substr(7) : "";
                while (!arg.empty() && std::isspace(static_cast<unsigned char>(arg.back()))) arg.pop_back();
                if (arg.empty()) {
                    std::cout << "Thinking level: " << daedalus::to_string(active)
                              << "\nUsage: /level <default|none|low|high|max|#>\n";
                    continue;
                }
                daedalus::ThinkingLevel level;
                if (arg == "0") {
                    level = daedalus::ThinkingLevel::Default;  // row 0 in /levels
                } else if (const auto idx = parse_one_based_index(arg, levels.size())) {
                    level = levels[*idx];
                } else {
                    level = daedalus::thinking_level_from_string(arg);
                    // Unrecognized names also yield Default — distinguish by
                    // re-checking the input itself.
                    if (level == daedalus::ThinkingLevel::Default &&
                        arg != "default" && arg != "Default" && arg != "DEFAULT") {
                        std::cout << "Unknown thinking level '" << arg << "'. "
                                     "Valid: default, none, low, high, max"
                                  << (levels.empty() ? "" : " or an index from /levels")
                                  << "\n";
                        continue;
                    }
                }
                // daedalus silently ignores levels the model rejects — catch
                // that here instead (Default is always allowed: it sends
                // nothing; an empty supported list means no control at all).
                if (level != daedalus::ThinkingLevel::Default &&
                    std::find(levels.begin(), levels.end(), level) == levels.end()) {
                    std::cout << "Level '" << daedalus::to_string(level)
                              << "' is not supported by "
                              << (agent.active_model().name.empty() ? "the current model"
                                                                    : agent.active_model().name);
                    if (!levels.empty()) {
                        std::cout << " (supported:";
                        for (const auto l : levels)
                            std::cout << " " << daedalus::to_string(l);
                        std::cout << ")";
                    }
                    std::cout << "\n";
                    continue;
                }
                agent.set_thinking_level(level);
                daedalus::SettingsStore::for_home(settings_home)
                    .set_string("thinking_level", daedalus::to_string(level));
                std::cout << "[LEVEL] Thinking level set to "
                          << daedalus::to_string(level) << "\n";
                continue;
            }
            if (line == "/paths") {
                print_paths_table(cfg_view, effective_agent_name,
                                  icfg.history_file, render_cfg);
                continue;
            }

            // ----------------------------------------------------------------
            // Todo commands.
            // ----------------------------------------------------------------
            if (line == "/todo" || line.rfind("/todo ", 0) == 0) {
                std::string rest = line.size() > 5 ? line.substr(6) : "";
                // Trim leading whitespace from rest.
                while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front()))) rest.erase(rest.begin());

                auto first_token = [&]() -> std::pair<std::string, std::string> {
                    std::size_t pos = rest.find(' ');
                    if (pos == std::string::npos) return {rest, ""};
                    std::string cmd = rest.substr(0, pos);
                    std::string args = rest.substr(pos + 1);
                    while (!args.empty() && std::isspace(static_cast<unsigned char>(args.front()))) args.erase(args.begin());
                    return {cmd, args};
                };

                auto [sub, arg] = first_token();
                if (sub.empty() || sub == "list") {
                    std::string filter = "all";
                    if (!arg.empty() && (arg == "all" || arg == "pending" || arg == "done")) filter = arg;
                    auto result = agent.list_todos(filter);
                    if (result.contains("error")) {
                        std::cout << "[TODO] Error: " << result["error"].get<std::string>() << "\n";
                    } else {
                        std::string out = result.value("output", "");
                        if (out.empty()) {
                            auto box = icarus::ui::make_box({"No todos yet. Use /todo add <text>."}, std::nullopt, render_cfg, icarus::ui::Alignment::Auto);
                            for (const auto& l : box.lines) std::cout << l << "\n";
                        } else {
                            // daedalus returns a markdown-formatted list; strip the
                            // leading blank line so it sits cleanly after the prompt.
                            while (!out.empty() && out.front() == '\n') out.erase(out.begin());
                            std::cout << out;
                            if (out.empty() || out.back() != '\n') std::cout << "\n";
                        }
                    }
                } else if (sub == "add") {
                    if (arg.empty()) {
                        std::cout << "Usage: /todo add <text>\n";
                    } else {
                        std::cout << todo_status(agent.add_todo(arg, "pending"), "added") << "\n";
                    }
                } else if (sub == "done") {
                    if (arg.empty()) { std::cout << "Usage: /todo done <id>\n"; }
                    else try { std::cout << todo_status(agent.edit_todo(std::stoi(arg), std::nullopt, "done"), "marked done") << "\n"; } catch (...) { std::cout << "[TODO] Invalid id\n"; }
                } else if (sub == "undone") {
                    if (arg.empty()) { std::cout << "Usage: /todo undone <id>\n"; }
                    else try { std::cout << todo_status(agent.edit_todo(std::stoi(arg), std::nullopt, "pending"), "marked pending") << "\n"; } catch (...) { std::cout << "[TODO] Invalid id\n"; }
                } else if (sub == "edit") {
                    std::size_t space = arg.find(' ');
                    if (space == std::string::npos || space == 0) {
                        std::cout << "Usage: /todo edit <id> <new text>\n";
                    } else try {
                        int id = std::stoi(arg.substr(0, space));
                        std::string text = arg.substr(space + 1);
                        std::cout << todo_status(agent.edit_todo(id, text, std::nullopt), "updated") << "\n";
                    } catch (...) { std::cout << "[TODO] Invalid id\n"; }
                } else if (sub == "delete") {
                    if (arg.empty()) { std::cout << "Usage: /todo delete <id>\n"; }
                    else try { std::cout << todo_status(agent.delete_todo(std::stoi(arg)), "deleted") << "\n"; } catch (...) { std::cout << "[TODO] Invalid id\n"; }
                } else if (sub == "clear") {
                    std::cout << todo_status(agent.clear_todos(), "cleared") << "\n";
                } else if (sub == "max") {
                    if (arg.empty()) { std::cout << "Usage: /todo max <N>\n"; }
                    else try { std::cout << todo_status(agent.set_max_todos(std::stoi(arg)), "max set") << "\n"; } catch (...) { std::cout << "[TODO] Invalid number\n"; }
                } else {
                    std::cout << "Unknown /todo subcommand: " << sub << "\n";
                    std::cout << "Usage: /todo [list|add|done|undone|edit|delete|clear|max]\n";
                }
                continue;
            }

            // ----------------------------------------------------------------
            // Settings command.
            // ----------------------------------------------------------------
            if (line == "/settings" || line.rfind("/settings ", 0) == 0) {
                std::string rest = line.size() > 9 ? line.substr(10) : "";
                while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front()))) rest.erase(rest.begin());

                if (!rest.empty()) {
                    // §Settings TUI (2026-07-30): the /settings subcommands
                    // (providers|models|add-*|delete-*|switch-provider) were
                    // removed — everything they did is covered by the TUI
                    // tabs or by /providers, /models, /provider, /model.
                    std::cout << "/settings takes no arguments — it opens the "
                                 "interactive settings TUI.\n";
                    continue;
                }

                // §Settings TUI (2026-07-30): /settings opens the embedded
                // daedalus settings TUI (FTXUI tabs: Providers, Models, MCP
                // Servers, Agent) against the same settings DB the agent
                // uses. When not attached to a terminal (piped stdin/stdout)
                // fall back to the read-only table summary so scripts keep
                // working.
                if (::isatty(STDIN_FILENO) && ::isatty(STDOUT_FILENO)) {
                    // Pause the background input thread so it does not race
                    // FTXUI for stdin (same pattern as the sink approval
                    // reader). Resume on exit.
                    input_thread.pause();
                    struct ResumeGuard {
                        icarus::ui::InputThread* t;
                        ~ResumeGuard() { if (t) t->resume(); }
                    } resume_guard{&input_thread};

                    auto store = daedalus::SettingsStore::for_home(settings_home);

                    // §Settings TUI sync (2026-07-30): daedalus only writes
                    // the current_provider row on switch_provider(), so a
                    // session that never switched shows a blank provider in
                    // the Agent tab. Seed it from the active model (only
                    // when empty — a non-empty value may be the deliberate
                    // fallback provider for discovered-model switches).
                    if (store.get_current_provider().empty()) {
                        const auto active_provider =
                            agent.active_model().provider;
                        if (!active_provider.empty()) {
                            store.set_current_provider(active_provider);
                        }
                    }

                    // §Settings TUI sync (2026-07-30): snapshot everything
                    // the running agent only snapshots at initialize() /
                    // switch time, so edits made in the TUI can be detected
                    // and pushed into the live agent after it closes.
                    const auto active_before    = agent.active_model();
                    const auto provider_before  = store.get_provider(active_before.provider);
                    const auto model_before     = store.get_model(active_before.name);
                    const auto mcp_before       = store.list_mcp_servers();
                    const auto kv_before = std::tuple{
                        store.get_max_agent_iterations(),
                        store.get_max_tool_result_size(),
                        store.get_max_request_size(),
                        store.get_max_response_size(),
                        store.get_compression_ratio_percent(),
                        store.get_mcp_tool_schema_soft_cap_percent(),
                    };

                    settings_tui::RunApp(store);

                    // §Settings TUI sync (2026-07-30): re-apply settings the
                    // running agent only snapshots at initialize() —
                    // auto_approve (unless --auto made the CLI authoritative,
                    // §BUG-006) and current_provider.
                    if (!args.auto_mode) {
                        agent.set_auto_approve(store.get_auto_approve());
                    }
                    // §ThinkingLevel (2026-08-01): re-apply the persisted
                    // thinking level — the TUI Agent tab edits the kv row;
                    // set_thinking_level() takes effect on the next request.
                    agent.set_thinking_level(daedalus::thinking_level_from_string(
                        store.get_string("thinking_level", "default")));
                    bool provider_switched = false;
                    const std::string tui_provider =
                        store.get_current_provider();
                    if (!tui_provider.empty() &&
                        tui_provider != agent.active_model().provider) {
                        if (agent.switch_provider(tui_provider)) {
                            current_provider = tui_provider;
                            last_discovered_models.clear();
                            provider_switched = true;
                            std::cout << "[SETTINGS] Switched to provider '"
                                      << tui_provider << "'.\n";
                        } else {
                            std::cout << "[SETTINGS] Could not switch to "
                                         "provider '" << tui_provider
                                      << "' (unknown or has no models).\n";
                        }
                    }

                    // Active model/provider edits: switch_provider (above)
                    // already rebuilds the client config when it fires.
                    // Otherwise re-selecting the same model forces daedalus
                    // to rebuild it from the store (credentials, endpoints,
                    // temperature) — at the cost of the §MODEL-SWITCH
                    // conversation-history reset, so only do it on a real
                    // change and say so.
                    if (!provider_switched && !active_before.name.empty()) {
                        const auto model_after    = store.get_model(active_before.name);
                        const auto provider_after = store.get_provider(active_before.provider);
                        auto provider_eq = [](const daedalus::ProviderConfig& a,
                                              const daedalus::ProviderConfig& b) {
                            return a.host == b.host && a.api_key == b.api_key &&
                                   a.chat_endpoint == b.chat_endpoint &&
                                   a.discovery_endpoint == b.discovery_endpoint &&
                                   a.timeout_seconds == b.timeout_seconds;
                        };
                        auto model_eq = [](const daedalus::ModelConfig& a,
                                           const daedalus::ModelConfig& b) {
                            return a.provider == b.provider &&
                                   a.model_id == b.model_id &&
                                   a.max_context_size == b.max_context_size &&
                                   a.temperature == b.temperature;
                        };
                        const bool changed =
                            (model_after.has_value() != model_before.has_value()) ||
                            (model_after && model_before && !model_eq(*model_after, *model_before)) ||
                            (provider_after.has_value() != provider_before.has_value()) ||
                            (provider_after && provider_before && !provider_eq(*provider_after, *provider_before));
                        if (!model_after.has_value()) {
                            std::cout << "[SETTINGS] Active model '"
                                      << active_before.name
                                      << "' was deleted — use /model <name> "
                                         "to pick another.\n";
                        } else if (changed && agent.switch_model(active_before.name)) {
                            std::cout << "[SETTINGS] Applied provider/model "
                                         "changes to the running agent "
                                         "(conversation history reset).\n";
                        }
                    }

                    // MCP servers: enable/disable is hot-appliable via the
                    // runtime API; added/edited/removed servers need a
                    // restart (daedalus loads the registry at initialize()).
                    std::vector<std::string> mcp_restart;
                    const auto mcp_after = store.list_mcp_servers();
                    for (const auto& after : mcp_after) {
                        const auto it = std::find_if(
                            mcp_before.begin(), mcp_before.end(),
                            [&](const auto& b) { return b.name == after.name; });
                        if (it == mcp_before.end()) {
                            mcp_restart.push_back(after.name);
                            continue;
                        }
                        if (it->enabled != after.enabled) {
                            agent.set_mcp_server_enabled(after.name, after.enabled);
                        }
                        if (it->transport != after.transport ||
                            it->command != after.command ||
                            it->args != after.args ||
                            it->env != after.env ||
                            it->url != after.url ||
                            it->bearer_token != after.bearer_token) {
                            mcp_restart.push_back(after.name);
                        }
                    }
                    for (const auto& before : mcp_before) {
                        const auto it = std::find_if(
                            mcp_after.begin(), mcp_after.end(),
                            [&](const auto& a) { return a.name == before.name; });
                        if (it == mcp_after.end()) {
                            // Removed: disconnect the runtime instance.
                            agent.set_mcp_server_enabled(before.name, false);
                            mcp_restart.push_back(before.name);
                        }
                    }
                    if (!mcp_restart.empty()) {
                        std::cout << "[SETTINGS] MCP server changes (";
                        for (std::size_t i = 0; i < mcp_restart.size(); ++i) {
                            if (i) std::cout << ", ";
                            std::cout << mcp_restart[i];
                        }
                        std::cout << ") fully apply on next launch.\n";
                    }

                    // Remaining Agent-tab kv values (iterations, size caps,
                    // compression ratio)
                    // are read at initialize() — no runtime API.
                    // (Web-search provider edits are NOT tracked here: the
                    // §WEB-SEARCH-CORE web_search tool reads its provider
                    // chain per call, so those apply immediately.)
                    const auto kv_after = std::tuple{
                        store.get_max_agent_iterations(),
                        store.get_max_tool_result_size(),
                        store.get_max_request_size(),
                        store.get_max_response_size(),
                        store.get_compression_ratio_percent(),
                        store.get_mcp_tool_schema_soft_cap_percent(),
                    };
                    if (kv_after != kv_before) {
                        std::cout << "[SETTINGS] Agent limits/retry/compression "
                                     "changes apply on next launch.\n";
                    }

                    std::cout << "[SETTINGS] Closed. Use /model <name> or "
                                 "/provider <name> to activate changes.\n";
                } else {
                    auto active = agent.active_model();
                    std::cout << "Active model: " << (active.name.empty() ? "(none)" : active.name) << "\n";
                    auto providers = agent.list_providers_detailed();
                    if (!providers.empty()) {
                        std::cout << "Providers:\n";
                        print_providers_table(providers,
                            current_provider.empty() ? active.provider : current_provider, render_cfg);
                    }
                    auto models = agent.list_models();
                    if (!models.empty()) {
                        std::cout << "Models:\n";
                        print_configured_models_table(agent, models, render_cfg);
                    }
                    if (providers.empty() && models.empty()) {
                        std::cout << "No providers or models configured.\n";
                    }
                }
                continue;
            }

            // ----------------------------------------------------------------
            // Utility commands.
            // ----------------------------------------------------------------
            if (line == "/memory" || line.rfind("/memory ", 0) == 0) {
                std::string rest = line.size() > 7 ? line.substr(8) : "";
                while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front()))) rest.erase(rest.begin());
                if (rest == "clear") {
                    agent.clear_memory();
                    std::cout << "[MEMORY] Cleared.\n";
                } else {
                    daedalus::ProjectPaths pp(cfg_view.project_root, effective_agent_name);
                    std::cout << "Memory: " << (agent.config().memory_enabled ? "enabled" : "disabled") << "\n";
                    std::cout << "Path: " << pp.memory_file().string() << "\n";
                }
                continue;
            }
            if (line == "/history" || line.rfind("/history ", 0) == 0) {
                std::string rest = line.size() > 8 ? line.substr(9) : "";
                while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front()))) rest.erase(rest.begin());
                std::size_t limit = 0; // 0 = all
                if (!rest.empty()) try { limit = std::stoull(rest); } catch (...) {}
                auto messages_path = data_dir / "messages.json";
                if (!fs::exists(messages_path)) {
                    std::cout << "No history found.\n";
                } else {
                    std::ifstream ifs(messages_path);
                    nlohmann::json msgs;
                    try { ifs >> msgs; } catch (...) { msgs = nlohmann::json::array(); }
                    if (!msgs.is_array() || msgs.empty()) {
                        std::cout << "No history found.\n";
                    } else {
                        std::vector<nlohmann::json> hist;
                        for (auto it = msgs.rbegin(); it != msgs.rend() && (limit == 0 || hist.size() < limit); ++it) {
                            hist.push_back(*it);
                        }
                        std::reverse(hist.begin(), hist.end());
                        std::vector<std::string> lines;
                        for (const auto& m : hist) {
                            std::string role = m.value("role", "unknown");
                            std::string content = m.value("content", "");
                            if (content.size() > 120) content.replace(117, std::string::npos, "...");
                            lines.push_back(role + ": " + content);
                        }
                        auto box = icarus::ui::make_box(lines, "History", render_cfg);
                        for (const auto& l : box.lines) std::cout << l << "\n";
                    }
                }
                continue;
            }
            if (line == "/export" || line.rfind("/export ", 0) == 0) {
                std::string rest = line.size() > 7 ? line.substr(8) : "";
                while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front()))) rest.erase(rest.begin());
                std::string out_path;
                if (rest.empty()) {
                    auto now = std::chrono::system_clock::now();
                    auto t = std::chrono::system_clock::to_time_t(now);
                    std::tm tm{};
                    localtime_r(&t, &tm);
                    std::ostringstream oss;
                    oss << "icarus-export-"
                        << std::put_time(&tm, "%Y%m%d-%H%M%S") << ".json";
                    out_path = oss.str();
                } else {
                    out_path = rest;
                }
                auto messages_path = data_dir / "messages.json";
                if (!fs::exists(messages_path)) {
                    std::cout << "[EXPORT] No messages to export.\n";
                } else {
                    try {
                        fs::copy_file(messages_path, out_path, fs::copy_options::overwrite_existing);
                        std::cout << "[EXPORT] Wrote " << out_path << "\n";
                    } catch (const std::exception& e) {
                        std::cout << "[EXPORT] Failed: " << e.what() << "\n";
                    }
                }
                continue;
            }
            if (line == "/tool" || line.rfind("/tool ", 0) == 0) {
                std::string rest = line.size() > 5 ? line.substr(6) : "";
                while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front()))) rest.erase(rest.begin());
                std::size_t space = rest.find(' ');
                std::string tool_name = rest.substr(0, space);
                std::string args_str = (space == std::string::npos) ? "" : rest.substr(space + 1);
                while (!args_str.empty() && std::isspace(static_cast<unsigned char>(args_str.front()))) args_str.erase(args_str.begin());
                if (tool_name.empty()) {
                    std::cout << "Usage: /tool <name> [json-args]\n";
                } else {
                    nlohmann::json tool_args = nlohmann::json::object();
                    if (!args_str.empty()) {
                        try { tool_args = nlohmann::json::parse(args_str); }
                        catch (const std::exception& e) {
                            std::cout << "[TOOL] Invalid JSON args: " << e.what() << "\n";
                            continue;
                        }
                    }
                    try {
                        auto result = agent.execute_tool(tool_name, tool_args);
                        std::cout << result.dump(2) << "\n";
                    } catch (const std::exception& e) {
                        std::cout << "[TOOL] Error: " << e.what() << "\n";
                    }
                }
                continue;
            }
            if (line == "/last") {
                std::string text = agent.last_assistant_text();
                if (text.empty()) {
                    std::cout << "No assistant response yet.\n";
                } else {
                    std::cout << text << "\n";
                }
                continue;
            }
            if (line == "/continue") {
                if (last_user_message.empty()) {
                    std::cout << "No previous message to continue.\n";
                } else {
                    std::cout << "[CONTINUE] Re-submitting previous message.\n";
                    input_thread.resume();
                    sink->start_agent_turn();
                    agent.submit_user_message(last_user_message);
                    agent.run_one_turn();
                    sink->stop_agent_turn();
                }
                continue;
            }

            // Actual user input — drive one agent turn.
            // Resume the reader first so ESC can cancel the turn.
            input_thread.resume();
            last_user_message = line;
            // Bracket the turn so the spinner runs from submit until the
            // agent finishes — the sink callbacks refine the label and
            // pause/resume around output in between.
            sink->start_agent_turn();
            agent.submit_user_message(std::move(line));
            agent.run_one_turn();
            sink->stop_agent_turn();
            // The next iteration prints the prompt before reading a line.
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
