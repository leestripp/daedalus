#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <memory>
#include <atomic>
#include <cstdint>
#include <stop_token>

#include "api/HttpSession.h"
#include "core/FileIO.h"
#include "core/SessionMemory.h"
#include "core/Messages.h"
#include "mcp/McpClient.h"
// §Phase 11: McpServerManager* is now passed in via the ctor (injected),
// not accessed via the singleton. Forward-declare instead of including.
namespace mcp { class McpServerManager; }
class SkillManager;                        // §Phase 16: per-agent skill manager
#include "tools/ToolHandler.h"
#include "tools/ToolFilter.h"

#include "daedalus/AgentSink.h"
#include "daedalus/settings.h"   // §v0.5.0: SettingsStore* replaces SettingsManager*

using json = nlohmann::json;

struct AppConfig {
    std::string base_url;
    std::string api_key;
    std::string model;            // model_id (sent to API), e.g. "MiniMax-M2.7"
    std::string model_name;       // settings.json key, e.g. "minimax-m2.7"
    std::string chat_endpoint;
    std::filesystem::path target_directory;
    std::string agent_name;        // §Phase 8.8 — per-agent runtime-state namespace
    bool auto_mode = false;  // Automatically approve tool executions
    bool memory_disabled = false;  // Disable session memory
    // §Phase 9.8: SSE streaming. Default = true (progressive tokens).
    // Set to false to get a single buffered JSON response instead.
    bool stream = true;

    // §Phase 8.7: resolved sampling temperature.
    // Priority (highest first):
    //   1. cfg.temperature >= 0  (explicit session override)
    //   2. model's temperature from settings.json
    //   3. 0.0 (deterministic default)
    double temperature = 0.0;

    // §Phase 6: host-supplied role prompt (from AgentConfig::system_prompt).
    // The library prepends it to the composed system prompt so the host
    // can fully control the agent's persona. Empty = library default role.
    std::string system_prompt;

    // §Phase 14: optional 1-line note about the system_info tool. The host
    // can use this to remind the agent when to call system_info (or not).
    // Empty = no note appended.
    std::string system_info_note;

    // Phase 2.7: removed sub_agent_mode, sub_agent_looping, manager_mode.
    // The Agent has no notion of modes — those are host concerns. Demos
    // for manager / sub-agent patterns live in demos/daedalus-manager/
    // and demos/daedalus-subagent/.
};

// Agent state machine states
enum class AgentState : std::uint8_t {
    UNDERSTAND,  // Analyse the project and focus on files relevant to the user's prompt
    PLANNING,    // Research with grep_search, analyze task, and formulate strategy
    ACT,         // Execution phase - implements changes
    BUILD_TEST,  // Verification phase - runs compiler/tests
    DONE,        // Task completed successfully
    CANCELLED    // Interrupted by user (ESC key)
};

// §Phase 5.1: ESCKeyDetector removed. Library has no terminal
// coupling — no termios, no stdin polling. Cancellation flows
// through `Agent::cancel()` → `LLMClient::cancel()` → a
// std::stop_source that's checked in the API wait loop. Hosts
// that want ESC-to-cancel capture it themselves (e.g. by reading
// from stdin in a separate thread, then calling agent.cancel()).

class LLMClient {
private:
    AppConfig config_;
    std::shared_ptr<daedalus::AgentSink> sink_;  // §Phase 3: routes all I/O + streaming
    std::unique_ptr<FileIO> file_io_;
    daedalus::SettingsStore* settings_store_ = nullptr;  // §v0.5.0: wired directly to SettingsStore
    std::unique_ptr<SessionMemory> session_memory_;
    std::unique_ptr<Messages> messages_;
    int iteration_count_;
    int max_iterations_ = 100;
    size_t max_tool_result_size_ = 10000;  // 10KB - truncate middle for tool results
    size_t max_request_size_ = 4000000;   // 4MB default
    size_t max_response_size_ = 5000000;  // 5MB default
    int last_total_tokens_ = 0;
    int last_prompt_tokens_ = 0;
    int last_completion_tokens_ = 0;

    // §B32: suppresses duplicate "no usage" warnings per turn.
    bool warned_no_usage_this_turn_ = false;

    // §B23: tracks the previous public AgentState so set_status tool can emit
    // on_state_change(prev, current) instead of (-1, current).
    int previous_public_state_ = 0;  // daedalus::AgentState int value
    int current_public_state_ = 0;   // daedalus::AgentState int value

    // §Phase 5.1: cancellation flows through this stop_source.
    // Agent::cancel() → LLMClient::cancel() → cancel_source_.request_stop().
    // The API wait loop and the per-iteration check both consult
    // cancel_source_.get_token().stop_requested().
    std::stop_source cancel_source_;

