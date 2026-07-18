// SPDX-License-Identifier: BSL-1.0
// demos/daedalus-cli/util.h
//
// CLI utilities: signal handler registration, path resolution, CLI flag parser.
// Extracted from main.cpp to keep it under 300 lines.

#pragma once

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <filesystem>
#include <iomanip>
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

static std::atomic<bool> g_shutting_down{false};

namespace {
// §Demo fix 2026-07-14: SIGINT must cancel any in-flight agent turn
// (the LLMClient's stop_source_). Holding a raw pointer in an atomic
// is OK because main() pins the lifetime: the pointer is set after
// `Agent agent(...)` and cleared before `agent.shutdown()`. The
// signal handler reads it with acquire ordering; the writer uses
// release.
std::atomic<daedalus::Agent*> g_agent_for_signal{nullptr};
} // namespace

extern "C" void daedalus_signal_handler(int /*sig*/) {
    // §Demo fix 2026-07-14: the ThinkingSpinner hides the cursor
    // (`\x1b[?25l`) at the start of its animation and restores it
    // (`\x1b[?25h`) at the end. If the process is killed by a signal
    // mid-animation (or abort() / unhandled exception unwinds
    // through main() without running the spinner's destructor) the
    // cursor is left hidden and the user's terminal stays cursor-less
    // until they type `tput cnorm` or restart it.
    //
    // Fix: the signal handler itself restores the cursor before
    // flagging shutdown. `write()` is async-signal-safe; `std::cerr`
    // is not. We also do a one-shot via std::atexit() so the cursor
    // is restored on a normal exit path too.
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
}

inline void install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = daedalus_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    // §Demo fix 2026-07-14: belt-and-braces cursor restore on
    // graceful exit (return from main). The signal handler covers
    // abnormal termination; atexit() covers the normal path.
    extern void daedalus_cli_restore_terminal_state() noexcept;
    std::atexit(daedalus_cli_restore_terminal_state);
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
    // `\x1b[?25h` = show cursor.
    // `\x1b[0m`   = reset all SGR attributes (colour, bold, etc.).
    // Together: leave the terminal exactly as the user found it.
    static constexpr char kSeq[] = "\x1b[?25h\x1b[0m";
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
    std::cout << "daedalus-cli — thin host for libdaedalus.a\n\n";
    std::cout << "Options:\n";
    std::cout << "  -h, --help          Show this help message and exit\n";
    std::cout << "  -v, --version       Show version information and exit\n";
    std::cout << "  --auto              Auto-approve bash/tool execution\n";
    std::cout << "  --no-memory         Disable session memory for this session\n";
    std::cout << "  --clear-memory      Clear all session memory and exit\n";
    std::cout << "  --home DIR          Use DIR as the daedalus home\n";
    std::cout << "  --model NAME        Use model NAME from the SQLite settings store\n";
    std::cout << "  --model-list        List available models and exit\n";
    std::cout << "  --agent-name NAME   Per-agent namespace for runtime state\n";
    std::cout << "  --threads N         Number of worker threads\n";
    std::cout << "  --stream            Enable SSE streaming (default)\n";
    std::cout << "  --no-stream         Disable SSE streaming\n";
    std::cout << "  --skills DIR        Load skills from DIR\n";
    std::cout << "  --system-prompt F   Override the demo's system_prompt.txt with F\n";
    std::cout << "\nArguments:\n";
    std::cout << "  directory       Target project directory (default: .)\n";
    std::cout << "\nIn-session commands:\n";
    std::cout << "  /quit, /exit    Exit\n";
    std::cout << "  /clear          Clear session state\n";
    std::cout << "  /models         List models\n";
    std::cout << "  /skills         List loaded skills\n";
    std::cout << "  /tools          List registered tools\n";
    std::cout << "  /usage          Show token/context usage summary\n";
    std::cout << "  /stats          Show raw token + iteration stats\n";
    std::cout << "  /state          Show current Agent state\n";
    std::cout << "  /help           Show this help\n";
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
        << "  Completion: " << st.completion_tokens << "\n"
        << "  Messages:   " << st.message_count << "\n"
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
    bool        stream_enabled      = true;
    std::string model_override;
    int         thread_count_override = 0;
    fs::path    target_path;
    fs::path    daedalus_home;
    std::vector<fs::path> skills_dirs;
    std::string agent_name;
    // §Phase 14: optional override of the system prompt source.
    // Empty = use the demo's bundled system_prompt.txt + per-project overlay.
    fs::path    system_prompt_file;
};

inline CliArgs parse_cli_args(int argc, char* argv[]) {
    CliArgs args;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if      (arg == "--auto")           args.auto_mode = true;
        else if (arg == "--no-memory")       args.no_memory = true;
        else if (arg == "--clear-memory")    args.clear_memory_flag = true;
        else if (arg == "--model-list")     args.model_list_only = true;
        else if (arg == "--stream")          args.stream_enabled = true;
        else if (arg == "--no-stream")      args.stream_enabled = false;
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
                try { args.thread_count_override = std::stoi(argv[++i]); }
                catch (...) {}
            }
        }
        else if (arg == "--skills") {
            if (i + 1 < argc) args.skills_dirs.emplace_back(argv[++i]);
            else { std::cerr << "Error: --skills requires a directory\n"; std::exit(1); }
        }
        else if (arg == "--system-prompt") {
            // §Phase 14: --system-prompt FILE overrides the default
            // system_prompt.txt (the per-demo sidecar). Useful for one-shot
            // experiments without recompiling or editing files.
            if (i + 1 < argc) args.system_prompt_file = argv[++i];
            else { std::cerr << "Error: --system-prompt requires a FILE\n"; std::exit(1); }
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
//   2. <demo sidecar>           <exe-dir>/system_prompt.txt
//   3. <per-project overlay>    <project_root>/.daedalus/system_prompt.txt
//      (concatenated AFTER the demo default — overrides win on conflict)
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

    // 2. Demo sidecar — next to the executable.
    if (override_file.empty()) {
        fs::path exe_dir;
#ifdef __linux__
        // /proc/self/exe is the running binary; resolve its directory.
        char self_path[4096] = {0};
        ssize_t n = ::readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
        if (n > 0) {
            self_path[n] = '\0';
            std::error_code ec;
            exe_dir = fs::weakly_canonical(fs::path(self_path).parent_path(), ec);
        }
#endif
        fs::path sidecar = exe_dir / "system_prompt.txt";
        if (!exe_dir.empty() && fs::exists(sidecar)) {
            std::ifstream f(sidecar);
            if (f) {
                std::ostringstream ss;
                ss << f.rdbuf();
                out += ss.str();
            }
        }
    }

    // 3. Per-project overlay (appended; can override any demo setting).
    if (!project_root.empty()) {
        fs::path overlay = project_root / ".daedalus" / "system_prompt.txt";
        if (fs::exists(overlay)) {
            std::ifstream f(overlay);
            if (f) {
                std::ostringstream ss;
                ss << f.rdbuf();
                // §Plan.md §1.1: project file is concatenated AFTER the demo
                // default. Empty separator is fine — the library adds its
                // own \n separators at section boundaries.
                out += "\n# Per-project system prompt overlay\n\n";
                out += ss.str();
            }
        }
    }

    return out;
}