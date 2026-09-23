// SPDX-License-Identifier: BSL-1.0
// src/util.h
//
// CLI utilities: signal handler registration, path resolution, CLI flag parser.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// §Demo fix 2026-07-14: util.h now references daedalus::Agent for
// the SIGINT → cancel() wiring. Tests that include util.h (notably
// tests/test_cli_system_prompt.cpp) get the public Agent header
// transitively from here.
#include <daedalus.h>

namespace fs = std::filesystem;

// =============================================================================
// Signal handling
// =============================================================================

// §Demo fix 2026-07-16: use `inline` for process-wide globals instead
// of `static`/anonymous-namespace so multiple TUs that include util.h
// share one instance and don't trigger ODR/internal-linkage issues.
inline std::atomic<bool> g_shutting_down{false};
inline std::atomic<daedalus::Agent*> g_agent_for_signal{nullptr};

// §Demo fix 2026-07-16: mark the signal handlers inline so the
// definition can safely appear in a header included by multiple TUs.
extern "C" inline void daedalus_signal_handler(int sig) {
    // §Demo fix 2026-07-14: the ThinkingSpinner hides the cursor
    // (`\x1b[?25l`) at the start of its animation and restores it
    // (`\x1b[?25h`) at the end. If the process is killed by a signal
    // mid-animation (or abort() / unhandled exception unwinds
    // through main() without running the spinner's destructor) the
    // cursor is left hidden and the user's terminal stays cursor-less
    // until they type `tput cnorm` or restart it.
    //
    // Fix: the signal handler itself restores the cursor and SGR
    // attributes. `write()` is async-signal-safe; `std::cerr` is not.
    extern void daedalus_cli_restore_terminal_state() noexcept;
    daedalus_cli_restore_terminal_state();

    // §Demo fix 2026-07-14: cancel any in-flight turn so the loop
    // returns promptly. Agent::cancel() is noexcept and just fires
    // internal stop_sources (atomic ops), so it's safe-ish to call
    // from a signal handler in practice (the C++ standard is strict
    // about async-signal-safety; in practice this is what every
    // production agent does).
    if (auto* p = g_agent_for_signal.load(std::memory_order_acquire)) {
        p->cancel();
    }

    g_shutting_down.store(true, std::memory_order_release);

    // If this is the second signal while already shutting down, force an
    // immediate exit after restoring the terminal.
    static std::atomic<bool> g_sigint_received{false};
    if ((sig == SIGINT || sig == SIGTERM) && g_sigint_received.exchange(true)) {
        daedalus_cli_restore_terminal_state();
        std::_Exit(1);
    }
}

inline void install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = daedalus_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    std::signal(SIGPIPE, SIG_IGN);
}

// Storage for chaining crash handlers installed by daedalus::Agent.
// §Demo fix 2026-07-16: `inline` so multiple TUs share one instance.
struct ChainedCrashHandler {
    struct sigaction old_sa{};
    bool installed = false;
};
inline ChainedCrashHandler g_chained_sigsegv;
inline ChainedCrashHandler g_chained_sigabrt;
inline ChainedCrashHandler g_chained_sigfpe;
inline ChainedCrashHandler g_chained_sigill;

extern "C" inline void daedalus_cli_crash_handler(int sig, siginfo_t* info, void* context) {
    // Restore the cursor and SGR attributes before chaining to daedalus's
    // crash handler so a crashing process does not leave the terminal painted.
    extern void daedalus_cli_restore_terminal_state() noexcept;
    daedalus_cli_restore_terminal_state();

    ChainedCrashHandler* h = nullptr;
    switch (sig) {
        case SIGSEGV: h = &g_chained_sigsegv; break;
        case SIGABRT: h = &g_chained_sigabrt; break;
        case SIGFPE:  h = &g_chained_sigfpe;  break;
        case SIGILL:  h = &g_chained_sigill;  break;
    }
    if (h && h->installed) {
        if (h->old_sa.sa_flags & SA_SIGINFO && h->old_sa.sa_sigaction) {
            h->old_sa.sa_sigaction(sig, info, context);
        } else if (h->old_sa.sa_handler != SIG_DFL && h->old_sa.sa_handler != SIG_IGN) {
            h->old_sa.sa_handler(sig);
        } else {
            struct sigaction dfl{};
            dfl.sa_handler = SIG_DFL;
            ::sigaction(sig, &dfl, nullptr);
            ::raise(sig);
        }
    } else {
        struct sigaction dfl{};
        dfl.sa_handler = SIG_DFL;
        ::sigaction(sig, &dfl, nullptr);
        ::raise(sig);
    }
}