    // MCP legacy path removed. The MCP system is now driven entirely by
    // McpServerManager + McpProxiedTool (Agent.md §18, §18.9).
    // See ToolHandler::get_tool_definitions() — which now includes each
    // server's tools as native first-class tools — for the LLM-visible
    // surface.

    // §Phase 11: injected McpServerManager* instead of ::instance() singleton.
    // nullptr means mcp_enabled=false — all MCP operations become no-ops.
    mcp::McpServerManager* mcp_manager_ = nullptr;

    // §Phase 16: per-agent SkillManager. nullptr means no skills configured.
    SkillManager* skill_manager_ = nullptr;

    // §Phase 16: per-agent tool filter. nullptr means all tools allowed.
    const ToolFilter* tool_filter_ = nullptr;

    // Tracks whether McpServerManager has signalled that the MCP tool
    // surface needs rebuilding. Set true when mcp_tools_dirty() returns true.
    bool mcp_tools_refresh_pending_ = true;

    // State machine methods
    // §NC-8 (2026-07-16): CURL* parameter dropped. The sync fallback
    // path now uses OmniNet::HTTP::Client internally.
    AgentState run_agent_cycle(const std::string& api_url, json& messages, const json& tool_defs, bool& compress_after_done);
    [[nodiscard]] std::string get_state_name(AgentState state) const;
    [[nodiscard]] std::string extract_error_message(const std::string& response) const;

    // Buffer management
    // Truncate messages if they exceed max_request_size (keeps system + recent messages)
    json truncate_messages_if_needed(json& messages, size_t max_size);

    // Helper to build multi-modal user messages
    // Detects image references in the format ![alt](path_or_url) and converts to array content
    json build_multimodal_message(const std::string& user_input);
    
    // Extract and store token usage from a parsed LLM response.
    // Called after every successful API response so that
    // last_total_tokens_ / last_prompt_tokens_ / last_completion_tokens_
    // stay up-to-date for the stats display and compression threshold.
    void update_token_usage(const json& response_json);

    // §Phase 9.10: called by the compression pipeline (Messages::compress /
    // call_llm_for_summary) after its non-streaming LLM call succeeds.
    // The compression call gets real usage counts (MiniMax SSE is always null);
    // we mirror them here so the main agent's token bar stays accurate between
    // streaming turns. Thread-safe for the compression thread calling this
    // while the main thread reads last_total_tokens_ — the staleness of
    // ~1 second is acceptable for a progress indicator.
    void report_compression_token_usage(int prompt_tokens, int completion_tokens, int total_tokens) {
        last_prompt_tokens_     = prompt_tokens;
        last_completion_tokens_ = completion_tokens;
        last_total_tokens_     = total_tokens;
    }

    // Helper to get max context size (from messages if available, else default)
    size_t get_max_context_size_value() const {
        if (messages_) {
            return messages_->get_config().max_context_size;
        }
        return 200000;
    }

public:
    // §Phase 11: mcp_manager added (injected instead of ::instance()).
    // §v0.5.0: settings_manager replaced with settings_store (SettingsStore*).
    explicit LLMClient(const AppConfig& config,
                       daedalus::SettingsStore* settings_store = nullptr,
                       mcp::McpServerManager* mcp_manager = nullptr,
                       SkillManager* skill_manager = nullptr,
                       const ToolFilter* tool_filter = nullptr);

    /// @brief Attach a sink. Call this after construction and before
    ///        initialize(). Required for streaming output, log routing, and
    ///        blocking I/O (bash approval).
    ///        Also propagates the sink to ToolHandler so execute_bash can
    ///        call request_command_approval when not auto-approved.
    void set_sink(std::shared_ptr<daedalus::AgentSink> sink) {
        sink_ = sink;
        ToolHandler::set_sink(std::move(sink));
    }

    /// @brief Update the running AppConfig (e.g., after a model switch).
    ///        Also refreshes the Messages compression config so context-size
    ///        and API settings follow the new model.
    void set_config(const AppConfig& config);

    /// @brief Clear the conversation history and persist the empty state.
    ///        Called when switching models so the new model starts with a
    ///        fresh context.
    void clear_messages();

    // Refresh the MCP tool surface from McpServerManager. Call this at
    // startup and any time the manager's dirty flag flips (the LLMClient
    // tracks the flag in mcp_tools_refresh_pending_ and calls this
    // automatically at the start of each agent cycle).
    void refresh_mcp_tools();

