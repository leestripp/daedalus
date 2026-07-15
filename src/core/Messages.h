/**
 * @file Messages.h
 * @brief Unified message handling and compression for daedalus
 * 
 * This class handles:
 * - Message storage (json array operations)
 * - Message persistence (save/load to JSON file)
 * - Message compression (compress middle via LLM)
 * - Tool call lifecycle management (state derived from message chain)
 * 
 * ARCHITECTURE: Single Source of Truth
 * Tool call state is DERIVED from the message chain, not stored separately:
 * - PENDING: assistant message has tool_calls[X].id="abc", no role="tool",tool_call_id="abc" exists
 * - EXECUTING: runtime marker only (ephemeral, not persisted) - stored in executing_tools_ set
 * - COMPLETED: role="tool",tool_call_id="abc" exists with non-error content
 * - FAILED: role="tool",tool_call_id="abc" exists with error content in "content"
 * - INVALID: assistant message has tool_calls with malformed JSON arguments
 * 
 * Refactored from MiniMaxClient and ContextCompressor to consolidate
 * all message-related functionality in one place.
 */

#pragma once

#include <string>
#include <filesystem>
#include <optional>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include <future>
#include <thread>
#include <chrono>

using json = nlohmann::json;

// ============================================================================
// Configuration & Result Structs
// ============================================================================

/**
 * @brief Message compression configuration
 */
struct MessageCompressionConfig {
    // Token management
    size_t max_context_size = 200000;        // API token limit
    float compress_threshold = 0.9f;          // Trigger at 90% of max
    
    // Compression output
    size_t max_compress_output = 1500;        // Max tokens in LLM summary

    // Split ratio: compress the FIRST (compress_ratio * 100)% of messages,
    // keep the LAST ((1 - compress_ratio) * 100)% intact.
    // Default: compress oldest 70%, keep newest 30%
    float compress_ratio = 0.70f;             // Compress first 70%, keep last 30%
    
    // What to keep intact (messages, not including system at index 0)
    // Note: keep_head_count and keep_tail_count are DEPRECATED - we now compress ALL non-system messages
    // The new compression rebuilds as: system + user(instruction) + summary
    // Keeping these as placeholders (not used) for backward compatibility with tests
    int keep_head_count = 2;                  // DEPRECATED - not used anymore
    int keep_tail_count = 2;                 // DEPRECATED - not used anymore
    
    // API settings
    std::string model = "MiniMax-M2.7";
    std::string api_url;
    std::string api_key;
    double temperature = 0.0;  // §Phase 8.7: sampling temperature for summary LLM
    int compression_timeout = 30;             // Seconds for compression API call (first attempt, retries use 60/120s)
    
    // Debug
    bool debug_output = false;

    // §Phase 9.10: optional callback invoked by call_llm_for_summary after a
    // successful compression API call. Reports (prompt_tokens, completion_tokens,
    // total_tokens) to the main LLMClient so the agent's token bar stays
    // accurate between streaming turns (SSE is always usage=null).
    // Signature: void(int, int, int) — set by LLMClient::initialize().
    std::function<void(int, int, int)> on_compression_tokens;
};

/**
 * @brief Result of a compression operation
 */
struct CompressionResult {
    json prefix_messages;           // First N messages after system (kept)
    std::string middle_summary;    // LLM-generated summary of middle
    json suffix_messages;          // Last N messages (kept - current state)
    size_t original_count = 0;     // Original message count
    size_t compressed_count = 0;   // New message count (prefix + 1 + suffix)
    size_t original_tokens = 0;    // Estimated tokens before
    size_t compressed_tokens = 0;   // Estimated tokens after
};

// ============================================================================
// Phase 11: removed SubAgentCompressor class + SubAgentCompressionResult
// struct. They wrapped the 'daedalus --sub-agent' subprocess path which
// called a CLI flag removed in Phase 2.7. The synchronous compression
// path (Messages::compress_messages → Messages::call_llm_for_summary)
// is the only path used in production.

// ============================================================================
// Messages Class
// ============================================================================

class Messages {
public:
    /**
     * @brief Construct Messages with a per-agent data directory (§Phase 8.8).
     * @param data_dir Fully-resolved namespace directory — either
     *                 `<project_root>/.daedalus/` (empty agent_name) or
     *                 `<project_root>/.daedalus-<name>/` (named agent).
     *                 The directory is auto-created if missing.
     *                 AgentImpl calls resolve_agent_dir(project_root,
     *                 cfg.agent_name) to compute this; Messages
     *                 itself does NOT know about agent_name.
     * @param config Compression configuration
     */
    Messages(std::filesystem::path data_dir,
             const MessageCompressionConfig& config = MessageCompressionConfig());
    
