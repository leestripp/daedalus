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
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "daedalus/AgentSink.h"
#include "daedalus/model_discovery.h"
#include "daedalus/types/AgentConfig.h"
#include "daedalus/types/AgentError.h"
#include "daedalus/types/CompressResult.h"
#include "daedalus/types/HistoryEntry.h"
#include "daedalus/types/AgentState.h"
#include "daedalus/types/AgentStats.h"
#include "daedalus/types/BaseTool.h"
#include "daedalus/types/HostTool.h"   // deprecated alias
#include "daedalus/types/ImageAttachment.h"
#include "daedalus/types/ModelInfo.h"
#include "daedalus/types/PendingUserMessage.h"
#include "daedalus/types/SkillInfo.h"
#include "daedalus/types/ToolInfo.h"
#include "daedalus/settings.h"

#include "api/LLMClient.h"
#include "core/SkillManager.h"
#include "mcp/McpServerManager.h"  // §Phase 11
#include "tools/ToolFilter.h"

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
    [[nodiscard]] size_t pending_count() const noexcept {
        // §QUEUE-MUTEX (2026-09-10): internally synchronized — any thread.
        std::lock_guard<std::mutex> lk(pending_mutex_);
        return pending_user_messages_.size();
    }

    // Pending queue management (see daedalus/Agent.h for semantics).
    [[nodiscard]] std::vector<daedalus::PendingUserMessage> pending_messages() const {
        std::lock_guard<std::mutex> lk(pending_mutex_);
        return pending_user_messages_;
    }
    bool edit_pending_message(
        size_t index,
        std::string new_text,
        std::optional<std::vector<daedalus::ImageAttachment>> new_images);
    bool remove_pending_message(size_t index);
    void clear_pending_messages() noexcept {
        std::lock_guard<std::mutex> lk(pending_mutex_);
        pending_user_messages_.clear();
    }
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

    // Thinking levels the current model honors (§ThinkingLevel).
    // Empty when the model has no thinking control or no LLMClient
    // exists yet (no model configured).
    [[nodiscard]] std::vector<daedalus::ThinkingLevel>
        supported_thinking_levels() const;

    // Runtime thinking-level toggle (§ThinkingLevel). Applies from the
    // next chat request; no agent rebuild needed.
    void set_thinking_level(daedalus::ThinkingLevel level) noexcept;

    // Host-requested context compression (§AutoCompress). Compresses
    // immediately when idle; flags the LLMClient to compress at the end
    // of the current/next agent cycle when busy. §COMPRESS-RESULT:
    // returns what actually happened (NoModel when no LLMClient yet).
    daedalus::CompressResult compress_context() noexcept;

    // §TURN-ERROR (2026-09-10, MonkeyByte BUG-4): terminal-error query.
    // Driving thread (mirrored from LLMClient at turn end; AgentImpl's
    // own pre-dispatch terminal errors are recorded directly).
    void record_terminal_error(daedalus::ErrorKind kind, std::string message);
    [[nodiscard]] bool turn_had_terminal_error() const noexcept {
        return turn_terminal_error_.has_value();
    }
    [[nodiscard]] std::optional<daedalus::AgentError> last_error() const {
        return turn_terminal_error_;
    }

    // §HISTORY-SNAPSHOT (2026-09-10, MonkeyByte REQ-2). Driving thread.
    [[nodiscard]] std::vector<daedalus::HistoryEntry> history_snapshot() const;

    /// @brief Names of MCP servers currently configured (loaded from the
    ///        SQLite SettingsStore during initialize(), with the legacy
    ///        `mcp/servers.json` merged in as a fallback). Empty when
    ///        `cfg.mcp_enabled == false` or no servers are configured.
    [[nodiscard]] std::vector<std::string> mcp_configured_servers() const;
    [[nodiscard]] ModelInfo              active_model() const noexcept;
    [[nodiscard]] bool                   auto_approve() const noexcept;

    // §BUG-006/007/008: host-facing approval control. See daedalus/Agent.h.
    void set_auto_approve(bool enabled) noexcept;
    void clear_session_approvals() noexcept;
    void clear_persisted_approvals() noexcept;
    void set_remember_approvals(bool enabled) noexcept;
    static std::string     library_version() noexcept;
    static ModelDiscoveryResult discover_models(
        const std::string& model_name,
        const std::filesystem::path& daedalus_home = {});
    nlohmann::json execute_tool(const std::string& name,
                                const nlohmann::json& args);

    // Todo list
    nlohmann::json add_todo(
        const std::string& text,
        const std::optional<std::string>& status = std::nullopt,
        const std::optional<std::string>& group = std::nullopt);
    nlohmann::json edit_todo(
        int id,
        const std::optional<std::string>& text = std::nullopt,
        const std::optional<std::string>& status = std::nullopt,
        const std::optional<std::string>& group = std::nullopt);
    nlohmann::json delete_todo(int id);
    nlohmann::json list_todos(
        const std::string& filter = "all",
        const std::optional<std::string>& group = std::nullopt);
    nlohmann::json count_todos();
    nlohmann::json set_max_todos(int max);
    nlohmann::json clear_todos();

    // Extension
    void register_tool(std::unique_ptr<BaseTool> tool);
    void clear_memory();
    bool switch_model(const std::string& model_name);
    [[nodiscard]] std::vector<std::string> list_providers() const;
    [[nodiscard]] std::vector<ProviderConfig> list_providers_detailed() const;
    bool upsert_provider(const ProviderConfig& provider);
    bool delete_provider(const std::string& provider);
    /// §ProviderPlugins: resolved plugin name for the active session.
    std::string active_provider_plugin() const;
    bool upsert_model(const ModelConfig& model);
    bool delete_model(const std::string& name);
    bool switch_provider(const std::string& provider);
    [[nodiscard]] ModelDiscoveryResult discover_provider_models() const;
    [[nodiscard]] std::string last_assistant_text() const;

    // §Phase 16: runtime enable/disable
    bool set_skill_enabled(const std::string& name, bool enabled);
    [[nodiscard]] bool is_skill_enabled(const std::string& name) const;
    [[nodiscard]] std::vector<std::string> disabled_skills() const;

    /// @brief §SKILL-ORDER regression hook: true when the LLMClient
    ///        exists and has the per-agent SkillManager attached.
    ///        Exposed for tests; not part of the public Agent API.
    [[nodiscard]] bool client_has_skill_manager() const noexcept;

    bool set_mcp_server_enabled(const std::string& name, bool enabled);
    [[nodiscard]] bool is_mcp_server_enabled(const std::string& name) const;
    [[nodiscard]] std::vector<std::string> disabled_mcp_servers() const;

    bool set_tool_enabled(const std::string& name, bool enabled);
    [[nodiscard]] bool is_tool_enabled(const std::string& name) const;
    [[nodiscard]] std::vector<std::string> disabled_tools() const;

