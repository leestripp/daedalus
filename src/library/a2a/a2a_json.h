// SPDX-License-Identifier: BSL-1.0
// src/library/a2a/a2a_json.h
//
// Internal JSON serialization / deserialization helpers for the A2A
// HTTP+JSON/REST binding. Shared by A2aClient and A2aServer.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "daedalus/a2a/types.h"

namespace daedalus::a2a::detail {

using json = nlohmann::json;

// ============================================================================
// JSON helpers
// ============================================================================

[[nodiscard]] inline std::optional<std::string> opt_str(const json& j, std::string_view key) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return std::nullopt;
    return it->get<std::string>();
}

template <typename T>
[[nodiscard]] inline std::optional<T> opt_val(const json& j, std::string_view key) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return std::nullopt;
    return it->get<T>();
}

[[nodiscard]] inline json opt_metadata(const json& j, std::string_view key) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return nullptr;
    return *it;
}

template <typename T>
inline void fill_vec(const json& j, std::string_view key, std::vector<T>& out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_array()) return;
    out.reserve(it->size());
    for (const auto& e : *it) out.push_back(e.get<T>());
}

// ============================================================================
// Serialization (C++ -> JSON)
// ============================================================================

[[nodiscard]] inline json to_json(const Part& p) {
    json j;
    j["kind"] = to_string(p.kind);
    if (!p.metadata.is_null()) j["metadata"] = p.metadata;
    switch (p.kind) {
        case PartKind::text:
            if (p.text) j["text"] = *p.text;
            break;
        case PartKind::file: {
            json file;
            if (p.filename) file["name"] = *p.filename;
            if (p.media_type) file["mimeType"] = *p.media_type;
            if (p.raw) file["bytes"] = *p.raw;
            if (p.url) file["uri"] = *p.url;
            j["file"] = std::move(file);
            break;
        }
        case PartKind::data:
            if (!p.data.is_null()) j["data"] = p.data;
            break;
    }
    return j;
}

[[nodiscard]] inline json to_json(const Message& m) {
    json j;
    j["kind"] = "message";
    j["messageId"] = m.message_id;
    j["role"] = to_string(m.role);
    j["parts"] = json::array();
    for (const auto& p : m.parts) j["parts"].push_back(to_json(p));
    if (m.context_id) j["contextId"] = *m.context_id;
    if (m.task_id) j["taskId"] = *m.task_id;
    if (!m.metadata.is_null()) j["metadata"] = m.metadata;
    if (!m.extensions.empty()) j["extensions"] = m.extensions;
    if (!m.reference_task_ids.empty()) j["referenceTaskIds"] = m.reference_task_ids;
    return j;
}

[[nodiscard]] inline json to_json(const SendMessageConfiguration& c) {
    json j;
    if (!c.accepted_output_modes.empty()) j["acceptedOutputModes"] = c.accepted_output_modes;
    if (c.history_length) j["historyLength"] = *c.history_length;
    if (c.return_immediately) j["returnImmediately"] = true;
    return j;
}

[[nodiscard]] inline json to_json(const SendMessageRequest& r) {
    json j;
    j["message"] = to_json(r.message);
    if (r.configuration) j["configuration"] = to_json(*r.configuration);
    if (!r.metadata.is_null()) j["metadata"] = r.metadata;
    if (r.tenant) j["tenant"] = *r.tenant;
    return j;
}

[[nodiscard]] inline json to_json(const CancelTaskRequest& r) {
    json j;
    if (r.tenant) j["tenant"] = *r.tenant;
    if (!r.metadata.is_null()) j["metadata"] = r.metadata;
    return j;
}

[[nodiscard]] inline json to_json(const TaskStatus& s) {
    json j;
    j["state"] = to_string(s.state);
    if (s.message) j["message"] = to_json(*s.message);
    if (s.timestamp) j["timestamp"] = *s.timestamp;
    return j;
}

[[nodiscard]] inline json to_json(const Artifact& a) {
    json j;
    j["artifactId"] = a.artifact_id;
    if (a.name) j["name"] = *a.name;
    if (a.description) j["description"] = *a.description;
    if (!a.metadata.is_null()) j["metadata"] = a.metadata;
    if (!a.extensions.empty()) j["extensions"] = a.extensions;
    j["parts"] = json::array();
    for (const auto& p : a.parts) j["parts"].push_back(to_json(p));
    return j;
}

[[nodiscard]] inline json to_json(const Task& t) {
    json j;
    j["kind"] = "task";
    j["id"] = t.id;
    if (t.context_id) j["contextId"] = *t.context_id;
    j["status"] = to_json(t.status);
    if (!t.metadata.is_null()) j["metadata"] = t.metadata;
    if (!t.artifacts.empty()) {
        j["artifacts"] = json::array();
        for (const auto& a : t.artifacts) j["artifacts"].push_back(to_json(a));
    }
    if (!t.history.empty()) {
        j["history"] = json::array();
        for (const auto& m : t.history) j["history"].push_back(to_json(m));
    }
    return j;
}