// Chain our terminal-restoring handler in front of the crash handlers
// daedalus::Agent installs. Call this *after* agent.install_crash_handlers().
inline void install_terminal_restoring_crash_handlers() {
    struct sigaction sa{};
    sa.sa_sigaction = daedalus_cli_crash_handler;
    sigemptyset(&sa.sa_mask);
    // SA_NODEFER: the handler re-raises, so don't block the signal.
    // SA_RESETHAND: after our handler runs once, revert to SIG_DFL so
    // the chained daedalus re-raise produces a core dump/default action.
    sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESETHAND;

    auto chain = [&](int sig, ChainedCrashHandler& h) {
        if (::sigaction(sig, &sa, &h.old_sa) == 0) {
            h.installed = true;
        }
    };

    chain(SIGSEGV, g_chained_sigsegv);
    chain(SIGABRT, g_chained_sigabrt);
    chain(SIGFPE,  g_chained_sigfpe);
    chain(SIGILL,  g_chained_sigill);
}

// =============================================================================
// §Demo fix 2026-07-14: terminal state restore.
//
// ThinkingSpinner hides the cursor and resets the colour palette
// during its animation. We restore both on every exit path
// (normal return, signal, unhandled exception). The implementation
// uses raw `write(2)` because it is async-signal-safe — `std::cerr`
// is NOT, and `daedalus_signal_handler` may run with arbitrary
// locks held.
//
// Multiple invocations (atexit + signal + catch block) are harmless:
//   - `\x1b[?25h` is idempotent (show cursor; visible if already shown).
//   - `\x1b[0m`   is idempotent (reset SGR attributes; no-op if none set).
//   - `write()` to the same fd is atomic for sizes < PIPE_BUF (4096).
//     Our payload is 10 bytes, well below that, so concurrent calls
//     from atexit + signal handler + catch block cannot interleave.
//
// Defined here in the header (rather than util.cpp) because the
// signal handler at the top of this file needs a forward declaration
// and util.h is header-only by convention for this demo.
// =============================================================================

#include <unistd.h>

inline void daedalus_cli_restore_terminal_state() noexcept {
    // Reset common terminal modes that applications leave behind:
    //   \x1b[r          = reset scroll region
    //   \x1b[?1000l etc = disable mouse tracking
    //   \x1b[?1004l     = disable focus reporting
    //   \x1b[?2004l     = disable bracketed paste
    //   \x1b[?25h       = show cursor
    //   \x1b[0m         = reset all SGR attributes
    static constexpr char kSeq[] =
        "\x1b[r"
        "\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1006l"
        "\x1b[?1004l"
        "\x1b[?2004l"
        "\x1b[?25h"
        "\x1b[0m";
    // write() returns ssize_t and may be interrupted by signals; loop
    // until we've written everything or hit a hard error.
    std::size_t total = 0;
    while (total < sizeof(kSeq) - 1) {
        ssize_t n = ::write(STDERR_FILENO,
                            kSeq + total,
                            sizeof(kSeq) - 1 - total);
        if (n <= 0) break;
        total += static_cast<std::size_t>(n);
    }
}

