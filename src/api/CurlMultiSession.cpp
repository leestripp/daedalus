/**
 * @file CurlMultiSession.cpp
 * @brief Async HTTP session implementation
 */

#include "CurlMultiSession.h"

#include <cstring>
#include <iostream>
#include <memory>
#include <stop_token>
#include <utility>

#include "core/Logger.h"

// §Phase 5.1: helper macro that converts LOG_CERR(INFO,  ...); style
// statements into LOG_INFO/LOG_WARNING/LOG_ERROR calls via the
// existing Logger. Same formatted text routed through the Logger
// instead of writing to stderr directly.
#define LOG_CERR(level, expr) do {                          \
        std::ostringstream _daedalus_msg;                   \
        _daedalus_msg << expr;                               \
        LOG_##level(_daedalus_msg.str());                    \
    } while (0)
namespace daedalus::net {

// ============================================================================
// RequestState - per-request data
// ============================================================================

struct CurlMultiSession::RequestState {
    CurlRequest request;
    CurlResponse response;
    std::promise<std::expected<CurlResponse, CurlError>> promise;
    std::stop_token stop_token;
    CURL* easy = nullptr;
    curl_slist* headers = nullptr;
    bool in_multi = false;
    std::chrono::steady_clock::time_point start_time;
};

// ============================================================================
// Singleton
// ============================================================================

CurlMultiSession& CurlMultiSession::get_instance() {
    static CurlMultiSession instance;
    return instance;
}

// ============================================================================
// Construction / Destruction
// ============================================================================

CurlMultiSession::CurlMultiSession() {
    multi_ = curl_multi_init();
    if (multi_ == nullptr) {
        LOG_CERR(ERROR,  "[CurlMultiSession] Failed to initialize curl multi handle\n");
        return;
    }

    // Set max concurrent connections (default 100)
    curl_multi_setopt(multi_, CURLMOPT_MAX_TOTAL_CONNECTIONS, 100L);
    curl_multi_setopt(multi_, CURLMOPT_MAX_HOST_CONNECTIONS, 6L);

    // Start the event loop thread
    worker_ = std::jthread([this](std::stop_token st) {
        event_loop(st);
    });
}

CurlMultiSession::~CurlMultiSession() {
    shutdown();
}

void CurlMultiSession::shutdown() {
    if (stop_.exchange(true)) {
        return;  // Already shutting down
    }

    stop_source_.request_stop();
    queue_cv_.notify_all();

    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }

    if (multi_ != nullptr) {
        curl_multi_cleanup(multi_);
        multi_ = nullptr;
    }
}

// ============================================================================
// Event loop
// ============================================================================

void CurlMultiSession::event_loop(std::stop_token st) {
    while (!st.stop_requested() && !stop_.load()) {
        // Move pending requests from queue to multi handle
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait_for(lock, std::chrono::milliseconds(50), [this, &st] {
                return stop_.load() || st.stop_requested() || !pending_.empty();
            });

            while (!pending_.empty() && !stop_.load() && !st.stop_requested()) {
                auto state = std::move(pending_.front());
                pending_.pop();
                lock.unlock();
                add_to_multi(state.release());
                lock.lock();
            }
        }

        if (stop_.load() || st.stop_requested()) break;

        // Perform any pending transfers (non-blocking)
        int still_running = 0;
        curl_multi_perform(multi_, &still_running);

        // Check for cancellations
        {
            std::scoped_lock lock(queue_mutex_);
            // Iterate over active easy handles... but curl_multi doesn't expose
            // a way to iterate. Instead, we check inside the info read.
        }

        // Wait for activity with a short timeout so we can check stop/cancel
        int numfds = 0;
        curl_multi_poll(multi_, nullptr, 0, 50, &numfds);

        // Read info about completed transfers
        process_completed();

        // Brief yield
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // On shutdown, cancel all in-flight requests
    CURLMsg* msg = nullptr;
    int msgs_left = 0;
    while ((msg = curl_multi_info_read(multi_, &msgs_left)) != nullptr) {
        if (msg->msg == CURLMSG_DONE) {
            CURL* easy = msg->easy_handle;
            // Find the request state via curl_easy_getinfo
            RequestState* state = nullptr;
            curl_easy_getinfo(easy, CURLINFO_PRIVATE, &state);
            if (state != nullptr) {
                state->response.cancelled = true;
                state->promise.set_value(std::unexpected(CurlError::Cancelled));
                curl_multi_remove_handle(multi_, easy);
                curl_easy_cleanup(easy);
                if (state->headers != nullptr) curl_slist_free_all(state->headers);
                delete state;
            }
        }
    }
}

// ============================================================================
// Add a request to the multi handle
// ============================================================================

