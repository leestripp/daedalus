#include "monkeybyte/http/EventSourceClient.h"

#include "monkeybyte/ui/UiGuard.h"

#include <curl/curl.h>

namespace monkeybyte {

size_t EventSourceClient::stream_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* self = static_cast<EventSourceClient*>(userdata);
    const size_t len = size * nmemb;
    // feed() runs the on_event callback; an exception must never unwind
    // through libcurl's C frames — abort the transfer instead.
    ui_guard("EventSourceClient::stream_cb", [&] { self->feed(ptr, len); });
    return self->cancel_.load() ? 0 : len; // returning < len aborts the transfer
}

EventSourceClient::~EventSourceClient() { close(); }

void EventSourceClient::start(const std::string& url, EventCallback on_event,
                              std::function<void()> on_close) {
    close();
    on_event_ = std::move(on_event);
    on_close_ = std::move(on_close);
    cancel_ = false;
    running_ = true;
    buffer_.clear();
    pending_event_.clear();
    pending_data_.clear();
    ui_guard("EventSourceClient::start",
             [&] { thread_ = std::thread(&EventSourceClient::run, this, url); });
    if (!thread_.joinable()) running_ = false; // spawn failed (logged above)
}

void EventSourceClient::close() {
    cancel_ = true;
    if (thread_.joinable()) thread_.join();
    running_ = false;
}

void EventSourceClient::run(std::string url) {
    // Worker-thread body: any escape is std::terminate.
    ui_guard("EventSourceClient::run", [&] {
        CURL* curl = curl_easy_init();
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_cb);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
            curl_easy_perform(curl); // blocks until done/aborted
            curl_easy_cleanup(curl);
        }
        if (on_close_) on_close_();
    });
    running_ = false;
}

void EventSourceClient::feed(const char* data, size_t len) {
    buffer_.append(data, len);
    size_t pos = 0;
    std::string line;
    while ((pos = buffer_.find('\n')) != std::string::npos) {
        line = buffer_.substr(0, pos);
        buffer_.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line.empty()) {
            // blank line = dispatch
            if (!pending_data_.empty()) {
                if (pending_data_.back() == '\n') pending_data_.pop_back();
                const std::string event =
                    pending_event_.empty() ? "message" : pending_event_;
                if (on_event_) on_event_(event, pending_data_);
            }
            pending_event_.clear();
            pending_data_.clear();
        } else if (line.starts_with("event:")) {
            pending_event_ = line.substr(6);
            if (!pending_event_.empty() && pending_event_.front() == ' ')
                pending_event_.erase(0, 1);
        } else if (line.starts_with("data:")) {
            std::string d = line.substr(5);
            if (!d.empty() && d.front() == ' ') d.erase(0, 1);
            pending_data_ += d;
            pending_data_ += '\n';
        }
        // "id:", "retry:", comments (":") ignored
    }
}

} // namespace monkeybyte
