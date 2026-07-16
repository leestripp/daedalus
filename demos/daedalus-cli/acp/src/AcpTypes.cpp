/**
 * @file src/acp/src/AcpTypes.cpp
 * @brief ACP type implementations
 */

#include "acp/AcpFwd.h"
#include <stdexcept>

namespace acp {

// ============================================================================
// Serialization helpers
// ============================================================================

void from_json(const nlohmann::json& j, MessagePart& m) {
    if (j.contains("name")) m.name = j.at("name").get<std::string>();
    m.content_type = j.value("content_type", "text/plain");
    if (j.contains("content")) m.content = j.at("content").get<std::string>();
    if (j.contains("content_encoding")) m.content_encoding = j.at("content_encoding").get<std::string>();
    if (j.contains("content_url")) m.content_url = j.at("content_url").get<std::string>();
    if (j.contains("metadata")) m.metadata = j.at("metadata");
}

void to_json(nlohmann::json& j, const MessagePart& m) {
    if (m.name) j["name"] = *m.name;
    j["content_type"] = m.content_type;
    if (m.content) j["content"] = *m.content;
    if (m.content_encoding) j["content_encoding"] = *m.content_encoding;
    if (m.content_url) j["content_url"] = *m.content_url;
    if (m.metadata) j["metadata"] = *m.metadata;
}

void from_json(const nlohmann::json& j, Message& m) {
    m.role = j.at("role").get<std::string>();
    m.parts = j.at("parts").get<std::vector<MessagePart>>();
    if (j.contains("created_at")) {
        // Stored as opaque ISO8601 string — caller can parse if it cares.
        m.created_at = j.at("created_at").get<std::string>();
    }
}

void to_json(nlohmann::json& j, const Message& m) {
    j["role"] = m.role;
    j["parts"] = m.parts;
    if (m.created_at) j["created_at"] = *m.created_at;
}

void from_json(const nlohmann::json& j, Run& r) {
    if (j.contains("run_id"))     r.run_id     = j.at("run_id").get<std::string>();
    if (j.contains("agent_name")) r.agent_name = j.at("agent_name").get<std::string>();
    if (j.contains("session_id")) r.session_id = j.at("session_id").get<std::string>();
    if (j.contains("status"))     r.status     = parse_run_status(j.at("status").get<std::string>());
    if (j.contains("created_at")) r.created_at = j.at("created_at").get<std::string>();
    if (j.contains("updated_at")) r.updated_at = j.at("updated_at").get<std::string>();
}

// ============================================================================
// Status <-> wire string
// ============================================================================

RunStatus parse_run_status(const std::string& s) {
    if (s == "created")     return RunStatus::Created;
    if (s == "in-progress") return RunStatus::InProgress;
    if (s == "awaiting")    return RunStatus::Awaiting;
    if (s == "completed")   return RunStatus::Completed;
    if (s == "cancelled")   return RunStatus::Cancelled;
    if (s == "failed")      return RunStatus::Failed;
    return RunStatus::Created;
}

const char* run_status_to_string(RunStatus s) {
    switch (s) {
        case RunStatus::Created:    return "created";
        case RunStatus::InProgress: return "in-progress";
        case RunStatus::Awaiting:   return "awaiting";
        case RunStatus::Completed:  return "completed";
        case RunStatus::Cancelled:  return "cancelled";
        case RunStatus::Failed:     return "failed";
    }
    return "created";  // unreachable; keeps -Wreturn-type happy
}

void to_json(nlohmann::json& j, const RunCreateRequest& req) {
    j["agent_name"] = req.agent_name;
    j["input"] = req.input;
    
    std::string mode_str;
    switch (req.mode) {
        case RunMode::Sync: mode_str = "sync"; break;
        case RunMode::Async: mode_str = "async"; break;
        case RunMode::Stream: mode_str = "stream"; break;
    }
    j["mode"] = mode_str;
    
    if (req.session_id) {
        // TODO: add session_id
    }
}

} // namespace acp
