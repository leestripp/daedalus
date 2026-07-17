/**
 * @file src/acp/src/AcpTypes.cpp
 * @brief ACP type implementations
 */

#include "acp/AcpFwd.h"
#include "acp/AcpClient.h"   // §ACP-7: IRunEventHandler definition is needed for dispatch.

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

void from_json(const nlohmann::json& j, Session& s) {
    if (j.contains("session_id")) s.session_id = j.at("session_id").get<std::string>();
    if (j.contains("agent_name")) s.agent_name = j.at("agent_name").get<std::string>();
    if (j.contains("messages") && j["messages"].is_array()) {
        s.messages = j.at("messages").get<std::vector<Message>>();
    }
    if (j.contains("created_at")) s.created_at = j.at("created_at").get<std::string>();
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
        j["session_id"] = *req.session_id;
    }
}

// ============================================================================
// SSE event dispatch (ACP-7)
// ============================================================================
//
// Wire format (from docs/acp-openapi.yaml):
//   data: {"type": "<event>", <payload>}\n
//   \n
//
// The `type` discriminator selects the IRunEventHandler callback. The
// `event:` SSE field is NOT used — everything is in the data JSON.
//
// All payload structs (`message`, `run`, `part`, `error`) are deserialised
// via the existing from_json() helpers. A parse failure in a payload
// means we drop that event silently — better to drop a bad event than
// to crash the whole stream on transient corruption.
//
// Unknown `type` values fall through to on_generic with the raw JSON,
// so forward-compatible additions (new event types in future spec
// revisions) don't get lost.

void dispatch_sse_event(const RunId& run_id,
                        const std::string& event_data_json,
                        IRunEventHandler& handler) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(event_data_json);
    } catch (const std::exception&) {
        // Malformed JSON — drop.
        return;
    }
    if (!j.is_object()) {
        // Spec requires an object with `type` discriminator.
        return;
    }
    const std::string type = j.value("type", std::string{});
    if (type.empty()) {
        // Missing `type` — drop. Could surface as on_generic, but the
        // spec is explicit that `type` is required.
        return;
    }

    try {
        if (type == "message.created" || type == "message.completed") {
            Message m;
            if (j.contains("message")) from_json(j["message"], m);
            if (type == "message.created") handler.on_message_created(run_id, std::move(m));
            else                            handler.on_message_completed(run_id, std::move(m));
        } else if (type == "message.part") {
            MessagePart p;
            if (j.contains("part")) from_json(j["part"], p);
            handler.on_message_part(run_id, std::move(p));
        } else if (type == "generic") {
            nlohmann::json payload = j.value("generic", nlohmann::json::object());
            handler.on_generic(run_id, payload);
        } else if (type == "run.created" || type == "run.in-progress" ||
                   type == "run.awaiting"  || type == "run.completed"  ||
                   type == "run.failed"    || type == "run.cancelled") {
            Run run;
            if (j.contains("run")) from_json(j["run"], run);
            if      (type == "run.created")     handler.on_run_created(run_id,     std::move(run));
            else if (type == "run.in-progress") handler.on_run_in_progress(run_id, std::move(run));
            else if (type == "run.awaiting")    handler.on_run_awaiting(run_id,    std::move(run));
            else if (type == "run.completed")   handler.on_run_completed(run_id,   std::move(run));
            else if (type == "run.failed")      handler.on_run_failed(run_id,      std::move(run));
            else /* run.cancelled */            handler.on_run_cancelled(run_id,   std::move(run));
        } else if (type == "error") {
            AcpError err;
            if (j.contains("error") && j["error"].is_object()) {
                const auto& e = j["error"];
                err.code    = e.value("code", 0);
                // §ACP-15 (2026-07-16): truncate to prevent a malicious or
                // buggy server from sending an unbounded error (e.g. full HTML
                // error page / stack trace) into a std::string that could
                // grow to consume all memory on the receiving end.
                constexpr std::size_t MAX_ERROR_MSG = 1024;
                err.message = e.value("message", std::string{});
                if (err.message.size() > MAX_ERROR_MSG) {
                    err.message.resize(MAX_ERROR_MSG);
                    err.message += "…[truncated]";
                }
                if (e.contains("cause")) {
                    err.cause = e["cause"].get<std::string>();
                    if (err.cause->size() > MAX_ERROR_MSG) {
                        err.cause->resize(MAX_ERROR_MSG);
                        *err.cause += "…[truncated]";
                    }
                }
            }
            handler.on_error(run_id, std::move(err));
        } else {
            // Unknown event type — surface via on_generic with the raw JSON
            // so callers can inspect. Spec-compatible behaviour.
            handler.on_generic(run_id, j);
        }
    } catch (const std::exception& e) {
        // Per-event parse failure — log via on_generic with a warning.
        // Better to keep streaming than to abort.
        handler.on_generic(run_id, nlohmann::json{
            {"warning", std::string("dispatch failed: ") + e.what()},
            {"event_type", type},
            {"raw", event_data_json}});
    }
}

} // namespace acp
