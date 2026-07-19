// SPDX-License-Identifier: BSL-1.0
// demos/daedalus-webui/routes.h
//
// HTTP route handlers for the webui demo. Extracted from main.cpp.

#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "OmniNet.h"
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"

#include "index_html.h"
#include "session_manager.h"

namespace websse {
using namespace OmniNet;

// ============================================================================
// Path helpers
// ============================================================================
//
// OmniNet's Router accepts regex-pattern routes (e.g. /sessions/([^/]+)/messages)
// but doesn't pass capture groups to the handler. So we parse the path
// from `req.path()` ourselves to pull out the session id.

static std::string extract_session_id(std::string_view path) {
    // /sessions/<id>/<something> → <id>
    constexpr std::string_view prefix = "/sessions/";
    if (path.substr(0, prefix.size()) != prefix) return {};
    auto rest = path.substr(prefix.size());
    auto slash = rest.find('/');
    if (slash == std::string_view::npos) return {};
    return std::string(rest.substr(0, slash));
}

static std::string json_error(const std::string& msg) {
    return json{ {"error", msg} }.dump();
}

static std::string extract_message(const HTTP::Request& req) {
    try {
        auto body = req.bodyAsString();
        if (body.empty()) return {};
        auto j = json::parse(body);
        if (j.contains("message") && j["message"].is_string())
            return j["message"].get<std::string>();
    } catch (...) {}
    return {};
}

static bool extract_approval(const HTTP::Request& req, bool& approved, bool& sticky) {
    try {
        auto body = req.bodyAsString();
        if (body.empty()) return false;
        auto j = json::parse(body);
        if (!j.contains("approved") || !j["approved"].is_boolean()) return false;
        approved = j["approved"].get<bool>();
        sticky   = j.value("sticky", false);
        return true;
    } catch (...) {}
    return false;
}

// ============================================================================
// Route registration
// ============================================================================

static void register_routes(HTTP::Server& server, SessionManager& mgr) {
    using Req = HTTP::Request;
    using Resp = HTTP::Response;

    // GET / — serve the embedded HTML page.
    server.router().get("/", [](const Req&, Resp& resp) {
        resp.setStatus(HTTP::StatusCode::OK);
        resp.setContentType("text/html; charset=utf-8");
        resp.setBody(kIndexHtml);
    });

    // POST /sessions — create a new session.
    // §Phase 9.6: response now includes "model" (the resolved
    // Agent::active_model()) so the UI can display which model
    // the session is bound to without an extra round-trip.
    server.router().post("/sessions", [&mgr](const Req&, Resp& resp) {
        std::string id = mgr.create();
        json j = { {"session_id", id} };
        // Embed the model JSON inline (already a string).
        std::string model_json = mgr.get_model(id);
        // We need to merge model_json's keys into j. parse + insert.
        try {
            auto mj = json::parse(model_json);
            j["model"] = mj;
        } catch (...) {
            j["model"] = json::object();
        }
        resp.setStatus(HTTP::StatusCode::OK);
        resp.setContentType("application/json");
        resp.setBody(j.dump());
    });

    // POST /sessions/<id>/messages — submit user message.
    server.router().post(R"(/sessions/([^/]+)/messages)",
        [&mgr](const Req& req, Resp& resp) {
            std::string sid = extract_session_id(req.path());
            std::string text = extract_message(req);
            if (sid.empty() || text.empty()) {
                resp.setStatus(HTTP::StatusCode::BAD_REQUEST);
                resp.setContentType("application/json");
                resp.setBody(json_error("missing session_id or message"));
                return;
            }
            mgr.submit_message(sid, std::move(text));
            resp.setStatus(HTTP::StatusCode::ACCEPTED);
            resp.setContentType("application/json");
            resp.setBody(R"({"status":"queued"})");
        });

    // POST /sessions/<id>/cancel — cancel current turn.
    server.router().post(R"(/sessions/([^/]+)/cancel)",
        [&mgr](const Req& req, Resp& resp) {
            std::string sid = extract_session_id(req.path());
            if (sid.empty()) {
                resp.setStatus(HTTP::StatusCode::BAD_REQUEST);
                resp.setContentType("application/json");
                resp.setBody(json_error("bad path"));
                return;
            }
            mgr.cancel(sid);
            resp.setStatus(HTTP::StatusCode::OK);
            resp.setContentType("application/json");
            resp.setBody(R"({"status":"cancelled"})");
        });

    // POST /sessions/<id>/approve — approve or deny a pending bash command.
    // §webui-approval: the browser sends {approved: true/false, sticky: bool}.
    server.router().post(R"(/sessions/([^/]+)/approve)",
        [&mgr](const Req& req, Resp& resp) {
            std::string sid = extract_session_id(req.path());
            bool approved = false, sticky = false;
            if (sid.empty() || !extract_approval(req, approved, sticky)) {
                resp.setStatus(HTTP::StatusCode::BAD_REQUEST);
                resp.setContentType("application/json");
                resp.setBody(json_error("missing session_id or approved boolean"));
                return;
            }
            if (!mgr.resolve_approval(sid, approved, sticky)) {
                resp.setStatus(HTTP::StatusCode::NOT_FOUND);
                resp.setContentType("application/json");
                resp.setBody(json_error("no such session"));
                return;
            }
            resp.setStatus(HTTP::StatusCode::OK);
            resp.setContentType("application/json");
            resp.setBody(R"({"status":"resolved"})");
        });

    // GET /sessions/<id>/state — return agent state as JSON.
    server.router().get(R"(/sessions/([^/]+)/state)",
        [&mgr](const Req& req, Resp& resp) {
            std::string sid = extract_session_id(req.path());
            resp.setStatus(HTTP::StatusCode::OK);
            resp.setContentType("application/json");
            resp.setBody(mgr.get_state(sid));
        });

    // GET /sessions/<id>/stats — return AgentStats as JSON.
    // §webui-usage: polled by the header HUD.
    server.router().get(R"(/sessions/([^/]+)/stats)",
        [&mgr](const Req& req, Resp& resp) {
            std::string sid = extract_session_id(req.path());
            resp.setStatus(HTTP::StatusCode::OK);
            resp.setContentType("application/json");
            resp.setBody(mgr.get_stats(sid));
        });

    // GET /healthz — liveness probe.
    server.router().get("/healthz", [](const Req&, Resp& resp) {
        resp.setStatus(HTTP::StatusCode::OK);
        resp.setContentType("application/json");
        resp.setBody(R"({"ok":true})");
    });

    // GET /info — server-side metadata for the UI. cwd is the working
    // directory the webui was launched in (fs::current_path()), which
    // is exactly the "project location it was passed". Stable for the
    // lifetime of the process; the UI fetches it once on page load.
    server.router().get("/info", [](const Req&, Resp& resp) {
        resp.setStatus(HTTP::StatusCode::OK);
        resp.setContentType("application/json");
        std::error_code ec;
        auto cwd = fs::current_path(ec);
        json j = {
            {"cwd", ec ? std::string{} : cwd.string()},
        };
        resp.setBody(j.dump());
    });
}



} // namespace websse
