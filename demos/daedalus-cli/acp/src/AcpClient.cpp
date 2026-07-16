/**
 * @file demos/daedalus-cli/acp/src/AcpClient.cpp
 * @brief ACP Client implementation
 *
 * Error model (2026-07-16): all public methods return AcpResult<T> =
 * std::expected<T, AcpError>. A method that "succeeds with no value"
 * returns AcpResult<void>. Callers MUST inspect the result before
 * dereferencing — no more silent `{}` returns that conflate
 * "empty list" with "transport failed".
 */

#include "acp/AcpClient.h"
#include <iostream>

namespace acp {

// Convert a transport Response into an AcpResult. Returns the parsed
// JSON body on success, an AcpError on transport / HTTP failure.
static AcpResult<nlohmann::json> response_to_result(const IAcpTransport::Response& r) {
    if (!r.transport_error.empty()) {
        // Transport failures don't have an HTTP status — use -1 as a
        // sentinel so callers can distinguish "never reached the server"
        // from "HTTP 0 returned something weird".
        int code = r.status_code != 0 ? r.status_code : -1;
        std::optional<std::string> cause;
        // Tag the cause as "transport" for non-curl failures (HTTP errors
        // already say "HTTP N" in the message).
        if (r.transport_error.find("curl:") != 0) cause = "transport";
        return std::unexpected(make_error(r.transport_error, code, std::move(cause)));
    }
    // Defensive: if transport_error is empty but status is 0, treat as
    // failure (something went wrong before status was set).
    if (r.status_code == 0) {
        return std::unexpected(make_error("unknown transport failure", -1));
    }
    return r.body;
}

// Parse a Run from a JSON response. Used by create_run / get_run /
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

// ----- Agent operations ----------------------------------------------------

AcpResult<std::vector<AgentManifest>> AcpClient::list_agents(int limit, int offset) {
    std::string path = "/v1/agents?limit=" + std::to_string(limit) + "&offset=" + std::to_string(offset);
    auto r = response_to_result(transport_->get(path));
    if (!r) return std::unexpected(r.error());

    const nlohmann::json& body = *r;
    std::vector<AgentManifest> agents;
    if (body.contains("agents") && body["agents"].is_array()) {
        for (const auto& agent_json : body["agents"]) {
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
            agents.push_back(std::move(agent));
        }
    }
    return agents;
}

AcpResult<AgentManifest> AcpClient::get_agent(const AgentName& name) {
    std::string path = "/v1/agents/" + name;
    auto r = response_to_result(transport_->get(path));
    if (!r) return std::unexpected(r.error());

    AgentManifest agent;
    const nlohmann::json& response = *r;
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
            agent.metadata.capabilities.streaming     = caps.value("streaming", false);
            agent.metadata.capabilities.async_mode   = caps.value("async_mode", false);
            agent.metadata.capabilities.input_feeding = caps.value("input_feeding", false);
        }
    }
    return agent;
}

// ----- Run operations ------------------------------------------------------

AcpResult<Run> AcpClient::create_run(
    const AgentName& agent,
    std::vector<Message> input,
    RunMode mode,
    std::optional<SessionId> session_id)
{
    std::string path = "/v1/runs";

    nlohmann::json body;
    body["agent_name"] = agent;
    body["input"] = input;

    switch (mode) {
        case RunMode::Sync:   body["mode"] = "sync"; break;
        case RunMode::Async:  body["mode"] = "async"; break;
        case RunMode::Stream: body["mode"] = "stream"; break;
    }
    if (session_id) body["session_id"] = *session_id;

    auto r = response_to_result(transport_->post(path, body));
    if (!r) return std::unexpected(r.error());
    return parse_run_response(*r, agent);
}

AcpResult<Run> AcpClient::get_run(const RunId& run_id) {
    std::string path = "/v1/runs/" + run_id;
    auto r = response_to_result(transport_->get(path));
    if (!r) return std::unexpected(r.error());
    auto run = parse_run_response(*r);
    if (run.run_id.empty()) run.run_id = run_id;
    return run;
}

AcpResult<Run> AcpClient::resume_run(
    const RunId& run_id,
    const nlohmann::json& resume_data,
    RunMode mode)
{
    std::string path = "/v1/runs/" + run_id + "/resume";

    nlohmann::json body;
    body["resume_data"] = resume_data;
    switch (mode) {
        case RunMode::Sync:   body["mode"] = "sync"; break;
        case RunMode::Async:  body["mode"] = "async"; break;
        case RunMode::Stream: body["mode"] = "stream"; break;
    }

    auto r = response_to_result(transport_->post(path, body));
    if (!r) return std::unexpected(r.error());
    auto run = parse_run_response(*r);
    if (run.run_id.empty()) run.run_id = run_id;
    return run;
}

AcpResult<Run> AcpClient::cancel_run(const RunId& run_id) {
    std::string path = "/v1/runs/" + run_id + "/cancel";
    auto r = response_to_result(transport_->post(path, nlohmann::json::object()));
    if (!r) return std::unexpected(r.error());
    auto run = parse_run_response(*r);
    if (run.run_id.empty()) run.run_id = run_id;
    if (run.status == RunStatus::Created) run.status = RunStatus::Cancelled;
    return run;
}

AcpResult<void> AcpClient::stream_run_events(const RunId& run_id, IRunEventHandler& handler) {
    // SSE is a stub for now — return an explicit error so callers know.
    return std::unexpected(make_error(
        "SSE streaming not implemented (ACP-7 open)", 501 /* Not Implemented */));
    (void)run_id; (void)handler;  // suppress unused-param warnings
}

// ----- Session operations --------------------------------------------------

AcpResult<Session> AcpClient::get_session(const SessionId& session_id) {
    std::string path = "/v1/sessions/" + session_id;
    auto r = response_to_result(transport_->get(path));
    if (!r) return std::unexpected(r.error());

    Session session;
    from_json(*r, session);
    if (session.session_id.empty()) session.session_id = session_id;
    return session;
}

// ----- Utility -------------------------------------------------------------

bool AcpClient::ping() {
    auto r = transport_->get("/v1/ping");
    if (r.transport_error.empty() && r.status_code >= 200 && r.status_code < 300) {
        return true;
    }
    r = transport_->get("/");
    return r.transport_error.empty() && r.status_code >= 200 && r.status_code < 300;
}

// ============================================================================
// Convenience factory
// ============================================================================

std::unique_ptr<AcpClient> create_client(const std::string& base_url) {
    auto transport = create_curl_transport(base_url);
    return std::make_unique<AcpClient>(std::move(transport), base_url);
}

} // namespace acp