/**
 * @file src/acp/src/AcpClient.cpp
 * @brief ACP Client implementation
 */

#include "acp/AcpClient.h"
#include <iostream>

namespace acp {

// Helper to check if response is valid (not null/empty/discarded)
static bool is_valid_response(const nlohmann::json& j) {
    return !j.is_discarded() && !j.is_null() && !j.empty();
}

// Parse ACP status string to enum. Protocol strings are kebab-case
// ("created", "in-progress", "awaiting", "completed", "cancelled", "failed").
// See AcpTypes.cpp for the canonical implementation.
using acp::parse_run_status;

// Extract a Run from a JSON response. Used by create_run / get_run /
// resume_run / cancel_run. The server may omit optional fields; we just
// leave them default-constructed.
static Run parse_run_response(const nlohmann::json& response,
                              const AgentName& fallback_agent = "") {
    Run run;
    run.run_id     = response.value("run_id", "");
    run.agent_name = response.value("agent_name", fallback_agent);
    run.session_id = response.value("session_id", std::string{});
    run.status     = parse_run_status(response.value("status", "created"));
    if (response.contains("created_at")) run.created_at = response["created_at"].get<std::string>();
    if (response.contains("updated_at")) run.updated_at = response["updated_at"].get<std::string>();
    return run;
}

// ============================================================================
// AcpClient implementation
// ============================================================================

AcpClient::AcpClient(std::unique_ptr<IAcpTransport> transport, std::string base_url)
    : transport_(std::move(transport))
    , base_url_(std::move(base_url))
{
}

AcpClient::~AcpClient() = default;

std::vector<AgentManifest> AcpClient::list_agents(int limit, int offset) {
    std::string path = "/v1/agents?limit=" + std::to_string(limit) + "&offset=" + std::to_string(offset);
    nlohmann::json response = transport_->get(path);
    
    if (!is_valid_response(response)) {
        return {};
    }
    
    try {
        std::vector<AgentManifest> agents;
        if (response.contains("agents")) {
            for (const auto& agent_json : response["agents"]) {
                AgentManifest agent;
                agent.name = agent_json.value("name", "");
                if (agent_json.contains("metadata")) {
                    const auto& meta = agent_json["metadata"];
                    if (meta.contains("description")) {
                        agent.metadata.description = meta["description"].get<std::string>();
                    }
                    if (meta.contains("version")) {
                        agent.metadata.version = meta["version"].get<std::string>();
                    }
                }
                agents.push_back(agent);
            }
        }
        return agents;
    } catch (const std::exception& e) {
        std::cerr << "[AcpClient] Error parsing agents: " << e.what() << "\n";
        return {};
    }
}

AgentManifest AcpClient::get_agent(const AgentName& name) {
    std::string path = "/v1/agents/" + name;
    nlohmann::json response = transport_->get(path);
    
    if (!is_valid_response(response)) {
        return {};
    }
    
    AgentManifest agent;
    agent.name = response.value("name", name);
    
    if (response.contains("metadata")) {
        const auto& meta = response["metadata"];
        if (meta.contains("description")) {
            agent.metadata.description = meta["description"].get<std::string>();
        }
        if (meta.contains("version")) {
            agent.metadata.version = meta["version"].get<std::string>();
        }
        
        if (meta.contains("capabilities")) {
            const auto& caps = meta["capabilities"];
            agent.metadata.capabilities.streaming = caps.value("streaming", false);
            agent.metadata.capabilities.async_mode = caps.value("async_mode", false);
            agent.metadata.capabilities.input_feeding = caps.value("input_feeding", false);
        }
    }
    
    return agent;
}

Run AcpClient::create_run(
    const AgentName& agent,
    std::vector<Message> input,
    RunMode mode,
    std::optional<SessionId> session_id
) {
    std::string path = "/v1/runs";
    
    nlohmann::json body;
    body["agent_name"] = agent;
    body["input"] = input;
    
    switch (mode) {
        case RunMode::Sync: body["mode"] = "sync"; break;
        case RunMode::Async: body["mode"] = "async"; break;
        case RunMode::Stream: body["mode"] = "stream"; break;
    }
    
    if (session_id) {
        body["session_id"] = *session_id;
    }
    
    nlohmann::json response = transport_->post(path, body);

    if (!is_valid_response(response)) {
        return {};
    }

    return parse_run_response(response, agent);
}

Run AcpClient::get_run(const RunId& run_id) {
    std::string path = "/v1/runs/" + run_id;
    nlohmann::json response = transport_->get(path);

    if (!is_valid_response(response)) {
        return {};
    }

    auto run = parse_run_response(response);
    // Preserve the requested run_id even if the server response omits it
    // (some servers return only status / agent_name).
    if (run.run_id.empty()) run.run_id = run_id;
    return run;
}

Run AcpClient::resume_run(
    const RunId& run_id,
    const nlohmann::json& resume_data,
    RunMode mode
) {
    std::string path = "/v1/runs/" + run_id + "/resume";

    nlohmann::json body;
    body["resume_data"] = resume_data;

    switch (mode) {
        case RunMode::Sync: body["mode"] = "sync"; break;
        case RunMode::Async: body["mode"] = "async"; break;
        case RunMode::Stream: body["mode"] = "stream"; break;
    }

    nlohmann::json response = transport_->post(path, body);

    if (!is_valid_response(response)) {
        return {};
    }

    auto run = parse_run_response(response);
    if (run.run_id.empty()) run.run_id = run_id;
    return run;
}

Run AcpClient::cancel_run(const RunId& run_id) {
    std::string path = "/v1/runs/" + run_id + "/cancel";
    nlohmann::json response = transport_->post(path, nlohmann::json::object());

    if (!is_valid_response(response)) {
        return {};
    }

    auto run = parse_run_response(response);
    if (run.run_id.empty()) run.run_id = run_id;
    // If the server doesn't echo status, treat a successful cancel as
    // Cancelled — the user explicitly asked for this state.
    if (run.status == RunStatus::Created) run.status = RunStatus::Cancelled;
    return run;
}

void AcpClient::stream_run_events(const RunId& run_id, IRunEventHandler& handler) {
    transport_->stream_events(run_id, handler);
}

Session AcpClient::get_session(const SessionId& session_id) {
    std::string path = "/v1/sessions/" + session_id;
    nlohmann::json response = transport_->get(path);
    
    if (!is_valid_response(response)) {
        return {};
    }
    
    Session session;
    session.session_id = session_id;
    session.agent_name = response.value("agent_name", "");
    
    return session;
}

bool AcpClient::ping() {
    // Try /v1/ping first, fall back to root
    nlohmann::json response = transport_->get("/v1/ping");
    if (is_valid_response(response)) {
        return true;
    }
    
    response = transport_->get("/");
    return is_valid_response(response);
}

// ============================================================================
// Convenience factory
// ============================================================================

std::unique_ptr<AcpClient> create_client(const std::string& base_url) {
    auto transport = create_curl_transport(base_url);
    return std::make_unique<AcpClient>(std::move(transport), base_url);
}

} // namespace acp