[[nodiscard]] inline json to_json(const ListTasksResponse& r) {
    json j;
    j["tasks"] = json::array();
    for (const auto& t : r.tasks) j["tasks"].push_back(to_json(t));
    if (!r.next_page_token.empty()) j["nextPageToken"] = r.next_page_token;
    if (r.page_size != 0) j["pageSize"] = r.page_size;
    if (r.total_size != 0) j["totalSize"] = r.total_size;
    return j;
}

[[nodiscard]] inline json to_json(const AgentInterface& i) {
    json j;
    j["url"] = i.url;
    j["transport"] = i.transport;
    if (i.tenant) j["tenant"] = *i.tenant;
    return j;
}

[[nodiscard]] inline json to_json(const AgentCapabilities& c) {
    json j;
    if (c.streaming.has_value()) j["streaming"] = *c.streaming;
    if (c.push_notifications.has_value()) j["pushNotifications"] = *c.push_notifications;
    if (c.state_transition_history.has_value()) j["stateTransitionHistory"] = *c.state_transition_history;
    if (!c.extensions.empty()) j["extensions"] = c.extensions;
    return j;
}

[[nodiscard]] inline json to_json(const AgentSkill& s) {
    json j;
    j["id"] = s.id;
    j["name"] = s.name;
    j["description"] = s.description;
    if (!s.tags.empty()) j["tags"] = s.tags;
    if (!s.examples.empty()) j["examples"] = s.examples;
    if (!s.input_modes.empty()) j["inputModes"] = s.input_modes;
    if (!s.output_modes.empty()) j["outputModes"] = s.output_modes;
    return j;
}

[[nodiscard]] inline json to_json(const AgentCard& c) {
    json j;
    j["protocolVersion"] = c.protocol_version;
    j["name"] = c.name;
    j["description"] = c.description;
    j["url"] = c.url;
    j["preferredTransport"] = c.preferred_transport;
    if (!c.additional_interfaces.empty()) {
        j["additionalInterfaces"] = json::array();
        for (const auto& i : c.additional_interfaces) j["additionalInterfaces"].push_back(to_json(i));
    }
    if (!c.provider.is_null()) j["provider"] = c.provider;
    j["version"] = c.version;
    if (c.documentation_url) j["documentationUrl"] = *c.documentation_url;
    j["capabilities"] = to_json(c.capabilities);
    if (!c.security_schemes.is_null()) j["securitySchemes"] = c.security_schemes;
    if (!c.security_requirements.is_null()) j["security"] = c.security_requirements;
    if (!c.default_input_modes.empty()) j["defaultInputModes"] = c.default_input_modes;
    if (!c.default_output_modes.empty()) j["defaultOutputModes"] = c.default_output_modes;
    if (!c.skills.empty()) {
        j["skills"] = json::array();
        for (const auto& s : c.skills) j["skills"].push_back(to_json(s));
    }
    if (c.supports_authenticated_extended_card) j["supportsAuthenticatedExtendedCard"] = true;
    if (!c.signatures.is_null()) j["signatures"] = c.signatures;
    if (c.icon_url) j["iconUrl"] = *c.icon_url;
    return j;
}

[[nodiscard]] inline json to_json(const TaskStatusUpdateEvent& e) {
    json j;
    j["taskId"] = e.task_id;
    j["contextId"] = e.context_id;
    j["status"] = to_json(e.status);
    if (!e.metadata.is_null()) j["metadata"] = e.metadata;
    return j;
}

[[nodiscard]] inline json to_json(const TaskArtifactUpdateEvent& e) {
    json j;
    j["taskId"] = e.task_id;
    j["contextId"] = e.context_id;
    j["artifact"] = to_json(e.artifact);
    if (e.append) j["append"] = true;
    if (e.last_chunk) j["lastChunk"] = true;
    if (!e.metadata.is_null()) j["metadata"] = e.metadata;
    return j;
}

// ============================================================================
// Deserialization (JSON -> C++)
// ============================================================================