// Clear the terminal screen and move the cursor home. Uses async-signal-safe
// write() so it can be called from the signal handler as well as normal exit.
inline void daedalus_cli_clear_terminal() noexcept {
    // Reset scroll region first so the clear affects the whole screen,
    // then clear entire screen and move cursor to top-left home position.
    static constexpr char kSeq[] = "\x1b[r\x1b[2J\x1b[H";
    std::size_t total = 0;
    while (total < sizeof(kSeq) - 1) {
        ssize_t n = ::write(STDERR_FILENO,
                            kSeq + total,
                            sizeof(kSeq) - 1 - total);
        if (n <= 0) break;
        total += static_cast<std::size_t>(n);
    }
}

// =============================================================================
// Path resolution
// =============================================================================

// Resolve target_path to a real, accessible directory.
// CLI-level concern (filesystem portability).
inline fs::path resolve_target_path(fs::path p) {
    if (p.empty()) {
        try { return fs::current_path(); }
        catch (...) { /* fall through */ }
    }
    const char* home = std::getenv("HOME");
    auto fallback_home = [&]() -> fs::path {
        if (home && fs::exists(home)) return home;
        return fs::temp_directory_path();
    };
    try {
        return fs::weakly_canonical(fs::absolute(p));
    } catch (const fs::filesystem_error& e) {
        std::cerr << "[WARNING] Cannot resolve path '" << p.string()
                  << "': " << e.what() << "\n";
        std::cerr << "[WARNING] Falling back to HOME / /tmp.\n";
        return fallback_home();
    }
}

// =============================================================================
// Help text
// =============================================================================

