#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace monkeybyte {

// Server-Sent Events client over libcurl (design §7.7, §10.1).
// Parses "event: <name>" / "data: <json>" pairs and dispatches on a callback.
// Runs on its own thread; callbacks arrive on that thread — the receiver
// must bounce UI work to the GTK main loop (Glib::signal_idle).
class EventSourceClient {
public:
    using EventCallback = std::function<void(const std::string& event,
                                             const std::string& data)>;

    EventSourceClient() = default;
    ~EventSourceClient();

    EventSourceClient(const EventSourceClient&) = delete;
    EventSourceClient& operator=(const EventSourceClient&) = delete;

    // Starts streaming `url` (blocking curl on a worker thread).
    // on_event fires per SSE event; on_close fires once when the stream ends
    // (normally or via close()).
    void start(const std::string& url, EventCallback on_event,
               std::function<void()> on_close = {});
    void close(); // aborts the transfer and joins the thread
    bool running() const { return running_.load(); }

private:
    void run(std::string url);
    void feed(const char* data, size_t len); // incremental SSE parser
    static size_t stream_cb(char* ptr, size_t size, size_t nmemb, void* userdata);

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_{false};
    EventCallback on_event_;
    std::function<void()> on_close_;

    // parser state
    std::string buffer_;
    std::string pending_event_;
    std::string pending_data_;
};

} // namespace monkeybyte