[[nodiscard]] inline Part parse_part(const json& j) {
    Part p;
    p.kind = part_kind_from_string(opt_str(j, "kind").value_or(""));
    p.metadata = opt_metadata(j, "metadata");

    switch (p.kind) {
        case PartKind::text:
            p.text = opt_str(j, "text");
            break;
        case PartKind::file: {
            auto fit = j.find("file");
            if (fit != j.end() && fit->is_object()) {
                p.filename = opt_str(*fit, "name");
                p.media_type = opt_str(*fit, "mimeType");
                p.raw = opt_str(*fit, "bytes");
                p.url = opt_str(*fit, "uri");
            }
            break;
        }
        case PartKind::data:
            p.data = opt_metadata(j, "data");
            break;
    }

    // Backward compatibility with legacy flat parts that omitted "kind".
    if (!j.contains("kind")) {
        if (j.contains("text")) {
            p.kind = PartKind::text;
            p.text = opt_str(j, "text");
        } else if (j.contains("data")) {
            p.kind = PartKind::data;
            p.data = opt_metadata(j, "data");
        } else if (j.contains("url") || j.contains("raw")) {
            p.kind = PartKind::file;
            p.url = opt_str(j, "url");
            p.raw = opt_str(j, "raw");
        }
        p.filename = opt_str(j, "filename");
        p.media_type = opt_str(j, "mediaType");
    }
    return p;
}

[[nodiscard]] inline Message parse_message(const json& j) {
    Message m;
    m.message_id = opt_str(j, "messageId").value_or("");
    m.context_id = opt_str(j, "contextId");
    m.task_id = opt_str(j, "taskId");
    m.role = role_from_string(opt_str(j, "role").value_or(""));
    m.metadata = opt_metadata(j, "metadata");
    fill_vec(j, "extensions", m.extensions);
    fill_vec(j, "referenceTaskIds", m.reference_task_ids);
    auto it = j.find("parts");
    if (it != j.end() && it->is_array()) {
        for (const auto& e : *it) m.parts.push_back(parse_part(e));
    }
    return m;
}

[[nodiscard]] inline TaskStatus parse_task_status(const json& j) {
    TaskStatus s;
    s.state = task_state_from_string(opt_str(j, "state").value_or(""));
    auto it = j.find("message");
    if (it != j.end() && !it->is_null()) s.message = parse_message(*it);
    s.timestamp = opt_str(j, "timestamp");
    return s;
}

[[nodiscard]] inline Artifact parse_artifact(const json& j) {
    Artifact a;
    a.artifact_id = opt_str(j, "artifactId").value_or("");
    a.name = opt_str(j, "name");
    a.description = opt_str(j, "description");
    a.metadata = opt_metadata(j, "metadata");
    fill_vec(j, "extensions", a.extensions);
    auto it = j.find("parts");
    if (it != j.end() && it->is_array()) {
        for (const auto& e : *it) a.parts.push_back(parse_part(e));
    }
    return a;
}

[[nodiscard]] inline Task parse_task(const json& j) {
    Task t;
    t.id = opt_str(j, "id").value_or("");
    t.context_id = opt_str(j, "contextId");
    auto sit = j.find("status");
    if (sit != j.end() && !sit->is_null()) t.status = parse_task_status(*sit);
    t.metadata = opt_metadata(j, "metadata");
    auto ait = j.find("artifacts");
    if (ait != j.end() && ait->is_array()) {
        for (const auto& e : *ait) t.artifacts.push_back(parse_artifact(e));
    }
    auto hit = j.find("history");
    if (hit != j.end() && hit->is_array()) {
        for (const auto& e : *hit) t.history.push_back(parse_message(e));
    }
    return t;
}

[[nodiscard]] inline AgentInterface parse_agent_interface(const json& j) {
    AgentInterface i;
    i.url = opt_str(j, "url").value_or("");
    i.transport = opt_str(j, "transport").value_or("");
    i.tenant = opt_str(j, "tenant");
    if (i.transport.empty()) {
        i.transport = opt_str(j, "protocolBinding").value_or("");
    }
    return i;
}

[[nodiscard]] inline AgentCapabilities parse_agent_capabilities(const json& j) {
    AgentCapabilities c;
    c.streaming = opt_val<bool>(j, "streaming");
    c.push_notifications = opt_val<bool>(j, "pushNotifications");
    c.state_transition_history = opt_val<bool>(j, "stateTransitionHistory");
    if (!c.state_transition_history.has_value()) {
        c.state_transition_history = opt_val<bool>(j, "extendedAgentCard");
    }
    auto it = j.find("extensions");
    if (it != j.end() && it->is_array()) {
        for (const auto& e : *it) c.extensions.push_back(e);
    }
    return c;
}

[[nodiscard]] inline AgentSkill parse_agent_skill(const json& j) {
    AgentSkill s;
    s.id = opt_str(j, "id").value_or("");
    s.name = opt_str(j, "name").value_or("");
    s.description = opt_str(j, "description").value_or("");
    fill_vec(j, "tags", s.tags);
    fill_vec(j, "examples", s.examples);
    fill_vec(j, "inputModes", s.input_modes);
    fill_vec(j, "outputModes", s.output_modes);
    return s;
}