private:
    void set_state(AgentState s) noexcept;

    AgentConfig cfg_;
    std::shared_ptr<AgentSink> sink_;
    std::atomic<AgentState> state_{AgentState::Idle};
    AgentStats stats_;
    std::atomic<bool> cancel_requested_{false};

    // Pending user messages — agents consume them one per turn.
    // §READ-IMAGE-CORE follow-up (2026-08-10): queued user messages carry
    // optional image attachments from the public
    // submit_user_message(text, imgs) overload.
    // The element type is the public daedalus::PendingUserMessage so
    // pending_messages() can hand hosts a snapshot without a conversion.
    // §QUEUE-MUTEX (2026-09-10, MonkeyByte BUG-3/REQ-4): guards every
    // access to pending_user_messages_. Previously the queue was
    // driving-thread-only by contract, but Agent::cancel() read empty()
    // unguarded — a data race for hosts with a UI-thread Stop button.
    // The queue API is now callable from any thread; run_one_turn()
    // remains driving-thread-only.
    mutable std::mutex pending_mutex_;
    std::vector<daedalus::PendingUserMessage> pending_user_messages_;

    // §TURN-ERROR: terminal error of the current/most recent turn.
    // Driving-thread only (set at run_one_turn entry/end, read by the
    // public query API) — no mutex needed under that contract.
    std::optional<daedalus::AgentError> turn_terminal_error_;

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
        std::unique_ptr<SkillManager>          skill_manager;  // §Phase 16: per-agent
        std::unique_ptr<LLMClient>             client;
        std::stop_source                       cancel_source;
    };

    // §Phase 16: per-agent tool/MCP filter. Mutable so const list methods
    // can apply the current disabled sets.
    mutable ToolFilter tool_filter_;
    std::unique_ptr<Body> body_;

    // Host-registered tools (Phase 6: registered with ToolFactory).
    // Stored as shared_ptr so the HostToolAdapter can hold the same pointer.
    std::vector<std::shared_ptr<BaseTool>> host_tools_;
    std::shared_ptr<HostToolContext>       host_ctx_;

    bool initialized_ = false;
};

}  // namespace daedalus::detail
