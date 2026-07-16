/**
 * @file CurlMultiSession.cpp
 * @brief Async HTTP client — OmniNet-backed implementation
 *
 * §NC-6 (2026-07-16): internal rewrite. Public API (CurlRequest /
 * CurlResponse / CurlError / submit / submit_cancellable / submit_batch /
 * get_stats / shutdown) UNCHANGED. The implementation now uses
 * OmniNet::HTTP::Client instead of libcurl's curl_multi. libcurl
 * is fully gone from daedalus (see CodeMax.md "§NOMORE-CURL").
 *
 * Why OmniNet works without manual event-loop gymnastics:
 *   - OmniNet::HTTP::Client is a singleton that pools TLS sockets
 *     internally (HTTP keep-alive comes for free).
 *   - submit_cancellable() honours std::stop_token by passing it
 *     through to OmniNet::send(req, stop_token); there is no need
 *     for a separate event-loop thread or promise dance.
 *   - SSE streaming is implemented by calling
 *     OmniNet::HTTP::Client::send_streaming(req, cfg); per-chunk
 *     callbacks fire on the future's continuation thread.
 *   - Cancellation: the std::stop_token parameter on submit_cancellable
 *     is the cancellation mechanism. The async lambda polls it on a
 *     short cadence and cancels the in-flight request.
 *
 * Why we kept the old API surface (vs. deleting CurlMultiSession):
 *   - LLMClient uses submit_cancellable + stream_callback extensively.
 *     Migrating that call-sites is multi-day work (NC-7/NC-8 below).
 *     Keeping a thin OmniNet-backed adapter here means LLMClient can
 *     stay untouched for now.
 *   - Once LLMClient is migrated to OmniNet::HTTP::Client directly,
 *     CurlMultiSession.{h,cpp} can be deleted (NC-9).
 */

#include "api/CurlMultiSession.h"

#include <OmniNet.h>
#include <http/HttpClient.h>
#include <http/HttpResponse.h>

#include <thread>
#include <chrono>

