/**
 * @file demos/daedalus-cli/acp/src/AcpTransport.cpp
 * @brief ACP Transport implementation using OmniNet (NOT libcurl)
 *
 * §NC-1 (2026-07-16): replaced the entire libcurl implementation with
 * OmniNet::HTTP::Client. No direct libcurl anywhere in the daedalus
 * codebase — see CodeMax.md "§NOMORE-CURL" for the full migration plan.
 *
 * Sync GET/POST → OmniNet::HTTP::Client::send_blocking(req, cfg).
 * Streaming     → OmniNet::HTTP::Client::send_streaming(req, cfg).
 *
 * The SSE parser (used by stream_events) lives in
 * <daedalus/net/SseParser.h> — see NC-2.
 */

#include "acp/AcpClient.h"

#include <daedalus/net/SseParser.h>

#include <OmniNet.h>
#include <http/HttpClient.h>
#include <http/HttpResponse.h>
#include <utils/Utils.h>

#include <sstream>
#include <iostream>

namespace acp {

namespace {

// Build a request with common JSON headers + the JSON-Content-Type
// pre-set. Caller adds method/url/body before sending.
OmniNet::HTTP::ClientRequest make_request(const std::string& url) {
    OmniNet::HTTP::ClientRequest req;
    req.url(url);
    req.header("Content-Type", "application/json");
    req.header("Accept",       "application/json");
    return req;
}

/// Convert an OmniNet Result into our Response. We always return a
/// Response (never throw); success populates status_code + body,
/// failure populates transport_error.
IAcpTransport::Response to_response(const OmniNet::Result<OmniNet::HTTP::ClientResponse, OmniNet::HTTP::ClientError>& r,
                                     const std::string& method_path) {
    IAcpTransport::Response out;
    if (!r.has_value()) {
        out.transport_error = std::string("omninet: ") +
                              OmniNet::HTTP::clientErrorName(r.error());
        std::cerr << "[AcpTransport] " << method_path
                  << " failed: " << out.transport_error << "\n";
        return out;
    }
    const auto& resp = r.value();
    out.status_code = resp.statusCode();
    const auto& bytes = resp.body();
    if (bytes.empty()) {
        // Successful response with no body — return an empty JSON object
        // so callers can still iterate keys without crashing.
        out.body = nlohmann::json::object();
        return out;
    }
    try {
        out.body = nlohmann::json::parse(resp.bodyAsString());
    } catch (const nlohmann::json::parse_error& e) {
        out.transport_error = std::string("JSON parse: ") + e.what();
        std::cerr << "[AcpTransport] " << method_path
                  << " JSON parse error: " << e.what() << "\n";
    }
    return out;
}

} // anonymous namespace

namespace {

// ============================================================================
// OmninetTransport — uses OmniNet::HTTP::Client for sync GET/POST
// and send_streaming for SSE.
// ============================================================================

class OmninetTransport : public IAcpTransport {
public:
    explicit OmninetTransport(std::string base_url)
        : base_url_(std::move(base_url))
    {
        cfg_.connect_timeout    = std::chrono::seconds(10);
        cfg_.request_timeout    = std::chrono::seconds(30);
        cfg_.follow_redirects   = true;
        cfg_.verify_tls         = true;
    }

    Response get(const std::string& path) override {
        auto req = make_request(base_url_ + path);
        req.method("GET");
        return to_response(
            OmniNet::HTTP::Client::get_instance().send_blocking(req, cfg_),
            "GET " + path);
    }

    Response post(const std::string& path, const nlohmann::json& body) override {
        auto req = make_request(base_url_ + path);
        req.method("POST");
        req.body(body.dump());
        return to_response(
            OmniNet::HTTP::Client::get_instance().send_blocking(req, cfg_),
            "POST " + path);
    }

    void stream_events(const RunId& run_id, IRunEventHandler& handler) override;

    bool is_connected() const override {
        auto req = make_request(base_url_ + "/v1/ping");
        req.method("GET");
        auto r = OmniNet::HTTP::Client::get_instance().send_blocking(req, cfg_);
        return r.has_value() &&
               r.value().statusCode() >= 200 &&
               r.value().statusCode() <  300;
    }

private:
    std::string base_url_;
    OmniNet::HTTP::ClientConfig cfg_;
};

// §NC-3 + §ACP-7 (2026-07-16): structured event dispatch.
//
// Each parsed SSE event's `data` is a JSON object whose shape
// depends on the `type` discriminator. dispatch_sse_event (defined
// in AcpTypes.cpp) reads `type` and calls the matching callback:
//
//   message.created / .part / .completed → on_message_*
//   run.created / .in-progress / .awaiting /
//     .completed / .failed / .cancelled    → on_run_*
//   generic                                → on_generic
//   error                                  → on_error
//
// Unknown types + malformed payloads fall through to on_generic,
// so the stream stays live even if a future spec adds new events.
// ============================================================================
void dispatch_sse_event_inline(const RunId& run_id,
                               const daedalus::net::SseEvent& event,
                               IRunEventHandler& handler) {
    if (event.data.empty()) {
        // No payload — nothing to do.
        return;
    }
    dispatch_sse_event(run_id, event.data, handler);
}

// §NC-3: SSE streaming using OmniNet::HTTP::Client::send_streaming +
// SseParser. Runs on a background thread and joins at end-of-stream
// so the IAcpTransport contract stays synchronous.
void OmninetTransport::stream_events(const RunId& run_id,
                                     IRunEventHandler& handler) {
    const std::string url = base_url_ + "/v1/runs/" + run_id + "/events";
    OmniNet::HTTP::ClientRequest req;
    req.url(url);
    req.method("GET");
    req.header("Accept", "text/event-stream");

    std::thread worker([this, req, run_id, &handler]() {
        auto r = OmniNet::HTTP::Client::get_instance().send_streaming(req, cfg_);
        if (!r.has_value()) {
            handler.on_error(run_id, make_error(
                std::string("omninet: ") + OmniNet::HTTP::clientErrorName(r.error()),
                -1));
            return;
        }
        auto& stream = *r.value();
        daedalus::net::SseParser parser;
        while (true) {
            auto chunk = stream.read_some();
            if (!chunk) break;          // EOF
            if (chunk->empty()) continue;
            const char* data = reinterpret_cast<const char*>(chunk->data());
            parser.feed(std::string_view(data, chunk->size()));
            // Drain parsed events; dispatch.
            for (;;) {
                auto event = parser.try_pop();
                if (!event) break;
                dispatch_sse_event_inline(run_id, *event, handler);
            }
        }
        parser.finish();  // flush any trailing in-flight event
    });
    worker.join();
}

} // anonymous namespace

// ============================================================================
// Transport factory — OmniNet only. No libcurl in this translation unit.
// ============================================================================

std::unique_ptr<IAcpTransport> create_curl_transport(const std::string& base_url) {
    (void)base_url;
    // Kept for ABI compatibility — names say "curl" but the implementation
    // is OmniNet. New code should call create_transport() below.
    return std::make_unique<OmninetTransport>(base_url);
}

std::unique_ptr<IAcpTransport> create_transport(const std::string& base_url) {
    return std::make_unique<OmninetTransport>(base_url);
}

} // namespace acp