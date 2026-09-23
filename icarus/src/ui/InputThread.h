// SPDX-License-Identifier: BSL-1.0
// src/ui/InputThread.h
//
// Dedicated thread that reads user input via InputReader and pushes
// InputItems into an InputQueue. The thread can be paused so the main
// thread can perform synchronous prompts, and it invokes a cancel
// callback when the user presses ESC (or returns an empty line).

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>

#include "ui/InputQueue.h"
#include "ui/InputReader.h"

namespace icarus::ui {

class InputThread {
public:
    using CancelCallback = std::function<void()>;

    InputThread() = default;

    // Construct and start the reader thread.
    // `shutting_down` is an external atomic flag (e.g. g_shutting_down)
    // that the thread checks each loop iteration.
    // If `start_paused` is true the thread parks immediately so the owning
    // thread can control when the first prompt is drawn.
    InputThread(InputReader& reader,
                InputQueue& queue,
                const std::atomic<bool>& shutting_down,
                CancelCallback on_cancel = nullptr,
                bool start_paused = false)
        : reader_(&reader)
        , queue_(&queue)
        , shutting_down_(&shutting_down)
        , on_cancel_(std::move(on_cancel)) {
        if (start_paused) {
            pause_requested_.store(true, std::memory_order_release);
        }
        thread_ = std::jthread([this](std::stop_token st) { run(std::move(st)); });
    }

    // Stop the thread and wait for it to finish.
    ~InputThread() {
        stop();
    }

    InputThread(const InputThread&) = delete;
    InputThread& operator=(const InputThread&) = delete;
    InputThread(InputThread&&) = delete;
    InputThread& operator=(InputThread&&) = delete;

    // Start the reader thread. Must not be already running.
    void start(InputReader& reader,
               InputQueue& queue,
               const std::atomic<bool>& shutting_down,
               CancelCallback on_cancel = nullptr) {
        stop();
        reader_ = &reader;
        queue_ = &queue;
        shutting_down_ = &shutting_down;
        on_cancel_ = std::move(on_cancel);
        stop_requested_.store(false, std::memory_order_release);
        pause_requested_.store(false, std::memory_order_release);
        parked_.store(false, std::memory_order_release);
        exited_.store(false, std::memory_order_release);
        thread_ = std::jthread([this](std::stop_token st) { run(std::move(st)); });
    }

    // Request stop and join the reader thread. Safe to call multiple times.
    void stop() {
        stop_requested_.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(pause_mutex_);
            pause_cv_.notify_all();
        }
        if (thread_.joinable()) {
            thread_.request_stop();
            if (queue_) queue_->wake();
            thread_.join();
        }
    }

    // Temporarily stop reading so the owning thread can use the same
    // InputReader synchronously (e.g. the command-approval prompt).
    // The thread returns from its current read as soon as the poll
    // interval expires and then parks.
    void pause() {
        {
            std::lock_guard<std::mutex> lk(pause_mutex_);
            pause_requested_.store(true, std::memory_order_release);
        }
        if (queue_) queue_->wake();
        std::unique_lock<std::mutex> lk(pause_mutex_);
        pause_cv_.wait(lk, [this] {
            return parked_.load(std::memory_order_acquire) ||
                   exited_.load(std::memory_order_acquire) ||
                   stop_requested_.load(std::memory_order_acquire) ||
                   !thread_.joinable();
        });
    }

    // Resume reading after pause().
    void resume() {
        {
            std::lock_guard<std::mutex> lk(pause_mutex_);
            pause_requested_.store(false, std::memory_order_release);
            // §Settings TUI deadlock fix (2026-07-30): do NOT clear
            // parked_ here. park() owns that flag (set on entry, cleared
            // on exit, both under pause_mutex_). Clearing it in resume()
            // races with a handler that resumes and immediately pauses
            // again (e.g. /settings running a synchronous read followed by
            // the REPL header redraw): if pause() re-sets pause_requested_
            // before the reader thread re-checks park()'s wait predicate,
            // the thread stays in park() with parked_ == false while
            // pause() waits for parked_ == true — a cv deadlock.
        }
        pause_cv_.notify_all();
    }

    bool running() const {
        return thread_.joinable();
    }

private:
    void park(std::unique_lock<std::mutex>& lk) {
        parked_.store(true, std::memory_order_release);
        pause_cv_.notify_all();
        pause_cv_.wait(lk, [this] {
            return !pause_requested_.load(std::memory_order_acquire) ||
                   stop_requested_.load(std::memory_order_acquire);
        });
        parked_.store(false, std::memory_order_release);
    }

    void run(std::stop_token st) {
        // Poll interval for the stop-aware read. Short enough that stop()
        // / pause() join promptly, long enough to avoid busy-waiting.
        constexpr auto kPollInterval = std::chrono::milliseconds(50);

        while (!stop_requested_.load(std::memory_order_acquire) &&
               !shutting_down_->load(std::memory_order_acquire) &&
               !st.stop_requested()) {
            {
                std::unique_lock<std::mutex> lk(pause_mutex_);
                if (pause_requested_.load(std::memory_order_acquire)) {
                    park(lk);
                    continue;
                }
            }

            auto maybe = reader_->read_line_with_stop("> ", kPollInterval, stop_requested_,
                                                       pause_requested_);
            if (!maybe) {
                // Stop/pause takes precedence over EOF handling.
                {
                    std::unique_lock<std::mutex> lk(pause_mutex_);
                    if (pause_requested_.load(std::memory_order_acquire)) {
                        park(lk);
                        continue;
                    }
                }
                if (stop_requested_.load(std::memory_order_acquire) ||
                    shutting_down_->load(std::memory_order_acquire) ||
                    st.stop_requested()) {
                    break;
                }
                // Otherwise the user sent EOF (Ctrl+D on empty buffer).
                queue_->push({InputItemType::Eof, {}});
                break;
            }

            const std::string& line = *maybe;
            if (line.empty()) {
                // ESC or bare Enter. Treat as cancel signal.
                if (on_cancel_) {
                    on_cancel_();
                }
                queue_->push({InputItemType::Cancel, {}});
            } else {
                queue_->push({InputItemType::Message, line});
            }
            // Ask the main thread to park us. We cannot call pause() ourselves
            // because pause() waits until we are parked, which would deadlock.
            {
                std::lock_guard<std::mutex> lk(pause_mutex_);
                pause_requested_.store(true, std::memory_order_release);
            }
        }

        // If the main thread is waiting in pause(), wake it. The thread is
        // exiting, so any synchronous prompt must abort.
        {
            std::lock_guard<std::mutex> lk(pause_mutex_);
            exited_.store(true, std::memory_order_release);
            if (pause_requested_.load(std::memory_order_acquire)) {
                parked_.store(true, std::memory_order_release);
            }
        }
        pause_cv_.notify_all();
    }

    InputReader* reader_ = nullptr;
    InputQueue* queue_ = nullptr;
    const std::atomic<bool>* shutting_down_ = nullptr;
    CancelCallback on_cancel_;

    std::jthread thread_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> pause_requested_{false};
    std::atomic<bool> parked_{false};
    std::atomic<bool> exited_{false};
    mutable std::mutex pause_mutex_;
    std::condition_variable pause_cv_;
};

} // namespace icarus::ui
