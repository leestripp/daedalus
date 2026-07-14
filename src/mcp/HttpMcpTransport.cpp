/**
 * @file HttpMcpTransport.cpp
 * @brief HTTP MCP transport implementation (uses CurlMultiSession)
 *
 * §18.9 — Streamable-HTTP session continuity:
 *   The MCP streamable-HTTP spec requires the server to assign a session
 *   ID at `initialize` time (returned in the `Mcp-Session-Id` response
 *   header) and the client to echo that header on every subsequent
 *   request. We honour that here by capturing the header into
 *   `session_id_` during `send_request` and replaying it from then on.
 *
 *   Most servers (including the project's own `apollo_mcp_server`) reject
 *   any post-`initialize` request that omits the header with HTTP 400
 *   "MCP-Session-Id header required after initialize". Without this fix
 *   apollocore appeared to connect but returned 0 tools.
 *
 * §18.9 — Mandatory request headers (apollocore gate):
 *   - Content-Type: application/json
 *   - Accept: application/json, text/event-stream  (some servers stream)
 *   - Origin: http://localhost                       (CORS gate)
 *   - MCP-Protocol-Version: 2025-11-25              (pin spec version)
 *   - Mcp-Session-Id: <captured>                    (replayed post-init)
 */

#include "HttpMcpTransport.h"
#include "api/CurlMultiSession.h"

#include <iostream>
#include <utility>

namespace mcp {

namespace {
// Spec version pinned to whatever apollocore speaks today. Bump when the
// upstream spec revs.
constexpr const char* kProtocolVersion = "2025-11-25";
}  // namespace

HttpMcpTransport::HttpMcpTransport(std::string base_url, std::string bearer_token)
    : base_url_(std::move(base_url)), bearer_token_(std::move(bearer_token)) {}

HttpMcpTransport::~HttpMcpTransport() {
    disconnect();
}

std::expected<void, TransportError> HttpMcpTransport::connect() {
    if (connected_.load()) {
        return std::unexpected(TransportError::ConnectionFailed);
    }
    if (base_url_.empty()) {
        return std::unexpected(TransportError::ConnectionFailed);
    }
    connected_.store(true);
    return {};
}

void HttpMcpTransport::disconnect() {
    connected_.store(false);
    // Drop the session id; reconnect requires a fresh initialize.
    std::scoped_lock lk(session_id_mu_);
    session_id_.clear();
}

// Populate the four mandatory headers on every outbound request, and
// the fifth (Mcp-Session-Id) once we have one.
static void add_standard_headers(
    std::vector<std::string>& dst,
    const std::string& bearer_token,
    const std::string& session_id
) {
    dst.push_back("Content-Type: application/json");
    dst.push_back("Accept: application/json, text/event-stream");
    dst.push_back("Origin: http://localhost");
    dst.push_back("MCP-Protocol-Version: " + std::string(kProtocolVersion));
    if (!session_id.empty()) {
        dst.push_back("Mcp-Session-Id: " + session_id);
    }
    if (!bearer_token.empty()) {
        dst.push_back("Authorization: Bearer " + bearer_token);
    }
}

std::future<std::expected<nlohmann::json, TransportError>>
HttpMcpTransport::send_request(
    const std::string& method,
    const nlohmann::json& params,
    int timeout_ms
) {
    auto promise = std::make_shared<std::promise<std::expected<nlohmann::json, TransportError>>>();
    auto future = promise->get_future();

    if (!connected_.load()) {
        promise->set_value(std::unexpected(TransportError::NotConnected));
        return future;
    }

    // Build JSON-RPC request
    nlohmann::json request = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", method}
    };
    if (!params.is_null()) {
        request["params"] = params;
    }

    // Snapshot the current session id; capture into the closure by value
    // so concurrent connect()/disconnect() can't tear us out from under
    // the in-flight request.
    std::string sid_snapshot;
    {
        std::scoped_lock lk(session_id_mu_);
        sid_snapshot = session_id_;
    }

    // Submit to CurlMultiSession
    using daedalus::net::CurlRequest;
    using daedalus::net::CurlMultiSession;
    CurlRequest req;
    req.url = base_url_;
    req.method = "POST";
    req.body = request.dump();
    add_standard_headers(req.headers, bearer_token_, sid_snapshot);
    req.timeout_seconds = timeout_ms / 1000;

    auto curl_fut = CurlMultiSession::get_instance().submit(req);

    // When curl completes, parse and fulfill the promise. We capture
    // method by value so we know whether this response might carry a
    // session-id header (only `initialize` does, per the spec).
    std::thread([curl_fut = std::move(curl_fut), promise, method, this]() mutable {
        auto result = curl_fut.get();
        if (!result) {
            switch (result.error()) {
                case daedalus::net::CurlError::Network:
                    promise->set_value(std::unexpected(TransportError::ConnectionFailed));
                    break;
                case daedalus::net::CurlError::Timeout:
                    promise->set_value(std::unexpected(TransportError::Timeout));
                    break;
                case daedalus::net::CurlError::Cancelled:
                    promise->set_value(std::unexpected(TransportError::Cancelled));
                    break;
                default:
                    promise->set_value(std::unexpected(TransportError::ReadFailed));
            }
            return;
        }

        // §18.9 — Capture Mcp-Session-Id from the initialize handshake
        // so subsequent requests can be authenticated against this
        // session. Spec: https://modelcontextprotocol.io/specification/
        //   2025-06-18/basic/transports#session-management
        if (method == "initialize" && !result->mcp_session_id.empty()) {
            std::scoped_lock lk(session_id_mu_);
            session_id_ = std::move(result->mcp_session_id);
        }

        if (result->status_code < 200 || result->status_code >= 300) {
            promise->set_value(std::unexpected(TransportError::InvalidResponse));
            return;
        }

        try {
            nlohmann::json response = nlohmann::json::parse(result->body);
            if (response.contains("error")) {
                promise->set_value(std::unexpected(TransportError::InvalidResponse));
            } else if (response.contains("result")) {
                promise->set_value(response["result"]);
            } else {
                promise->set_value(nlohmann::json::object());
            }
        } catch (const std::exception& e) {
            promise->set_value(std::unexpected(TransportError::InvalidResponse));
        }
    }).detach();

    return future;
}

std::expected<void, TransportError> HttpMcpTransport::send_notification(
    const std::string& method,
    const nlohmann::json& params
) {
    if (!connected_.load()) {
        return std::unexpected(TransportError::NotConnected);
    }

    nlohmann::json notification = {
        {"jsonrpc", "2.0"},
        {"method", method}
    };
    if (!params.is_null()) {
        notification["params"] = params;
    }

    std::string sid_snapshot;
    {
        std::scoped_lock lk(session_id_mu_);
        sid_snapshot = session_id_;
    }

    using daedalus::net::CurlRequest;
    using daedalus::net::CurlMultiSession;
    CurlRequest req;
    req.url = base_url_;
    req.method = "POST";
    req.body = notification.dump();
    add_standard_headers(req.headers, bearer_token_, sid_snapshot);
    req.timeout_seconds = 30;

    auto fut = CurlMultiSession::get_instance().submit(req);
    auto result = fut.get();
    if (!result) {
        return std::unexpected(TransportError::WriteFailed);
    }
    return {};
}

bool HttpMcpTransport::health_check() {
    return connected_.load();
}

} // namespace mcp
