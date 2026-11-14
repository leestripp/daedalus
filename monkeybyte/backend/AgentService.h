// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "SessionCatalog.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

#include <daedalus/Agent.h>
#include <nlohmann/json.hpp>

namespace monkeybyte {

using EventCallback = std::function<void(std::string, nlohmann::json)>;
using UiHandler = std::function<nlohmann::json(std::string, nlohmann::json)>;

class AgentService {
public:
    AgentService(std::filesystem::path index_path, EventCallback events, UiHandler ui);
    ~AgentService();

    AgentService(const AgentService&) = delete;
    AgentService& operator=(const AgentService&) = delete;

    nlohmann::json ensure_session(const std::filesystem::path& workspace,
                                  std::string_view requested_id = {});
    nlohmann::json create_session(const std::filesystem::path& workspace, std::string name);
    nlohmann::json select_session(std::string_view id);
    nlohmann::json rename_session(std::string_view id, std::string name);
    nlohmann::json delete_session(std::string_view id);
    nlohmann::json sessions() const { return catalog_.list(); }
    nlohmann::json send(std::string text, const nlohmann::json& attachments,
                        std::string source = "user:lee");
    nlohmann::json steer(std::string text, const nlohmann::json& attachments);
    nlohmann::json cancel();
    nlohmann::json respond(std::string_view id, bool approved);
    nlohmann::json history() const;
    nlohmann::json clear_history();
    nlohmann::json status() const;
    nlohmann::json stats() const;
    nlohmann::json settings() const;
    nlohmann::json chimpflow_config() const;
    nlohmann::json tools() const;
    nlohmann::json execute_ui(std::string command, nlohmann::json args);
    nlohmann::json update_settings(const nlohmann::json& patch);
    nlohmann::json select_model(std::string model);
    nlohmann::json select_thinking(std::string level);

private:
    class Sink;
    struct Approval;

    std::filesystem::path daedalus_home_;
    SessionCatalog catalog_;
    EventCallback events_;
    UiHandler ui_;
    std::shared_ptr<Sink> sink_;
    mutable std::recursive_mutex mutex_;
    std::unique_ptr<daedalus::Agent> agent_;
    std::optional<SessionRecord> record_;
    nlohmann::json history_cache_ = nlohmann::json::array();

    std::mutex driver_mutex_;
    std::condition_variable driver_cv_;
    std::jthread driver_;
    bool driver_stop_ = false;
    bool turn_requested_ = false;
    std::atomic_bool running_ = false;

    mutable std::mutex approval_mutex_;
    std::map<std::string, std::shared_ptr<Approval>> approvals_;
    std::atomic_uint64_t next_approval_ = 1;
    std::atomic_int state_ = static_cast<int>(daedalus::AgentState::Idle);

    void activate(const SessionRecord& record);
    nlohmann::json send_impl(std::string text, const nlohmann::json& attachments,
                             std::string source, bool steering);
    void request_turn();
    void run_driver(std::stop_token stop_token);
    void emit(std::string type, nlohmann::json payload) const;
    void emit_stats() const;
    void refresh_history();
    void require_idle() const;
    void reject_approvals();
    bool await_approval(std::string_view command);
};

}  // namespace monkeybyte
