/**
 * @file AgentImpl.h
 * @brief PIMPL for daedalus::Agent.
 *
 * §Phase 1+: backs the public Agent facade with the real subsystems. Hosts
 * see only `daedalus::Agent` (in include/daedalus/Agent.h); the heavy
 * implementation (LLMClient, McpServerManager) lives
 * here in src/library/.
 *
 * Responsibilities:
 *   - Own an LLMClient instance (the real state machine + loop).
 *   - Translate AgentConfig <-> AppConfig (private).
 *   - Translate LLMClient::AgentState <-> public daedalus::AgentState.
 *   - Route AgentSink events into LLMClient's sink expectations.
 *   - Track AgentStats (incremental updates from LLMClient getters).
 *   - Forward cancel() into LLMClient's stop_source.
 */
#pragma once

#include <atomic>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "daedalus/AgentSink.h"
#include "daedalus/types/AgentConfig.h"
#include "daedalus/types/AgentState.h"
#include "daedalus/types/AgentStats.h"
#include "daedalus/types/HostTool.h"
#include "daedalus/types/ImageAttachment.h"
#include "daedalus/types/ModelInfo.h"
#include "daedalus/types/SkillInfo.h"
#include "daedalus/types/ToolInfo.h"
#include "daedalus/settings.h"

#include "api/LLMClient.h"
#include "mcp/McpServerManager.h"  // §Phase 11

namespace daedalus::detail {

class AgentImpl {
public:
    explicit AgentImpl(AgentConfig cfg, std::shared_ptr<AgentSink> sink);

    // Lifecycle
    void initialize();
    void shutdown();
    // §CRASH-4 (2026-07-16): forward to LLMClient::install_crash_handlers.
    // Idempotent. Call after initialize() and before run_one_turn().
    void install_crash_handlers();
    void uninstall_crash_handlers();
    [[nodiscard]] bool is_initialized() const noexcept { return initialized_; }

    // Conversation control
    void run_one_turn(std::stop_token st = {});
    void submit_user_message(std::string text);
    void submit_user_message(std::string text,
                             std::vector<ImageAttachment> images);
    void cancel() noexcept;

    // Read-only views
    [[nodiscard]] AgentState state() const noexcept { return state_.load(std::memory_order_acquire); }
    [[nodiscard]] AgentStats stats() const noexcept;
    [[nodiscard]] const AgentConfig& config() const noexcept { return cfg_; }

    // §Phase 8.8: read-only mirror of cfg_.agent_name for the
    // public Agent's agent_name() getter. Validated in initialize().
    [[nodiscard]] std::string_view agent_name() const noexcept { return cfg_.agent_name; }

    [[nodiscard]] std::vector<ToolInfo>  list_tools()  const;
    [[nodiscard]] std::vector<SkillInfo> list_skills() const;
    [[nodiscard]] std::vector<ModelInfo> list_models() const;
    [[nodiscard]] ModelInfo              active_model() const noexcept;
    [[nodiscard]] bool                   auto_approve() const noexcept;
    static std::string     library_version() noexcept;
    nlohmann::json execute_tool(const std::string& name,
                                const nlohmann::json& args);

    // Extension
    void register_tool(std::unique_ptr<HostTool> tool);
    void clear_memory();
    bool switch_model(const std::string& model_name);
    [[nodiscard]] std::string last_assistant_text() const;

private:
    void set_state(AgentState s) noexcept;

    AgentConfig cfg_;
    std::shared_ptr<AgentSink> sink_;
    std::atomic<AgentState> state_{AgentState::Idle};
    AgentStats stats_;
    std::atomic<bool> cancel_requested_{false};

    // Pending user messages — agents consume them one per turn.
    std::vector<std::string> pending_user_messages_;

    // Forward-declared body. We define it inline here so that std::unique_ptr<Body>
    // can be destroyed at the AgentImpl instantiation point (PIMPL needs the
    // complete type visible to the deleter).
    struct Body {
        // §B2 fix (v0.5.1): stored by value. SettingsStore is now copyable
        // (Impl fields inlined), so the optional holds the owned instance.
        // LLMClient receives std::addressof(*store) — the pointer is stable
        // for the lifetime of Body (LLMClient is destroyed before store in
        // shutdown()).
        std::optional<daedalus::SettingsStore> store;
        std::unique_ptr<mcp::McpServerManager> mcp;   // §Phase 11: owned; nullptr when mcp_enabled=false
        std::unique_ptr<LLMClient>             client;
        std::stop_source                       cancel_source;
    };
    std::unique_ptr<Body> body_;

    // Host-registered tools (Phase 6: registered with ToolFactory).
    // Stored as shared_ptr so the HostToolAdapter can hold the same pointer.
    std::vector<std::shared_ptr<HostTool>> host_tools_;
    std::shared_ptr<HostToolContext>       host_ctx_;

    bool initialized_ = false;
};

}  // namespace daedalus::detail
