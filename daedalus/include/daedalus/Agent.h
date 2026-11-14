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

#include <cstddef>
#include <memory>
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
#include "daedalus/types/ProviderConfig.h"
#include "daedalus/types/AgentState.h"
#include "daedalus/types/AgentStats.h"
#include "daedalus/types/BaseTool.h"
#include "daedalus/types/HostTool.h"   // deprecated alias; kept for back-compat
#include "daedalus/types/ImageAttachment.h"
#include "daedalus/types/ModelInfo.h"
#include "daedalus/types/PendingUserMessage.h"
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
    ///
    /// @note §QUEUE-JOIN (2026-10-04): run_one_turn() consumes the ENTIRE
    ///        queue, not one message — everything queued while a turn was
    ///        busy (follow-ups, corrections, ...) is joined into a single
    ///        message (texts blank-line-joined FIFO, images concatenated,
    ///        sources merged) and dispatched as ONE turn. Submitting while
    ///        idle without a matching run_one_turn() still strands the
    ///        message for the next turn; use pending_count() to check for
    ///        unconsumed messages.
    void submit_user_message(std::string text);

    /// @brief Push a user message with attached images.
    ///
    /// Images are loaded when the message drains (Path and Url refs are
    /// fetched/encoded then; Bytes are used as-is), attached to the next
    /// chat request as image_url content parts, and never persisted in
    /// the conversation history — see docs/tools.md "Image input".
    /// Requires a vision-capable model; text-only models drop the images
    /// with a warning instead of failing the request.
    void submit_user_message(std::string text,
                             std::vector<ImageAttachment> imgs);

    /// @brief Push a user message with an explicit origin (§MSG-HEADERS,
    ///        2026-10-04). `source` is a free-form `kind:id` string —
    ///        "user:<name>" for the interactive host, "a2a:<agent>",
    ///        "trigger:<name>", "email:<addr>", ... — stamped into the
    ///        message's `_meta` together with a library-generated
    ///        timestamp when the turn consumes it. The model then sees
    ///        who/what sent each message and when, so it can tell an
    ///        old, already-addressed message from a fresh one. Future
    ///        source kinds need no API change: new `kind:id` strings
    ///        just work. Prefer this overload for any non-interactive
    ///        ingress (webhooks, schedulers, device bridges, mail).
    void submit_user_message(std::string text,
                             std::vector<ImageAttachment> imgs,
                             std::string source);

    /// @brief Steer the active turn at a safe tool-result boundary.
    ///
    /// Unlike submit_user_message(), this does not blindly wait for a later
    /// turn. While a turn is active, the message is drained only after ALL
    /// results for the preceding assistant tool_calls have been appended,
    /// immediately before the next model request. It is never inserted into
    /// a streaming assistant response or between a tool call and its result.
    /// Multiple steering messages drain FIFO as one joined user message.
    /// If submitted idle, or if the turn ends before the boundary, it becomes
    /// an ordinary pending message so it is neither lost nor misordered.
    void submit_steer_message(std::string text,
                              std::vector<ImageAttachment> imgs = {},
                              std::string source = "steer");

    /// @brief Number of submitted user messages not yet consumed by
    ///        run_one_turn(). Anything other than the expected 0/1 means
    ///        submits and turns have drifted out of sync.
    [[nodiscard]] size_t pending_count() const noexcept;

    // ------------------------------------------------------------------
    // Pending queue management
    //
    // Flexible host-side control over the queued user messages that
    // submit_user_message() added and run_one_turn() has not consumed
    // yet — e.g. a chat UI that wants to show, rewrite or drop the
    // user's queued follow-ups. Index-based: index 0 is the message
    // the next run_one_turn() will consume. §QUEUE-MUTEX (2026-09-10):
    // the queue is internally synchronized — submit_user_message(),
    // pending_count(), the management API below and cancel() are all
    // callable from ANY thread (e.g. a UI thread). run_one_turn()
    // remains driving-thread-only.
    // ------------------------------------------------------------------

    /// @brief Snapshot of the pending queue, oldest (next to drain) first.
    [[nodiscard]] std::vector<PendingUserMessage> pending_messages() const;

    /// @brief Replace the text — and optionally the attachments — of the
    ///        queued message at `index`. `new_images` == std::nullopt
    ///        keeps the existing attachments; a value (even an empty
    ///        vector) replaces them. Returns false when `index` is out
    ///        of range; the queue is left untouched.
    bool edit_pending_message(
        size_t index,
        std::string new_text,
        std::optional<std::vector<ImageAttachment>> new_images = std::nullopt);

    /// @brief Drop the queued message at `index`. Returns false when
    ///        `index` is out of range; the queue is left untouched.
    bool remove_pending_message(size_t index);

    /// @brief Drop every queued user message. No-op when the queue is
    ///        already empty.
    void clear_pending_messages() noexcept;

    /// @brief Programmatic cancel. Equivalent to stop_token firing.
    ///        Thread-safe: callable from any thread (e.g. a UI Stop
    ///        button) — the pending-queue check it performs is guarded
    ///        internally (§QUEUE-MUTEX, MonkeyByte BUG-3).
    void cancel() noexcept;

    // ------------------------------------------------------------------
    // Turn outcome (§TURN-ERROR, 2026-09-10 — MonkeyByte BUG-4)
    // ------------------------------------------------------------------

    /// @brief True when the current/most recent turn hit a terminal
    ///        (non-retried) error — fatal HTTP 4xx, retries exhausted,
    ///        no model configured. Terminal LLM failures still end the
    ///        turn with public AgentState::Done (the internal state
    ///        machine has no error state for those paths), so hosts
    ///        that need to know the turn failed query this after
    ///        run_one_turn() returns instead of tracking
    ///        AgentSink::on_error(will_retry=false) themselves.
    ///        Cleared when the next turn starts. Driving thread.
    [[nodiscard]] bool turn_had_terminal_error() const noexcept;

    /// @brief The terminal error of the current/most recent turn, if
    ///        any (kind + full message). std::nullopt when the turn
    ///        succeeded. Driving thread.
    [[nodiscard]] std::optional<AgentError> last_error() const;

    /// @brief Read-only snapshot of the conversation history the agent
    ///        actually sees (post-restore, post-compression,
    ///        post-sanitization) — §HISTORY-SNAPSHOT (2026-09-10,
    ///        MonkeyByte REQ-2). Projection: system/user/assistant
    ///        entries with their text content; tool calls and tool
    ///        results are elided. Empty when no model is configured.
    ///        Driving thread; do not call mid-turn.
    [[nodiscard]] std::vector<HistoryEntry> history_snapshot() const;

    /// Clear this session's conversation and synchronously erase its persisted
    /// messages. Driving thread only; reject the operation while a turn runs.
    void clear_history();

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

    /// @brief Thinking levels the current model actually honors
    ///        (subset of None/Low/High/Max; see ThinkingLevel.h).
    ///        Hosts use this to build per-model pickers for
    ///        AgentConfig::thinking_level. Empty when the model has no
    ///        thinking control or no model is configured yet.
    ///        Default is never included: it is always a valid selection
    ///        meaning "provider default" (nothing is sent), so hosts
    ///        rendering pickers should show it alongside these entries —
    ///        e.g. on/off models like MiniMax-M3 return {None} and rely
    ///        on Default to represent "thinking on".
    [[nodiscard]] std::vector<ThinkingLevel> supported_thinking_levels() const;

    /// @brief Change the thinking level at runtime. Takes effect on the
    ///        very next chat request — the provider plugin maps the level
    ///        into each outgoing payload, so no agent rebuild is needed.
    ///        Also updates the stored config so later model switches keep
    ///        the selection. Levels outside supported_thinking_levels()
    ///        are silently ignored by the plugin (nothing invalid is sent).
    ///        Does NOT persist anything.
    void set_thinking_level(ThinkingLevel level);

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

    /// @brief Effective auto-approve flag. When `cfg.auto_approve` is set
    ///        (std::optional has a value) it is authoritative; when unset,
    ///        falls back to the SettingsStore `auto_approve` row (legacy).
    ///        The library's tool dispatcher uses this combined value, so
    ///        hosts that display it should too. (Previously the daedalus-cli
    ///        banner showed only the cfg value, missing settings.)
    [[nodiscard]] bool auto_approve() const noexcept;

    /// @brief Compress the conversation context (§AutoCompress). When no
    ///        turn is running, compression happens immediately —
    ///        synchronously on the caller's thread, with
    ///        AgentSink::on_compaction() fired from that thread. When a
    ///        turn is running, compression is deferred to the end of the
    ///        current agent cycle (never mid-tool-call) and is honored
    ///        even if the turn is cancelled. If there is nothing to
    ///        compress yet, the request carries over to the next turn's
    ///        first cycle. Uses the same 70/30 split + LLM summary path
    ///        as the automatic 90%-threshold trigger (see
    ///        docs/how_compress_works.md).
    /// @return §COMPRESS-RESULT (2026-09-10, MonkeyByte REQ-3): what
    ///         actually happened — Compressed (context reduced),
    ///         Deferred (a turn is running; happens at the cycle
    ///         checkpoint), NothingToCompress (history too small; the
    ///         request is carried over to the next turn), Failed
    ///         (summary failed, history untouched; also carried over),
    ///         NoModel (no model configured — no-op).
    [[nodiscard]] CompressResult compress_context() noexcept;

    // ------------------------------------------------------------------
    // Approval control (§BUG-006/007/008)
    // ------------------------------------------------------------------

    /// @brief Flip this agent's auto-approve mode at runtime. Authoritative:
    ///        overrides both the config field and the SettingsStore row for
    ///        this agent. Takes effect on the next command approval check.
    ///        Does NOT persist anything — hosts that want persistence
    ///        read/write the SettingsStore themselves.
    void set_auto_approve(bool enabled);

    /// @brief Drop the in-memory session-approved command cache. Subsequent
    ///        commands will prompt again (unless auto-approve is on).
    void clear_session_approvals();

    /// @brief Delete this project's persisted approvals
    ///        (`<project>/.daedalus/daedalus[-<name>]/approvals.json`) and
    ///        clear the in-memory persisted-approval set. Idempotent.
    void clear_persisted_approvals();

    /// @brief Opt in/out of auto-recording host-approved commands into the
    ///        session-approved cache. Default true (legacy). Set false for
    ///        "ask every time" semantics. Note: like the underlying
    ///        ToolHandler state, this switch is process-wide.
    void set_remember_approvals(bool enabled);

    /// @brief Library version (git hash or fallback "v0.0.0").
    ///        Useful for hosts that want to display the lib they link.
    static std::string library_version() noexcept;

    /// @brief Discover models exposed by the provider for `model_name`.
    ///
    /// Looks up the model in the SettingsStore (using `daedalus_home` if
    /// non-empty, otherwise the default store) and queries its
    /// OpenAI-compatible `GET /v1/models` endpoint.
    static ModelDiscoveryResult discover_models(
        const std::string& model_name,
        const std::filesystem::path& daedalus_home = {});

    /// @brief Execute a registered tool directly (without LLM involvement).
    nlohmann::json execute_tool(const std::string& name,
                                const nlohmann::json& args);

    // ------------------------------------------------------------------
    // Todo list (per-project, per-agent)
    // ------------------------------------------------------------------

    /// @brief Add a todo item. Returns the same JSON the `todo_add` tool returns.
    /// status is one of "pending", "in_progress", "done", or "blocked".
    nlohmann::json add_todo(
        const std::string& text,
        const std::optional<std::string>& status = std::nullopt,
        const std::optional<std::string>& group = std::nullopt);

    /// @brief Edit a todo item by id. Optional text, status, and/or group update.
    nlohmann::json edit_todo(
        int id,
        const std::optional<std::string>& text = std::nullopt,
        const std::optional<std::string>& status = std::nullopt,
        const std::optional<std::string>& group = std::nullopt);

    /// @brief Delete a todo item by id.
    nlohmann::json delete_todo(int id);

    /// @brief List todos. filter is "all" (default), "pending", "in_progress",
    /// "done", or "blocked".
    nlohmann::json list_todos(
        const std::string& filter = "all",
        const std::optional<std::string>& group = std::nullopt);

    /// @brief Return total / pending / in_progress / done / blocked counts plus
    /// the current max and per-group counts.
    nlohmann::json count_todos();

    /// @brief Set the maximum number of todos (1-1000).
    nlohmann::json set_max_todos(int max);

    /// @brief Clear the entire todo list.
    nlohmann::json clear_todos();

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

    /// @brief List configured provider names.
    [[nodiscard]] std::vector<std::string> list_providers() const;

    /// @brief List full provider configurations.
    [[nodiscard]] std::vector<ProviderConfig> list_providers_detailed() const;

    /// @brief Add or update a provider configuration.
    bool upsert_provider(const ProviderConfig& provider);

    /// @brief Delete a provider and all models that reference it.
    bool delete_provider(const std::string& provider);

    /// @brief Name of the provider plugin active for this session
    ///        (§ProviderPlugins). Empty when no LLM client exists yet.
    [[nodiscard]] std::string active_provider_plugin() const;

    /// @brief Add or update a model configuration.
    bool upsert_model(const ModelConfig& model);

    /// @brief Delete a model configuration by name.
    bool delete_model(const std::string& name);

    /// @brief Switch the current provider. Persisted in SettingsStore.
    ///        If the current model does not belong to the new provider,
    ///        switches to the first configured model of that provider.
    bool switch_provider(const std::string& provider);

    /// @brief Discover models from the current provider's discovery endpoint.
    ///        Returns an error if no provider is set.
    [[nodiscard]] ModelDiscoveryResult discover_provider_models() const;

    // ------------------------------------------------------------------
    // Runtime enable/disable for skills, MCP servers, and tools
    // ------------------------------------------------------------------

    /// @brief Enable or disable a skill by name at runtime. Disabled skills
    ///        are hidden from the skill catalog and cannot be activated.
    ///        Returns true if the skill exists and the state changed.
    bool set_skill_enabled(const std::string& name, bool enabled);

    /// @brief Query whether a skill is currently enabled.
    [[nodiscard]] bool is_skill_enabled(const std::string& name) const;

    /// @brief Names of skills currently disabled by host policy.
    [[nodiscard]] std::vector<std::string> disabled_skills() const;

    /// @brief Enable or disable an MCP server by name at runtime. A disabled
    ///        server is disconnected and its proxied tools are removed from
    ///        the tool surface; enabling reconnects and refreshes tools.
    ///        Returns true if the server is known and the state changed.
    bool set_mcp_server_enabled(const std::string& name, bool enabled);

    /// @brief Query whether an MCP server is currently enabled.
    [[nodiscard]] bool is_mcp_server_enabled(const std::string& name) const;

    /// @brief Names of MCP servers currently disabled by host policy.
    [[nodiscard]] std::vector<std::string> disabled_mcp_servers() const;

    /// @brief Enable or disable a tool by name at runtime. Disabled tools are
    ///        omitted from the tool definitions and rejected if invoked.
    ///        Returns true if the tool exists and the state changed.
    bool set_tool_enabled(const std::string& name, bool enabled);

    /// @brief Query whether a tool is currently enabled.
    [[nodiscard]] bool is_tool_enabled(const std::string& name) const;

    /// @brief Names of tools currently disabled by host policy.
    [[nodiscard]] std::vector<std::string> disabled_tools() const;

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