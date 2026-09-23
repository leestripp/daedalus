// SPDX-License-Identifier: BSL-1.0
// src/ui/InputReader.h
//
// Thin RAII wrapper around Salvatore Sanfilippo's linenoise library
// (fetched via CMake FetchContent, vendored at build time). See
// CodeMax.md §16 for the rationale.
//
// Provides:
//   - read_line() with optional '\' continuation for multi-line input
//   - persistent history at $XDG_DATA_HOME/icarus/input_history.txt
//   - hints + tab-completion for in-session slash commands, driven by
//     the canonical detail::kSlashCommands table below
//
// Linenoise is single-threaded (it stores file-scope state for the
// completion / hints callbacks and the global history buffer), so
// InputReader is also single-threaded. Create one per session, use it
// from the main thread only.
//
// §Why header-only?
// Same convention as ThinkingSpinner.h: small class, no external deps
// beyond <linenoise.h>, and a .cpp would just complicate CMakeLists.

#pragma once

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <ostream>
#include <poll.h>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#ifdef __linux__
#include <sys/ioctl.h>
#endif

#include <linenoise.h>

#include "ui/AnsiRenderer.h"

namespace icarus::ui {

// =============================================================================
// §Detail
// =============================================================================
namespace detail {

// Global (per-process) C-style trampolines. Linenoise stores function
// pointers at file scope, so we route through these to reach the
// C++ state installed via set_completion() and the hints config.
//
// All of these are inline so multiple translation units that include
// this header share one slot each.
inline std::function<std::vector<std::string>(std::string_view)>
    g_completion_fn;

inline void completion_trampoline(const char* buf, linenoiseCompletions* lc) {
    if (!g_completion_fn) return;
    for (const auto& m : g_completion_fn(buf)) {
        ::linenoiseAddCompletion(lc, m.c_str());
    }
}

inline char* hints_trampoline(const char* buf, int* color, int* bold);

// Defined next to the canonical command table below; registered by the
// InputReader constructor.
inline std::vector<std::string> slash_command_completions(std::string_view buf);

// hints are strdup()'d strings — linenoise frees them via the
// freeHintsCallback slot we register with linenoiseSetFreeHintsCallback.
inline void hints_free_trampoline(void* p) {
    std::free(p);
}

// §ESC-fix: forward bytes from fd_in (the tty) to fd_out (the pipe
// linenoise reads), detecting a bare ESC keypress.
//
// A chunk whose tail is a lone ESC byte is held back for a 60 ms grace
// window: escape sequences arrive atomically, so if nothing follows the
// ESC it was a bare keypress — set bare_esc and drop the byte instead
// of forwarding it (forwarding would strand linenoise in its blocking
// escape-sequence reads). pump_done is always set on exit so the poll
// loop can notice stdin EOF underneath the shim. Exits promptly when
// pump_stop is set (all polls are ≤ 60 ms).
inline void stdin_pump(int fd_in, int fd_out,
                       std::atomic<bool>& bare_esc,
                       std::atomic<bool>& pump_done,
                       std::atomic<bool>& pump_stop) {
    bool hold_esc = false;
    while (!pump_stop.load(std::memory_order_acquire)) {
        struct ::pollfd pfd{};
        pfd.fd = fd_in;
        pfd.events = POLLIN;
        const int ready = ::poll(&pfd, 1, hold_esc ? 60 : 50);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) {
            if (hold_esc) {
                // Nothing followed the ESC in time: bare ESC keypress.
                bare_esc.store(true, std::memory_order_release);
                break;
            }
            continue;
        }

        char chunk[256];
        const ssize_t n = ::read(fd_in, chunk, sizeof(chunk));
        if (n <= 0) break;  // EOF or error — poll loop sees pump_done
        std::size_t len = static_cast<std::size_t>(n);

        if (hold_esc) {
            // Bytes followed the held ESC: it opens a real escape
            // sequence. Forward it before the new chunk.
            const char esc = '\x1b';
            if (::write(fd_out, &esc, 1) != 1) break;
            hold_esc = false;
        }
        if (chunk[len - 1] == '\x1b') {
            hold_esc = true;
            --len;
        }
        for (std::size_t off = 0; off < len;) {
            const ssize_t w = ::write(fd_out, chunk + off, len - off);
            if (w < 0) {
                if (errno == EINTR) continue;
                pump_done.store(true, std::memory_order_release);
                return;
            }
            off += static_cast<std::size_t>(w);
        }
    }
    pump_done.store(true, std::memory_order_release);
}

} // namespace detail

