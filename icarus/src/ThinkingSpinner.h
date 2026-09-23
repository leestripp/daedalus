/**
 * @file src/ThinkingSpinner.h
 * @brief Header-only "Thinking..." animation for terminal-based demos.
 *
 * This file is a *reference implementation* — its purpose is twofold:
 *
 *   1. Be immediately useful: drop it into a demo's main loop to get a
 *      flicker-free "Thinking..." spinner that animates while the agent
 *      is waiting for the LLM.
 *
 *   2. Be educational: a developer who wants to roll their own spinner
 *      (different style, GUI framework integration, async/await variant,
 *      etc.) can read this file top-to-bottom and see all the moving
 *      pieces: background thread, atomic flag, condition_variable,
 *      ANSI escape codes, auto-stop via AgentSink events.
 *
 * \section usage Quick start
 *
 * \code
 * #include "common/ThinkingSpinner.h"
 *
 * ThinkingSpinner spinner;
 * spinner.start();
 * agent.run_one_turn(stop_token);     // blocks until LLM turns are done
 * // spinner auto-stops on first on_assistant_delta, or call:
 * spinner.stop();
 * \endcode
 *
 * \section design Design at a glance
 *
 *   - One background thread runs a 100 ms tick loop.
 *   - Each tick prints `\r` + frame N + space + custom message.
 *   - A `std::condition_variable` parks the thread when the spinner is
 *     not running, so the CPU cost when idle is essentially zero.
 *   - Sink-side auto-stop: hook into AgentSink::on_assistant_delta
 *     and/or on_state_change — first call marks a `stopped_` flag
 *     that the next tick honours.
 *   - Destruction joins the thread cleanly (`stop()` is idempotent).
 *
 * \section why Why not just print a static "Thinking..." line?
 *
 * Static lines are confusing — the user can't tell whether the program
 * is hung or just slow. Animation breaks the ambiguity and gives the
 * session a "live" feel. SSE-streamed tokens replace the spinner once
 * the first delta arrives, so the spinner never fights the stream
 * output.
 *
 * \section portability Terminal portability
 *
 * The implementation uses only:
 *   - `\r` (carriage return) to overwrite the current line
 *   - `\x1b[K` (ANSI "Erase to End of Line") so leftover characters
 *     don't show when the spinner shrinks
 *   - Unicode braille characters for the spinning glyph (optional —
 *     a fallback ASCII set is used if the terminal can't do UTF-8)
 *
 * It does NOT clear the screen, move the cursor, or query the terminal.
 * Hosts can call `set_frames(...)` to swap in ASCII art (e.g. `|/-`)
 * for non-UTF-8 environments.
 *
 * \section thread_safety Thread safety
 *
 *   - `start()` and `stop()` are thread-safe with each other.
 *   - The background thread is the only writer to the output stream.
 *   - `notify_*()` hooks (which you wire into your AgentSink) may be
 *     called from the agent's worker thread; they atomically set
 *     `stopped_` and notify the background thread, which then exits
 *     cleanly on its next tick.
 *   - Destruction joins the thread before returning — no detached
 *     threads outlive the object.
 *
 * \section extension Extending
 *
 * Hosts that want a richer experience can subclass or compose this
 * with their own logic. The interesting knobs:
 *
 *   - `set_message(std::string)` — change the label, e.g. to show which
 *     state the agent is in ("Planning", "Acting"). Thread-safe; the new
 *     label appears on the next tick.
 *   - `set_frames(std::vector<std::string>)` — replace the braille
 *     pattern with whatever glyphs you want.
 *   - `set_interval(std::chrono::milliseconds)` — speed up / slow down.
 *   - `set_output(std::ostream&)` — redirect output (e.g. to a log
 *     file in `--verbose` mode).
 *
 * `frames` / `interval` / `output` are public fields configured once
 * before start(); only the message has a setter because it changes
 * while the worker thread is running.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <ostream>
#include <string>
#include <thread>
#include <vector>

class ThinkingSpinner {
public:
    /// @brief Default constructor. Creates an idle spinner.
    ThinkingSpinner() = default;

    /// @brief Destructor. Implicitly calls stop() and joins the thread.
    ~ThinkingSpinner() {
        stop();
    }

    // Disable copy: each spinner owns a thread + cv.
    ThinkingSpinner(const ThinkingSpinner&) = delete;
    ThinkingSpinner& operator=(const ThinkingSpinner&) = delete;

    // ------------------------------------------------------------------
    // Configuration (call BEFORE start() for the cleanest experience).
    // ------------------------------------------------------------------

    /// Custom message shown after the spinning glyph.
    /// Default: "Thinking..."
    ///
    /// The message is PRIVATE because the worker thread reads it on every
    /// tick while holding state_mutex_. Always change it via set_message()
    /// (also safe to call while the animation is running — the new label
    /// appears on the next tick).

    /// Frame sequence. Default: classic braille "spinner" used by
    /// Claude Code, Codex, and most modern CLIs. Override to use
    /// ASCII ("|/-") or your own glyphs if needed.
    std::vector<std::string> frames = {
        "\u280B", "\u2819", "\u2838", "\u2834",
        "\u2826", "\u2827", "\u2807", "\u280F"
    };

    /// Tick interval. Faster = more flickery; slower = looks stuck.
    /// 80-120 ms feels right for most terminals.
    std::chrono::milliseconds interval = std::chrono::milliseconds(100);

    /// Where the spinner writes. Default: std::cerr (since the agent
    /// streams text to std::cout in the CLI demo). Override to std::cout
    /// if you want them interleaved.
    std::ostream* output = &std::cerr;

    // ------------------------------------------------------------------
    // Public state to make AgentSink wiring a one-liner.
    // ------------------------------------------------------------------
    //
    //   struct MySink : daedalus::AgentSink {
    //       ThinkingSpinner& spinner;
    //       void on_assistant_delta(std::string_view) noexcept override {
    //           spinner.notify_assistant_delta();
    //       }
    //   };
    //
    // First call to either notify_ method stops the spinner. The agent
    // calls on_assistant_delta from its worker thread; the spinner's
    // background thread observes the flag and exits on the next tick.
    // ------------------------------------------------------------------

    /// @brief Hook for AgentSink::on_assistant_delta. Idempotent.
    ///
    /// Atomically sets `stopped_` and notifies the worker so it
    /// exits on its next tick. The thread is NOT joined here —
    /// joining happens in the next start() call, in stop(), or in
    /// the destructor. Joining on the agent's worker thread would
    /// be a deadlock risk (the worker might be holding state_mutex_
    /// mid-callback), so we leave cleanup for later.
    void notify_assistant_delta() noexcept {
        if (!stopped_.load(std::memory_order_acquire)) {
            stopped_.store(true, std::memory_order_release);
            std::lock_guard<std::mutex> lk(state_mutex_);
            cv_.notify_all();
        }
    }

    /// @brief Hook for AgentSink::on_state_change.
    /// @param to_state New state (cast-to-int AgentState value).
    /// Stops the spinner when the agent enters a terminal-like state
    /// that doesn't produce streaming text (Done, Error, Cancelled,
    /// AwaitingApproval, Compressing).
    void notify_state_change(int /*from*/, int to_state) noexcept {
        // daedalus::AgentState enum values (cast to int), from
        // daedalus/types/AgentState.h — keep in sync when daedalus
        // adds/reorders states:
        //   0=Idle, 1=Understanding, 2=Planning, 3=Acting,
        //   4=Verifying, 5=Documenting, 6=AwaitingApproval,
        //   7=Compressing, 8=Done, 9=Cancelled, 10=Error
        constexpr int kAwaitingApproval = 6;
        constexpr int kCompressing = 7;
        constexpr int kDone = 8;
        constexpr int kCancelled = 9;
        constexpr int kError = 10;

        if (to_state == kAwaitingApproval
            || to_state == kCompressing || to_state == kDone
            || to_state == kCancelled || to_state == kError) {
            if (!stopped_.load(std::memory_order_acquire)) {
                stopped_.store(true, std::memory_order_release);
                std::lock_guard<std::mutex> lk(state_mutex_);
                cv_.notify_all();
            }
        }
    }

    // ------------------------------------------------------------------
    // Lifecycle.
    // ------------------------------------------------------------------

    /// @brief Set the label drawn after the spinning glyph. Thread-safe:
    /// takes state_mutex_, which the worker holds while drawing and
    /// releases while waiting, so the new label shows on the next tick.
    /// Safe to call before start() or mid-animation.
    void set_message(std::string msg) {
        std::lock_guard<std::mutex> lk(state_mutex_);
        message_ = std::move(msg);
    }

    /// @brief Start the animation. Idempotent.
    ///
    /// If a previous run was auto-stopped via notify_assistant_delta
    /// (or notify_state_change) and the worker thread exited but was
    /// never joined, we join it here. We do NOT join a still-running
    /// worker — that would deadlock waiting for the thread to exit.
    void start() {
        // Only join a worker that has already exited.
        if (worker_.joinable() && exited_.load(std::memory_order_acquire)) {
            worker_.join();
        }

        running_.store(true, std::memory_order_release);
        stopped_.store(false, std::memory_order_release);
        exited_.store(false, std::memory_order_release);

        // If we already had a live worker (start() called twice without
        // an intervening stop or notify), leave it alone — it will pick
        // up the reset flags on its next tick.
        if (!worker_.joinable()) {
            worker_ = std::thread([this] { run(); });
        }
    }

    /// @brief Stop the animation. Idempotent. Joins the worker thread
    /// before returning so the function is safe to call from a
    /// destructor or signal handler context.
    void stop() {
        if (!running_.exchange(false, std::memory_order_acq_rel)) {
            return;  // Already stopped.
        }
        // Wake the worker out of its wait so it can see the new flag.
        {
            std::lock_guard<std::mutex> lk(state_mutex_);
            cv_.notify_all();
        }
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    /// @brief True if the animation is currently visible. Returns false
    /// once a notify_* hook has signalled auto-stop, even though the
    /// worker thread hasn't been joined yet.
    [[nodiscard]] bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire)
            && !stopped_.load(std::memory_order_acquire);
    }

