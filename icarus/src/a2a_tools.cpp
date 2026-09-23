// SPDX-License-Identifier: BSL-1.0
// src/a2a_tools.cpp
//
// A2A BaseTool implementation — lives at icarus level so it can implement
// daedalus::BaseTool without adding a daedalus dependency to a standalone
// client library (the old ACP library did that; A2A is already part of
// daedalus, so this file simply consumes it).

#include "a2a_tools.h"
#include <mutex>

namespace a2a {

namespace {

// Helper: create a client for the given URL (from args or default).
std::unique_ptr<daedalus::a2a::A2aClient> make_client(
    const std::string& default_url,
    const nlohmann::json& args) {
    std::string url = default_url;
    if (args.contains("a2a_url") && args["a2a_url"].is_string()) {
        url = args["a2a_url"].get<std::string>();
    }
    return daedalus::a2a::create_a2a_client(url);
}

// ============================================================================
// JSON conversion helpers for A2A public types.
// These are intentionally self-contained so we don't depend on internal
// daedalus/a2a JSON implementation details.
// ============================================================================
nlohmann::json part_to_json(const daedalus::a2a::Part& p) {
    nlohmann::json j;
    if (!p.metadata.is_null()) j["metadata"] = p.metadata;
    switch (p.kind) {
        case daedalus::a2a::PartKind::text:
            if (p.text) j["text"] = *p.text;
            if (p.media_type) j["mediaType"] = *p.media_type;
            break;
        case daedalus::a2a::PartKind::file:
            if (p.url) j["url"] = *p.url;
            if (p.filename) j["filename"] = *p.filename;
            if (p.media_type) j["mediaType"] = *p.media_type;
            break;
        case daedalus::a2a::PartKind::data:
            if (!p.data.is_null()) j["data"] = p.data;
            if (p.media_type) j["mediaType"] = *p.media_type;
            break;
    }
    return j;
}

daedalus::a2a::Part part_from_json(const nlohmann::json& j) {
    daedalus::a2a::Part p;
    if (j.contains("metadata") && !j["metadata"].is_null()) {
        p.metadata = j["metadata"];
    }
    if (j.contains("text")) {
        p.kind = daedalus::a2a::PartKind::text;
        if (j["text"].is_string()) p.text = j["text"].get<std::string>();
        if (j.contains("mediaType") && j["mediaType"].is_string()) {
            p.media_type = j["mediaType"].get<std::string>();
        }
    } else if (j.contains("url")) {
        p.kind = daedalus::a2a::PartKind::file;
        if (j["url"].is_string()) p.url = j["url"].get<std::string>();
        if (j.contains("filename") && j["filename"].is_string()) {
            p.filename = j["filename"].get<std::string>();
        }
        if (j.contains("mediaType") && j["mediaType"].is_string()) {
            p.media_type = j["mediaType"].get<std::string>();
        }
    } else if (j.contains("data")) {
        p.kind = daedalus::a2a::PartKind::data;
        p.data = j["data"];
        if (j.contains("mediaType") && j["mediaType"].is_string()) {
            p.media_type = j["mediaType"].get<std::string>();
        }
    } else {
        p.kind = daedalus::a2a::PartKind::text;
    }
    return p;
}

nlohmann::json message_to_json(const daedalus::a2a::Message& m) {
    nlohmann::json j;
    j["messageId"] = m.message_id;
    j["role"] = daedalus::a2a::to_string(m.role);
    j["parts"] = nlohmann::json::array();
    for (const auto& p : m.parts) j["parts"].push_back(part_to_json(p));
    if (m.context_id) j["contextId"] = *m.context_id;
    if (m.task_id) j["taskId"] = *m.task_id;
    if (!m.metadata.is_null()) j["metadata"] = m.metadata;
    if (!m.extensions.empty()) j["extensions"] = m.extensions;
    if (!m.reference_task_ids.empty()) j["referenceTaskIds"] = m.reference_task_ids;
    return j;
}

daedalus::a2a::Message message_from_json(const nlohmann::json& j) {
    daedalus::a2a::Message m;
    if (j.contains("messageId") && j["messageId"].is_string()) {
        m.message_id = j["messageId"].get<std::string>();
    }
    if (j.contains("contextId") && j["contextId"].is_string()) {
        m.context_id = j["contextId"].get<std::string>();
    }
    if (j.contains("taskId") && j["taskId"].is_string()) {
        m.task_id = j["taskId"].get<std::string>();
    }
    if (j.contains("role") && j["role"].is_string()) {
        m.role = daedalus::a2a::role_from_string(j["role"].get<std::string>());
    }
    if (j.contains("metadata") && !j["metadata"].is_null()) {
        m.metadata = j["metadata"];
    }
    if (j.contains("extensions") && j["extensions"].is_array()) {
        for (const auto& e : j["extensions"]) {
            if (e.is_string()) m.extensions.push_back(e.get<std::string>());
        }
    }
    if (j.contains("referenceTaskIds") && j["referenceTaskIds"].is_array()) {
        for (const auto& e : j["referenceTaskIds"]) {
            if (e.is_string()) m.reference_task_ids.push_back(e.get<std::string>());
        }
    }
    if (j.contains("parts") && j["parts"].is_array()) {
        for (const auto& p : j["parts"]) m.parts.push_back(part_from_json(p));
    }
    return m;
}

nlohmann::json task_status_to_json(const daedalus::a2a::TaskStatus& s) {
    nlohmann::json j;
    j["state"] = daedalus::a2a::to_string(s.state);
    if (s.message) j["message"] = message_to_json(*s.message);
    if (s.timestamp) j["timestamp"] = *s.timestamp;
    return j;
}

nlohmann::json artifact_to_json(const daedalus::a2a::Artifact& a) {
    nlohmann::json j;
    j["artifactId"] = a.artifact_id;
    if (a.name) j["name"] = *a.name;
    if (a.description) j["description"] = *a.description;
    if (!a.metadata.is_null()) j["metadata"] = a.metadata;
    if (!a.extensions.empty()) j["extensions"] = a.extensions;
    j["parts"] = nlohmann::json::array();
    for (const auto& p : a.parts) j["parts"].push_back(part_to_json(p));
    return j;
}

nlohmann::json task_to_json(const daedalus::a2a::Task& t) {
    nlohmann::json j;
    j["id"] = t.id;
    if (t.context_id) j["contextId"] = *t.context_id;
    j["status"] = task_status_to_json(t.status);
    if (!t.metadata.is_null()) j["metadata"] = t.metadata;
    if (!t.artifacts.empty()) {
        j["artifacts"] = nlohmann::json::array();
        for (const auto& a : t.artifacts) j["artifacts"].push_back(artifact_to_json(a));
    }
    if (!t.history.empty()) {
        j["history"] = nlohmann::json::array();
        for (const auto& m : t.history) j["history"].push_back(message_to_json(m));
    }
    return j;
}

nlohmann::json agent_interface_to_json(const daedalus::a2a::AgentInterface& i) {
    nlohmann::json j;
    j["url"] = i.url;
    j["transport"] = i.transport;
    if (!i.protocol_version.empty()) j["protocolVersion"] = i.protocol_version;
    if (i.tenant) j["tenant"] = *i.tenant;
    return j;
}

nlohmann::json agent_capabilities_to_json(const daedalus::a2a::AgentCapabilities& c) {
    nlohmann::json j;
    if (c.streaming.has_value()) j["streaming"] = *c.streaming;
    if (c.push_notifications.has_value()) j["pushNotifications"] = *c.push_notifications;
    if (c.extended_agent_card.has_value()) j["extendedAgentCard"] = *c.extended_agent_card;
    if (!c.extensions.empty()) j["extensions"] = c.extensions;
    return j;
}

nlohmann::json agent_skill_to_json(const daedalus::a2a::AgentSkill& s) {
    nlohmann::json j;
    j["id"] = s.id;
    j["name"] = s.name;
    j["description"] = s.description;
    if (!s.tags.empty()) j["tags"] = s.tags;
    if (!s.examples.empty()) j["examples"] = s.examples;
    if (!s.input_modes.empty()) j["inputModes"] = s.input_modes;
    if (!s.output_modes.empty()) j["outputModes"] = s.output_modes;
    return j;
}

nlohmann::json agent_card_to_json(const daedalus::a2a::AgentCard& c) {
    nlohmann::json j;
    j["name"] = c.name;
    j["description"] = c.description;
    if (!c.supported_interfaces.empty()) {
        j["supportedInterfaces"] = nlohmann::json::array();
        for (const auto& i : c.supported_interfaces) {
            j["supportedInterfaces"].push_back(agent_interface_to_json(i));
        }
    }
    if (!c.provider.is_null()) j["provider"] = c.provider;
    j["version"] = c.version;
    if (c.documentation_url) j["documentationUrl"] = *c.documentation_url;
    j["capabilities"] = agent_capabilities_to_json(c.capabilities);
    if (!c.security_schemes.is_null()) j["securitySchemes"] = c.security_schemes;
    if (!c.security_requirements.is_null()) j["securityRequirements"] = c.security_requirements;
    if (!c.default_input_modes.empty()) j["defaultInputModes"] = c.default_input_modes;
    if (!c.default_output_modes.empty()) j["defaultOutputModes"] = c.default_output_modes;
    if (!c.skills.empty()) {
        j["skills"] = nlohmann::json::array();
        for (const auto& s : c.skills) j["skills"].push_back(agent_skill_to_json(s));
    }
    if (!c.signatures.is_null()) j["signatures"] = c.signatures;
    if (c.icon_url) j["iconUrl"] = *c.icon_url;
    return j;
}

nlohmann::json a2a_error_to_json(const daedalus::a2a::A2aError& err) {
    nlohmann::json j;
    j["http_status"] = err.http_status;
    j["code"] = err.code;
    j["message"] = err.message;
    if (!err.details.is_null()) j["details"] = err.details;
    return j;
}

// Helper: build a generic error response.
nlohmann::json error_response(const std::string& message) {
    return nlohmann::json::object({
        {"status", "error"},
        {"message", message}
    });
}

// Helper: build a generic error response from an A2aError.
nlohmann::json error_response(const daedalus::a2a::A2aError& err) {
    nlohmann::json j = a2a_error_to_json(err);
    j["status"] = "error";
    return j;
}

} // anonymous namespace

// ============================================================================
// A2aTool
// ============================================================================

A2aTool::A2aTool(std::string default_url)
    : default_url_(std::move(default_url)) {}

std::string A2aTool::get_name() const {
    return "a2a";
}

std::string A2aTool::get_description() const {
    return "Interact with remote A2A (Agent2Agent Protocol) agents. "
           "Use get_agent_card to discover an agent, send_message to start "
           "a task, and get_task/list_tasks/cancel_task/subscribe_to_task "
           "to manage task lifecycle. Returns structured JSON.";
}

nlohmann::json A2aTool::get_definition() const {
    return nlohmann::json::object({
        {"type", "function"},
        {"function", nlohmann::json::object({
            {"name", get_name()},
            {"description", get_description()},
            {"parameters", nlohmann::json::object({
                {"type", "object"},
                {"properties", nlohmann::json::object({
                    {"action", nlohmann::json::object({
                        {"type", "string"},
                        {"description", "A2A action: get_agent_card | send_message | send_streaming_message | get_task | list_tasks | cancel_task | subscribe_to_task"},
                        {"enum", nlohmann::json::array({
                            "get_agent_card", "send_message",
                            "send_streaming_message", "get_task",
                            "list_tasks", "cancel_task", "subscribe_to_task"
                        })}
                    })},
                    {"message", nlohmann::json::object({
                        {"type", "object"},
                        {"description", "A2A Message object {role, parts[]} for send_message/send_streaming_message"}
                    })},
                    {"task_id", nlohmann::json::object({
                        {"type", "string"},
                        {"description", "Task ID (for get_task, cancel_task, subscribe_to_task)"}
                    })},
                    {"context_id", nlohmann::json::object({
                        {"type", "string"},
                        {"description", "Optional context ID for list_tasks"}
                    })},
                    {"configuration", nlohmann::json::object({
                        {"type", "object"},
                        {"description", "Optional SendMessageConfiguration {acceptedOutputModes, historyLength, returnImmediately}"}
                    })},
                    {"a2a_url", nlohmann::json::object({
                        {"type", "string"},
                        {"description", "Override default A2A server URL"}
                    })}
                })},
                {"required", nlohmann::json::array({"action"})}
            })}
        })}
    });
}

nlohmann::json A2aTool::execute(const nlohmann::json& args,
                                daedalus::HostToolContext& /*ctx*/) {
    if (!args.is_object()) {
        return error_response("arguments must be a JSON object");
    }

    std::string action = args.value("action", "");
    if (action.empty()) {
        return error_response("missing required field: action");
    }

    auto client = make_client(default_url_, args);

    // ── get_agent_card ─────────────────────────────────────────────────────
    if (action == "get_agent_card") {
        auto result = client->get_agent_card();
        if (!result) return error_response(result.error());
        return nlohmann::json::object({
            {"status", "success"},
            {"agent_card", agent_card_to_json(*result)}
        });
    }

    // ── send_message ───────────────────────────────────────────────────────
    if (action == "send_message") {
        daedalus::a2a::SendMessageRequest req;
        if (args.contains("message") && args["message"].is_object()) {
            req.message = message_from_json(args["message"]);
        } else {
            return error_response("send_message requires 'message' object");
        }
        if (args.contains("configuration") && args["configuration"].is_object()) {
            const auto& cfg = args["configuration"];
            daedalus::a2a::SendMessageConfiguration c;
            if (cfg.contains("acceptedOutputModes") && cfg["acceptedOutputModes"].is_array()) {
                for (const auto& e : cfg["acceptedOutputModes"]) {
                    if (e.is_string()) c.accepted_output_modes.push_back(e.get<std::string>());
                }
            }
            if (cfg.contains("historyLength") && cfg["historyLength"].is_number_integer()) {
                c.history_length = cfg["historyLength"].get<int>();
            }
            if (cfg.contains("returnImmediately") && cfg["returnImmediately"].is_boolean()) {
                c.return_immediately = cfg["returnImmediately"].get<bool>();
            }
            req.configuration = std::move(c);
        }
        auto result = client->send_message(req);
        if (!result) return error_response(result.error());
        nlohmann::json out;
        out["status"] = "success";
        if (result->task) out["task"] = task_to_json(*result->task);
        if (result->message) out["message"] = message_to_json(*result->message);
        return out;
    }

    // ── send_streaming_message ─────────────────────────────────────────────
    if (action == "send_streaming_message") {
        daedalus::a2a::SendMessageRequest req;
        if (args.contains("message") && args["message"].is_object()) {
            req.message = message_from_json(args["message"]);
        } else {
            return error_response("send_streaming_message requires 'message' object");
        }
        if (args.contains("configuration") && args["configuration"].is_object()) {
            const auto& cfg = args["configuration"];
            daedalus::a2a::SendMessageConfiguration c;
            if (cfg.contains("acceptedOutputModes") && cfg["acceptedOutputModes"].is_array()) {
                for (const auto& e : cfg["acceptedOutputModes"]) {
                    if (e.is_string()) c.accepted_output_modes.push_back(e.get<std::string>());
                }
            }
            if (cfg.contains("historyLength") && cfg["historyLength"].is_number_integer()) {
                c.history_length = cfg["historyLength"].get<int>();
            }
            if (cfg.contains("returnImmediately") && cfg["returnImmediately"].is_boolean()) {
                c.return_immediately = cfg["returnImmediately"].get<bool>();
            }
            req.configuration = std::move(c);
        }

        struct CollectingHandler : public daedalus::a2a::IA2aEventHandler {
            std::vector<nlohmann::json> events;
            std::mutex mu;

            void on_task(const daedalus::a2a::Task& t) override {
                std::lock_guard g(mu);
                events.push_back(nlohmann::json::object({
                    {"type", "task"},
                    {"task", task_to_json(t)}
                }));
            }
            void on_message(const daedalus::a2a::Message& m) override {
                std::lock_guard g(mu);
                events.push_back(nlohmann::json::object({
                    {"type", "message"},
                    {"message", message_to_json(m)}
                }));
            }
            void on_status_update(const daedalus::a2a::TaskStatusUpdateEvent& e) override {
                std::lock_guard g(mu);
                events.push_back(nlohmann::json::object({
                    {"type", "statusUpdate"},
                    {"taskId", e.task_id},
                    {"contextId", e.context_id},
                    {"status", task_status_to_json(e.status)}
                }));
            }
            void on_artifact_update(const daedalus::a2a::TaskArtifactUpdateEvent& e) override {
                std::lock_guard g(mu);
                events.push_back(nlohmann::json::object({
                    {"type", "artifactUpdate"},
                    {"taskId", e.task_id},
                    {"contextId", e.context_id},
                    {"artifact", artifact_to_json(e.artifact)},
                    {"append", e.append},
                    {"lastChunk", e.last_chunk}
                }));
            }
            void on_error(const daedalus::a2a::A2aError& err) override {
                std::lock_guard g(mu);
                events.push_back(nlohmann::json::object({
                    {"type", "error"},
                    {"error", a2a_error_to_json(err)}
                }));
            }
            void on_complete() override {
                std::lock_guard g(mu);
                events.push_back(nlohmann::json::object({{"type", "complete"}}));
            }
        };

        CollectingHandler handler;
        auto stream_result = client->send_streaming_message(req, handler);
        if (!stream_result) return error_response(stream_result.error());

        std::lock_guard g(handler.mu);
        return nlohmann::json::object({
            {"status", "success"},
            {"events", std::move(handler.events)}
        });
    }

    // ── get_task ───────────────────────────────────────────────────────────
    if (action == "get_task") {
        std::string id = args.value("task_id", "");
        if (id.empty()) return error_response("get_task requires task_id");
        daedalus::a2a::GetTaskRequest req;
        req.id = id;
        if (args.contains("historyLength") && args["historyLength"].is_number_integer()) {
            req.history_length = args["historyLength"].get<int>();
        }
        auto result = client->get_task(req);
        if (!result) return error_response(result.error());
        return nlohmann::json::object({
            {"status", "success"},
            {"task", task_to_json(*result)}
        });
    }

    // ── list_tasks ─────────────────────────────────────────────────────────
    if (action == "list_tasks") {
        daedalus::a2a::ListTasksRequest req;
        if (args.contains("context_id") && args["context_id"].is_string()) {
            req.context_id = args["context_id"].get<std::string>();
        }
        if (args.contains("status") && args["status"].is_string()) {
            req.status = daedalus::a2a::task_state_from_string(args["status"].get<std::string>());
        }
        if (args.contains("pageSize") && args["pageSize"].is_number_integer()) {
            req.page_size = args["pageSize"].get<int>();
        }
        if (args.contains("pageToken") && args["pageToken"].is_string()) {
            req.page_token = args["pageToken"].get<std::string>();
        }
        auto result = client->list_tasks(req);
        if (!result) return error_response(result.error());
        nlohmann::json out;
        out["status"] = "success";
        out["tasks"] = nlohmann::json::array();
        for (const auto& t : result->tasks) out["tasks"].push_back(task_to_json(t));
        if (!result->next_page_token.empty()) out["nextPageToken"] = result->next_page_token;
        if (result->page_size != 0) out["pageSize"] = result->page_size;
        if (result->total_size != 0) out["totalSize"] = result->total_size;
        return out;
    }

    // ── cancel_task ────────────────────────────────────────────────────────
    if (action == "cancel_task") {
        std::string id = args.value("task_id", "");
        if (id.empty()) return error_response("cancel_task requires task_id");
        daedalus::a2a::CancelTaskRequest req;
        req.id = id;
        auto result = client->cancel_task(req);
        if (!result) return error_response(result.error());
        return nlohmann::json::object({
            {"status", "success"},
            {"task", task_to_json(*result)}
        });
    }

    // ── subscribe_to_task ──────────────────────────────────────────────────
    if (action == "subscribe_to_task") {
        std::string id = args.value("task_id", "");
        if (id.empty()) return error_response("subscribe_to_task requires task_id");
        daedalus::a2a::SubscribeToTaskRequest req;
        req.id = id;

        struct CollectingHandler : public daedalus::a2a::IA2aEventHandler {
            std::vector<nlohmann::json> events;
            std::mutex mu;

            void on_task(const daedalus::a2a::Task& t) override {
                std::lock_guard g(mu);
                events.push_back(nlohmann::json::object({
                    {"type", "task"},
                    {"task", task_to_json(t)}
                }));
            }
            void on_message(const daedalus::a2a::Message& m) override {
                std::lock_guard g(mu);
                events.push_back(nlohmann::json::object({
                    {"type", "message"},
                    {"message", message_to_json(m)}
                }));
            }
            void on_status_update(const daedalus::a2a::TaskStatusUpdateEvent& e) override {
                std::lock_guard g(mu);
                events.push_back(nlohmann::json::object({
                    {"type", "statusUpdate"},
                    {"taskId", e.task_id},
                    {"contextId", e.context_id},
                    {"status", task_status_to_json(e.status)}
                }));
            }
            void on_artifact_update(const daedalus::a2a::TaskArtifactUpdateEvent& e) override {
                std::lock_guard g(mu);
                events.push_back(nlohmann::json::object({
                    {"type", "artifactUpdate"},
                    {"taskId", e.task_id},
                    {"contextId", e.context_id},
                    {"artifact", artifact_to_json(e.artifact)},
                    {"append", e.append},
                    {"lastChunk", e.last_chunk}
                }));
            }
            void on_error(const daedalus::a2a::A2aError& err) override {
                std::lock_guard g(mu);
                events.push_back(nlohmann::json::object({
                    {"type", "error"},
                    {"error", a2a_error_to_json(err)}
                }));
            }
            void on_complete() override {
                std::lock_guard g(mu);
                events.push_back(nlohmann::json::object({{"type", "complete"}}));
            }
        };

        CollectingHandler handler;
        auto stream_result = client->subscribe_to_task(req, handler);
        if (!stream_result) return error_response(stream_result.error());

        std::lock_guard g(handler.mu);
        return nlohmann::json::object({
            {"status", "success"},
            {"task_id", id},
            {"events", std::move(handler.events)}
        });
    }

    // ── unknown action ─────────────────────────────────────────────────────
    return error_response("unknown action: " + action +
        ". Valid: get_agent_card, send_message, send_streaming_message, "
        "get_task, list_tasks, cancel_task, subscribe_to_task");
}

} // namespace a2a