// =============================================================================
// §InputReader
// =============================================================================
class InputReader {
public:
    struct Config {
        std::filesystem::path history_file;            // empty → no persistence
        std::size_t           history_max       = 1000;
        bool                  multiline         = true;
        std::string_view      continuation_prompt = "... ";

        // Header rendered before each read_line() — see print_header().
        // Disabled automatically when stdin is not a TTY, but can be
        // forced off via show_header = false.
        bool                  show_header       = true;
        std::string_view      header_hint       = "Type /help for available commands";
        icarus::ui::RenderConfig header_render   = {};
    };

    explicit InputReader(Config cfg);
    ~InputReader();

    InputReader(const InputReader&)            = delete;
    InputReader& operator=(const InputReader&) = delete;
    InputReader(InputReader&&)                 = delete;
    InputReader& operator=(InputReader&&)      = delete;

    // Read one logical line from the user.
    //
    // Return value:
    //   - std::nullopt   → EOF (Ctrl+D on empty buffer). Exit signal.
    //   - empty string   → bare Enter or ESC. Skip-the-turn signal.
    //   - non-empty      → the typed line. With cfg.multiline = true,
    //                      trailing unescaped '\' triggers continuation;
    //                      joined lines are separated by '\n'.
    std::optional<std::string> read_line(std::string_view prompt);

    // Single-line read (no continuation). Used for approval prompts.
    std::optional<std::string> read_line_single(std::string_view prompt);

    // Stop-aware reads. Used by the background input thread so it can be
    // interrupted on shutdown or when the main thread needs to perform a
    // synchronous prompt. The implementation polls stdin with the supplied
    // interval and checks both flags so the thread does not block
    // indefinitely inside linenoise.
    std::optional<std::string> read_line_with_stop(
        std::string_view prompt,
        std::chrono::milliseconds poll_interval,
        const std::atomic<bool>& stop_requested,
        const std::atomic<bool>& interrupt_requested);

    // Adapter: returns a std::function suitable for injection into
    // ConsoleAgentSink::set_approval_reader().
    using LineReader = std::function<std::optional<std::string>(std::string_view)>;
    LineReader approval_reader();

    // History controls. load_history() / save_history() are best-effort
    // — they silently ignore "file missing" and other recoverable
    // errors. add_history() lets the caller explicitly record a
    // command that wasn't entered through read_line() (e.g. seeded
    // startup commands).
    void load_history();
    void save_history();
    static void add_history(std::string_view line);

    // Tab-completion hook. The constructor registers the default
    // slash-command completer (detail::kSlashCommands); call this to
    // override it. std::function so capturing lambdas are accepted.
    using CompletionFn = std::function<std::vector<std::string>(std::string_view buf)>;
    static void set_completion(CompletionFn fn);

    // Write the input header (hint line + horizontal rule) to `os`.
    // Caller invokes this *before* read_line() so the header is
    // rendered while stdout is still in cooked mode — linenoise
    // switches the terminal to raw mode as part of the first
    // linenoise() call, so anything written after that fights the
    // cursor positioning of the live edit line.
    //
    // In non-TTY mode the header is suppressed automatically (the
    // REPL is running unattended via stdin pipe — decorative
    // separators would just spam the consumer's stream). Set
    // cfg.show_header = true to force it on regardless.
    void print_header(std::ostream& os = std::cout) const;

private:
    Config cfg_;

    // Buffer for bytes already read from stdin but not yet returned as a
    // complete line. Preserved across read_line_async() calls so that a
    // pause()/resume() cycle does not drop partially-typed or piped input.
    std::string pending_input_;

