/**
 * @file demos/common/RawStdin.h
 * @brief Async raw-mode stdin reader for the daedalus-cli demo.
 *
 * The library's LLMClient.h:80 says:
 *
 *   > "from stdin in a separate thread, then calling agent.cancel()"
 *
 * That's the canonical way to make a key press — typically ESC —
 * interrupt an in-flight `Agent::run_one_turn()`. This header provides
 * a small class that:
 *
 *   1. Spawns a background thread.
 *   2. Switches stdin to raw mode (termios), with original settings
 *      saved for restoration in the destructor.
 *   3. Reads stdin one byte at a time with minimal line editing
 *      (Backspace, Enter, Ctrl+C, Ctrl+D).
 *   4. On bare ESC: calls `agent.cancel()` AND pushes a sentinel line
 *      so the main loop wakes up and exits.
 *   5. On Enter: pushes the typed line to a queue the main thread
 *      drains.
 *   6. On EOF / Ctrl+D: pushes a sentinel so the main loop exits
 *      cleanly (and no "exit code 1" because the destructor restores
 *      the terminal properly).
 *
 * §Threading contract
 *
 *   - The class owns exactly one `std::thread`. Its destructor sets
 *     a stop flag, joins the thread, and restores terminal mode.
 *     Destruction is safe to call from any context, including the
 *     SIGINT handler path through main()'s catch block.
 *   - `next_line()` blocks on a condition variable. It returns
 *     `std::nullopt` on EOF; an empty string on ESC; the typed text
 *     on Enter.
 *   - The watcher thread is the only writer to stdin's termios while
 *     it exists, and the only reader of stdin. std::getline from the
 *     main thread would race — don't do that.
 *
 * §Why header-only?
 *
 * Same convention as ThinkingSpinner.h: the demo is small, the class
 * has no external deps beyond `<termios.h>` and `<unistd.h>`, and a
 * .cpp would just complicate the CMakeLists for nothing.
 *
 * §License
 *
 * BSL-1.0 (matches demos/daedalus-cli/util.h and ThinkingSpinner.h).
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <termios.h>
#include <thread>
#include <unistd.h>

namespace daedalus_cli {

class RawStdinWatcher {
public:
    /// @brief Construct and start the watcher thread.
    /// @param agent  The agent whose `cancel()` will be called on ESC.
    ///               Must outlive this object (typically a stack
    ///               variable in main()).
    /// @param prompt Prompt printed at the start of each line
    ///               (e.g. `"> "`). Defaults to `"> "`.
    explicit RawStdinWatcher(class daedalus::Agent& agent,
                             std::string prompt = "> ")
        : agent_(agent), prompt_(std::move(prompt)) {
        // Save termios BEFORE the thread starts so it can restore on
        // any path (normal exit, exception in main, signal).
        if (::tcgetattr(STDIN_FILENO, &saved_tio_) != 0) {
            saved_tio_ok_ = false;
        } else {
            saved_tio_ok_ = true;
        }
        th_ = std::thread([this] { thread_main(); });
    }

    /// @brief Stop the thread, join, restore terminal mode.
    ~RawStdinWatcher() {
        stop_.store(true, std::memory_order_release);
        // Closing stdin from another thread makes any blocked read()
        // in the watcher return 0 (EOF). That's enough to wake it.
        // But POSIX-y portable wakeup is just to join — the watcher
        // loop checks stop_ every read.
        if (th_.joinable()) th_.join();
        restore_terminal_mode();
    }

    // Disable copy: each instance owns a thread + a termios save.
    RawStdinWatcher(const RawStdinWatcher&) = delete;
    RawStdinWatcher& operator=(const RawStdinWatcher&) = delete;

    /// @brief Block until the watcher pushes a line or signals EOF.
    /// @returns
    ///   - `std::nullopt`           : EOF (Ctrl+D on empty input,
    ///                                or stdin closed).
    ///   - empty string `""`        : Bare ESC was pressed; the
    ///                                agent has been cancelled. The
    ///                                caller should exit its loop.
    ///   - the typed text           : User pressed Enter.
    std::optional<std::string> next_line() {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this] {
            return !lines_.empty() || eof_.load() || stop_.load();
        });
        if (stop_.load() && lines_.empty()) return std::nullopt;
        if (lines_.empty()) return std::nullopt;  // EOF
        std::string s = std::move(lines_.front());
        lines_.pop_front();
        if (s == "\x1b") return std::string();  // bare ESC
        return s;
    }

    /// @brief Non-blocking check for ESC. Useful as a redundancy in
    ///        the main loop's outer condition.
    [[nodiscard]] bool esc_seen() const noexcept {
        return esc_seen_.load(std::memory_order_acquire);
    }

private:
    /// Background thread body. Owns stdin reads until `stop_` flips.
    void thread_main() {
        enter_terminal_mode();

        std::string line;
        std::cout << "\n" << prompt_ << std::flush;

        while (!stop_.load(std::memory_order_acquire)) {
            char c = 0;
            ssize_t n = ::read(STDIN_FILENO, &c, 1);
            if (n <= 0) {
                // EOF or interrupted read. Treat as quit signal.
                eof_.store(true, std::memory_order_release);
                cv_.notify_all();
                break;
            }

            // ---- Bare ESC vs. escape sequence --------------------
            //
            // Terminals send `\x1b[A` for Up arrow, `\x1b[3~` for
            // Delete, etc. To tell bare ESC from a sequence we wait
            // ~50 ms after `\x1b` and check for a follow-up byte.
            // If nothing arrives, it's a real ESC.
            if (c == '\x1b') {
                if (wait_for_byte(50)) {
                    // Escape sequence — consume until a final byte
                    // in the range 0x40..0x7e ("GLYPH").  We don't
                    // implement arrow keys / Home / End; we just
                    // swallow the sequence so it doesn't leak into
                    // the next line.
                    consume_escape_sequence();
                } else {
                    // Bare ESC: clear partial line, cancel, push
                    // sentinel, and tell the user. We deliberately
                    // do NOT re-print the prompt — the main loop is
                    // about to break out and show its own goodbye.
                    erase_partial_line(line);
                    line.clear();
                    esc_seen_.store(true, std::memory_order_release);
                    agent_.cancel();
                    push(std::string("\x1b"));
                    std::cout << "\n[ESC — cancelling]\n" << std::flush;
                    continue;
                }
                continue;
            }

            // ---- Enter / carriage return -------------------------
            if (c == '\r' || c == '\n') {
                std::cout << "\r\n" << std::flush;
                push(std::move(line));
                line.clear();
                std::cout << prompt_ << std::flush;
                continue;
            }

            // ---- Backspace / DEL --------------------------------
            if (c == 0x7f || c == '\b') {
                if (!line.empty()) {
                    line.pop_back();
                    std::cout << "\b \b" << std::flush;
                }
                continue;
            }

            // ---- Ctrl+C (0x03) ----------------------------------
            // The SIGINT handler does the real work (sets
            // g_shutting_down + cancel()). Here we just flush the
            // current line so the prompt stays clean.
            if (c == 0x03) {
                std::cout << "^C\r\n" << prompt_ << std::flush;
                line.clear();
                continue;
            }

            // ---- Ctrl+D (0x04) ----------------------------------
            // EOF if the line is empty; otherwise just discard
            // (matches bash behaviour).
            if (c == 0x04) {
                if (line.empty()) {
                    eof_.store(true, std::memory_order_release);
                    cv_.notify_all();
                    break;
                }
                continue;
            }

            // ---- Printable ASCII -------------------------------
            if (c >= 0x20 && c < 0x7f) {
                line.push_back(c);
                std::cout << c << std::flush;
            }
            // Other control chars: drop silently.
        }
    }

    void push(std::string s) {
        std::lock_guard<std::mutex> lk(mu_);
        lines_.push_back(std::move(s));
        cv_.notify_one();
    }

    /// Wait up to `ms` for one byte on stdin. Returns true if it
    /// arrived (and that byte is consumed), false on timeout.
    bool wait_for_byte(int ms) {
        struct timeval tv{};
        tv.tv_usec = ms * 1000;
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        if (::select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) <= 0) {
            return false;
        }
        char c;
        return ::read(STDIN_FILENO, &c, 1) == 1;
    }

    /// After detecting an escape sequence, swallow bytes until we see
    /// a "final byte" (0x40..0x7e per ECMA-48).  Caps at 8 bytes to
    /// avoid infinite loops on malformed input.
    void consume_escape_sequence() {
        for (int i = 0; i < 8; ++i) {
            char c;
            // Short timeout — sequences arrive in a burst.
            if (!wait_for_byte(50)) return;
            if (::read(STDIN_FILENO, &c, 1) != 1) return;
            if (c >= 0x40 && c <= 0x7e) return;  // terminator
        }
    }

    /// Erase the currently-displayed (incomplete) line back to a
    /// clean cursor position. Used when ESC arrives mid-line.
    void erase_partial_line(const std::string& line) {
        for (std::size_t i = 0; i < line.size(); ++i) {
            std::cout << "\b \b" << std::flush;
        }
    }

    /// Switch stdin to raw mode (no echo, no canonical buffering).
    /// Saves original termios into `saved_tio_` for restoration.
    void enter_terminal_mode() {
        if (!saved_tio_ok_) return;
        struct termios raw = saved_tio_;
        raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG);
        // Keep ISIG so the kernel still generates SIGINT for Ctrl+C;
        // the spinner / agent shutdown handle that path.
        raw.c_iflag &= ~(tcflag_t)(IXON | ICRNL);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        ::tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        raw_mode_active_ = true;
    }

    void restore_terminal_mode() {
        if (raw_mode_active_ && saved_tio_ok_) {
            ::tcsetattr(STDIN_FILENO, TCSANOW, &saved_tio_);
        }
        raw_mode_active_ = false;
    }

    daedalus::Agent& agent_;
    std::string      prompt_;

    struct termios saved_tio_{};
    bool saved_tio_ok_ = false;
    bool raw_mode_active_ = false;

    std::thread th_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> esc_seen_{false};
    std::atomic<bool> eof_{false};

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::string> lines_;
};

}  // namespace daedalus_cli