    ~Messages();
    
    // Disable copy (each session has its own Messages instance)
    Messages(const Messages&) = delete;
    Messages& operator=(const Messages&) = delete;
    
    // Allow move
    Messages(Messages&&) noexcept;
    Messages& operator=(Messages&&) noexcept;

    // =========================================================================
    // Message Access
    // =========================================================================
    
    /**
     * @brief Get direct reference to message array (for API calls, etc.)
     */
    json& get_messages() { return messages_; }
    const json& get_messages() const { return messages_; }
    
    /**
     * @brief Get message count
     */
    size_t size() const { return messages_.size(); }
    bool empty() const { return messages_.empty(); }
    
    /**
     * @brief Get system message content
     */
    std::string get_system_content() const;
    
    /**
     * @brief Get last assistant message content
     */
    std::string get_last_assistant_content() const;
    
    /**
     * @brief Get last assistant message (for adding tool_calls)
     */
    json& get_last_assistant_message() { return messages_.back(); }
    const json& get_last_assistant_message() const { return messages_.back(); }

    // =========================================================================
    // Message Operations
    // =========================================================================
    
    /**
     * @brief Add system message (only if messages empty)
     */
    void add_system(const std::string& content);
    
    /**
     * @brief Add user message (supports multi-modal with image_url)
     */
    void add_user(const std::string& content, const std::string& image_url = "");
    
    /**
     * @brief Add assistant message
     */
    void add_assistant(const std::string& content);
    
    /**
     * @brief Add tool result message
     * @param tool_call_id The ID of the tool call this result is for
     * @param content The result content
     * @param is_error Whether this is an error result (defaults to false)
     */
    void add_tool_result(const std::string& tool_call_id, const std::string& content, bool is_error = false);
    
    /**
     * @brief Add raw message (for loading from file, etc.)
     */
    void add_message(const json& msg);
    
    /**
     * @brief Set messages array directly (used by LLMClient to sync)
     */
    void set_messages(const json& msgs);

    // =========================================================================
    // Persistence
    // =========================================================================
    
    /**
     * @brief Load messages from .daedalus/messages.json
     * @return true if loaded successfully
     */
    bool load();
    
    /**
     * @brief Save messages to .daedalus/messages.json
     */
    void save() const;
    
    /**
     * @brief Clear all messages (keep system if exists)
     */
    void clear();
    
    /**
     * @brief Check if messages file exists
     */
    bool has_saved_messages() const;

    // =========================================================================
    // Compression
    // =========================================================================
    
    /**
     * @brief Check if compression should be triggered (use in main loop only)
     * @param current_tokens Current token count from API
     * @return true if at or above compress_threshold
     */
    bool should_compress(size_t current_tokens) const;
    
    /**
     * @brief Compress middle messages - just compress, no threshold check
     *        (threshold check should be done in main loop before calling this)
     * @return Compressed messages as JSON string, or nullopt on failure
     */
    std::optional<std::string> compress();
    
    /**
     * @brief Replace messages array with compressed messages from JSON string
     * @param compressed_json Output from compress()
     */
    void replace_messages(const std::string& compressed_json);

    /**
     * @brief Truncate messages if they exceed max_size (keeps system + recent)
     * @param max_size Maximum serialized size in bytes
     * @return JSON object with "truncated" flag and size info, or empty if no truncation
     */
    json truncate_if_needed(size_t max_size);

    /**
     * @brief Get compression ratio
     */
    float get_compression_ratio(size_t current_tokens) const;

    // =========================================================================
    // Helpers
    // =========================================================================
    
    /**
     * @brief Estimate total tokens in messages (rough: size/4)
     */
    size_t estimate_tokens() const;
    
    /**
     * @brief Inject context stats into system message
     * @param current_tokens Current token count
     * @param iteration Current iteration number
     */
    void inject_context_stats(size_t current_tokens, size_t iteration, size_t max_iterations);
    
    /**
     * @brief Get project root path
     */
    const std::filesystem::path& get_project_root() const { return data_dir_; }
    
    /**
     * @brief Get compression config
     */
    const MessageCompressionConfig& get_config() const { return config_; }

    /**
     * @brief Update compression config (used when switching models)
     *
     * Updates fields needed for context size + compression API calls:
     * max_context_size, model, api_url, api_key. Other fields are preserved.
     */
    void set_config(const MessageCompressionConfig& new_config) { config_ = new_config; }
    
    /**
     * @brief Get split info for testing and debugging
     */
    void get_split_info(size_t& prefix_size, size_t& middle_size, size_t& suffix_size) const;
    /**
     * @brief Get indices of first half messages that will be deleted and compressed
     * @return pair of (start_index, end_index_exclusive) in messages_ array
     *         Returns (0, 0) if no messages to compress
     */
    std::pair<size_t, size_t> get_compression_indices() const;