    std::optional<std::string> read_line_impl(std::string_view prompt,
                                             bool multiline);

    // Low-level non-blocking read using linenoiseEditStart/Feed/Stop.
    // Polls stdin with `poll_interval` and returns early if either
    // `stop_requested` or `interrupt_requested` becomes true. Returns
    // std::nullopt for EOF, stop, interrupt, or error.
    std::optional<std::string> read_line_async(
        std::string_view prompt,
        std::chrono::milliseconds poll_interval,
        const std::atomic<bool>& stop_requested,
        const std::atomic<bool>& interrupt_requested);
};

// =============================================================================
// §Implementation
// =============================================================================
inline InputReader::InputReader(Config cfg)
    : cfg_(std::move(cfg))
{
    // Multi-line is off by default in upstream; turn it on if requested.
    ::linenoiseSetMultiLine(cfg_.multiline ? 1 : 0);

    // History capacity. 0 disables history.
    if (cfg_.history_max > 0) {
        ::linenoiseHistorySetMaxLen(static_cast<int>(cfg_.history_max));
    }

    // Hints: register free callback first (so the hint strings we
    // strdup() are freed with std::free — same allocator linenoise
    // uses by default for malloc()), then the hints callback itself.
    static const bool hints_registered = []{
        ::linenoiseSetFreeHintsCallback(&detail::hints_free_trampoline);
        ::linenoiseSetHintsCallback(&detail::hints_trampoline);
        return true;
    }();
    (void)hints_registered;

    // Tab-completion for in-session slash commands (canonical table in
    // the detail namespace below).
    set_completion(&detail::slash_command_completions);

    load_history();
}

inline InputReader::~InputReader() {
    save_history();
}

inline void InputReader::load_history() {
    if (cfg_.history_file.empty()) return;
    std::error_code ec;
    if (!std::filesystem::exists(cfg_.history_file, ec)) return;
    // linenoiseHistoryLoad returns -1 on error (incl. missing file,
    // which we just filtered). Best-effort.
    ::linenoiseHistoryLoad(cfg_.history_file.string().c_str());
}

inline void InputReader::save_history() {
    if (cfg_.history_file.empty()) return;
    if (auto parent = cfg_.history_file.parent_path(); !parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        // Silently ignore — save_history will report the real error.
    }
    ::linenoiseHistorySave(cfg_.history_file.string().c_str());
    // Best-effort.
}

inline void InputReader::add_history(std::string_view line) {
    if (line.empty()) return;
    // linenoiseHistoryAdd takes a null-terminated string. Build a
    // std::string to get a guaranteed-owned nul terminator.
    const std::string s{line};
    ::linenoiseHistoryAdd(s.c_str());
}

inline std::optional<std::string> InputReader::read_line(std::string_view prompt) {
    return read_line_impl(prompt, cfg_.multiline);
}

inline std::optional<std::string> InputReader::read_line_single(std::string_view prompt) {
    return read_line_impl(prompt, /*multiline=*/false);
}

inline std::optional<std::string> InputReader::read_line_impl(std::string_view prompt,
                                                              bool multiline)
{
    std::string accumulated;
    std::string  current_prompt{prompt};

    while (true) {
        char* raw = ::linenoise(current_prompt.c_str());
        if (raw == nullptr) {
            // EOF. If the user already built up a multi-line buffer
            // and then hit Ctrl+D, return what we have. Otherwise
            // this is the "leave the REPL" signal.
            if (!accumulated.empty()) {
                if (accumulated.back() == '\n') accumulated.pop_back();
                return accumulated;
            }
            return std::nullopt;
        }
        std::string line{raw};
        ::linenoiseFree(raw);

        if (!multiline) {
            return line;
        }

        // Multi-line rule: a line whose last character is an
        // unescaped '\' requests a continuation. For v1 we do not
        // try to track leading '\\' escapes — typing a literal
        // backslash at end-of-line therefore needs to be entered
        // with an extra space or followed by another character. A
        // future task can grow escape tracking (see CodeMax.md
        // §16.7 open-decision #1).
        const bool continues =
            !line.empty() && line.back() == '\\';
        if (!continues) {
            accumulated += line;
            if (!accumulated.empty() && accumulated.back() == '\n') {
                accumulated.pop_back();
            }
            return accumulated;
        }

        // Continuation: strip the trailing '\' and accumulate a
        // physical newline. Subsequent iterations show the
        // continuation prompt until the user closes the input
        // without a trailing '\'.
        line.pop_back();
        accumulated += line;
        accumulated += '\n';
        current_prompt.assign(cfg_.continuation_prompt.data(),
                              cfg_.continuation_prompt.size());
    }
}

