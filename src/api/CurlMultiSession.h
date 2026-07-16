/**
 * @file CurlMultiSession.h
 * @brief Async HTTP client using libcurl's multi interface
 *
 * Why this exists:
 * - Replaces synchronous curl_easy_perform with non-blocking curl_multi.
 * - Allows multiple concurrent HTTP requests (batch tool calls, parallel
 *   searches, etc.) in a single event-loop thread.
 * - Supports cancellation via std::stop_token (ESCs mid-request actually work).
 * - Reuses connections via HTTP keep-alive (faster subsequent calls).
 * - Returns std::expected<CurlResponse, CurlError> for type-safe error handling.
 *
 * Architecture:
 * - One CurlMultiSession = one CURLM handle + one event-loop jthread.
 * - Submit enqueues a request; the event loop adds it to curl_multi and polls.
 * - When a transfer completes, the corresponding promise is fulfilled.
 * - Cancellation removes the handle from curl_multi and fulfills with CurlError::Cancelled.
 *
 * Thread-safety:
 * - submit() can be called from any thread.
 * - The event loop runs in one dedicated thread (jthread).
 * - Per-request state is owned by the request and not shared.
 *
 * Usage:
 * @code
 *   auto& session = CurlMultiSession::get_instance();
 *   CurlRequest req;
 *   req.url = "https://api.example.com/data";
 *   req.method = "GET";
 *   req.bearer_token = "sk-...";
 *   req.timeout_seconds = 30;
 *
 *   auto fut = session.submit(req);
 *   auto result = fut.get();
 *   if (result) {
 *       std::cout << "Status: " << result->status_code << "\n";
 *       std::cout << "Body: " << result->body << "\n";
 *   } else {
 *       std::cerr << "Error: " << static_cast<int>(result.error()) << "\n";
 *   }
 * @endcode
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <curl/curl.h>
#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace daedalus::net {

// ============================================================================
// Request / Response types
// ============================================================================

/**
 * @brief HTTP request configuration
 */
struct CurlRequest {
    std::string url;                 ///< Target URL (required)
    std::string method = "GET";      ///< HTTP method (GET, POST, PUT, DELETE, etc.)
    std::string body;                ///< Request body (for POST/PUT)
    std::vector<std::string> headers; ///< Extra headers (e.g., "Content-Type: application/json")
    std::string bearer_token;        ///< Optional bearer token (added as Authorization: Bearer ...)
    long timeout_seconds = 300;      ///< Total request timeout (default 5 min)
    bool follow_redirects = true;    ///< Follow 3xx redirects
    std::string user_agent = "daedalus/1.0";  ///< User-Agent header

    /// Optional streaming callback. When set, the write callback invokes it
    /// for each chunk of data received (SSE streaming). The response.body
    /// is NOT accumulated in this mode. The caller is responsible for
    /// parsing SSE lines and extracting text deltas.
    std::function<void(std::string_view)> stream_callback;
};

/**
 * @brief HTTP response
 */
struct CurlResponse {
    long status_code = 0;            ///< HTTP status (200, 404, etc.)
    std::string body;                ///< Response body
    std::string content_type;        ///< Content-Type header (if any)
    /// MCP streamable-HTTP session ID (read from the Mcp-Session-Id
    /// response header by the curl header callback). Empty if the server
    /// didn't send one. Used by HttpMcpTransport to replay the header on
    /// every subsequent request per the MCP 2025-06-18 streamable-HTTP
    /// spec §Session Management.
    std::string mcp_session_id;
    bool timed_out = false;          ///< True if request timed out
    bool cancelled = false;          ///< True if cancelled via stop_token

    [[nodiscard]] bool ok() const noexcept { return status_code >= 200 && status_code < 300; }
};

/**
 * @brief Error categories for HTTP operations
 */