private:
    /// @brief Worker loop. Runs on its own std::thread.
    ///
    /// Uses a condition_variable to park between ticks so an idle
    /// spinner consumes near-zero CPU. The wait_for returns early on
    /// either a stop signal OR when stopped_ flips true (so the
    /// animation ends as soon as the first delta arrives).
    void run() {
        std::unique_lock<std::mutex> lk(state_mutex_);

        // Hide the cursor while we're animating (ANSI).
        // Hosts that don't want this can wrap the body in #ifdef.
        if (output && *output) {
            (*output) << "\x1b[?25l" << std::flush;
        }

        std::size_t frame_idx = 0;

        while (running_.load(std::memory_order_acquire)) {
            // Skip if a notify_* hook asked us to stop.
            if (stopped_.load(std::memory_order_acquire)) {
                break;
            }

            // Draw current frame. \r returns to start of line; \x1b[K
            // erases to end of line so leftover characters don't show
            // when the frame shrinks.
            if (output && *output && !frames.empty()) {
                const std::string& glyph = frames[frame_idx % frames.size()];
                (*output) << '\r' << glyph << ' ' << message_ << "\x1b[K"
                          << std::flush;
                ++frame_idx;
            }

            // Sleep for `interval`, but wake early on stop signal.
            cv_.wait_for(lk, interval, [this] {
                return !running_.load(std::memory_order_acquire)
                    || stopped_.load(std::memory_order_acquire);
            });
        }

        // Clear the line and restore the cursor.
        if (output && *output) {
            (*output) << "\r\x1b[K\x1b[?25h" << std::flush;
        }

        // Signal start() / stop() that the thread is done and safe to join.
        exited_.store(true, std::memory_order_release);
    }

    // ------------------------------------------------------------------
    // Internal state.
    //
    // Mutex order: state_mutex_ protects cv_ and is taken by the
    // background thread during wait_for(). The notify_* methods also
    // take it briefly to perform the notify_all. We use a separate
    // mutex rather than the atomic alone because std::condition_variable
    // needs a real mutex.
    // ------------------------------------------------------------------
    std::atomic<bool> running_{false};   // True while animation is live.
    std::atomic<bool> stopped_{false};   // True once a notify_* hook fired.
    std::atomic<bool> exited_{false};    // True once worker thread returned.
    std::mutex state_mutex_;
    std::condition_variable cv_;
    std::thread worker_;

    /// Label drawn after the glyph. Guarded by state_mutex_: the worker
    /// reads it while holding the lock, set_message() writes it under the
    /// same lock. Never access directly from outside run().
    std::string message_ = "Thinking...";
};

// ======================================================================
// §Additional reading
// ======================================================================
//
// If you're new to the moving parts, the canonical resources are:
//
//   - std::thread / std::jthread: how to start a background loop and
//     guarantee it joins before destruction.
//     https://en.cppreference.com/w/cpp/thread/thread
//
//   - std::condition_variable::wait_for: the canonical idiom for a
//     tick-loop with an early-exit predicate.
//     https://en.cppreference.com/w/cpp/thread/condition_variable/wait_for
//
//   - ANSI escape codes reference (focus on CSI sequences like \x1B[?25l
//     to hide the cursor, \x1B[K to erase to end of line).
//     https://en.wikipedia.org/wiki/ANSI_escape_code
//
//   - The agent's sink contract (Phase 6 / Phase 9.6):
//     https://github.com/leestripp/daedalus/blob/main/include/daedalus/AgentSink.h
//