inline std::optional<std::string> InputReader::read_line_async(
    std::string_view prompt,
    std::chrono::milliseconds poll_interval,
    const std::atomic<bool>& stop_requested,
    const std::atomic<bool>& interrupt_requested)
{
    const int timeout_ms = static_cast<int>(
        std::min(poll_interval, std::chrono::milliseconds(INT_MAX)).count());

    // Non-TTY (pipes, files): linenoiseEditStart fails because it tries to
    // enable raw mode. Read a line manually with poll so the thread remains
    // responsive to stop requests. Partial input is kept in pending_input_
    // across calls so pause()/resume() cycles do not drop characters.
    const char* assume_tty = std::getenv("LINENOISE_ASSUME_TTY");
    const bool force_tty = assume_tty != nullptr && assume_tty[0] != '\0';
    if (!::isatty(STDIN_FILENO) && !force_tty) {
        auto extract_line = [this]() -> std::optional<std::string> {
            if (auto nl = pending_input_.find('\n'); nl != std::string::npos) {
                std::string line = pending_input_.substr(0, nl);
                pending_input_.erase(0, nl + 1);
                return line;
            }
            return std::nullopt;
        };

        while (!stop_requested.load(std::memory_order_acquire) &&
               !interrupt_requested.load(std::memory_order_acquire)) {
            if (auto line = extract_line()) {
                return line;
            }

            struct ::pollfd pfd{};
            pfd.fd = STDIN_FILENO;
            pfd.events = POLLIN;

            const int ready = ::poll(&pfd, 1, timeout_ms);
            if (ready < 0) {
                if (errno == EINTR) continue;
                return std::nullopt;
            }
            if (ready == 0) continue;

            char c;
            const ssize_t n = ::read(STDIN_FILENO, &c, 1);
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                return std::nullopt;
            }
            if (n == 0) {
                // EOF. Return accumulated text, or nullopt on empty buffer.
                if (pending_input_.empty()) return std::nullopt;
                std::string line = std::move(pending_input_);
                pending_input_.clear();
                return line;
            }
            if (c == '\n') {
                std::string line = std::move(pending_input_);
                pending_input_.clear();
                return line;
            }
            if (c != '\r') {
                pending_input_.push_back(c);
            }
        }
        return std::nullopt;
    }

    // TTY: use linenoise's non-blocking edit API.
    constexpr std::size_t kInitialBuflen = 4096;
    constexpr std::size_t kMaxLine = 1024 * 1024;

    struct ::linenoiseState ls{};
    char* buf = static_cast<char*>(std::malloc(kInitialBuflen));
    if (!buf) return std::nullopt;

    const std::string prompt_str{prompt};
    if (::linenoiseEditStart(&ls, STDIN_FILENO, STDOUT_FILENO, buf,
                             kInitialBuflen,
                             prompt_str.c_str()) == -1) {
        std::free(buf);
        return std::nullopt;
    }
    // linenoiseEditStart() puts the tty in raw mode (OPOST off). Re-enable
    // output post-processing so the kernel expands '\n' to CR+NL; otherwise
    // any output written by the main thread while the input thread is
    // reading lands with the cursor not returning to column 0.
    {
        struct termios tio{};
        if (::tcgetattr(STDIN_FILENO, &tio) == 0) {
            tio.c_oflag |= (OPOST | ONLCR);
            ::tcsetattr(STDIN_FILENO, TCSANOW, &tio);
        }
    }
    ls.buflen_max = kMaxLine;

    // §ESC-fix: interpose a byte pump between stdin and linenoise.
    //
    // linenoise's `case ESC` (linenoise.c) issues two *blocking* read()s
    // for the escape-sequence bytes, so a bare ESC keypress strands the
    // reader inside read() until another key arrives — the cancel path
    // (empty line → InputThread on_cancel) never fires. Peeking a tty
    // byte without consuming it is not possible (TIOCSTI is disabled on
    // hardened kernels; FIONREAD reports a count only), so instead we:
    //
    //   1. create a raw-mode pty pair and point the public ls.ifd at
    //      the slave — linenoiseEditFeed() then reads our pty (a pipe
    //      would trip its isatty() check into the non-tty path), while
    //      rendering still goes to stdout and raw mode stays on the
    //      real tty;
    //   2. run a pump thread that forwards stdin bytes to the pty
    //      master, holding back a chunk that ends in a lone ESC for a
    //      60 ms grace window: if more bytes arrive it is a real escape
    //      sequence (forward, keep going); if not, it is a bare ESC
    //      keypress → set bare_esc and discard it;
    //   3. the poll loop below watches bare_esc and returns "" (the
    //      established cancel signal) when it fires.
    //
    // If the pty cannot be created we fall back to the direct-stdin
    // behaviour (ESC broken, but no regression).
    int esc_master = -1, esc_slave = -1;
    std::atomic<bool> bare_esc{false};
    std::atomic<bool> pump_done{false};
    std::atomic<bool> pump_stop{false};
    std::thread pump;
    {
        esc_master = ::posix_openpt(O_RDWR | O_NOCTTY);
        if (esc_master >= 0 &&
            (::grantpt(esc_master) != 0 || ::unlockpt(esc_master) != 0)) {
            ::close(esc_master);
            esc_master = -1;
        }
        if (esc_master >= 0) {
            esc_slave = ::open(::ptsname(esc_master), O_RDWR | O_NOCTTY);
            if (esc_slave < 0) {
                ::close(esc_master);
                esc_master = -1;
            } else {
                // Raw slave: the pump's bytes must reach linenoise
                // unmodified (no canonical line buffering, no echo).
                struct termios tio{};
                if (::tcgetattr(esc_slave, &tio) == 0) {
                    ::cfmakeraw(&tio);
                    ::tcsetattr(esc_slave, TCSANOW, &tio);
                }
            }
        }
    }
    const bool shim_ok = (esc_master >= 0 && esc_slave >= 0);
    if (shim_ok) {
        ls.ifd = esc_slave;
        pump = std::thread([fd_in = STDIN_FILENO, fd_out = esc_master,
                            &bare_esc, &pump_done, &pump_stop] {
            detail::stdin_pump(fd_in, fd_out, bare_esc, pump_done, pump_stop);
        });
    }

    std::optional<std::string> result;
    const int poll_fd = shim_ok ? esc_slave : STDIN_FILENO;
    while (!stop_requested.load(std::memory_order_acquire) &&
           !interrupt_requested.load(std::memory_order_acquire)) {
        if (bare_esc.load(std::memory_order_acquire)) {
            // Bare ESC keypress: cancel signal (empty line).
            result = std::string();
            break;
        }
        if (pump_done.load(std::memory_order_acquire)) {
            // stdin hit EOF/error underneath the shim.
            break;
        }
        struct ::pollfd pfd{};
        pfd.fd = poll_fd;
        pfd.events = POLLIN;

        const int ready = ::poll(&pfd, 1, timeout_ms);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) continue;

        char* res = ::linenoiseEditFeed(&ls);
        if (res == ::linenoiseEditMore) continue;
        if (res != nullptr) {
            result = std::string(res);
            ::linenoiseFree(res);
        }
        break;
    }

    if (shim_ok) {
        pump_stop.store(true, std::memory_order_release);
        if (pump.joinable()) pump.join();  // exits within one poll interval
        ::close(esc_master);
        ::close(esc_slave);
        // Restore the real tty fd so linenoiseEditStop() can disable
        // raw mode — on the pty slave fd its isatty() check would bail
        // out and leave the terminal in raw mode.
        ls.ifd = STDIN_FILENO;
    }

    ::linenoiseEditStop(&ls);
    std::free(ls.buf);
    return result;
}