inline void print_help(const char* program_name) {
    std::cout << "Usage: " << program_name << " [options] [directory]\n\n";
    std::cout << "icarus — AI coding agent powered by daedalus\n\n";
    std::cout << "Options:\n";
    std::cout << "  -h, --help          Show this help message and exit\n";
    std::cout << "  -v, --version       Show version information and exit\n";
    std::cout << "  --auto              Auto-approve bash/tool execution\n";
    std::cout << "  --no-memory         Disable session memory for this session\n";
    std::cout << "  --clear-memory      Clear all session memory and exit\n";
    std::cout << "  --home DIR          Use DIR as the daedalus home\n";
    std::cout << "  --model NAME        Use model NAME from the SQLite settings store\n";
    std::cout << "  --model-list        List available models and exit\n";
    std::cout << "  --discover-models NAME\n";
    std::cout << "                      Query the provider's /v1/models endpoint for\n";
    std::cout << "                      the configured model NAME and exit\n";
    std::cout << "  --agent-name NAME   Per-agent namespace for runtime state\n";
    std::cout << "  --threads N         Number of worker threads\n";
    std::cout << "  --stream            Enable SSE streaming (default)\n";
    std::cout << "  --no-stream         Disable SSE streaming\n";
    std::cout << "  --no-color          Disable ANSI colors\n";
    std::cout << "  --no-hud            Disable the status HUD/spinner\n";
    std::cout << "  --no-unicode        Use ASCII-only drawing characters\n";
    std::cout << "  --plain             Alias for --no-color --no-hud --no-unicode\n";
    std::cout << "  --skills DIR        Load skills from DIR\n";
    std::cout << "  --system-prompt F   Override the project AGENTS.md/system prompt with F\n";
    std::cout << "  --a2a-url URL       A2A server URL (default: http://localhost:8000)\n";
    std::cout << "  --a2a-port N        A2A agent HTTP port (0 = ephemeral, default: 0)\n";
    std::cout << "  --a2a-agent         Run as an A2A agent HTTP server (for IDE integration)\n";
    std::cout << "  --acp-url URL       Deprecated alias for --a2a-url\n";
    std::cout << "  --acp-agent         Deprecated alias for --a2a-agent\n";
    std::cout << "\nArguments:\n";
    std::cout << "  directory       Target project directory (default: .)\n";
    std::cout << "\nIn-session commands:\n";
    std::cout << "  /quit, /exit           Exit\n";
    std::cout << "  /help                  Show this help\n";
    std::cout << "  /init                  Create or update AGENTS.md for this project\n";
    std::cout << "  /clear                 Clear session state\n";
    std::cout << "  /compress              Compress conversation context\n";
    std::cout << "  /continue              Re-submit the last user message\n";
    std::cout << "  /last                  Print the last assistant response\n";
    std::cout << "\n";
    std::cout << "  /providers             List configured providers (table)\n";
    std::cout << "  /provider-add N H K [P]  Add provider: name, host, api_key ('-' = none),\n";
    std::cout << "                           optional plugin (auto-detected when omitted)\n";
    std::cout << "  /provider-delete NAME|#  Delete a provider by name or index\n";
    std::cout << "  /provider NAME|#       Switch to provider by name or index\n";
    std::cout << "  /models                Discover models for current provider\n";
    std::cout << "                         (falls back to that provider's configured models\n";
    std::cout << "                         if it has no /v1/models endpoint)\n";
    std::cout << "  /models --configured [P]  List configured models, optionally filtered\n";
    std::cout << "                         to provider P (table)\n";
    std::cout << "  /model-add N P M       Add model: name, provider, model_id\n";
    std::cout << "  /model-delete NAME|#   Delete a configured model by name or index\n";
    std::cout << "  /model NAME|#          Switch active model by name or index\n";
    std::cout << "                         (# = discovered list, or configured list when\n";
    std::cout << "                         discovery is unavailable)\n";
    std::cout << "\n";
    std::cout << "  /skills                List loaded skills (table)\n";
    std::cout << "  /skills enable|disable <name|#>\n";
    std::cout << "  /tools                 List registered tools (table)\n";
    std::cout << "  /tools enable|disable  <name|#>\n";
    std::cout << "  /mcp-servers           List configured MCP servers (table)\n";
    std::cout << "  /mcp-servers enable|disable <name|#>\n";
    std::cout << "\n";
    std::cout << "  /todo [list|pending|done]\n";
    std::cout << "                         List todos (default: all)\n";
    std::cout << "  /todo add <text>       Add a new todo\n";
    std::cout << "  /todo done <id>        Mark todo as done\n";
    std::cout << "  /todo undone <id>      Mark todo as pending\n";
    std::cout << "  /todo edit <id> <text> Update todo text\n";
    std::cout << "  /todo delete <id>      Delete a todo\n";
    std::cout << "  /todo clear            Delete all todos\n";
    std::cout << "  /todo max <N>          Set max allowed todos\n";
    std::cout << "\n";
    std::cout << "  /settings              Open the interactive settings TUI\n";
    std::cout << "                         (table summary when not on a terminal)\n";
    std::cout << "\n";
    std::cout << "  /memory [clear]        Show memory status or clear it\n";
    std::cout << "  /history [N]           Show last N messages (default: all)\n";
    std::cout << "  /export [file]         Export session messages to JSON\n";
    std::cout << "  /tool <name> [json]    Execute a registered tool directly\n";
    std::cout << "\n";
    std::cout << "  /usage                 Show token/context usage summary\n";
    std::cout << "  /stats                 Show raw token + iteration stats\n";
    std::cout << "  /state                 Show current Agent state\n";
    std::cout << "  /levels                List thinking levels for the current model\n";
    std::cout << "  /level NAME|#          Set thinking level (default|none|low|high|max)\n";
    std::cout << "  /paths                 Show all config/data/project paths (table)\n";
}

// =============================================================================
// §CLI usage: clean, host-side formatting of AgentStats.
// The library only exposes raw numbers; the CLI demo decides how to draw them.
// =============================================================================

