// SPDX-License-Identifier: BSL-1.0
// demos/daedalus-webui/settings_routes.h
//
// Settings-management REST endpoints for the webui demo.
//
// Demonstrates how a host can read and write daedalus::SettingsStore
// directly (models, general kv settings, MCP servers). All routes use
// SettingsStore::instance(), which is the same DB the Agent loads from.

#pragma once

#include <charconv>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "OmniNet.h"
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"

#include <daedalus/settings.h>
#include <daedalus/Agent.h>
#include <daedalus/model_discovery.h>

namespace websse {
using namespace OmniNet;
using json = nlohmann::json;

namespace detail {

inline std::string json_ok(const json& j = json::object()) {
    json r = j;
    r["status"] = "ok";
    return r.dump();
}

inline std::string json_error(const std::string& msg) {
    return json{{"error", msg}}.dump();
}

inline std::string url_decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hex = 0;
            std::from_chars(s.data() + i + 1, s.data() + i + 3, hex, 16);
            out.push_back(static_cast<char>(hex));
            i += 2;
        } else if (s[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

inline std::string extract_last_segment(std::string_view path) {
    auto pos = path.rfind('/');
    if (pos == std::string_view::npos) return std::string(path);
    return url_decode(path.substr(pos + 1));
}

inline std::string mask_secret(std::string_view s) {
    if (s.empty()) return "";
    if (s.size() <= 8) return std::string(s.size(), '*');
    return std::string(s.substr(0, 4)) + std::string(s.size() - 8, '*') + std::string(s.substr(s.size() - 4));
}

inline json provider_to_json(const daedalus::ProviderConfig& p) {
    return json{
        {"name",               p.name},
        {"host",               p.host},
        {"api_key",            mask_secret(p.api_key)},
        {"chat_endpoint",      p.chat_endpoint},
        {"discovery_endpoint", p.discovery_endpoint},
        {"timeout_seconds",    p.timeout_seconds},
    };
}

inline std::optional<daedalus::ProviderConfig> json_to_provider(const json& j) {
    if (!j.contains("name") || !j["name"].is_string()) return std::nullopt;
    daedalus::ProviderConfig p;
    p.name               = j.value("name", "");
    p.host               = j.value("host", "");
    p.api_key            = j.value("api_key", "");
    p.chat_endpoint      = j.value("chat_endpoint", "/v1/chat/completions");
    p.discovery_endpoint = j.value("discovery_endpoint", "/v1/models");
    p.timeout_seconds    = j.value("timeout_seconds", 30);
    return p;
}

inline json model_to_json(const daedalus::ModelConfig& m) {
    return json{
        {"name",            m.name},
        {"provider",        m.provider},
        {"model_id",        m.model_id},
        {"max_context_size",m.max_context_size},
        {"temperature",     m.temperature},
    };
}

inline std::optional<daedalus::ModelConfig> json_to_model(const json& j) {
    if (!j.contains("name") || !j["name"].is_string()) return std::nullopt;
    daedalus::ModelConfig m;
    m.name             = j.value("name", "");
    m.provider         = j.value("provider", "");
    m.model_id         = j.value("model_id", "");
    m.max_context_size = j.value("max_context_size", 0);
    m.temperature      = j.value("temperature", 0.0);
    return m;
}

inline json mcp_to_json(const daedalus::SettingsStore::McpServer& s) {
    json env = json::object();
    for (const auto& [k, v] : s.env) env[k] = v;
    return json{
        {"name",         s.name},
        {"transport",    s.transport},
        {"command",      s.command},
        {"args",         s.args},
        {"env",          env},
        {"url",          s.url},
        {"bearer_token", mask_secret(s.bearer_token)},
        {"description",  s.description},
        {"enabled",      s.enabled},
    };
}

inline std::optional<daedalus::SettingsStore::McpServer> json_to_mcp(const json& j) {
    if (!j.contains("name") || !j["name"].is_string()) return std::nullopt;
    daedalus::SettingsStore::McpServer s;
    s.name      = j.value("name", "");
    s.transport = j.value("transport", "stdio");
    s.command   = j.value("command", "");
    if (j.contains("args") && j["args"].is_array()) {
        for (const auto& a : j["args"]) s.args.push_back(a.get<std::string>());
    }
    if (j.contains("env") && j["env"].is_object()) {
        for (const auto& [k, v] : j["env"].items()) {
            s.env[k] = v.get<std::string>();
        }
    }
    s.url          = j.value("url", "");
    s.bearer_token = j.value("bearer_token", "");
    s.description  = j.value("description", "");
    s.enabled      = j.value("enabled", true);
    return s;
}

} // namespace detail

// ============================================================================
// Register settings routes
// ============================================================================

inline void register_settings_routes(HTTP::Server& server) {
    using Req = HTTP::Request;
    using Resp = HTTP::Response;
    using namespace detail;

    // ------------------------------------------------------------------------
    // Providers
    // ------------------------------------------------------------------------

    // GET /settings/providers — list all configured providers.
    server.router().get("/settings/providers", [](const Req&, Resp& resp) {
        auto& store = daedalus::SettingsStore::instance();
        json arr = json::array();
        for (const auto& p : store.list_providers_detailed()) {
            arr.push_back(provider_to_json(p));
        }
        resp.setStatus(HTTP::StatusCode::OK);
        resp.setContentType("application/json");
        resp.setBody(json_ok({{"providers", arr}}));
    });

    // POST /settings/providers — upsert a provider.
    server.router().post("/settings/providers", [](const Req& req, Resp& resp) {
        try {
            auto j = json::parse(req.bodyAsString());
            auto p = json_to_provider(j);
            if (!p) {
                resp.setStatus(HTTP::StatusCode::BAD_REQUEST);
                resp.setContentType("application/json");
                resp.setBody(json_error("provider requires a 'name'"));
                return;
            }
            auto& store = daedalus::SettingsStore::instance();
            if (p->api_key.empty()) {
                if (auto existing = store.get_provider(p->name)) {
                    p->api_key = existing->api_key;
                }
            }
            store.upsert_provider(*p);
            resp.setStatus(HTTP::StatusCode::OK);
            resp.setContentType("application/json");
            resp.setBody(json_ok());
        } catch (const std::exception& e) {
            resp.setStatus(HTTP::StatusCode::BAD_REQUEST);
            resp.setContentType("application/json");
            resp.setBody(json_error(e.what()));
        }
    });

    // DELETE /settings/providers/<name> — delete a provider.
    server.router().delete_(R"(/settings/providers/(.+))",
        [](const Req& req, Resp& resp) {
            std::string name = extract_last_segment(req.path());
            if (name.empty()) {
                resp.setStatus(HTTP::StatusCode::BAD_REQUEST);
                resp.setContentType("application/json");
                resp.setBody(json_error("missing provider name"));
                return;
            }
            daedalus::SettingsStore::instance().delete_provider(name);
            resp.setStatus(HTTP::StatusCode::OK);
            resp.setContentType("application/json");
            resp.setBody(json_ok());
        });

    // GET /settings/providers/discover?name=<provider> — query the provider's
    // /v1/models endpoint directly.
    server.router().get("/settings/providers/discover", [](const Req& req, Resp& resp) {
        std::string name;
        if (auto q = req.queryParameter("name"); q.has_value()) {
            name = *q;
        }
        if (name.empty()) {
            resp.setStatus(HTTP::StatusCode::BAD_REQUEST);
            resp.setContentType("application/json");
            resp.setBody(json_error("missing 'name' query parameter"));
            return;
        }
        auto provider = daedalus::SettingsStore::instance().get_provider(name);
        if (!provider) {
            resp.setStatus(HTTP::StatusCode::NOT_FOUND);
            resp.setContentType("application/json");
            resp.setBody(json_error("provider not found"));
            return;
        }
        auto result = daedalus::discover_models(
            provider->host, provider->api_key, provider->discovery_endpoint);
        if (!result.success) {
            resp.setStatus(HTTP::StatusCode::BAD_GATEWAY);
            resp.setContentType("application/json");
            resp.setBody(json_error(result.error_message));
            return;
        }
        json arr = json::array();
        for (const auto& m : result.models) {
            arr.push_back(json{
                {"id", m.id},
                {"owned_by", m.owned_by},
                {"created", m.created},
                {"context_length", m.context_length},
                {"supports_vision", m.supports_vision},
                {"supports_reasoning", m.supports_reasoning}
            });
        }
        resp.setStatus(HTTP::StatusCode::OK);
        resp.setContentType("application/json");
        resp.setBody(json_ok({{"models", arr}}));
    });

    // ------------------------------------------------------------------------
    // Models
    // ------------------------------------------------------------------------

    // GET /settings/models — list all configured models.
    server.router().get("/settings/models", [](const Req&, Resp& resp) {
        auto& store = daedalus::SettingsStore::instance();
        json arr = json::array();
        for (const auto& m : store.list_models()) arr.push_back(model_to_json(m));
        resp.setStatus(HTTP::StatusCode::OK);
        resp.setContentType("application/json");
        resp.setBody(json_ok({{"models", arr},
                              {"default", store.get_default_model_name()}}));
    });

    // GET /settings/models/discover?name=<model> — query the provider's
    // /v1/models endpoint for the named configured model.
    server.router().get("/settings/models/discover", [](const Req& req, Resp& resp) {
        std::string name;
        if (auto q = req.queryParameter("name"); q.has_value()) {
            name = *q;
        }
        if (name.empty()) {
            resp.setStatus(HTTP::StatusCode::BAD_REQUEST);
            resp.setContentType("application/json");
            resp.setBody(json_error("missing 'name' query parameter"));
            return;
        }
        auto result = daedalus::Agent::discover_models(
            name, daedalus::Paths::global().user_root());
        if (!result.success) {
            resp.setStatus(HTTP::StatusCode::BAD_GATEWAY);
            resp.setContentType("application/json");
            resp.setBody(json_error(result.error_message));
            return;
        }
        json arr = json::array();
        for (const auto& m : result.models) {
            arr.push_back(json{
                {"id", m.id},
                {"owned_by", m.owned_by},
                {"created", m.created},
                {"context_length", m.context_length},
                {"supports_vision", m.supports_vision},
                {"supports_reasoning", m.supports_reasoning}
            });
        }
        resp.setStatus(HTTP::StatusCode::OK);
        resp.setContentType("application/json");
        resp.setBody(json_ok({{"models", arr}}));
    });

    // POST /settings/models — upsert a model.
    server.router().post("/settings/models", [](const Req& req, Resp& resp) {
        try {
            auto j = json::parse(req.bodyAsString());
            auto m = json_to_model(j);
            if (!m) {
                resp.setStatus(HTTP::StatusCode::BAD_REQUEST);
                resp.setContentType("application/json");
                resp.setBody(json_error("model requires a 'name'"));
                return;
            }
            auto& store = daedalus::SettingsStore::instance();
            store.upsert_model(*m);
            resp.setStatus(HTTP::StatusCode::OK);
            resp.setContentType("application/json");
            resp.setBody(json_ok());
        } catch (const std::exception& e) {
            resp.setStatus(HTTP::StatusCode::BAD_REQUEST);
            resp.setContentType("application/json");
            resp.setBody(json_error(e.what()));
        }
    });

    // DELETE /settings/models/<name> — delete a model.
    server.router().delete_(R"(/settings/models/(.+))",
        [](const Req& req, Resp& resp) {
            std::string name = extract_last_segment(req.path());
            if (name.empty()) {
                resp.setStatus(HTTP::StatusCode::BAD_REQUEST);
                resp.setContentType("application/json");
                resp.setBody(json_error("missing model name"));
                return;
            }
            daedalus::SettingsStore::instance().delete_model(name);
            resp.setStatus(HTTP::StatusCode::OK);
            resp.setContentType("application/json");
            resp.setBody(json_ok());
        });

    // POST /settings/models/default — set the default model name.
    server.router().post("/settings/models/default", [](const Req& req, Resp& resp) {
        try {
            auto j = json::parse(req.bodyAsString());
            if (!j.contains("name") || !j["name"].is_string()) {
                resp.setStatus(HTTP::StatusCode::BAD_REQUEST);
                resp.setContentType("application/json");
                resp.setBody(json_error("missing 'name'"));
                return;
            }
            daedalus::SettingsStore::instance().set_default_model_name(j["name"].get<std::string>());
            resp.setStatus(HTTP::StatusCode::OK);
            resp.setContentType("application/json");
            resp.setBody(json_ok());
        } catch (const std::exception& e) {
            resp.setStatus(HTTP::StatusCode::BAD_REQUEST);
            resp.setContentType("application/json");
            resp.setBody(json_error(e.what()));
        }
    });

    // ------------------------------------------------------------------------
    // General / agent settings
    // ------------------------------------------------------------------------

    // GET /settings/general — read general settings.
    server.router().get("/settings/general", [](const Req&, Resp& resp) {
        auto& store = daedalus::SettingsStore::instance();
        auto ses = store.get_server_error_settings();
        resp.setStatus(HTTP::StatusCode::OK);
        resp.setContentType("application/json");
        resp.setBody(json_ok({
            {"default_model", store.get_default_model_name()},
            {"auto_approve", store.get_auto_approve()},
            {"max_agent_iterations", store.get_max_agent_iterations()},
            {"max_tool_result_size", store.get_max_tool_result_size()},
            {"max_request_size", store.get_max_request_size()},
            {"max_response_size", store.get_max_response_size()},
            {"mcp_tool_schema_soft_cap_percent", store.get_mcp_tool_schema_soft_cap_percent()},
            {"db_path", store.db_path().string()},
            {"server_error", {
                {"enabled", ses.enabled},
                {"initial_wait_seconds", ses.initial_wait_seconds},
                {"max_wait_seconds", ses.max_wait_seconds},
                {"backoff_multiplier", ses.backoff_multiplier},
                {"max_retries_per_call", ses.max_retries_per_call},
            }},
        }));
    });

    // POST /settings/general — update general settings.
    server.router().post("/settings/general", [](const Req& req, Resp& resp) {
        try {
            auto j = json::parse(req.bodyAsString());
            auto& store = daedalus::SettingsStore::instance();
            if (j.contains("default_model")) {
                store.set_default_model_name(j["default_model"].get<std::string>());
            }
            if (j.contains("auto_approve")) {
                store.set_auto_approve(j["auto_approve"].get<bool>());
            }
            if (j.contains("max_agent_iterations")) {
                store.set_max_agent_iterations(j["max_agent_iterations"].get<int>());
            }
            if (j.contains("max_tool_result_size")) {
                store.set_max_tool_result_size(j["max_tool_result_size"].get<size_t>());
            }
            if (j.contains("max_request_size")) {
                store.set_max_request_size(j["max_request_size"].get<size_t>());
            }
            if (j.contains("max_response_size")) {
                store.set_max_response_size(j["max_response_size"].get<size_t>());
            }
            if (j.contains("mcp_tool_schema_soft_cap_percent")) {
                store.set_mcp_tool_schema_soft_cap_percent(j["mcp_tool_schema_soft_cap_percent"].get<int>());
            }
            if (j.contains("server_error") && j["server_error"].is_object()) {
                const auto& se = j["server_error"];
                daedalus::ServerErrorSettings ses;
                ses.enabled             = se.value("enabled", true);
                ses.initial_wait_seconds= se.value("initial_wait_seconds", 30);
                ses.max_wait_seconds    = se.value("max_wait_seconds", 300);
                ses.backoff_multiplier  = se.value("backoff_multiplier", 2.0);
                ses.max_retries_per_call= se.value("max_retries_per_call", 10);
                // SettingsStore doesn't expose a setter for the whole struct,
                // so we persist it as a JSON blob in kv_settings.
                store.set_string("server_error", se.dump());
            }
            resp.setStatus(HTTP::StatusCode::OK);
            resp.setContentType("application/json");
            resp.setBody(json_ok());
        } catch (const std::exception& e) {
            resp.setStatus(HTTP::StatusCode::BAD_REQUEST);
            resp.setContentType("application/json");
            resp.setBody(json_error(e.what()));
        }
    });

    // ------------------------------------------------------------------------
    // MCP servers
    // ------------------------------------------------------------------------

    // GET /settings/mcp — list MCP servers.
    server.router().get("/settings/mcp", [](const Req&, Resp& resp) {
        auto& store = daedalus::SettingsStore::instance();
        json arr = json::array();
        for (const auto& s : store.list_mcp_servers()) arr.push_back(mcp_to_json(s));
        resp.setStatus(HTTP::StatusCode::OK);
        resp.setContentType("application/json");
        resp.setBody(json_ok({{"servers", arr}}));
    });

    // POST /settings/mcp — upsert an MCP server.
    // §webui-settings: if bearer_token is empty, preserve the existing token
    // so the masked GET response can be edited without re-entering secrets.
    server.router().post("/settings/mcp", [](const Req& req, Resp& resp) {
        try {
            auto j = json::parse(req.bodyAsString());
            auto s = json_to_mcp(j);
            if (!s) {
                resp.setStatus(HTTP::StatusCode::BAD_REQUEST);
                resp.setContentType("application/json");
                resp.setBody(json_error("server requires a 'name'"));
                return;
            }
            auto& store = daedalus::SettingsStore::instance();
            if (s->bearer_token.empty()) {
                if (auto existing = store.get_mcp_server(s->name)) {
                    s->bearer_token = existing->bearer_token;
                }
            }
            store.upsert_mcp_server(*s);
            resp.setStatus(HTTP::StatusCode::OK);
            resp.setContentType("application/json");
            resp.setBody(json_ok());
        } catch (const std::exception& e) {
            resp.setStatus(HTTP::StatusCode::BAD_REQUEST);
            resp.setContentType("application/json");
            resp.setBody(json_error(e.what()));
        }
    });

    // DELETE /settings/mcp/<name> — delete an MCP server.
    server.router().delete_(R"(/settings/mcp/(.+))",
        [](const Req& req, Resp& resp) {
            std::string name = extract_last_segment(req.path());
            if (name.empty()) {
                resp.setStatus(HTTP::StatusCode::BAD_REQUEST);
                resp.setContentType("application/json");
                resp.setBody(json_error("missing server name"));
                return;
            }
            daedalus::SettingsStore::instance().delete_mcp_server(name);
            resp.setStatus(HTTP::StatusCode::OK);
            resp.setContentType("application/json");
            resp.setBody(json_ok());
        });
}

} // namespace websse