inline std::optional<std::string> InputReader::read_line_with_stop(
    std::string_view prompt,
    std::chrono::milliseconds poll_interval,
    const std::atomic<bool>& stop_requested,
    const std::atomic<bool>& interrupt_requested)
{
    std::string accumulated;
    std::string current_prompt{prompt};

    while (!stop_requested.load(std::memory_order_acquire) &&
           !interrupt_requested.load(std::memory_order_acquire)) {
        auto maybe = read_line_async(current_prompt, poll_interval,
                                     stop_requested, interrupt_requested);
        if (!maybe) {
            // EOF, stop, or error. If the user already built up a
            // multi-line buffer and then hit Ctrl+D, return what we
            // have. Otherwise this is the "leave the REPL" signal.
            if (!accumulated.empty()) {
                if (accumulated.back() == '\n') accumulated.pop_back();
                return accumulated;
            }
            return std::nullopt;
        }

        std::string line = std::move(*maybe);
        if (!cfg_.multiline) {
            return line;
        }

        const bool continues = !line.empty() && line.back() == '\\';
        if (!continues) {
            accumulated += line;
            if (!accumulated.empty() && accumulated.back() == '\n') {
                accumulated.pop_back();
            }
            return accumulated;
        }

        line.pop_back();
        accumulated += line;
        accumulated += '\n';
        current_prompt.assign(cfg_.continuation_prompt.data(),
                              cfg_.continuation_prompt.size());
    }

    return std::nullopt;
}

