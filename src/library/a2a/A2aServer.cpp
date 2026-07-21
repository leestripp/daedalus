// SPDX-License-Identifier: BSL-1.0
// src/library/a2a/A2aServer.cpp
//
// A2A HTTP+JSON/REST server implementation backed by OmniNet.

#include "daedalus/a2a/A2aServer.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "OmniNet.h"
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"
#include "http/HttpServer.h"

#include "core/Logger.h"
#include "library/a2a/a2a_json.h"

namespace daedalus::a2a {

namespace {

using json = nlohmann::json;
using namespace daedalus::a2a::detail;

using OmniReq = OmniNet::HTTP::Request;
using OmniResp = OmniNet::HTTP::Response;
using OmniMethod = OmniNet::HTTP::Method;

// ============================================================================
// Path helpers
// ============================================================================

[[nodiscard]] std::string_view task_id_prefix() { return "/v1/tasks/"; }

[[nodiscard]] std::string extract_task_id(std::string_view path) {
    auto prefix = task_id_prefix();
    if (!path.starts_with(prefix)) return {};
    auto id = path.substr(prefix.size());
    // Strip :cancel or :subscribe suffix.
    if (auto pos = id.rfind(':'); pos != std::string_view::npos) {
        id = id.substr(0, pos);
    }
    return std::string(id);
}

// ============================================================================
// SSE builder
// ============================================================================

[[nodiscard]] std::string sse_line(std::string_view event_name, const json& payload) {
    json wrapper;
    wrapper[event_name] = payload;
    return std::string("data: ") + wrapper.dump() + "\n\n";
}

struct CollectingHandler : IA2aEventHandler {
    std::string body;

