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
    
    // Synchronous operations
    virtual nlohmann::json get(const std::string& path) = 0;
    virtual nlohmann::json post(const std::string& path, const nlohmann::json& body) = 0;
    
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
     * @return List of agent manifests
     */
    std::vector<AgentManifest> list_agents(int limit = 10, int offset = 0);
    
    /**
     * @brief Get manifest for specific agent
     * @param name Agent name (RFC 1123)
     * @return Agent manifest
     * @throws AcpError if agent not found
     */
    AgentManifest get_agent(const AgentName& name);
    
    // ========================================================================
    // Run operations
    // ========================================================================
    
    /**
     * @brief Create and start a new agent run
     * @param agent Agent name to run
     * @param input Messages to send
     * @param mode Sync/Async/Stream
     * @param session_id Optional session ID for stateful runs
     * @return Run object
     */
    Run create_run(
        const AgentName& agent,
        std::vector<Message> input,
        RunMode mode = RunMode::Sync,
        std::optional<SessionId> session_id = std::nullopt
    );
    
    /**
     * @brief Get run status
     * @param run_id Run identifier
     * @return Current run state
     */
    Run get_run(const RunId& run_id);
    
    /**
     * @brief Resume an awaiting run
     * @param run_id Run to resume
     * @param resume_data Data to resume with (from await_request)
     * @param mode Sync/Async/Stream
     * @return Updated run
     */
    Run resume_run(
        const RunId& run_id,
        const nlohmann::json& resume_data,
        RunMode mode = RunMode::Sync
    );
    
    /**
     * @brief Cancel a run
     * @param run_id Run to cancel
     * @return Updated run (status should be Cancelled)
     */
    Run cancel_run(const RunId& run_id);
    
    /**
     * @brief Stream run events via SSE
     * @param run_id Run to watch
     * @param handler Event callbacks
     */
    void stream_run_events(const RunId& run_id, IRunEventHandler& handler);
    
    // ========================================================================
    // Session operations
    // ========================================================================
    
    /**
     * @brief Get session details
     * @param session_id Session identifier
     * @return Session with history URIs
     */
    Session get_session(const SessionId& session_id);
    
    // ========================================================================
    // Utility
    // ========================================================================
    
    /**
     * @brief Ping the server
     * @return true if server responds
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
std::unique_ptr<IAcpTransport> create_curl_transport(const std::string& base_url);

std::unique_ptr<AcpClient> create_client(const std::string& base_url);

} // namespace acp