enum class CurlError : std::uint8_t {
    InitFailed,         ///< curl_easy_init returned null
    InvalidUrl,         ///< URL is empty or malformed
    Network,            ///< DNS, connection refused, etc.
    Timeout,            ///< Operation exceeded timeout_seconds
    Cancelled,          ///< Cancelled via stop_token
    HttpStatus,         ///< Non-2xx response (use response.status_code for details)
    WriteError,         ///< Write callback failed
    Unknown,            ///< Other libcurl error
};

/**
 * @brief Convert CurlError to human-readable string
 */
inline const char* to_string(CurlError e) {
    switch (e) {
        case CurlError::InitFailed:   return "Failed to initialize curl";
        case CurlError::InvalidUrl:   return "Invalid or empty URL";
        case CurlError::Network:      return "Network error";
        case CurlError::Timeout:      return "Request timed out";
        case CurlError::Cancelled:    return "Request cancelled";
        case CurlError::HttpStatus:   return "HTTP error status";
        case CurlError::WriteError:   return "Write callback error";
        case CurlError::Unknown:      return "Unknown error";
    }
    return "Unknown error";
}

// ============================================================================
// CurlMultiSession
// ============================================================================

/**
 * @brief Async HTTP session using libcurl's multi interface
 *
 * Singleton per process. Use get_instance() to access.
 */
class CurlMultiSession {
public:
    static CurlMultiSession& get_instance();

    // Non-copyable
    CurlMultiSession(const CurlMultiSession&) = delete;
    CurlMultiSession& operator=(const CurlMultiSession&) = delete;

    /**
     * @brief Submit a request and get a future to the result
     *
     * The request is queued and processed by the event-loop thread. Use
     * the returned future to await the result.
     *
     * @param request Request configuration
     * @return std::future<std::expected<CurlResponse, CurlError>>
     */
    std::future<std::expected<CurlResponse, CurlError>> submit(const CurlRequest& request);

    /**
     * @brief Submit a request that can be cancelled via stop_token
     *
     * When the stop_token is requested, the event loop removes the curl
     * handle from curl_multi and fulfills the future with CurlError::Cancelled.
     */
    std::future<std::expected<CurlResponse, CurlError>> submit_cancellable(
        const CurlRequest& request,
        std::stop_token stop_token
    );

    /**
     * @brief Submit multiple requests concurrently (batch)
     *
     * All requests are in-flight at once. Use the returned futures to
     * await each result. Useful for parallel tool calls.
     */
    std::vector<std::future<std::expected<CurlResponse, CurlError>>> submit_batch(
        const std::vector<CurlRequest>& requests
    );

    /**
     * @brief Get session statistics
     */
    struct Stats {
        size_t active;       ///< Requests currently in-flight
        size_t completed;    ///< Total requests completed (cumulative)
        size_t total;        ///< Total requests ever submitted
    };
    Stats get_stats() const;

    /**
     * @brief Graceful shutdown
     *
     * Stops accepting new requests, cancels in-flight ones, joins the
     * event loop thread. Safe to call multiple times.
     */
    void shutdown();

    ~CurlMultiSession();

private:
    CurlMultiSession();
    void event_loop(std::stop_token st);
    struct RequestState;
    void process_completed();
    void add_to_multi(RequestState* state);
    void remove_from_multi(RequestState* state, CurlError error);
    static size_t write_cb(void* contents, size_t size, size_t nmemb, void* userp);
    static size_t header_cb(void* contents, size_t size, size_t nmemb, void* userp);

    CURLM* multi_ = nullptr;
    std::jthread worker_;
    std::stop_source stop_source_;

    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::queue<std::unique_ptr<RequestState>> pending_;
    /// §B16 fix: tracks RequestState* for every handle currently in curl_multi.
    /// Used at shutdown to cancel all in-flight requests — curl_multi_info_read
    /// only returns CURLMSG_DONE (completed), so in-flight handles would leak.
    std::vector<RequestState*> in_flight_;
    std::mutex in_flight_mutex_;

    std::atomic<bool> stop_{false};
    std::atomic<size_t> active_count_{0};
    std::atomic<size_t> completed_count_{0};
    std::atomic<size_t> total_count_{0};
};

} // namespace daedalus::net