void CurlMultiSession::add_to_multi(RequestState* state) {
    state->easy = curl_easy_init();
    if (!state->easy) {
        state->promise.set_value(std::unexpected(CurlError::InitFailed));
        delete state;
        return;
    }

    // Set URL
    if (state->request.url.empty()) {
        curl_easy_cleanup(state->easy);
        state->promise.set_value(std::unexpected(CurlError::InvalidUrl));
        delete state;
        return;
    }
    curl_easy_setopt(state->easy, CURLOPT_URL, state->request.url.c_str());

    // Set method (default GET; only set POSTFIELDS for POST/PUT)
    if (state->request.method == "POST" || state->request.method == "PUT" ||
        state->request.method == "PATCH") {
        curl_easy_setopt(state->easy, CURLOPT_POST, 1L);
        if (state->request.method == "PUT") {
            curl_easy_setopt(state->easy, CURLOPT_CUSTOMREQUEST, "PUT");
        } else if (state->request.method == "PATCH") {
            curl_easy_setopt(state->easy, CURLOPT_CUSTOMREQUEST, "PATCH");
        }
        if (!state->request.body.empty()) {
            curl_easy_setopt(state->easy, CURLOPT_POSTFIELDS, state->request.body.c_str());
            curl_easy_setopt(state->easy, CURLOPT_POSTFIELDSIZE,
                             static_cast<long>(state->request.body.size()));
        }
    } else if (state->request.method == "DELETE") {
        curl_easy_setopt(state->easy, CURLOPT_CUSTOMREQUEST, "DELETE");
    } else if (state->request.method != "GET") {
        curl_easy_setopt(state->easy, CURLOPT_CUSTOMREQUEST, state->request.method.c_str());
    }

    // Build headers
    // Note: always append Content-Type first so it is closest to the body.
    state->headers = nullptr;
    // When the request already carries an Accept header (e.g. "text/event-stream"
    // for SSE streaming), honour it instead of prepending a generic fallback.
    bool has_accept = false;
    for (const auto& h : state->request.headers) {
        if (h.starts_with("Accept:")) has_accept = true;
        state->headers = curl_slist_append(state->headers, h.c_str());
    }
    if (!has_accept) {
        state->headers = curl_slist_append(state->headers, "Accept: application/json");
    }
    if (!state->request.bearer_token.empty()) {
        std::string auth_header = "Authorization: Bearer " + state->request.bearer_token;
        state->headers = curl_slist_append(state->headers, auth_header.c_str());
    }
    if (state->headers) {
        curl_easy_setopt(state->easy, CURLOPT_HTTPHEADER, state->headers);
    }

    // Timeouts
    if (state->request.timeout_seconds > 0) {
        curl_easy_setopt(state->easy, CURLOPT_TIMEOUT, state->request.timeout_seconds);
    }
    curl_easy_setopt(state->easy, CURLOPT_FOLLOWLOCATION,
                     state->request.follow_redirects ? 1L : 0L);
    if (!state->request.user_agent.empty()) {
        curl_easy_setopt(state->easy, CURLOPT_USERAGENT, state->request.user_agent.c_str());
    }

    // Write callback
    curl_easy_setopt(state->easy, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(state->easy, CURLOPT_WRITEDATA, state);

    // Header callback (for Content-Type extraction)
    curl_easy_setopt(state->easy, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(state->easy, CURLOPT_HEADERDATA, state);

    // Store RequestState* for retrieval in completion callback
    curl_easy_setopt(state->easy, CURLOPT_PRIVATE, state);

    // Add to multi handle
    CURLMcode rc = curl_multi_add_handle(multi_, state->easy);
    if (rc != CURLM_OK) {
        LOG_CERR(ERROR,  "[CurlMultiSession] curl_multi_add_handle failed: " << rc);
        curl_easy_cleanup(state->easy);
        if (state->headers) curl_slist_free_all(state->headers);
        state->promise.set_value(std::unexpected(CurlError::Unknown));
        delete state;
        return;
    }

    state->in_multi = true;
    state->start_time = std::chrono::steady_clock::now();
    active_count_.fetch_add(1, std::memory_order_relaxed);
    total_count_.fetch_add(1, std::memory_order_relaxed);
}

// ============================================================================
// Process completed transfers
// ============================================================================

void CurlMultiSession::process_completed() {
    CURLMsg* msg = nullptr;
    int msgs_left = 0;
    while ((msg = curl_multi_info_read(multi_, &msgs_left)) != nullptr) {
        if (msg->msg != CURLMSG_DONE) continue;

        CURL* easy = msg->easy_handle;
        RequestState* state = nullptr;
        curl_easy_getinfo(easy, CURLINFO_PRIVATE, &state);
        if (state == nullptr) continue;

        // Check if cancelled via stop_token
        if (state->stop_token.stop_requested()) {
            state->response.cancelled = true;
            state->promise.set_value(std::unexpected(CurlError::Cancelled));
        } else {
            // Get response code
            long code = 0;
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &code);
            state->response.status_code = code;

            if (msg->data.result == CURLE_OK) {
                if (code >= 200 && code < 300) {
                    state->promise.set_value(std::move(state->response));
                } else {
                    // Non-2xx response: return the full response so callers can
                    // inspect status_code and body (e.g. API error messages).
                    // Previously we discarded the body and returned only the error tag.
                    state->response.status_code = code;
                    state->promise.set_value(std::move(state->response));
                }
            } else if (msg->data.result == CURLE_OPERATION_TIMEDOUT) {
                state->response.timed_out = true;
                state->promise.set_value(std::unexpected(CurlError::Timeout));
            } else {
                state->promise.set_value(std::unexpected(CurlError::Network));
            }
        }

        // Cleanup
        curl_multi_remove_handle(multi_, easy);
        curl_easy_cleanup(easy);
        if (state->headers) curl_slist_free_all(state->headers);
        delete state;

        active_count_.fetch_sub(1, std::memory_order_relaxed);
        completed_count_.fetch_add(1, std::memory_order_relaxed);
    }
}

void CurlMultiSession::remove_from_multi(RequestState* state, CurlError error) {
    if (state->in_multi && state->easy) {
        curl_multi_remove_handle(multi_, state->easy);
    }
    if (state->easy) {
        curl_easy_cleanup(state->easy);
    }
    if (state->headers) {
        curl_slist_free_all(state->headers);
    }
    state->promise.set_value(std::unexpected(error));
    delete state;
}

// ============================================================================
// Callbacks
// ============================================================================

size_t CurlMultiSession::write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    auto* state = static_cast<RequestState*>(userp);

    // Streaming mode: invoke the per-chunk callback instead of buffering.
    // The callback is responsible for parsing SSE lines and extracting text deltas.
    if (state->request.stream_callback) {
        state->request.stream_callback(
            std::string_view(static_cast<char*>(contents), realsize));
        return realsize;
    }

    // Non-streaming: accumulate into response.body for later JSON parse.
    state->response.body.append(static_cast<char*>(contents), realsize);
    return realsize;
}

