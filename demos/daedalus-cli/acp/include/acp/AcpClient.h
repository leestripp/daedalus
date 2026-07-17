/**
 * @file src/acp/include/acp/AcpClient.h
 * @brief ACP Client - main interface for Agent Communication Protocol
 */
#pragma once

#include "AcpFwd.h"
#include <memory>
#include <string>
#include <functional>

namespace acp {

// ============================================================================
// Event handler interface for SSE streaming
// ============================================================================

class IRunEventHandler {
public:
    virtual ~IRunEventHandler() = default;
    
    virtual void on_message_created(const RunId& run_id, Message message) = 0;
    virtual void on_message_part(const RunId& run_id, MessagePart part) = 0;
    virtual void on_message_completed(const RunId& run_id, Message message) = 0;
    virtual void on_run_created(const RunId& run_id, Run run) = 0;
    virtual void on_run_in_progress(const RunId& run_id, Run run) = 0;
    virtual void on_run_awaiting(const RunId& run_id, Run run) = 0;
    virtual void on_run_completed(const RunId& run_id, Run run) = 0;
    virtual void on_run_cancelled(const RunId& run_id, Run run) = 0;
    virtual void on_run_failed(const RunId& run_id, Run run) = 0;
    virtual void on_error(const RunId& run_id, AcpError error) = 0;
    virtual void on_generic(const RunId& run_id, const nlohmann::json& data) = 0;
};

// ============================================================================
// Transport interface
// ============================================================================

class IAcpTransport {
public:
    virtual ~IAcpTransport() = default;

    /// Result of one HTTP round-trip. Transport-level errors populate
    /// `transport_error` (CURLE failure); HTTP errors populate `status_code`
    /// (still a valid response, just non-2xx). Successful responses have
    /// status_code 2xx, empty transport_error, and a parsed JSON body.
    struct Response {
        long status_code = 0;              // HTTP status (0 if request never reached server)
        nlohmann::json body;               // parsed JSON (may be null on transport error)
        std::string transport_error;       // non-empty if curl_easy_perform failed
    };

    // Synchronous operations
    virtual Response get(const std::string& path) = 0;
    virtual Response post(const std::string& path, const nlohmann::json& body) = 0;

    // Streaming
    virtual void stream_events(const RunId& run_id, IRunEventHandler& handler) = 0;

    // Connection check
    virtual bool is_connected() const = 0;
};

// ============================================================================
// ACP Client
// ============================================================================

class AcpClient {
public:
    /**
     * @brief Construct ACP client
     * @param transport HTTP transport implementation
     * @param base_url Base URL of ACP server (e.g., "http://localhost:8000")
     */
    AcpClient(std::unique_ptr<IAcpTransport> transport, std::string base_url);
    
    ~AcpClient();
    
    // ========================================================================
    // Agent operations
    // ========================================================================

    /**
     * @brief List available agents
     * @param limit Maximum agents to return (1-1000, default 10)
     * @param offset Number to skip for pagination
     * @return List of agent manifests, or AcpError on transport/HTTP failure.
     *         An empty list is a valid success result (the server has no agents).
     */
    AcpResult<std::vector<AgentManifest>> list_agents(int limit = 10, int offset = 0);

    /**
     * @brief Get manifest for specific agent
     * @param name Agent name (RFC 1123)
     * @return Agent manifest, or AcpError if not found / transport failed.
     */
    AcpResult<AgentManifest> get_agent(const AgentName& name);

    // ========================================================================
    // Run operations
    // ========================================================================

    /**
     * @brief Create and start a new agent run
     * @return Run object with server-assigned run_id + initial status, or AcpError.
     */
    AcpResult<Run> create_run(
        const AgentName& agent,
        std::vector<Message> input,
        RunMode mode = RunMode::Sync,
        std::optional<SessionId> session_id = std::nullopt
    );

    /**
     * @brief Get run status
     * @return Current run state, or AcpError if run_id unknown / transport failed.
     */
    AcpResult<Run> get_run(const RunId& run_id);

    /**
     * @brief Resume an awaiting run
     * @return Updated run, or AcpError.
     */
    AcpResult<Run> resume_run(
        const RunId& run_id,
        const nlohmann::json& resume_data,
        RunMode mode = RunMode::Sync
    );

    /**
     * @brief Cancel a run
     * @return Updated run (status Cancelled), or AcpError.
     */
    AcpResult<Run> cancel_run(const RunId& run_id);

    /**
     * @brief Stream run events via SSE
     * @return AcpResult<void> — success once the stream ends; error on
     *         transport failure or if the server rejected the request.
     */
    AcpResult<void> stream_run_events(const RunId& run_id, IRunEventHandler& handler);

    // ========================================================================
    // Session operations
    // ========================================================================

    /**
     * @brief Get session details (history included)
     * @return Session with messages, or AcpError if not found / transport failed.
     */
    AcpResult<Session> get_session(const SessionId& session_id);

    // ========================================================================
    // Utility
    // ========================================================================

    /**
     * @brief Ping the server
     * @return true if server responds with 2xx, false otherwise.
     */
    bool ping();
    
    /**
     * @brief Get base URL
     */
    const std::string& base_url() const { return base_url_; }
    
private:
    std::unique_ptr<IAcpTransport> transport_;
    std::string base_url_;
};

// ============================================================================
// Convenience: Create with default libcurl transport
// ============================================================================

// Internal factory for curl transport
std::unique_ptr<IAcpTransport> create_omni_transport(const std::string& base_url);

std::unique_ptr<AcpClient> create_client(const std::string& base_url);

} // namespace acp