    // =========================================================================
    // Tool Call Lifecycle Management (Single Source of Truth)
    // =========================================================================
    
    /**
     * @brief Tool call states (derived from message chain, except EXECUTING)
     * 
     * State is determined by the message chain:
     * - PENDING: assistant message has tool_calls[X].id="abc", no tool result exists
     * - EXECUTING: runtime marker only (ephemeral) - stored in executing_tools_
     * - COMPLETED: role="tool",tool_call_id="abc" exists with non-error content
     * - FAILED: role="tool",tool_call_id="abc" exists with error content
     * - INVALID: assistant message has tool_calls with malformed JSON arguments
     */
    enum class ToolState {
        PENDING,    // LLM requested tool call, not yet executed
        EXECUTING,  // Tool is currently running (ephemeral runtime state)
        COMPLETED,  // Tool succeeded with result
        FAILED,     // Tool failed with error
        INVALID     // Arguments malformed, cannot execute
    };
    
    /**
     * @brief Mark a tool call as executing (ephemeral runtime state)
     * @param id tool_call_id to mark
     * @return true if found and marked, false if not found
     */
    bool mark_tool_executing(const std::string& id);
    
    /**
     * @brief Clear EXECUTING state for a specific tool (called when tool completes)
     * @param id tool_call_id to clear from executing set
     */
    void clear_executing(const std::string& id);
    
    /**
     * @brief Check if a tool is currently in EXECUTING state (ephemeral)
     * @param id tool_call_id to check
     * @return true if in executing_tools_ set
     */
    bool is_executing(const std::string& id) const;
    
    /**
     * @brief Derive tool state from message chain
     * @param id tool_call_id to check
     * @return current state of the tool call
     */
    ToolState get_tool_state(const std::string& id) const;
    
    /**
     * @brief Check if all tool calls have been resolved
     * @return true if all pending or executing have been resolved
     */
    bool all_tools_resolved() const;
    
    /**
     * @brief Check if there are any pending tool calls
     * @return true if there are PENDING tool calls
     */
    bool has_pending_tools() const;
    
    /**
     * @brief Check if there are any executing tool calls
     * @return true if there are EXECUTING tool calls (ephemeral)
     */
    bool has_executing_tools() const;
    
    /**
     * @brief Get all pending tool call IDs
     * @return vector of pending tool call IDs
     */
    std::vector<std::string> get_pending_tool_ids() const;
    
    /**
     * @brief Sanitize an assistant message (validate tool_calls arguments)
     * @param assistant_msg assistant message to sanitize
     * @return sanitized assistant message
     */
    json sanitize_assistant_message(const json& assistant_msg,
                                        const std::unordered_set<std::string>& tool_result_ids = {}) const;
    
    /**
     * @brief Validate and sanitize entire message array
     * @param messages message array to sanitize
     * @return sanitized message array (removes orphans)
     */
    json sanitize_message_chain(const json& messages) const;
    
    /**
     * @brief Clear executing markers (called during compression)
     */
    void clear_executing_markers();

private:
    std::filesystem::path data_dir_;     // §Phase 8.8: per-agent namespace dir
    std::filesystem::path messages_file_;
    std::string session_id_ = "default"; // §Plan.md v0.3.5: SQLite mirror key
    json messages_;
    MessageCompressionConfig config_;
    
    // Ephemeral runtime state only (NOT persisted)
    // Tracks which tools are currently executing
    std::unordered_set<std::string> executing_tools_;
    
    // Curl handle for compression API calls
    
    // Helper: Find assistant message index with tool call
    int find_assistant_with_tool(const std::string& id) const;
    
    // Helper: Check if tool result exists
    bool has_tool_result(const std::string& id) const;
    
    // Helper: Check if tool result is an error
    bool is_error_result(const std::string& id) const;
    
    // Helper: Split messages into prefix, middle, suffix
    void split_messages(json& prefix, json& middle, json& suffix) const;

    // §Phase 11: was on SubAgentCompressor (named
    // build_compression_prompt_for_subagent). Moved into Messages
    // because the only caller is Messages::compress_messages.
    // Synchronous compression path — no subprocess.
    static std::string build_compression_prompt(const std::string& middle_plain_text);
    
    // Helper: Call LLM for summary
    std::string call_llm_for_summary(const std::string& prompt);
    
    // Helper: Extract summary text from LLM response
    std::string extract_summary_text(const std::string& response_body);
    
    // Helper: Token estimation
    size_t estimate_tokens_from_json(const json& j) const;
    
    // Helper: Write debug file
    void write_debug_file(const std::string& filename, const std::string& content);
};