size_t CurlMultiSession::header_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    auto* state = static_cast<RequestState*>(userp);
    std::string header(static_cast<char*>(contents), realsize);

    // We extract exactly two headers from the response: Content-Type
    // (used by callers to detect SSE vs JSON) and Mcp-Session-Id (used
    // by MCP streamable-HTTP clients to replay the session on every
    // subsequent request per spec §Session Management). Other response
    // headers are dropped — we don't need a generic map.

    // Header is "Name: value\r\n" (curl strips the CRLF). Skip the
    // status line ("HTTP/1.1 200 OK").
    size_t colon = header.find(':');
    if (colon == std::string::npos) {
        return realsize;
    }
    std::string name = header.substr(0, colon);
    std::string value = header.substr(colon + 1);
    // Trim leading whitespace from value.
    size_t start = value.find_first_not_of(" \t");
    if (start != std::string::npos) value = value.substr(start);
    // Trim trailing CR/LF/whitespace.
    size_t end = value.find_last_not_of(" \t\r\n");
    if (end != std::string::npos) value = value.substr(0, end + 1);

    // Lowercase name once for case-insensitive comparison.
    std::string lc_name;
    lc_name.reserve(name.size());
    for (char c : name) {
        lc_name.push_back(static_cast<char>(
            (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c));
    }

    if (lc_name == "content-type") {
        state->response.content_type = value;
    } else if (lc_name == "mcp-session-id") {
        // §18.9 — MCP streamable-HTTP. Cache the session id so the
        // transport can replay it on every subsequent request.
        state->response.mcp_session_id = value;
    }
    return realsize;
}

// ============================================================================
// Public API
// ============================================================================

std::future<std::expected<CurlResponse, CurlError>> CurlMultiSession::submit(
    const CurlRequest& request) {
    return submit_cancellable(request, std::stop_token{});
}

std::future<std::expected<CurlResponse, CurlError>> CurlMultiSession::submit_cancellable(
    const CurlRequest& request,
    std::stop_token stop_token
) {
    auto* state = new RequestState();
    state->request = request;
    state->stop_token = stop_token;
    state->start_time = std::chrono::steady_clock::now();

    auto fut = state->promise.get_future();

    if (stop_.load()) {
        // Shutting down - return error immediately
        state->promise.set_value(std::unexpected(CurlError::Cancelled));
        delete state;
        return fut;
    }

    {
        std::scoped_lock lock(queue_mutex_);
        pending_.push(std::unique_ptr<RequestState>(state));
    }
    queue_cv_.notify_one();

    return fut;
}

std::vector<std::future<std::expected<CurlResponse, CurlError>>> CurlMultiSession::submit_batch(
    const std::vector<CurlRequest>& requests
) {
    std::vector<std::future<std::expected<CurlResponse, CurlError>>> results;
    results.reserve(requests.size());

    for (const auto& req : requests) {
        results.push_back(submit(req));
    }
    return results;
}

CurlMultiSession::Stats CurlMultiSession::get_stats() const {
    return Stats{
        .active = active_count_.load(std::memory_order_relaxed),
        .completed = completed_count_.load(std::memory_order_relaxed),
        .total = total_count_.load(std::memory_order_relaxed),
    };
}

} // namespace daedalus::net
