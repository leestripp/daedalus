/**
 * @file daedalus/Agent.h
 * @brief Top-level facade for the libdaedalus static library.
 *
 * Hosts embed an Agent by:
 *   1) constructing one with an AgentConfig + a shared AgentSink,
 *   2) calling initialize() once,
 *   3) driving turn-by-turn with run_one_turn() + submit_user_message(),
 *   4) calling shutdown() before destruction.
 *
 * Agent is the only public type in libdaedalus that the host instantiates.
 * Everything else is hidden behind the detail:: namespace.
 *
 * Threading: an Agent instance is NOT thread-safe to share between
 * threads. Construct one Agent per session. Hosts that need to drive
 * multiple agents in parallel should construct multiple Agent objects.
 */
#pragma once

#include <memory>
#include <stop_token>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "daedalus/AgentSink.h"
#include "daedalus/types/AgentConfig.h"
#include "daedalus/types/AgentState.h"
#include "daedalus/types/AgentStats.h"
#include "daedalus/types/BaseTool.h"
#include "daedalus/types/HostTool.h"   // deprecated alias; kept for back-compat
#include "daedalus/types/ImageAttachment.h"
#include "daedalus/types/ModelInfo.h"
#include "daedalus/types/SkillInfo.h"
#include "daedalus/types/ToolInfo.h"

namespace daedalus {

namespace detail {
class AgentImpl;  // PIMPL
}

/// @brief Top-level facade. See file comment for usage.
class Agent {
public:
    /// @brief Construct an Agent. Does not touch the filesystem or network;
    ///        call initialize() to do that.
    Agent(AgentConfig cfg, std::shared_ptr<AgentSink> sink);

    ~Agent();

    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;
    Agent(Agent&&) noexcept;
    Agent& operator=(Agent&&) noexcept;

    // ------------------------------------------------------------------
    // Lifecycle
    // ------------------------------------------------------------------

    /// @brief Load settings/skills/MCP, initialize thread pool, etc.
    ///        Idempotent. Safe to call before run_*().
    void initialize();

    /// @brief Graceful shutdown. Idempotent. Automatically called by dtor.
    void shutdown();

    /// @brief §CRASH-4 (2026-07-16): install async-signal-safe crash
    ///        handlers for SIGSEGV/SIGABRT/SIGFPE/SIGILL that write
    ///        the current messages to messages.json.crash before the
    ///        signal is re-raised. Idempotent. Pairs with
    ///        uninstall_crash_handlers() in the graceful shutdown path.
    void install_crash_handlers();
    void uninstall_crash_handlers();

    // ------------------------------------------------------------------
    // Conversation control
    // ------------------------------------------------------------------

    /// @brief Drive a single turn: one user message -> possibly many
    ///        LLM/tool cycles -> stop/cancel/error. Returns when the
    ///        agent stops, errors, or the stop_token fires.
    void run_one_turn(std::stop_token st = {});

    /// @brief Push a user message into the conversation queue. The next
    ///        call to run_one_turn() will consume it.
    void submit_user_message(std::string text);

    /// @brief Push a user message with attached images.
    void submit_user_message(std::string text,
                             std::vector<ImageAttachment> imgs);

    /// @brief Programmatic cancel. Equivalent to stop_token firing.
    void cancel() noexcept;

    // ------------------------------------------------------------------
    // Read-only views
    // ------------------------------------------------------------------

    [[nodiscard]] AgentState  state()  const noexcept;
    [[nodiscard]] AgentStats  stats()  const noexcept;
    [[nodiscard]] const AgentConfig& config() const noexcept;

    /// @brief Read-only view of the per-agent namespace (§Phase 8.8).
    ///        Empty string means "no namespace; runtime state lives
    ///        under `<project_root>/.daedalus/`". Non-empty values
    ///        are validated in initialize() — if you see a non-empty
    ///        value here it has passed `is_valid_agent_name()`.
    [[nodiscard]] std::string_view agent_name() const noexcept;

    [[nodiscard]] std::vector<ToolInfo>  list_tools()  const;
    [[nodiscard]] std::vector<SkillInfo> list_skills() const;
    [[nodiscard]] std::vector<ModelInfo> list_models() const;

    /// @brief Names of MCP servers currently configured (loaded from the
    ///        SQLite SettingsStore during initialize(), with the legacy
    ///        `mcp/servers.json` merged in as a fallback). Empty when
    ///        `cfg.mcp_enabled == false` or no servers are configured.
    /// @note   Useful for hosts wanting to surface "what MCP servers are
    ///         available to the agent?" in their UI. Each name matches
    ///         a row in the store's `mcp_servers` table (or a key in
    ///         the legacy JSON).
    [[nodiscard]] std::vector<std::string> mcp_configured_servers() const;

    /// @brief Return the model that's actually loaded for this session.
    ///        This is the model that subsequent turns will hit.
    ///        - If `cfg.model_name` was non-empty, the matching entry.
    ///        - Else the model named by `default_model` in settings.json.
    ///        - Else the first model in `settings.json::models`.
    ///        Returns a default-constructed `ModelInfo` if no model
    ///        could be resolved (e.g. settings.json is empty).
    [[nodiscard]] ModelInfo active_model() const noexcept;

    /// @brief Effective auto-approve flag = `cfg.auto_approve`
    ///        OR settings.json::auto_approve. The library's tool
    ///        dispatcher uses this combined value, so hosts that
    ///        display it should too. (Previously the daedalus-cli
    ///        banner showed only the cfg value, missing settings.)
    [[nodiscard]] bool auto_approve() const noexcept;

    /// @brief Library version (git hash or fallback "v0.0.0").
    ///        Useful for hosts that want to display the lib they link.
    static std::string library_version() noexcept;

    /// @brief Execute a registered tool directly (without LLM involvement).
    nlohmann::json execute_tool(const std::string& name,
                                const nlohmann::json& args);

    // ------------------------------------------------------------------
    // Extension
    // ------------------------------------------------------------------

    /// @brief Register a host-implemented tool. Available on the next
    ///        turn. Tool is owned by the Agent until shutdown().
    /// @note `daedalus::HostTool` is a deprecated alias for
    ///       `daedalus::BaseTool` and is accepted here via the typedef.
    void register_tool(std::unique_ptr<BaseTool> tool);

    /// @brief Clear all session memory for the project root. Idempotent.
    void clear_memory();

    /// @brief Switch the model used for subsequent turns. Returns false
    ///        if the model isn't in the SettingsStore. On success the
    ///        conversation history is cleared so the new model starts
    ///        with a fresh context (the new model may have a smaller
    ///        context window than the previous one).
    bool switch_model(const std::string& model_name);

    /// @brief Return the body of the most recent assistant message,
    ///        or empty string if no assistant turn has completed yet.
    ///        Useful for hosts that need the agent's final text after
    ///        run_one_turn() returns — e.g. composition patterns like
    ///        "spawn a sub-agent and capture its answer".
    [[nodiscard]] std::string last_assistant_text() const;

private:
    std::unique_ptr<detail::AgentImpl> impl_;
};

}  // namespace daedalus