[[nodiscard]] inline AgentCard parse_agent_card(const json& j) {
    AgentCard c;
    c.protocol_version = opt_str(j, "protocolVersion").value_or("");
    c.name = opt_str(j, "name").value_or("");
    c.description = opt_str(j, "description").value_or("");
    c.url = opt_str(j, "url").value_or("");
    c.preferred_transport = opt_str(j, "preferredTransport").value_or("");
    c.version = opt_str(j, "version").value_or("");
    c.documentation_url = opt_str(j, "documentationUrl");
    c.icon_url = opt_str(j, "iconUrl");
    c.provider = opt_metadata(j, "provider");
    c.security_schemes = opt_metadata(j, "securitySchemes");
    c.security_requirements = opt_metadata(j, "security");
    if (c.security_requirements.is_null()) {
        c.security_requirements = opt_metadata(j, "securityRequirements");
    }
    c.signatures = opt_metadata(j, "signatures");
    c.supports_authenticated_extended_card = opt_val<bool>(j, "supportsAuthenticatedExtendedCard").value_or(false);
    c.capabilities = parse_agent_capabilities(opt_metadata(j, "capabilities"));
    fill_vec(j, "defaultInputModes", c.default_input_modes);
    fill_vec(j, "defaultOutputModes", c.default_output_modes);
    auto ait = j.find("additionalInterfaces");
    if (ait != j.end() && ait->is_array()) {
        for (const auto& e : *ait) c.additional_interfaces.push_back(parse_agent_interface(e));
    }
    auto sit = j.find("supportedInterfaces");
    if (sit != j.end() && sit->is_array()) {
        for (const auto& e : *sit) c.additional_interfaces.push_back(parse_agent_interface(e));
    }
    auto skit = j.find("skills");
    if (skit != j.end() && skit->is_array()) {
        for (const auto& e : *skit) c.skills.push_back(parse_agent_skill(e));
    }
    return c;
}

[[nodiscard]] inline SendMessageResult parse_send_message_response(const json& j) {
    SendMessageResult r;
    auto tit = j.find("task");
    if (tit != j.end() && !tit->is_null()) r.task = parse_task(*tit);
    auto mit = j.find("message");
    if (mit != j.end() && !mit->is_null()) r.message = parse_message(*mit);
    return r;
}

[[nodiscard]] inline ListTasksResponse parse_list_tasks_response(const json& j) {
    ListTasksResponse r;
    r.next_page_token = opt_str(j, "nextPageToken").value_or("");
    r.page_size = opt_val<int>(j, "pageSize").value_or(0);
    r.total_size = opt_val<int>(j, "totalSize").value_or(0);
    auto it = j.find("tasks");
    if (it != j.end() && it->is_array()) {
        for (const auto& e : *it) r.tasks.push_back(parse_task(e));
    }
    return r;
}

[[nodiscard]] inline TaskStatusUpdateEvent parse_status_update(const json& j) {
    TaskStatusUpdateEvent e;
    e.task_id = opt_str(j, "taskId").value_or("");
    e.context_id = opt_str(j, "contextId").value_or("");
    auto sit = j.find("status");
    if (sit != j.end() && !sit->is_null()) e.status = parse_task_status(*sit);
    e.metadata = opt_metadata(j, "metadata");
    return e;
}

[[nodiscard]] inline TaskArtifactUpdateEvent parse_artifact_update(const json& j) {
    TaskArtifactUpdateEvent e;
    e.task_id = opt_str(j, "taskId").value_or("");
    e.context_id = opt_str(j, "contextId").value_or("");
    auto ait = j.find("artifact");
    if (ait != j.end() && !ait->is_null()) e.artifact = parse_artifact(*ait);
    e.append = opt_val<bool>(j, "append").value_or(false);
    e.last_chunk = opt_val<bool>(j, "lastChunk").value_or(false);
    e.metadata = opt_metadata(j, "metadata");
    return e;
}

inline void dispatch_stream_response(const json& j, IA2aEventHandler& h) {
    auto tit = j.find("task");
    if (tit != j.end() && !tit->is_null()) {
        h.on_task(parse_task(*tit));
        return;
    }
    auto mit = j.find("message");
    if (mit != j.end() && !mit->is_null()) {
        h.on_message(parse_message(*mit));
        return;
    }
    auto sut = j.find("statusUpdate");
    if (sut != j.end() && !sut->is_null()) {
        h.on_status_update(parse_status_update(*sut));
        return;
    }
    auto aut = j.find("artifactUpdate");
    if (aut != j.end() && !aut->is_null()) {
        h.on_artifact_update(parse_artifact_update(*aut));
        return;
    }
}

} // namespace daedalus::a2a::detail