inline InputReader::LineReader InputReader::approval_reader() {
    // Capture `this` by value: the only safe way to cross the
    // lambda → std::function boundary. ConsoleAgentSink holds the
    // resulting LineReader; InputReader outlives the sink because
    // main() owns both and tears down in the right order
    // (agent.shutdown() → sink dtor → InputReader dtor).
    return [this](std::string_view p) {
        return this->read_line_impl(p, /*multiline=*/false);
    };
}

inline void InputReader::set_completion(CompletionFn fn) {
    // Store the function in the global slot. Trampoline registration
    // happens once below. set_completion is called at most once per
    // session, so the implicit move/assign into the std::function
    // is enough — no allocation leak to worry about.
    detail::g_completion_fn = std::move(fn);

    static const bool registered = []{
        ::linenoiseSetCompletionCallback(&detail::completion_trampoline);
        return true;
    }();
    (void)registered;
}

// =============================================================================
// §Canonical in-session command table — single source of truth for both the
// hints ghost text and tab-completion. Command words only (no CLI flags,
// no argument completion).
// =============================================================================
namespace detail {

struct SlashCommand { std::string_view name; std::string_view summary; };

inline const SlashCommand kSlashCommands[] = {
    {"/quit",            "exit"},
    {"/exit",            "exit"},
    {"/help",            "show help"},
    {"/init",            "create or update AGENTS.md"},
    {"/clear",           "clear session state"},
    {"/compress",        "compress conversation context"},
    {"/continue",        "re-submit last user message"},
    {"/last",            "show last assistant reply"},
    {"/providers",       "list configured providers"},
    {"/provider-add",    "add provider: name host api_key|- [plugin]"},
    {"/provider-delete", "delete provider by name|#"},
    {"/provider",        "switch provider by name|#"},
    {"/models",          "discover models for current provider"},
    {"/model-add",       "add model: name provider model_id"},
    {"/model-delete",    "delete configured model by name|#"},
    {"/model",           "switch active model by name|#"},
    {"/skills",          "list loaded skills"},
    {"/tools",           "list registered tools"},
    {"/mcp-servers",     "list configured MCP servers"},
    {"/todo",            "manage todos"},
    {"/settings",        "open the interactive settings TUI"},
    {"/memory",          "show memory status"},
    {"/history",         "show recent messages"},
    {"/export",          "export session messages to JSON"},
    {"/tool",            "execute a registered tool directly"},
    {"/usage",           "token/context usage summary"},
    {"/stats",           "raw token + iteration stats"},
    {"/state",           "show current agent state"},
    {"/levels",          "list thinking levels for current model"},
    {"/level",           "set thinking level by name|#"},
    {"/paths",           "show all config/data/project paths"},
};

// True when the buffer is a command word still being typed: starts with
// '/' and contains no space (a space means the command is complete and
// the user is typing arguments — we don't complete/hint those).
inline bool is_command_word(std::string_view buf) {
    return buf.size() >= 1 && buf[0] == '/' && buf.find(' ') == std::string_view::npos;
}

// Tab-completion candidates: every canonical command starting with the
// typed prefix. linenoise replaces the whole buffer with the cycled
// candidate (completeLine() in linenoise.c), so candidates are full
// command strings.
inline std::vector<std::string> slash_command_completions(std::string_view buf) {
    std::vector<std::string> out;
    if (!is_command_word(buf)) return out;
    for (const auto& c : kSlashCommands) {
        if (c.name.starts_with(buf) && c.name != buf) {
            out.emplace_back(c.name);
        }
    }
    return out;
}

inline char* hints_trampoline(const char* buf, int* color, int* bold) {
    // Only show hints for a command word still being typed.
    if (buf == nullptr || !is_command_word(buf)) return nullptr;

    const std::string_view typed{buf};

    // Collect canonical commands matching the typed prefix.
    const SlashCommand* match = nullptr;
    std::size_t count = 0;
    for (const auto& c : kSlashCommands) {
        if (c.name.starts_with(typed)) {
            ++count;
            match = &c;
        }
    }
    if (count != 1) return nullptr;  // none, or ambiguous — suppress

    *color = 90;  // bright black
    *bold  = 0;

    // linenoise renders the hint string immediately after the buffer, so
    // for a strict prefix we return ONLY the remaining suffix — the ghost
    // text then completes the command visually ("/mo" + "dels" → "/models").
    if (match->name != typed) {
        return ::strdup(std::string{match->name.substr(typed.size())}.c_str());
    }

    // Exact command — show its summary as the ghost text.
    return ::strdup((std::string{" — "} + std::string{match->summary}).c_str());
}

} // namespace detail