inline void print_usage(std::ostream& out, const daedalus::AgentStats& st) {
    float token_percent = 0.0f;
    if (st.max_context_size > 0) {
        token_percent = (static_cast<float>(st.total_tokens) /
                         static_cast<float>(st.max_context_size)) * 100.0f;
    }

    float iter_percent = 0.0f;
    if (st.max_iterations > 0) {
        iter_percent = (static_cast<float>(st.iteration) /
                        static_cast<float>(st.max_iterations)) * 100.0f;
    }

    constexpr int bar_width = 30;
    int filled = static_cast<int>((token_percent / 100.0f) * bar_width);
    if (filled > bar_width) filled = bar_width;
    if (filled < 0) filled = 0;

    std::ostringstream bar;
    bar << '[';
    for (int i = 0; i < bar_width; ++i) {
        bar << (i < filled ? "\xE2\x96\x88" : "\xE2\x96\x91");
    }
    bar << ']';

    std::ostringstream msg;
    msg << "Usage:\n"
        << "  Tokens:     " << st.total_tokens << " / " << st.max_context_size
        << " (" << std::fixed << std::setprecision(1) << token_percent << "%)\n"
        << "  " << bar.str() << "\n"
        << "  Prompt:     " << st.prompt_tokens << "\n"
        << "  Completion: " << st.completion_tokens << "\n";
    // §ContextCache (2026-07-31): only shown when the provider reported
    // server-side cache hits on the last response.
    if (st.cached_tokens > 0) {
        msg << "  Cached:     " << st.cached_tokens
            << " prompt tokens served from the provider's context cache\n";
    }
    msg << "  Messages:   " << st.message_count << "\n"
        << "  Iterations: " << st.iteration << " / " << st.max_iterations
        << " (" << std::fixed << std::setprecision(1) << iter_percent << "%)\n";
    out << msg.str();
}

// =============================================================================
// CLI flag parser
// =============================================================================

struct CliArgs {
    bool        auto_mode           = false;
    bool        no_memory           = false;
    bool        clear_memory_flag   = false;
    bool        model_list_only     = false;
    // §Task B4 (CodeMax.md §16): --discover-models NAME queries the
    // provider's /v1/models endpoint for the configured model NAME and
    // exits. Like --model-list, this short-circuits before Agent init.
    bool        discover_models_only = false;
    std::string discover_model_name;
    bool        stream_enabled      = true;
    bool        no_color            = false;
    bool        no_hud              = false;
    bool        no_unicode          = false;
    std::string model_override;
    int         thread_count_override = 0;
    fs::path    target_path;
    fs::path    daedalus_home;
    std::vector<fs::path> skills_dirs;
    std::string agent_name;
    // §Phase 14: optional override of the system prompt source.
    // Empty = use the per-project AGENTS.md.
    fs::path    system_prompt_file;
    // A2A (Agent2Agent Protocol) options.
    std::string a2a_url;
    uint16_t    a2a_port = 0;
    bool        a2a_agent_mode = false;

    // Deprecated aliases for old ACP v1 CLI flags.
    bool        acp_url_deprecated = false;
    bool        acp_agent_deprecated = false;
};

