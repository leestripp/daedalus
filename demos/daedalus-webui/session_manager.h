// SPDX-License-Identifier: BSL-1.0
// demos/daedalus-webui/session_manager.h
//
// Session struct + SessionManager. Owns all live Agent+SSE sessions.
// Thread-safe. Extracted from main.cpp to keep main.cpp under 300 lines.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "OmniNet.h"
#include "http/HttpServer.h"
#include "sse/SSEHandler.h"

#include <daedalus.h>

#include "web_sse_sink.h"

namespace websse {
using namespace OmniNet;
namespace fs = std::filesystem;

// ============================================================================
// Session — one Agent + one SSE::Handler + one sink + a worker thread.
// ============================================================================

struct Session {
    std::string id;
    std::unique_ptr<daedalus::Agent> agent;
    std::shared_ptr<WebSseSink>     sink;       // shared_ptr so the Agent
                                                //   can keep it alive
    std::unique_ptr<SSE::Handler>   sse;
    std::thread                     worker;     // long-lived loop thread
    std::atomic<bool>               busy{false};
    std::atomic<bool>               stop{false};
};

// ============================================================================
// SessionManager — owns all live sessions. Thread-safe.
// ============================================================================

class SessionManager {
public:
    explicit SessionManager(HTTP::Server& server,
                            std::string model_name = "",
                            std::filesystem::path project_root = {})
        : server_(server)
        , default_model_name_(std::move(model_name))
        , project_root_(std::move(project_root)) {}

    std::string create() {
        auto sess = std::make_unique<Session>();
        sess->id = new_session_id();

        // Build AgentConfig. Leave daedalus_home empty so AgentImpl uses
        // SettingsStore::instance() which reads from the XDG-compliant
        // location (~/.local/share/daedalus/daedalus.db) where the user's
        // models are migrated to.
        daedalus::AgentConfig cfg;
        cfg.session_id   = "webui-" + sess->id;
        cfg.project_root = project_root_.empty() ? fs::current_path() : project_root_;
        // §Phase 8.8: each HTTP session gets its own per-agent
        // namespace so two browser tabs don't share
        // messages.json. agent_name == sess->id where id is a
        // 16-char hex string from new_session_id() below
        // (mt19937_64 + snprintf %016lx). All hex chars pass
        // is_valid_agent_name() cleanly. If a host uses a
        // different id format (e.g. UUID with hyphens at fixed
        // positions), those also pass — the validator allows
        // '-' in non-leading positions.
        cfg.agent_name   = sess->id;
        cfg.model_name  = default_model_name_;

        // §Phase 9.7: opt in to the user's global skills. Without
        // this the Agent's system prompt always reports
        // "Active Skills | 0" because SkillManager::cached_skills_
        // is never populated at startup. The webui is a host that
        // wants the user's skills loaded — other hosts (embedded
        // sandboxed agents, tests) leave this empty by design.
        cfg.skills_dirs = { "/home/ubuntu/.daedalus/skills/" };

        sess->sink  = std::make_shared<WebSseSink>();
        sess->agent = std::make_unique<daedalus::Agent>(std::move(cfg), sess->sink);

        // SSE handler for this session's event stream at a unique path.
        // attach() registers it via server.registerProtocolSwitch which
        // is mutex-protected, so this works at runtime.
        const std::string sse_path = "/sessions/" + sess->id + "/events";
        sess->sse = std::make_unique<SSE::Handler>(sse_path);
        sess->sink->bind_sse(sess->sse.get());
        sess->sse->attach(server_);

        sess->agent->initialize();

        // Long-lived worker thread: pulls from `queued_[id]` and runs
        // one turn per message. Stops when `stop` flips true.
        const std::string captured_id = sess->id;
        Session* sptr = sess.get();
        sess->worker = std::thread([this, sptr, captured_id]() {
            while (!sptr->stop.load()) {
                std::string msg;
                {
                    std::unique_lock<std::mutex> ql(queued_mu_);
                    queued_cv_.wait_for(ql, std::chrono::milliseconds(200),
                        [&] { return sptr->stop.load()
                                  || !queued_[captured_id].empty(); });
                    if (sptr->stop.load()) break;
                    auto it = queued_.find(captured_id);
                    if (it == queued_.end() || it->second.empty()) continue;
                    msg = std::move(it->second);
                    it->second.clear();
                }
                sptr->agent->submit_user_message(std::move(msg));
                sptr->agent->run_one_turn();
            }
        });

        const std::string id = sess->id;
        {
            std::lock_guard<std::mutex> lock(mu_);
            sessions_.emplace(id, std::move(sess));
        }
        return id;
    }

    void submit_message(const std::string& id, std::string text) {
        // Push to the per-session queue. The session's worker thread
        // wakes up, submits the message, and runs a turn.
        {
            std::lock_guard<std::mutex> ql(queued_mu_);
            queued_[id] = std::move(text);
        }
        queued_cv_.notify_one();
    }

    void cancel(const std::string& id) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = sessions_.find(id);
        if (it != sessions_.end()) it->second->agent->cancel();
    }

    std::string get_state(const std::string& id) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = sessions_.find(id);
        if (it == sessions_.end()) return R"({"error":"no such session"})";
        json j = { {"state", static_cast<int>(it->second->agent->state())} };
        return j.dump();
    }

    /// §Phase 9.6: return the active model for a session.
    /// Uses Agent::active_model() which resolves cfg.model_name,
    /// settings.json default_model, or the first entry in settings —
    /// whichever applies. Returns a JSON object with name, model_id,
    /// host, max_context_size, or an empty object if the session
    /// doesn't exist.
    std::string get_model(const std::string& id) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = sessions_.find(id);
        if (it == sessions_.end()) return "{}";
        auto m = it->second->agent->active_model();
        json j = {
            {"name",            m.name},
            {"id",              m.model_id},
            {"host",            m.host},
            {"max_context_size",m.max_context_size},
        };
        return j.dump();
    }

private:
    HTTP::Server& server_;
    std::string default_model_name_;
    std::filesystem::path project_root_;
    std::mutex mu_;
    std::unordered_map<std::string, std::unique_ptr<Session>> sessions_;
    std::mutex queued_mu_;
    std::condition_variable queued_cv_;
    std::unordered_map<std::string, std::string> queued_;

    static std::string new_session_id() {
        static thread_local std::mt19937_64 rng{
            std::random_device{}() ^
            static_cast<uint64_t>(
                std::chrono::high_resolution_clock::now()
                    .time_since_epoch().count())
        };
        uint64_t v = rng();
        char buf[17];
        std::snprintf(buf, sizeof(buf), "%016lx", v);
        return std::string(buf);
    }
};


} // namespace websse