    // API interaction functions
    // §NC-8 (2026-07-16): CURL* parameter dropped. Implementation
    // uses OmniNet::HTTP::Client::send_blocking.
    std::string make_http_request_with_retry(const std::string& url, const std::string& payload, int max_retries = 3);

    // Async API request using HttpSession with optional cancellation.
    // Returns a future to the response body string. Throws std::runtime_error
    // on permanent failure (e.g., all retries exhausted).
    //
    // When streaming_callback is provided, the Accept header is set to
    // SSE and chunks are delivered to the callback as they arrive. The
    // caller is responsible for SSE-parsing and calling
    // sink_->on_assistant_delta() per text delta.
    std::future<std::string> make_api_request_async(
        const std::string& url,
        const std::string& payload,
        std::stop_token stop_token = {},
        std::function<void(std::string_view)> streaming_callback = nullptr
    );
    AgentState run_one_turn(const std::string& user_message);  // Phase 2.5: extracted agent cycle (returns when agent emits stop/cancel)
    // Note: the old run_interaction() / run_manager_mode() have been
    // removed. The Agent has exactly ONE loop. Manager / sub-agent
    // patterns are host concerns and will live in demos/daedalus-manager/
    // and demos/daedalus-subagent/.

    // Phase 2.7: lifecycle hooks. Renamed to match prompt.txt
    // ("easy cleanup() function"). The dtor also calls cleanup()
    // automatically; explicit calls are idempotent.
    //
    // Typical host usage:
    //   LLMClient client(cfg, settings);
    //   client.initialize();                        // one-time setup
    //   while (true) {
    //       std::string input = host.read_input();
    //       if (input == "/quit") break;
    //       AgentState final = client.run_one_turn(input);
    //       // host may inspect final, dispatch, etc.
    //   }
    //   client.cleanup();                           // explicit cleanup
    //   // ~LLMClient() also calls cleanup() if not already done
    void initialize();  // one-time setup: banner, persisted messages
    void cleanup();     // graceful teardown: save messages + session memory

    // §CRASH-4 (2026-07-16): forward crash-handler install/uninstall
    // to the underlying Messages instance. Idempotent.
    void install_crash_handlers();
    void uninstall_crash_handlers();

    // §Phase 5.1: cancel an in-flight turn. Fires cancel_source_, which
    // the API wait loop and the per-iteration check both consult. Safe
    // to call from any thread. Idempotent. Hosts that want ESC-to-cancel
    // capture ESC themselves and call this method (see
    // docs/embedding.md §"Wiring your own cancellation").
    void cancel() noexcept;
    
    // Stats getters for user display
    int get_total_tokens() const { return last_total_tokens_; }
    int get_prompt_tokens() const { return last_prompt_tokens_; }
    int get_completion_tokens() const { return last_completion_tokens_; }
    int get_max_context_size() const {
        if (messages_) {
            return static_cast<int>(messages_->get_config().max_context_size);
        }
        return 200000; // Default fallback
    }
    int get_iteration_count() const { return iteration_count_; }
    int get_max_iterations() const { return max_iterations_; }
    [[nodiscard]] int get_message_count() const noexcept {
        return messages_ ? static_cast<int>(messages_->size()) : 0;
    }

    /// §Phase 6: return the body of the most recent assistant message,
    /// or empty string if no assistant turn has completed yet. Hosts use
    /// this to capture the agent's final answer (e.g. composition
    /// patterns like spawn-a-sub-agent).
    std::string get_last_assistant_text() const {
        if (messages_) return messages_->get_last_assistant_content();
        return {};
    }
    
    // Inject context statistics into system message before API calls
    // This allows the agent to see token usage, memory stats, etc.
    void inject_context_stats(json& messages);
    
    // Session Memory Event Hooks
    void on_build_complete(const std::string& command, bool success) {
        if (session_memory_) session_memory_->on_build_complete(command, success);
    }
    void on_build_error(const std::string& error, const std::string& attempted_fix = "") {
        if (session_memory_) session_memory_->on_error(error, attempted_fix);
    }
    void on_error_resolved(const std::string& error, const std::string& solution) {
        if (session_memory_) session_memory_->on_error_resolved(error, solution);
    }
    void on_user_correction(const std::string& context, const std::string& correction) {
        if (session_memory_) session_memory_->on_user_correction(context, correction);
    }
    std::string get_session_memory_prompt() const {
        if (session_memory_) return session_memory_->get_system_prompt_addition();
        return "";
    }
    void end_session(const std::string& summary, const std::vector<std::string>& files = {}, int iterations = 0) {
        if (session_memory_) session_memory_->on_session_end(summary, files, iterations);
    }
};