    void on_task(const Task& t) override { body += sse_line("task", to_json(t)); }
    void on_message(const Message& m) override { body += sse_line("message", to_json(m)); }
    void on_status_update(const TaskStatusUpdateEvent& e) override {
        body += sse_line("statusUpdate", to_json(e));
    }
    void on_artifact_update(const TaskArtifactUpdateEvent& e) override {
        body += sse_line("artifactUpdate", to_json(e));
    }
    void on_error(const A2aError& err) override {
        json j{{"code", err.code.empty() ? "INTERNAL_ERROR" : err.code},
               {"message", err.message}};
        if (!err.details.is_null()) j["details"] = err.details;
        body += sse_line("error", j);
    }
};

// ============================================================================
// Error response
// ============================================================================

void send_error(OmniResp& resp, const A2aError& err) {
    json j;
    j["error"]["code"] = err.code.empty() ? "INTERNAL_ERROR" : err.code;
    j["error"]["message"] = err.message;
    if (!err.details.is_null()) j["error"]["details"] = err.details;

    int status = err.http_status != 0 ? err.http_status : 500;
    resp.setStatus(status);
    resp.setContentType("application/a2a+json");
    resp.setBody(j.dump());
}

void send_json(OmniResp& resp, const json& j, int status = 200) {
    resp.setStatus(status);
    resp.setContentType("application/a2a+json");
    resp.setBody(j.dump());
}

void send_sse(OmniResp& resp, const std::string& body) {
    resp.setStatus(OmniNet::HTTP::StatusCode::OK);
    resp.setContentType("text/event-stream");
    resp.setHeader("Cache-Control", "no-cache");
    resp.setConnectionClose();
    resp.setBody(body);
}

// ============================================================================
// Request builders from OmniNet query/body
// ============================================================================

[[nodiscard]] std::optional<std::string> opt_query(const OmniReq& req, std::string_view key) {
    auto v = req.query().queryString(std::string(key));
    if (!v || v->empty()) return std::nullopt;
    return *v;
}

[[nodiscard]] std::optional<TaskState> opt_task_state(const OmniReq& req, std::string_view key) {
    auto v = req.query().queryString(std::string(key));
    if (!v || v->empty()) return std::nullopt;
    return task_state_from_string(*v);
}

[[nodiscard]] std::optional<bool> opt_bool_query(const OmniReq& req, std::string_view key) {
    auto v = req.query().queryString(std::string(key));
    if (!v) return std::nullopt;
    if (*v == "true") return true;
    if (*v == "false") return false;
    return std::nullopt;
}

[[nodiscard]] GetTaskRequest build_get_task_request(const OmniReq& req, std::string_view path) {
    GetTaskRequest r;
    r.id = extract_task_id(path);
    r.tenant = opt_query(req, "tenant");
    r.history_length = req.query().queryInt("historyLength");
    return r;
}

[[nodiscard]] ListTasksRequest build_list_tasks_request(const OmniReq& req) {
    ListTasksRequest r;
    r.tenant = opt_query(req, "tenant");
    r.context_id = opt_query(req, "contextId");
    r.status = opt_task_state(req, "status");
    r.page_size = req.query().queryInt("pageSize");
    r.page_token = opt_query(req, "pageToken");
    r.history_length = req.query().queryInt("historyLength");
    r.status_timestamp_after = opt_query(req, "statusTimestampAfter");
    r.include_artifacts = opt_bool_query(req, "includeArtifacts");
    return r;
}

[[nodiscard]] CancelTaskRequest build_cancel_task_request(const OmniReq& req,
                                                           std::string_view path) {
    CancelTaskRequest r;
    r.id = extract_task_id(path);
    try {
        auto j = json::parse(req.bodyAsString());
        r.tenant = opt_str(j, "tenant");
        r.metadata = opt_metadata(j, "metadata");
    } catch (...) {
        // Empty body is acceptable; tenant/metadata default to null.
    }
    return r;
}

[[nodiscard]] SubscribeToTaskRequest build_subscribe_request(const OmniReq& req,
                                                              std::string_view path) {
    SubscribeToTaskRequest r;
    r.id = extract_task_id(path);
    r.tenant = opt_query(req, "tenant");
    return r;
}

[[nodiscard]] std::optional<SendMessageRequest> parse_send_message_request(const OmniReq& req,
                                                                           A2aError& err) {
    try {
        auto j = json::parse(req.bodyAsString());
        SendMessageRequest r;
        r.tenant = opt_str(j, "tenant");
        r.metadata = opt_metadata(j, "metadata");
        auto mit = j.find("message");
        if (mit == j.end() || mit->is_null()) {
            err = A2aError{400, "INVALID_PARAMETER", "missing 'message' field", nullptr};
            return std::nullopt;
        }
        r.message = parse_message(*mit);
        auto cit = j.find("configuration");
        if (cit != j.end() && !cit->is_null()) {
            SendMessageConfiguration c;
            fill_vec(*cit, "acceptedOutputModes", c.accepted_output_modes);
            c.history_length = opt_val<int>(*cit, "historyLength");
            c.return_immediately = opt_val<bool>(*cit, "returnImmediately").value_or(false);
            r.configuration = std::move(c);
        }
        return r;
    } catch (const std::exception& e) {
        err = A2aError{400, "INVALID_JSON", std::string("invalid JSON: ") + e.what(), nullptr};
        return std::nullopt;
    }
}

} // anonymous namespace

// ============================================================================
// A2aServer::Impl
// ============================================================================

class A2aServer::Impl {
public:
    explicit Impl(A2aServerConfig cfg)
        : cfg_(std::move(cfg)) {
        if (!cfg_.backend) {
            throw std::invalid_argument("A2aServerConfig::backend is required");
        }

        OmniNet::HTTP::Server::Config osc;
        osc.address = cfg_.address;
        osc.port = cfg_.port;
        osc.workerCount = static_cast<size_t>(cfg_.worker_count > 0 ? cfg_.worker_count : 2);
        osc.logRequests = false;

        server_ = std::make_unique<OmniNet::HTTP::Server>(osc);
        server_->router().any("*",
            [this](const OmniReq& req, OmniResp& resp) { handle(req, resp); });
    }

    void start() {
        running_ = true;
        server_->start();
        running_ = false;
    }

    void stop() {
        server_->stop();
    }

    [[nodiscard]] uint16_t port() const {
        return server_->boundPort();
    }

    [[nodiscard]] std::string base_url() const {
        return "http://" + cfg_.address + ":" + std::to_string(port());
    }