inline void InputReader::print_header(std::ostream& os) const {
    if (!cfg_.show_header) return;
    // Suppress decoration when stdin is not a terminal — piped input
    // is being consumed by another program and doesn't want ANSI
    // escapes / decorative rules in its data stream. The caller can
    // still force it on by leaving cfg_.show_header = true with
    // stdin attached, or override it later.
    const char* assume_tty = std::getenv("LINENOISE_ASSUME_TTY");
    const bool force_tty = assume_tty != nullptr && assume_tty[0] != '\0';
    if (!::isatty(STDIN_FILENO) && !force_tty) {
        return;
    }

    const Style style(cfg_.header_render);

    // Hint line: ✨ in unicode mode, * in ASCII mode. Both prefixed
    // with a single glyph + space for visual anchoring.
    const std::string prefix =
        cfg_.header_render.unicode_enabled ? "✨ " : "* ";
    os << '\n' << style.dim(prefix) << style.dim(cfg_.header_hint) << '\n';

    // Horizontal rule. Use the renderer helper so the character
    // choice (─ vs -) follows cfg.unicode_enabled automatically.
    // Uses the live terminal width (resize-aware) so a narrow terminal
    // does not wrap the rule and leave the subsequent "> " prompt
    // indented on the continuation line.
    const std::size_t width = [] (const RenderConfig& rc) -> std::size_t {
        const int w = effective_terminal_width(rc);
        return w > 0 ? static_cast<std::size_t>(w) : std::size_t{80};
    }(cfg_.header_render);
    os << style.dim(::icarus::ui::hrule(width, '-', cfg_.header_render))
       << '\n';
    os.flush();
}

} // namespace icarus::ui