inline CliArgs parse_cli_args(int argc, char* argv[]) {
    CliArgs args;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if      (arg == "--auto")           args.auto_mode = true;
        else if (arg == "--no-memory")       args.no_memory = true;
        else if (arg == "--clear-memory")    args.clear_memory_flag = true;
        else if (arg == "--model-list")     args.model_list_only = true;
        else if (arg == "--discover-models") {
            // §Task B4 (CodeMax.md §16): --discover-models NAME
            if (i + 1 < argc) {
                args.discover_models_only = true;
                args.discover_model_name = argv[++i];
            }
            else { std::cerr << "Error: --discover-models requires a model name\n"; std::exit(1); }
        }
        else if (arg == "--stream")          args.stream_enabled = true;
        else if (arg == "--no-stream")      args.stream_enabled = false;
        else if (arg == "--no-color")       args.no_color = true;
        else if (arg == "--no-hud")         args.no_hud = true;
        else if (arg == "--no-unicode")     args.no_unicode = true;
        else if (arg == "--plain") {
            args.no_color = true;
            args.no_hud = true;
            args.no_unicode = true;
        }
        else if (arg == "--home") {
            if (i + 1 < argc) args.daedalus_home = argv[++i];
            else { std::cerr << "Error: --home requires a directory\n"; std::exit(1); }
        }
        else if (arg == "--model") {
            if (i + 1 < argc) args.model_override = argv[++i];
            else { std::cerr << "Error: --model requires a model name\n"; std::exit(1); }
        }
        else if (arg == "--agent-name") {
            if (i + 1 < argc) args.agent_name = argv[++i];
            else { std::cerr << "Error: --agent-name requires a NAME\n"; std::exit(1); }
        }
        else if (arg == "--threads") {
            if (i + 1 < argc) {
                try {
                    args.thread_count_override = std::stoi(argv[++i]);
                    if (args.thread_count_override < 1) {
                        std::cerr << "Error: --threads requires a positive integer\n";
                        std::exit(1);
                    }
                }
                catch (...) {
                    std::cerr << "Error: --threads requires a positive integer\n";
                    std::exit(1);
                }
            }
            else { std::cerr << "Error: --threads requires a number\n"; std::exit(1); }
        }
        else if (arg == "--skills") {
            if (i + 1 < argc) args.skills_dirs.emplace_back(argv[++i]);
            else { std::cerr << "Error: --skills requires a directory\n"; std::exit(1); }
        }
        else if (arg == "--system-prompt") {
            // §Phase 14: --system-prompt FILE overrides the project AGENTS.md.
            // Useful for one-shot experiments without editing files.
            if (i + 1 < argc) args.system_prompt_file = argv[++i];
            else { std::cerr << "Error: --system-prompt requires a FILE\n"; std::exit(1); }
        }
        else if (arg == "--a2a-url") {
            if (i + 1 < argc) args.a2a_url = argv[++i];
            else { std::cerr << "Error: --a2a-url requires a URL\n"; std::exit(1); }
        }
        else if (arg == "--a2a-port") {
            if (i + 1 < argc) {
                try { args.a2a_port = static_cast<uint16_t>(std::stoi(argv[++i])); }
                catch (...) { args.a2a_port = 0; }
            }
            else { std::cerr << "Error: --a2a-port requires a port number\n"; std::exit(1); }
        }
        else if (arg == "--a2a-agent") {
            args.a2a_agent_mode = true;
        }
        else if (arg == "--acp-url") {
            args.acp_url_deprecated = true;
            if (i + 1 < argc) args.a2a_url = argv[++i];
            else { std::cerr << "Error: --acp-url requires a URL\n"; std::exit(1); }
        }
        else if (arg == "--acp-agent") {
            args.acp_agent_deprecated = true;
            args.a2a_agent_mode = true;
        }
        else if (arg.substr(0, 1) != "-") {
            args.target_path = arg;
        }
    }
    return args;
}
// ============================================================================
// §Phase 14: resolve the demo's system prompt.
//
// Resolution order (highest priority first):
//   1. <CLI flag override>      --system-prompt FILE  (if provided)
//   2. <per-project standard>   <project_root>/AGENTS.md
//
// The legacy <project_root>/.daedalus/daedalus/system_prompt.txt overlay and
// the <exe-dir>/system_prompt.txt sidecar are no longer used; project-specific
// instructions should live in AGENTS.md.
//
// Returns the merged text. Empty std::string if no source is found.
// ============================================================================

#include <fstream>
#include <sstream>
#include <iostream>
#include <system_error>

#ifdef __linux__
#include <unistd.h>
#endif

inline std::string resolve_system_prompt(
    const fs::path& override_file,
    const fs::path& project_root)
{
    std::string out;

    // 1. CLI override (highest priority).
    if (!override_file.empty()) {
        if (fs::exists(override_file)) {
            std::ifstream f(override_file);
            if (f) {
                std::ostringstream ss;
                ss << f.rdbuf();
                out += ss.str();
            }
        } else {
            std::cerr << "Warning: --system-prompt file does not exist: "
                      << override_file << "\n";
        }
    }

    // 2. Per-project AGENTS.md (open standard for agent instructions).
    if (override_file.empty() && !project_root.empty()) {
        fs::path agents_md = project_root / "AGENTS.md";
        if (fs::exists(agents_md)) {
            std::ifstream f(agents_md);
            if (f) {
                std::ostringstream ss;
                ss << f.rdbuf();
                out += ss.str();
            }
        }
    }

    return out;
}