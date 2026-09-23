// SPDX-License-Identifier: BSL-1.0
// src/ui/InputQueue.h
//
// Thread-safe queue used to pass user input from the dedicated input
// reader thread to the Agent Loop. Items are produced by the input
// thread and consumed (popped) by the main thread.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

namespace icarus::ui {

enum class InputItemType {
    Message, // Normal user text (commands or chat).
    Cancel,  // ESC / empty-line cancel signal.
    Eof,     // Ctrl+D / EOF; tells the loop to shut down.
};

struct InputItem {
    InputItemType type;
    std::string text; // Valid when type == Message.
};

// Thread-safe producer/consumer queue for InputItems.
class InputQueue {
public:
    InputQueue() = default;

    // Push an item to the back of the queue and wake a waiting consumer.
    void push(InputItem item) {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            items_.push_back(std::move(item));
        }
        cv_.notify_one();
    }

    // Pop the front item. Blocks until an item is available, the timeout
    // expires, or the queue is requested to wake early. Returns std::nullopt
    // only on timeout/early wake with no items.
    std::optional<InputItem> pop(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mutex_);
        const bool got = cv_.wait_for(lk, timeout, [this] {
            return !items_.empty() || wake_early_.load(std::memory_order_acquire);
        });
        if (!got || items_.empty()) {
            wake_early_.store(false, std::memory_order_release);
            return std::nullopt;
        }
        InputItem item = std::move(items_.front());
        items_.pop_front();
        return item;
    }

    // Remove all queued Message items. Cancel/Eof items are preserved so
    // the loop still sees them.
    void clear_messages() {
        std::lock_guard<std::mutex> lk(mutex_);
        std::deque<InputItem> kept;
        for (auto& item : items_) {
            if (item.type != InputItemType::Message) {
                kept.push_back(std::move(item));
            }
        }
        items_ = std::move(kept);
    }

    // Wake a waiting pop() early (used during shutdown).
    void wake() {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            wake_early_.store(true, std::memory_order_release);
        }
        cv_.notify_all();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return items_.empty();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return items_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<InputItem> items_;
    std::atomic<bool> wake_early_{false};
};

} // namespace icarus::ui