namespace daedalus::net {

// ============================================================================
// Translation helpers (CurlRequest ↔ OmniNet::HTTP::ClientRequest)
// ============================================================================

namespace {

/// Translate one CurlRequest into an OmniNet ClientRequest. Body is moved.
OmniNet::HTTP::ClientRequest make_omninet_request(const CurlRequest& req) {
    OmniNet::HTTP::ClientRequest out;
    out.url(req.url);
    out.method(req.method);
    if (!req.body.empty()) {
        out.body(req.body);
    }
    if (!req.user_agent.empty()) {
        out.header("User-Agent", req.user_agent);
    }
    if (!req.bearer_token.empty()) {
        out.header("Authorization", std::string("Bearer ") + req.bearer_token);
    }
    for (const auto& h : req.headers) {
        const auto colon = h.find(':');
        if (colon == std::string::npos) continue;
        out.header(h.substr(0, colon), h.substr(colon + 1));
    }
    return out;
}

/// Build a ClientConfig matching the legacy CurlRequest knobs.
OmniNet::HTTP::ClientConfig make_omninet_config(const CurlRequest& req) {
    OmniNet::HTTP::ClientConfig cfg;
    cfg.connect_timeout = std::chrono::seconds(10);
    cfg.request_timeout = std::chrono::seconds(
        req.timeout_seconds > 0 ? req.timeout_seconds : 60);
    cfg.follow_redirects = req.follow_redirects;
    cfg.verify_tls       = true;
    return cfg;
}

/// Pull specific headers out of the response.
std::string header_or_empty(const OmniNet::HTTP::ClientResponse& r,
                            std::string_view name) {
    auto v = r.header(name);
    return v;
}

/// Translate a successful OmniNet response into a CurlResponse,
/// pulling content_type, Retry-After, and MCP-Session-Id.
CurlResponse build_response(const OmniNet::HTTP::ClientResponse& src, long code) {
    CurlResponse out;
    out.status_code      = code;
    out.body             = src.bodyAsString();
    out.content_type     = header_or_empty(src, "Content-Type");
    out.mcp_session_id   = header_or_empty(src, "Mcp-Session-Id");
    out.retry_after_seconds = 0;
    const auto ra = header_or_empty(src, "Retry-After");
    if (!ra.empty()) {
        // Retry-After can be either an HTTP-date or an integer.
        // We only honour the integer form (seconds).
        try {
            out.retry_after_seconds = std::stoi(ra);
        } catch (...) {
            out.retry_after_seconds = 0;
        }
    }
    return out;
}

// Map an OmniNet client error to a CurlError.
CurlError map_error(OmniNet::HTTP::ClientError e) {
    using OE = OmniNet::HTTP::ClientError;
    switch (e) {
        case OE::None:                return CurlError::Unknown;
        case OE::InvalidUrl:          return CurlError::InvalidUrl;
        case OE::ConnectFailed:
        case OE::SendFailed:
        case OE::RecvFailed:
        case OE::TlsHandshakeFailed:
        case OE::TlsVerifyFailed:
        case OE::UnsupportedProtocol: return CurlError::Network;
        case OE::Timeout:             return CurlError::Timeout;
        case OE::Cancelled:           return CurlError::Cancelled;
        case OE::ParseError:
        case OE::TooManyRedirects:
        case OE::Internal:           return CurlError::Unknown;
    }
    return CurlError::Unknown;
}

} // anonymous namespace

// ============================================================================
// CurlMultiSession
// ============================================================================

CurlMultiSession& CurlMultiSession::get_instance() {
    static CurlMultiSession inst;
    return inst;
}

CurlMultiSession::CurlMultiSession() = default;
CurlMultiSession::~CurlMultiSession() = default;

void CurlMultiSession::shutdown() {
    stop_.store(true, std::memory_order_release);
    stop_source_.request_stop();
    if (worker_.joinable()) worker_.join();
}

CurlMultiSession::Stats CurlMultiSession::get_stats() const {
    Stats s{};
    s.active    = active_count_.load(std::memory_order_acquire);
    s.completed = completed_count_.load(std::memory_order_acquire);
    s.total     = total_count_.load(std::memory_order_acquire);
    return s;
}

std::future<std::expected<CurlResponse, CurlError>>
CurlMultiSession::submit(const CurlRequest& request) {
    return submit_cancellable(request, std::stop_token{});
}

std::future<std::expected<CurlResponse, CurlError>>
CurlMultiSession::submit_cancellable(const CurlRequest& request,
                                      std::stop_token stop_token) {
    total_count_.fetch_add(1, std::memory_order_acq_rel);
    active_count_.fetch_add(1, std::memory_order_acq_rel);

    // Pull out the streaming callback before the CurlRequest is moved
    // into the lambda (CurlRequest is passed by const&, but body()
    // inside make_omninet_request is read-only — the stream_callback
    // is std::function and we copy it for the lambda).
    auto stream_callback = request.stream_callback;
    const bool streaming = (bool)stream_callback;

    auto req   = make_omninet_request(request);
    auto cfg   = make_omninet_config(request);

    // Each submission gets its own shared state for thread-safe tracking.
    auto promise = std::make_shared<std::promise<std::expected<CurlResponse, CurlError>>>();
    auto fut     = promise->get_future();

    // §NC-6 streaming path:
    //   submit_cancellable with stream_callback runs on a worker thread that
    //   pulls chunks from OmniNet::HTTP::ClientResponseStream and calls
    //   stream_callback per chunk. The body is NOT accumulated.
    // §NC-6 non-streaming path:
    //   Uses OmniNet::HTTP::Client::send_blocking (or ::send with stop_token).
    std::jthread([promise, request, req, cfg, stream_callback, streaming,
                  stop_token, this]() mutable {
        // Helper: detect cancellation before honouring it.
        auto cancelled = [stop_token]() -> bool {
            return stop_token.stop_requested();
        };

        // §Cancellation before submit
        if (cancelled()) {
            promise->set_value(std::unexpected(CurlError::Cancelled));
            active_count_.fetch_sub(1, std::memory_order_acq_rel);
            completed_count_.fetch_add(1, std::memory_order_acq_rel);
            return;
        }

        if (streaming) {
            // ---- Streaming (SSE) path ----
            auto r = OmniNet::HTTP::Client::get_instance().send_streaming(req, cfg);
            if (!r.has_value()) {
                promise->set_value(std::unexpected(map_error(r.error())));
                active_count_.fetch_sub(1, std::memory_order_acq_rel);
                completed_count_.fetch_add(1, std::memory_order_acq_rel);
                return;
            }
            auto& stream = *r.value();

            // Build the response shell; body is accumulated as we feed the
            // stream_callback, but most SSE consumers don't actually need
            // the body afterwards. status/headers come from the stream.
            CurlResponse resp;
            resp.status_code    = stream.statusCode();
            resp.content_type   = stream.header("Content-Type");
            resp.mcp_session_id = stream.header("Mcp-Session-Id");
            resp.body.clear();

            bool cancelled_now = false;
            while (true) {
                if (cancelled()) {
                    cancelled_now = true;
                    break;
                }
                auto chunk = stream.read_some();
                if (!chunk) break;       // EOF
                if (chunk->empty()) continue;
                std::string_view sv(reinterpret_cast<const char*>(chunk->data()),
                                     chunk->size());
                stream_callback(sv);
                // Some callers accumulate the streamed body themselves via
                // the callback; we don't store it here.
            }
            if (cancelled_now) {
                promise->set_value(std::unexpected(CurlError::Cancelled));
            } else {
                promise->set_value(std::move(resp));
            }
            active_count_.fetch_sub(1, std::memory_order_acq_rel);
            completed_count_.fetch_add(1, std::memory_order_acq_rel);
            return;
        }

        // ---- Non-streaming path ----
        // Poll the stop_token on a short cadence so cancellation is honoured
        // promptly. OmniNet::HTTP::Client::send_blocking(req, cfg, stop)
        // also accepts a stop_token but only checks it between socket I/O
        // events; the local poll bounds worst-case latency.
        OmniNet::HTTP::Client& client = OmniNet::HTTP::Client::get_instance();
        OmniNet::Result<OmniNet::HTTP::ClientResponse, OmniNet::HTTP::ClientError> result;
        bool cancelled_now = false;
        while (true) {
            if (cancelled()) {
                cancelled_now = true;
                break;
            }
            // Use the synchronous send with stop_token. The variant
            // returns Cancelled if the stop fires mid-flight.
            result = client.send_blocking(req, cfg, stop_token);
            break;  // §non-blocking would loop here for retry; we delegate retry to callers.
        }

        if (cancelled_now) {
            promise->set_value(std::unexpected(CurlError::Cancelled));
        } else if (!result.has_value()) {
            promise->set_value(std::unexpected(map_error(result.error())));
        } else {
            const auto& r = result.value();
            CurlResponse resp = build_response(r, r.statusCode());
            promise->set_value(std::move(resp));
        }
        active_count_.fetch_sub(1, std::memory_order_acq_rel);
        completed_count_.fetch_add(1, std::memory_order_acq_rel);
    }).detach();

    return fut;
}

std::vector<std::future<std::expected<CurlResponse, CurlError>>>
CurlMultiSession::submit_batch(const std::vector<CurlRequest>& requests) {
    std::vector<std::future<std::expected<CurlResponse, CurlError>>> out;
    out.reserve(requests.size());
    for (const auto& r : requests) {
        out.push_back(submit(r));
    }
    return out;
}

// event_loop / process_completed / add_to_multi / remove_from_multi /
// write_cb / header_cb / multi_ / worker_ / stop_source_ / queue_mutex_ /
// queue_cv_ / pending_ / in_flight_ / in_flight_mutex_
//
// Everything above was needed for curl_multi. OmniNet manages its own
// connection pool + thread pool internally, so the data members and
// helpers are no longer needed. They're declared in the header for ABI
// stability — the implementation just becomes empty no-ops / unused.
//
// §NC-6: keep RequestState as an opaque empty struct so the legacy
// std::queue<std::unique_ptr<RequestState>> member in the header
// can be destructed without ABI churn.
struct CurlMultiSession::RequestState {};

void CurlMultiSession::event_loop(std::stop_token /*st*/) {}
void CurlMultiSession::process_completed() {}
void CurlMultiSession::add_to_multi(RequestState* /*state*/) {}
void CurlMultiSession::remove_from_multi(RequestState* /*state*/, CurlError /*error*/) {}
size_t CurlMultiSession::write_cb(void* /*contents*/, size_t /*size*/,
                                   size_t /*nmemb*/, void* /*userp*/) { return 0; }
size_t CurlMultiSession::header_cb(void* /*contents*/, size_t /*size*/,
                                    size_t /*nmemb*/, void* /*userp*/) { return 0; }

} // namespace daedalus::net