    [[nodiscard]] bool running() const { return running_.load(); }

private:
    void handle(const OmniReq& req, OmniResp& resp) {
        const auto method = req.method();
        const std::string path(req.path());

        try {
            if (method == OmniMethod::GET && path == "/.well-known/agent-card.json") {
                return handle_agent_card(req, resp);
            }
            if (method == OmniMethod::POST && path == "/v1/message:send") {
                return handle_send_message(req, resp);
            }
            if (method == OmniMethod::POST && path == "/v1/message:stream") {
                return handle_stream_message(req, resp);
            }
            if (method == OmniMethod::GET && path == "/v1/tasks") {
                return handle_list_tasks(req, resp);
            }
            if (method == OmniMethod::GET && path.starts_with(std::string(task_id_prefix())) &&
                !path.ends_with(":cancel") && !path.ends_with(":subscribe")) {
                return handle_get_task(req, resp, path);
            }
            if (method == OmniMethod::POST && path.starts_with(std::string(task_id_prefix())) &&
                path.ends_with(":cancel")) {
                return handle_cancel_task(req, resp, path);
            }
            if ((method == OmniMethod::GET || method == OmniMethod::POST) &&
                path.starts_with(std::string(task_id_prefix())) && path.ends_with(":subscribe")) {
                return handle_subscribe(req, resp, path);
            }

            resp.setStatus(OmniNet::HTTP::StatusCode::NOT_FOUND);
            resp.setContentType("application/a2a+json");
            resp.setBody(json{{"error", {{"code", "NOT_FOUND"}, {"message", "unknown endpoint"}}}}.dump());
        } catch (const std::exception& e) {
            LOG_ERROR(std::string("A2A server handler exception: ") + e.what());
            send_error(resp, A2aError{500, "INTERNAL_ERROR", e.what(), nullptr});
        }
    }

    void handle_agent_card(const OmniReq&, OmniResp& resp) {
        send_json(resp, to_json(cfg_.backend->get_agent_card()));
    }

    void handle_send_message(const OmniReq& req, OmniResp& resp) {
        A2aError err{400, "INVALID_JSON", "", nullptr};
        auto request = parse_send_message_request(req, err);
        if (!request) return send_error(resp, err);

        auto result = cfg_.backend->send_message(*request);
        if (!result) return send_error(resp, result.error());

        json j;
        if (result->task) j["task"] = to_json(*result->task);
        if (result->message) j["message"] = to_json(*result->message);
        send_json(resp, j);
    }

    void handle_stream_message(const OmniReq& req, OmniResp& resp) {
        A2aError err{400, "INVALID_JSON", "", nullptr};
        auto request = parse_send_message_request(req, err);
        if (!request) return send_error(resp, err);

        CollectingHandler handler;
        auto result = cfg_.backend->send_streaming_message(*request, handler);
        if (!result) return send_error(resp, result.error());
        send_sse(resp, handler.body);
    }

    void handle_get_task(const OmniReq& req, OmniResp& resp, const std::string& path) {
        auto request = build_get_task_request(req, path);
        if (request.id.empty()) {
            return send_error(resp, A2aError{400, "INVALID_PARAMETER", "missing task id", nullptr});
        }
        auto result = cfg_.backend->get_task(request);
        if (!result) return send_error(resp, result.error());
        send_json(resp, to_json(*result));
    }

    void handle_list_tasks(const OmniReq& req, OmniResp& resp) {
        auto request = build_list_tasks_request(req);
        auto result = cfg_.backend->list_tasks(request);
        if (!result) return send_error(resp, result.error());
        send_json(resp, to_json(*result));
    }

    void handle_cancel_task(const OmniReq& req, OmniResp& resp, const std::string& path) {
        auto request = build_cancel_task_request(req, path);
        if (request.id.empty()) {
            return send_error(resp, A2aError{400, "INVALID_PARAMETER", "missing task id", nullptr});
        }
        auto result = cfg_.backend->cancel_task(request);
        if (!result) return send_error(resp, result.error());
        send_json(resp, to_json(*result));
    }

    void handle_subscribe(const OmniReq& req, OmniResp& resp, const std::string& path) {
        auto request = build_subscribe_request(req, path);
        if (request.id.empty()) {
            return send_error(resp, A2aError{400, "INVALID_PARAMETER", "missing task id", nullptr});
        }
        CollectingHandler handler;
        auto result = cfg_.backend->subscribe_to_task(request, handler);
        if (!result) return send_error(resp, result.error());
        send_sse(resp, handler.body);
    }

    A2aServerConfig cfg_;
    std::unique_ptr<OmniNet::HTTP::Server> server_;
    std::atomic<bool> running_{false};
};

// ============================================================================
// A2aServer public API
// ============================================================================

A2aServer::A2aServer(A2aServerConfig cfg)
    : impl_(std::make_unique<Impl>(std::move(cfg))) {}

A2aServer::~A2aServer() = default;

void A2aServer::start() { impl_->start(); }

void A2aServer::stop() { impl_->stop(); }

uint16_t A2aServer::port() const { return impl_->port(); }

std::string A2aServer::base_url() const { return impl_->base_url(); }

bool A2aServer::running() const { return impl_->running(); }

} // namespace daedalus::a2a
