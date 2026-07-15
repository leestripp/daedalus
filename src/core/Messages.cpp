/**
 * @file Messages.cpp
 * @brief Implementation of unified message handling and compression
 * 
 * ARCHITECTURE: Single Source of Truth
 * Tool call state is DERIVED from the message chain, not stored separately:
 * - PENDING: assistant message has tool_calls[X].id="abc", no tool result exists
 * - EXECUTING: runtime marker only (ephemeral, not persisted) - stored in executing_tools_
 * - COMPLETED: role="tool",tool_call_id="abc" exists with non-error content
 * - FAILED: role="tool",tool_call_id="abc" exists with error content
 * - INVALID: assistant message has tool_calls with malformed JSON arguments
 */

#include "Messages.h"
#include "daedalus/settings.h"
#include <algorithm>
#include "api/CurlMultiSession.h"
#include "Utf8Utils.h"
#include "Logger.h"

#include <fstream>
#include <sstream>
#include <iostream>

// §Phase 5.1: helper macro that converts LOG_CERR(INFO,  ...); style
// statements into LOG_INFO/LOG_WARNING/LOG_ERROR via the existing
// Logger. Same formatted text routed through the Logger instead of
// writing to stderr directly.
#define LOG_CERR(level, expr) do {                          \
        std::ostringstream _daedalus_msg;                   \
        _daedalus_msg << expr;                               \
        LOG_##level(_daedalus_msg.str());                    \
    } while (0)
#include <thread>
#include <chrono>

// Helper function to sanitize strings to valid UTF-8
// Simple replacement-based approach (doesn't need iconv)
static std::string sanitize_utf8_simple(const std::string& input) {
    std::string result;
    result.reserve(input.size());
    
    size_t i = 0;
    while (i < input.size()) {
        unsigned char c = static_cast<unsigned char>(input[i]);
        
        if (c < 0x80) {
            // ASCII - valid
            result += static_cast<char>(c);
            i++;
        } else if ((c & 0xE0) == 0xC0) {
            // 2-byte sequence (110xxxxx 10xxxxxx)
            if (i + 1 < input.size() && (static_cast<unsigned char>(input[i+1]) & 0xC0) == 0x80) {
                result += input[i];
                result += input[i+1];
                i += 2;
            } else {
                result += '?';
                i++;
            }
        } else if ((c & 0xF0) == 0xE0) {
            // 3-byte sequence (1110xxxx 10xxxxxx 10xxxxxx)
            if (i + 2 < input.size() && 
                (static_cast<unsigned char>(input[i+1]) & 0xC0) == 0x80 &&
                (static_cast<unsigned char>(input[i+2]) & 0xC0) == 0x80) {
                result += input[i];
                result += input[i+1];
                result += input[i+2];
                i += 3;
            } else {
                result += '?';
                i++;
            }
        } else if ((c & 0xF8) == 0xF0) {
            // 4-byte sequence (11110xxx 10xxxxxx 10xxxxxx 10xxxxxx)
            if (i + 3 < input.size() && 
                (static_cast<unsigned char>(input[i+1]) & 0xC0) == 0x80 &&
                (static_cast<unsigned char>(input[i+2]) & 0xC0) == 0x80 &&
                (static_cast<unsigned char>(input[i+3]) & 0xC0) == 0x80) {
                result += input[i];
                result += input[i+1];
                result += input[i+2];
                result += input[i+3];
                i += 4;
            } else {
                result += '?';
                i++;
            }
        } else {
            // Invalid byte - replace
            result += '?';
            i++;
        }
    }
    
    return result;
}

// ============================================================================
// Helper: Convert JSON messages to plain text for compression
// ============================================================================

// ============================================================================
// Helper: Convert JSON messages to plain text for compression
// ============================================================================

/**
 * @brief Convert a JSON message array to plain text format
 * 
 * Converts: [{"role":"user","content":"..."}, {"role":"assistant","content":"..."}]
 * To: "User: ...\nAssistant: ...\n"
 * 
 * This removes JSON overhead (~50% reduction in size) allowing larger
 * conversations to be compressed via sub-agent.
 */
std::string messages_to_plain_text(const json& messages) {
    std::ostringstream oss;
    for (const auto& msg : messages) {
        std::string role = msg.value("role", "unknown");

        // Handle content - can be string (normal) or array (multimodal)
        std::string content_str;
        if (msg.contains("content")) {
            const auto& content_val = msg["content"];
            if (content_val.is_string()) {
                content_str = content_val.get<std::string>();
            } else if (content_val.is_array()) {
                // Multimodal content - extract text parts
                for (const auto& item : content_val) {
                    if (item.is_object()) {
                        std::string item_type = item.value("type", "");
                        if (item_type == "text") {
                            std::string text = item.value("text", "");
                            if (!content_str.empty()) content_str += "\n";
                            content_str += text;
                        } else if (item_type == "image_url") {
                            // Note image presence for context
                            if (!content_str.empty()) content_str += "\n";
                            content_str += "[image attached]";
                        }
                    }
                }
                // If array had no extractable text, note that
                if (content_str.empty()) {
                    content_str = "[multimodal content]";
                }
            } else {
                // Object or other - serialize as fallback
                content_str = content_val.dump();
            }
        }

        // Capitalize role for readability
        if (!role.empty()) {
            role[0] = std::toupper(role[0]);
        }

        oss << role << ": " << content_str << "\n\n";
    }
    return oss.str();
}

// ============================================================================
// Compression helpers
// ============================================================================

// §Phase 11: was SubAgentCompressor::build_compression_prompt_for_subagent.
// Renamed + moved into Messages as a private static. The function has
// nothing to do with sub-agents anymore — it's a prompt template used
// by the synchronous compression path (Messages::compress_messages →
// Messages::call_llm_for_summary).
std::string Messages::build_compression_prompt(const std::string& middle_plain_text) {
    std::ostringstream oss;
    oss << R"(
You are a compression assistant for a coding agent conversation.

Your job is to SUMMARIZE the given portion of a conversation. The given
portion contains the main work that was done. You need to compress it into
a concise summary.

GIVEN CONVERSATION TO SUMMARIZE:
)" << middle_plain_text << R"(

Requirements:
- Maximum 500 words (~1500 tokens output)
- Focus on: key decisions made, files modified, commands run, errors encountered
- Be concise - this is a transitional summary to bridge prefix and suffix
- Output ONLY the summary text, nothing else

Format your output as:
KEY EVENTS: [bullets of what happened]
FILES: [list of files modified]
NEXT: [what needs to happen next]

Start your response with "KEY EVENTS:" and end with "NEXT:" - do not include any
prefix, suffix, or explanation. Just output the summary in the format above.
)";
    return oss.str();
}
// Phase 11: deleted SubAgentCompressor::wait_for_subagent_result() and
// SubAgentCompressor::compress_via_subagent() — both spawned
// 'daedalus --sub-agent' subprocesses to do compression. That CLI flag
// was removed in Phase 2.7. The synchronous compression path
// (call_llm_for_summary below) is the only path used in practice.
// Constructor / Destructor
// ============================================================================

Messages::Messages(std::filesystem::path data_dir,
                 const MessageCompressionConfig& config)
    : data_dir_(std::move(data_dir))
    , messages_(json::array())
    , config_(config) {

    // §Phase 8.8: data_dir is the fully-resolved namespace
    // directory, passed in by AgentImpl::initialize() via
    // resolve_agent_dir(project_root, agent_name). The default
    // (empty agent_name) yields `<project>/.daedalus/`, so
    // existing users' data is in the same place as before.
    messages_file_ = data_dir_ / "messages.json";

    // Ensure the namespace directory exists.
    if (!std::filesystem::exists(data_dir_)) {
        std::filesystem::create_directories(data_dir_);
    }
}

Messages::~Messages() = default;

// Default move semantics are fine now that we no longer own a CURL*
Messages::Messages(Messages&&) noexcept = default;
Messages& Messages::operator=(Messages&&) noexcept = default;

// ============================================================================
// Message Access
// ============================================================================

std::string Messages::get_system_content() const {
    if (messages_.empty()) return "";
    if (messages_[0].value("role", "") == "system") {
        return messages_[0].value("content", "");
    }
    return "";
}

std::string Messages::get_last_assistant_content() const {
    for (int i = static_cast<int>(messages_.size()) - 1; i >= 0; --i) {
        if (messages_[i].value("role", "") == "assistant") {
            return messages_[i].value("content", "");
        }
    }
    return "";
}

// ============================================================================
// Message Operations
// ============================================================================

void Messages::add_system(const std::string& content) {
    if (!messages_.empty()) {
        LOG_CERR(INFO,  "[Messages] Warning: System message already exists, not adding" << '\n');
        return;
    }
    messages_.push_back({{"role", "system"}, {"content", content}});
}

void Messages::add_user(const std::string& content, const std::string& image_url) {
    json msg;
    msg["role"] = "user";
    
    if (!image_url.empty()) {
        // Multi-modal: array content with text and image
        json content_array = json::array();
        content_array.push_back({{"type", "text"}, {"text", content}});
        content_array.push_back({{"type", "image_url"}, {"image_url", {{"url", image_url}}}});
        msg["content"] = content_array;
    } else {
        msg["content"] = content;
    }
    
    messages_.push_back(msg);
}

void Messages::add_assistant(const std::string& content) {
    messages_.push_back({{"role", "assistant"}, {"content", content}});
}

void Messages::add_tool_result(const std::string& tool_call_id, const std::string& content, bool is_error) {
    // Auto-detect error if not explicitly specified and content starts with "Error:"
    if (!is_error && content.find("Error:") == 0) {
        is_error = true;
    }
    messages_.push_back({
        {"role", "tool"},
        {"tool_call_id", tool_call_id},
        {"content", content},
        {"status", is_error ? "error" : "success"}
    });
}

void Messages::add_message(const json& msg) {
    messages_.push_back(msg);
}

void Messages::set_messages(const json& msgs) {
    messages_ = msgs;
}

// ============================================================================
// Persistence
// ============================================================================

bool Messages::load() {
    // §Plan.md v0.3.5: try SQLite mirror first. Fall back to JSON file.
    try {
        auto store = daedalus::SettingsStore::for_home(
            messages_file_.parent_path());
        auto blob = store.get_messages_json(session_id_);
        if (!blob.empty()) {
            json saved = json::parse(blob);
            if (saved.is_array() && !saved.empty()) {
                messages_ = saved;
                messages_ = sanitize_message_chain(messages_);
                LOG_INFO("Loaded " + std::to_string(messages_.size())
                         + " messages from SQLite mirror");
                return true;
            }
        }
    } catch (const std::exception& e) {
        LOG_WARNING("Messages: SQLite mirror load failed ("
                    + std::string(e.what()) + "); falling back to JSON");
    }

    if (!std::filesystem::exists(messages_file_)) {
        return false;
    }

    try {
        std::ifstream f(messages_file_);
        if (!f.good()) {
            LOG_WARNING("Messages file exists but cannot be read: " + messages_file_.string());
            return false;
        }
        
        json saved = json::parse(f);
        if (!saved.is_array()) {
            LOG_WARNING("Messages file contains invalid format (not an array)");
            return false;
        }
        
        if (saved.empty()) {
            LOG_INFO("Messages file is empty");
            return false;
        }
        
        messages_ = saved;
        
        // Always sanitize on load to remove orphaned tool results
        messages_ = sanitize_message_chain(messages_);
        
        LOG_INFO("Loaded " + std::to_string(messages_.size()) + " messages from " + messages_file_.string());
        return true;
        
    } catch (const json::parse_error& e) {
        LOG_ERROR("JSON parse error in messages file: " + std::string(e.what()));
        return false;
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to load messages: " + std::string(e.what()));
        return false;
    }
}

void Messages::save() const {
    if (messages_.empty()) {
        // Remove empty messages file
        if (std::filesystem::exists(messages_file_)) {
            std::filesystem::remove(messages_file_);
            LOG_DEBUG("Removed empty messages file");
        }
        return;
    }
    
    // Sanitize before saving - remove orphaned tool results
    json to_save = sanitize_message_chain(messages_);
    
    try {
        // §Phase 8.8: parent_path() is the per-agent namespace dir
        // (e.g. `.daedalus/` or `.daedalus-bob/`), already created in
        // the ctor. We only need to ensure it still exists at save time
        // in case the user moved/deleted it between sessions.
        std::filesystem::path daedalus_dir = messages_file_.parent_path();
        if (!std::filesystem::exists(daedalus_dir)) {
            std::filesystem::create_directories(daedalus_dir);
        }
        
        std::ofstream f(messages_file_);
        if (!f.is_open()) {
            LOG_ERROR("Cannot open messages file for writing: " + messages_file_.string());
            return;
        }
        
        f << to_save.dump(2);
        if (f.fail()) {
            LOG_ERROR("Failed to write messages to file");
        } else {
            LOG_INFO("Saved " + std::to_string(to_save.size()) + " messages to " + messages_file_.string());
        }
        
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to save messages: " + std::string(e.what()));
    }

    // §Plan.md v0.3.5: write-through mirror to SQLite (best-effort).
    try {
        if (!messages_.empty()) {
            json mirror = sanitize_message_chain(messages_);
            auto store = daedalus::SettingsStore::for_home(
                messages_file_.parent_path());
            store.set_messages_json(session_id_, mirror.dump());
        } else {
            auto store = daedalus::SettingsStore::for_home(
                messages_file_.parent_path());
            store.delete_messages_json(session_id_);
        }
    } catch (const std::exception& e) {
        LOG_WARNING("Messages: SQLite mirror save failed ("
                    + std::string(e.what()) + ")");
    }
}

void Messages::clear() {
    messages_ = json::array();
    executing_tools_.clear();
}

bool Messages::has_saved_messages() const {
    return std::filesystem::exists(messages_file_);
}

// ============================================================================
// Compression
// ============================================================================

bool Messages::should_compress(size_t current_tokens) const {
    float ratio = get_compression_ratio(current_tokens);
    return ratio >= config_.compress_threshold;
}

float Messages::get_compression_ratio(size_t current_tokens) const {
    if (config_.max_context_size == 0) return 0.0f;
    return static_cast<float>(current_tokens) / static_cast<float>(config_.max_context_size);
}

void Messages::split_messages(json& prefix, json& middle, json& suffix) const {
    // Split at (compress_ratio * 100)% / ((1 - compress_ratio) * 100)%
    // Default: 70% / 30% - compress the oldest 70%, keep the newest 30%
    //
    // CRITICAL: Ensure we don't split in the middle of a tool call pair.
    // A tool call pair is:
    //   [assistant, tool_calls: [{id: "X"}]]   <- tool call
    //   [tool, tool_call_id: "X", content]      <- tool result
    // If we split between these, the tool result becomes orphaned
    // (its tool_call_id is in the compressed section that no longer exists).
    //
    // Safe split logic: If a tool result in [1, target_split] references a
    // tool call whose assistant is also in [1, target_split], EXTEND the
    // target to AFTER the tool result (include the whole chain in compression).

    prefix = json::array();
    if (!messages_.empty() && messages_[0].value("role", "") == "system") {
        prefix.push_back(messages_[0]);  // Keep system message in prefix
    }

    middle = json::array();
    suffix = json::array();

    // Calculate target split point (exclude system message from calculation)
    size_t total_non_system = 0;
    for (size_t i = 1; i < messages_.size(); ++i) {
        total_non_system++;
    }

    if (total_non_system == 0) {
        return;  // Nothing to split
    }

    // Target 70% of non-system messages (or whatever compress_ratio is set to)
    size_t target_split = static_cast<size_t>(total_non_system * config_.compress_ratio);

    // Find all tool_call_ids from assistant messages in [1, target_split]
    std::unordered_set<std::string> first_70_tool_calls;
    for (size_t i = 1; i <= target_split && i < messages_.size(); ++i) {
        const auto& msg = messages_[i];
        if (msg.value("role", "") == "assistant" && msg.contains("tool_calls")) {
            for (const auto& tc : msg["tool_calls"]) {
                std::string tc_id = tc.value("id", "");
                if (!tc_id.empty()) {
                    first_70_tool_calls.insert(tc_id);
                }
            }
        }
    }

    // Scan for tool results ANYWHERE in messages_ that reference a first_70
    // tool call. For each such tool result, extend target_split to AFTER
    // the tool result so the whole chain is included in the compressed
    // region (not split). This handles tool call pairs that straddle the
    // 70% boundary.
    bool adjusted = true;
    while (adjusted) {
        adjusted = false;
        for (size_t i = 1; i < messages_.size(); ++i) {
            const auto& msg = messages_[i];
            if (msg.value("role", "") == "tool") {
                std::string tc_id = msg.value("tool_call_id", "");
                if (!tc_id.empty() && first_70_tool_calls.count(tc_id) > 0) {
                    if (i + 1 > target_split) {
                        target_split = i + 1;
                        adjusted = true;
                    }
                    break;  // only need to extend once per tool result
                }
            }
        }
    }

    // Clamp to total non-system messages
    if (target_split > total_non_system) {
        target_split = total_non_system;
    }

    // Now do the actual split using the adjusted target_split
    for (size_t i = 1; i < messages_.size(); ++i) {
        const auto& msg = messages_[i];

        // Find actual position in non-system messages
        size_t non_system_index = i - 1;

        if (non_system_index < target_split) {
            middle.push_back(msg);
        } else {
            suffix.push_back(msg);
        }
    }
}

void Messages::get_split_info(size_t& prefix_size, size_t& middle_size, size_t& suffix_size) const {
    json prefix, middle, suffix;
    split_messages(prefix, middle, suffix);
    prefix_size = prefix.size();
    middle_size = middle.size();
    suffix_size = suffix.size();
}

std::pair<size_t, size_t> Messages::get_compression_indices() const {
    // Returns (start_index, end_index_exclusive) of the messages to be compressed.
    // These are the oldest messages (first X% based on compress_ratio) that will be
    // removed and replaced by a summary from the LLM.
    //
    // Algorithm:
    //   1. Calculate target split at (compress_ratio * 100)% of non-system messages
    //      (default 70%)
    //   2. Adjust split to NEVER break a tool call pair. If a tool result in
    //      [1, target_split] references a tool call whose assistant is also in
    //      [1, target_split], INCLUDE the whole chain (assistant + tool result)
    //      in the compressed region. Move target_split to AFTER the tool result.
    //   3. Return (start=1, end=1+adjusted_target_split) as exclusive range.

    size_t total_non_system = 0;
    for (size_t i = 1; i < messages_.size(); ++i) {
        total_non_system++;
    }

    if (total_non_system == 0) return {0, 0};

    // Target 70% of non-system messages (or whatever compress_ratio is set to)
    size_t target_split = static_cast<size_t>(total_non_system * config_.compress_ratio);
    if (target_split == 0) return {0, 0};

    // Find all tool_call_ids from assistant messages in [1, target_split]
    std::unordered_set<std::string> first_70_tool_calls;
    for (size_t i = 1; i <= target_split && i < messages_.size(); ++i) {
        const auto& msg = messages_[i];
        if (msg.value("role", "") == "assistant" && msg.contains("tool_calls")) {
            for (const auto& tc : msg["tool_calls"]) {
                std::string tc_id = tc.value("id", "");
                if (!tc_id.empty()) {
                    first_70_tool_calls.insert(tc_id);
                }
            }
        }
    }

    // Scan for tool results ANYWHERE in messages_ that reference a first_70
    // tool call. For each such tool result, extend target_split to AFTER
    // the tool result so the whole chain is included in the compressed
    // region (not split). This handles tool call pairs that straddle the
    // 70% boundary (e.g., tool call at 70, tool result at 71).
    bool adjusted = true;
    while (adjusted) {
        adjusted = false;
        for (size_t i = 1; i < messages_.size(); ++i) {
            const auto& msg = messages_[i];
            if (msg.value("role", "") == "tool") {
                std::string tc_id = msg.value("tool_call_id", "");
                if (!tc_id.empty() && first_70_tool_calls.count(tc_id) > 0) {
                    // Found a tool result that references a first_70 tool call.
                    // Extend target_split to AFTER this tool result (include
                    // the whole chain in the compressed region).
                    if (i + 1 > target_split) {
                        target_split = i + 1;
                        adjusted = true;  // re-scan for new overlaps
                    }
                    break;  // only need to extend once per tool result
                }
            }
        }
    }

    // Clamp to total non-system messages
    if (target_split > total_non_system) {
        target_split = total_non_system;
    }

    // Return indices: start=1 (after system), end=1+target_split (exclusive)
    size_t start = 1;
    size_t end = 1 + target_split;
    return {start, end};
}

// Phase 11: removed dead Messages::build_compression_prompt(prefix,
// middle, suffix) — superseded by Messages::build_compression_prompt(
// middle_plain_text). The 3-arg version was never called; its prompt
// template duplicated the work and the JSON serialization was an extra
// ~50% size hit. The 1-arg version (called from compress_messages)
// uses plain-text serialization which is faster.

std::string Messages::call_llm_for_summary(const std::string& prompt) {
    // ============================================================================
    // COMPRESSION TIMEOUT STRATEGY (DO NOT REMOVE THESE COMMENTS)
    // ============================================================================
    // We try the compression up to 3 times with INCREASING timeouts:
    //   Attempt 1: 30s  - fast path, works for small contexts (10-100 msgs)
    //   Attempt 2: 60s  - 2x, gives more time for medium contexts
    //   Attempt 3: 120s - 4x, for very large contexts (800+ messages)
    //
    // We do NOT use exponential backoff (wait longer between retries) because
    // that would just delay the inevitable. Instead, we give each retry MORE
    // TIME on the API call itself (the timeout grows).
    //
    // WHY EACH RETRY GETS MORE TIME: A 30s timeout might fail simply because
    // the LLM is slow to respond to a large prompt (800+ messages can take
    // minutes to summarize). A 60s or 120s timeout gives the model the time
    // it needs to finish. We don't want to give up early.
    //
    // IF ALL 3 RETRIES FAIL: We return empty string. The caller (compress())
    // will then fall back to DELETING THE FIRST 70% OF MESSAGES as a
    // last-resort measure to prevent context overflow. We have NO OTHER
    // CHOICE - if the API is down or rate-limited, the only way to keep
    // the agent running is to shrink the context manually. Better to lose
    // 70% of context than to have the agent crash from overflow.
    // ============================================================================

    // Build request payload
    json msg_content = json::array();
    msg_content.push_back(json{{"role", "user"}, {"content", prompt}});

    json request_payload = json::object();
    request_payload["model"] = config_.model;
    request_payload["messages"] = msg_content;
    request_payload["stream"] = false;
    request_payload["temperature"] = config_.temperature;
    request_payload["max_tokens"] = config_.max_compress_output;

    std::string request_body = request_payload.dump();

    // Timeout sequence: 30s, 60s, 120s (2x growth per retry)
    // DO NOT CHANGE WITHOUT READING THE COMMENT BLOCK ABOVE
    const int timeouts[] = {30, 60, 120};
    const int num_attempts = sizeof(timeouts) / sizeof(timeouts[0]);

    using daedalus::net::CurlRequest;
    using daedalus::net::CurlMultiSession;
    using daedalus::net::CurlError;

    for (int attempt = 0; attempt < num_attempts; ++attempt) {
        CurlRequest req;
        req.url = config_.api_url;
        req.method = "POST";
        req.body = request_body;
        req.headers.push_back("Content-Type: application/json");
        req.headers.push_back("Accept: application/json");
        if (!config_.api_key.empty()) {
            req.bearer_token = config_.api_key;
        }
        req.timeout_seconds = timeouts[attempt];  // 30s, 60s, 120s
        req.follow_redirects = true;

        LOG_CERR(INFO,  "[Messages] Compression attempt " << (attempt + 1) << "/"
                  << num_attempts << " (timeout=" << timeouts[attempt]
                  << "s, prompt_size=" << prompt.size() << " bytes)" << '\n');

        auto fut = CurlMultiSession::get_instance().submit(req);
        auto result = fut.get();

        if (result && result->status_code >= 200 && result->status_code < 300) {
            // SUCCESS — extract summary text AND usage for token reporting
            LOG_CERR(INFO,  "[Messages] Compression attempt " << (attempt + 1)
                      << " succeeded" << '\n');
            // §Phase 9.10: extract usage from compression response so the main
            // agent's token bar is accurate. (MiniMax SSE is always usage=null.)
            if (config_.on_compression_tokens) {
                try {
                    json resp = json::parse(result->body);
                    if (resp.contains("usage") && !resp["usage"].is_null()) {
                        int pt = resp["usage"].value("prompt_tokens", 0);
                        int ct = resp["usage"].value("completion_tokens", 0);
                        int tt = resp["usage"].value("total_tokens", 0);
                        LOG_CERR(INFO,  "[Messages] Compression usage: prompt=" << pt
                                  << " completion=" << ct << " total=" << tt << '\n');
                        config_.on_compression_tokens(pt, ct, tt);
                    } else {
                        LOG_CERR(INFO,  "[Messages] Compression response has no usage data"
                                         " — skipping token report" << '\n');
                    }
                } catch (const std::exception& e) {
                    LOG_CERR(INFO,  "[Messages] Could not parse compression usage: "
                                  << e.what() << '\n');
                }
            }
            return extract_summary_text(result->body);
        }

        if (!result) {
            CurlError err = result.error();
            LOG_CERR(ERROR,  "[Messages] Compression attempt " << (attempt + 1)
                      << " failed: " << daedalus::net::to_string(err)
                      << " (url=" << req.url
                      << ", timeout=" << timeouts[attempt] << "s)" << '\n');
        } else {
            // HTTP error (4xx/5xx) - log it but retry
            LOG_CERR(ERROR,  "[Messages] Compression attempt " << (attempt + 1)
                      << " failed: HTTP " << result->status_code << '\n');
            if (!result->body.empty()) {
                std::string body_snippet = result->body.substr(0, 200);
                LOG_CERR(INFO,  "[Messages] Response body: " << body_snippet << '\n');
            }
            // Do NOT retry on 4xx (client errors) - they won't fix themselves
            if (result->status_code >= 400 && result->status_code < 500) {
                LOG_CERR(INFO,  "[Messages] HTTP 4xx is a client error, not retrying" << '\n');
                return {};
            }
            // 5xx errors might be transient - retry
        }
    }

    // ALL 3 ATTEMPTS FAILED. Return empty string.
    // The caller (compress()) will see this and fall back to deleting
    // the first 70% of messages to prevent context overflow.
    // We have NO OTHER CHOICE - if the API is down or rate-limited,
    // we MUST reduce context size to keep the agent running.
    LOG_CERR(ERROR,  "[Messages] All " << num_attempts << " compression attempts failed."
              << " Caller will fall back to deleting first 70% of messages."
              << '\n');
    return {};
}

std::string Messages::extract_summary_text(const std::string& response_body) {
    try {
        json response_json = json::parse(response_body);
        if (response_json.contains("choices") && !response_json["choices"].empty()) {
            std::string content = response_json["choices"][0]["message"]["content"].get<std::string>();
            
            // Remove thinking tags if present
            size_t think_start = content.find("<answer>");
            if (think_start != std::string::npos) {
                size_t think_end = content.find("</answer>");
                if (think_end != std::string::npos) {
                    content = content.substr(think_start + 8, think_end - think_start - 8);
                }
            }
            
            // Remove ```markdown blocks if present
            size_t md_start = content.find("```");
            if (md_start != std::string::npos) {
                size_t md_end = content.find("```", md_start + 3);
                if (md_end != std::string::npos) {
                    content = content.substr(md_start + 3, md_end - md_start - 3);
                }
            }
            
            // Trim whitespace
            while (!content.empty() && (content.front() == ' ' || content.front() == '\n')) {
                content.erase(content.begin());
            }
            while (!content.empty() && (content.back() == ' ' || content.back() == '\n')) {
                content.pop_back();
            }
            
            return content;
        }
    } catch (const std::exception& e) {
        LOG_CERR(INFO,  "[Messages] Error parsing response: " << e.what() << '\n');
    }
    
    return {};
}

std::optional<std::string> Messages::compress() {

    // Get the indices of messages to delete (given section)
    auto [start_idx, end_idx] = get_compression_indices();  // 70/30 split with safe point

    // If no messages, nothing to compress
    if (start_idx >= end_idx) {
        return std::nullopt;
    }

    size_t middle_count = end_idx - start_idx;
    size_t before_count = messages_.size();

    // Make a COPY of messages to compress (do NOT delete originals yet)
    json middle_copy = json::array();
    for (size_t i = start_idx; i < end_idx && i < messages_.size(); ++i) {
        middle_copy.push_back(messages_[i]);
    }

    if (middle_copy.empty()) {
        return std::nullopt;
    }

    // Write debug files if enabled
    if (config_.debug_output) {
        write_debug_file("daedalus_compress_middle.txt", middle_copy.dump(2));
    }

    // Use plain text format - removes ~50% overhead vs JSON
    std::string middle_plain = messages_to_plain_text(middle_copy);

    // Build compression prompt
    std::string prompt = Messages::build_compression_prompt(middle_plain);

    // CRITICAL FIX: Call LLM for summary BEFORE deleting any messages.
    // If the API call fails, the user keeps all their messages.
    // Old behavior was to delete first, which destroyed context on any failure.
    std::string summary = call_llm_for_summary(prompt);

    if (summary.empty()) {
        // ============================================================================
        // COMPRESSION FAILED - LAST RESORT FALLBACK (DO NOT REMOVE)
        // ============================================================================
        // The compression API call failed (3 retries with timeouts 30s, 60s, 120s
        // all timed out or errored). We have NO OTHER CHOICE but to delete the
        // first 70% of messages to prevent context overflow. If we don't, the
        // next API call will exceed the model's context window and the agent
        // will crash. Better to lose 70% of context than to crash entirely.
        //
        // The agent will lose the early messages but keep:
        //   - The system message (first)
        //   - The last 30% of messages (recent context)
        // This gives the agent enough context to continue working, even if
        // it loses the history of what it was doing earlier.
        // ============================================================================

        size_t total = messages_.size();
        if (total <= 5) {
            // Too few messages to safely delete 70% - just give up
            LOG_CERR(ERROR,  "[CONTEXT] Compression failed and only " << total
                      << " messages remain. Cannot safely reduce. Giving up."
                      << '\n');
            return std::nullopt;
        }

        // Calculate the safe split point: keep system + last 30%
        size_t keep_count = std::max(size_t(2), total * 30 / 100);  // At least system + 1 other
        size_t delete_count = total - keep_count;

        // Find the start index for deletion (skip system message at index 0)
        size_t delete_start = (total > 0 && messages_[0].value("role", "") == "system") ? 1 : 0;
        // But make sure we don't delete more than delete_count
        if (delete_start >= total) delete_start = 0;
        size_t actual_delete_count = std::min(delete_count, total - delete_start);

        if (actual_delete_count == 0) {
            LOG_CERR(WARNING,  "[CONTEXT] Cannot safely reduce context (only system msg?). Giving up."
                      << '\n');
            return std::nullopt;
        }

        LOG_CERR(ERROR,  "[CONTEXT] FALLBACK: All " << (3)
                  << " compression attempts failed (timeouts 30s, 60s, 120s)."
                  << '\n');
        LOG_CERR(INFO,  "[CONTEXT] FALLBACK: Deleting first " << actual_delete_count
                  << " of " << total << " messages to prevent context overflow."
                  << '\n');
        LOG_CERR(INFO,  "[CONTEXT] FALLBACK: Keeping system message + last "
                  << (total - actual_delete_count) << " messages."
                  << '\n');

        messages_.erase(messages_.begin() + delete_start,
                        messages_.begin() + delete_start + actual_delete_count);
        return std::nullopt;  // Return nullopt but context has been reduced
    }

    // SUCCESS: Now safe to delete the middle messages and rebuild
    messages_.erase(messages_.begin() + start_idx, messages_.begin() + end_idx);
    // §Phase 5.1: route through Logger instead of std::cout.
    LOG_CERR(INFO, "[CONTEXT] Deleted " << middle_count << " middle messages (indices "
              << start_idx << "-" << end_idx << ")");

    // SUCCESS: Rebuild messages in correct order
    // Structure: system + compress_context pair + suffix (last 50%)
    json new_messages = json::array();

    // 1. Add system message (must be first)
    if (!messages_.empty() && messages_[0].value("role", "") == "system") {
        new_messages.push_back(messages_[0]);
    }

    // 2. Add assistant message with compress_context tool call
    std::string summary_content = "Context was compressed. Summary of first half:\n" + summary;
    new_messages.push_back({
        {"role", "assistant"},
        {"tool_calls", json::array({
            {
                {"id", "compress_internal"},
                {"type", "function"},
                {"function", {
                    {"name", "compress_context"},
                    {"arguments", "{}"}
                }}
            }
        })}
    });

    // 3. Add tool result with the summary (completes the tool call pair)
    new_messages.push_back({
        {"role", "tool"},
        {"tool_call_id", "compress_internal"},
        {"content", summary_content}
    });

    // 4. Add suffix messages (last remaining messages already in messages_ after deletion)
    //    These are the messages AFTER the given messages we just deleted
    //    (messages_ currently has: system + suffix)
    for (size_t i = 1; i < messages_.size(); ++i) {  // Start at 1 to skip system
        new_messages.push_back(messages_[i]);
    }

    size_t after_count = new_messages.size();

    // §Phase 5.1: route through Logger instead of std::cout.
    LOG_CERR(INFO, "\n[CONTEXT] Compressed " << before_count << " messages to " << after_count << " messages.");
    LOG_CERR(INFO, "[CONTEXT] Structure: system + compress_context pair + suffix(last 50%)");
    LOG_CERR(INFO, "[CONTEXT] Messages summarized: " << middle_count << " messages -> 1 summary");

    std::string result = new_messages.dump();

    // Clear ephemeral executing markers after compression
    executing_tools_.clear();

    return result;
}

void Messages::replace_messages(const std::string& compressed_json) {
    try {
        messages_ = json::parse(compressed_json);
        // §Phase 5.1: route through Logger instead of std::cout.
        LOG_CERR(INFO, "[CONTEXT] Replaced " << messages_.size() << " messages with compressed version");
    } catch (const std::exception& e) {
        LOG_CERR(ERROR,  "[Messages] Failed to replace messages: " << e.what() << '\n');
    }
}

json Messages::truncate_if_needed(size_t max_size) {
    // Serialize messages to check size
    // First, sanitize any strings with invalid UTF-8 to prevent JSON serialization errors
    std::function<void(json&)> sanitize_json_for_truncation;
    sanitize_json_for_truncation = [&](json& j) {
        if (j.is_object()) {
            for (auto& [key, value] : j.items()) {
                if (value.is_string()) {
                    value = sanitize_utf8_simple(value.get<std::string>());
                } else if (value.is_object() || value.is_array()) {
                    sanitize_json_for_truncation(value);
                }
            }
        } else if (j.is_array()) {
            for (auto& item : j) {
                if (item.is_string()) {
                    item = sanitize_utf8_simple(item.get<std::string>());
                } else if (item.is_object() || item.is_array()) {
                    sanitize_json_for_truncation(item);
                }
            }
        }
    };
    sanitize_json_for_truncation(messages_);
    
    std::string serialized = messages_.dump();
    
    if (serialized.size() <= max_size) {
        return json::object();  // No truncation needed
    }
    
    LOG_CERR(WARNING,  "[WARNING] Request payload (" << serialized.size() 
              << " bytes) exceeds max_request_size (" << max_size 
              << " bytes). Truncating oldest messages..." << '\n');
    
    // We need to keep:
    // 1. The system message (first message)
    // 2. As many recent messages as possible
    
    if (messages_.size() <= 1) {
        // Can't truncate further - just return warning
        LOG_ERROR("Cannot truncate - only system message present");
        return json::object({{"warning", "Request too large even for single message"}});
    }
    
    // Keep system message, try to keep as many recent messages as possible
    json system_msg = messages_[0];
    
    // Start from the end and work backwards, keeping messages until we hit the limit
    // IMPORTANT: Never cut between an assistant message with tool_calls and its tool results
    json truncated_messages = json::array();
    truncated_messages.push_back(system_msg);
    
    // Add messages from the end until we hit the limit
    size_t current_size = system_msg.dump().size();
    size_t target_size = max_size * 0.8;  // Aim for 80% of max to leave room
    
    int i = static_cast<int>(messages_.size()) - 1;
    while (i >= 1) {
        // Check if adding this message would exceed limit
        std::string msg_str = messages_[i].dump();
        if (current_size + msg_str.size() > target_size) {
            break;  // Would exceed limit
        }
        
        // If this is a tool result, we need to keep going back to find its parent
        // (assistant message with tool_calls). We can't cut the chain.
        std::string role = messages_[i].value("role", "");
        if (role == "tool") {
            // This is a tool result - we must include all preceding messages in this chain
            // until we find the assistant that made the tool call
            truncated_messages.push_back(messages_[i]);
            current_size += msg_str.size();
            i--;
            
            // Keep going back to find the tool call's parent assistant message
            while (i >= 1) {
                std::string prev_role = messages_[i].value("role", "");
                std::string prev_str = messages_[i].dump();
                if (current_size + prev_str.size() > target_size) {
                    // Can't fit the parent assistant - this is a problem
                    // The chain would be broken, so we stop here
                    LOG_CERR(WARNING,  "[WARNING] Cannot keep complete tool call chain, truncating tool result" << '\n');
                    break;
                }
                truncated_messages.push_back(messages_[i]);
                current_size += prev_str.size();
                i--;
                
                // Stop if we hit an assistant message (it should have tool_calls to match the tool result)
                // Tool results are always preceded by their parent assistant message
                if (prev_role == "assistant") {
                    // Check if this assistant has tool_calls (should have for the tool result we found)
                    if (messages_[i+1].contains("tool_calls") || 
                        (messages_[i].contains("tool_calls") && !messages_[i]["tool_calls"].empty())) {
                        // Found the parent - stop looking
                        break;
                    }
                }
            }
            continue;
        }
        
        truncated_messages.push_back(messages_[i]);
        current_size += msg_str.size();
        i--;
    }
    
    // Reverse to restore chronological order (system -> oldest -> newest)
    // Actually, we want newest first after system, so let's rebuild properly
    json result = json::array();
    result.push_back(system_msg);
    
    // Add remaining messages in reverse order (newest first)
    for (int i = static_cast<int>(truncated_messages.size()) - 1; i >= 1; --i) {
        result.push_back(truncated_messages[i]);
    }
    
    std::string new_size_str = result.dump();
    LOG_CERR(INFO,  "[INFO] Truncated from " << serialized.size() << " to " 
              << new_size_str.size() << " bytes (" << result.size() << " messages)" << '\n');
    
    // Update internal messages
    messages_ = result;
    
    return json::object({{"truncated", true}, {"original_size", serialized.size()}, {"new_size", new_size_str.size()}});
}

// ============================================================================
// Tool Call Lifecycle Management (Single Source of Truth)
// ============================================================================

bool Messages::mark_tool_executing(const std::string& id) {
    // First check if tool exists (has assistant with tool_calls[id])
    int assistant_idx = find_assistant_with_tool(id);
    if (assistant_idx < 0) {
        LOG_CERR(INFO,  "[Messages] mark_tool_executing: " << id << " not found in any assistant" << '\n');
        return false;
    }
    
    // Check state - can only go from PENDING to EXECUTING
    ToolState state = get_tool_state(id);
    if (state != ToolState::PENDING) {
        LOG_CERR(INFO,  "[Messages] mark_tool_executing: " << id << " not in PENDING state (current: " << static_cast<int>(state) << ")" << '\n');
        return false;
    }
    
    // Set ephemeral executing marker
    executing_tools_.insert(id);
    LOG_DEBUG("Tool " + id + " marked EXECUTING");
    return true;
}

void Messages::clear_executing(const std::string& id) {
    executing_tools_.erase(id);
}

bool Messages::is_executing(const std::string& id) const {
    return executing_tools_.count(id) > 0;
}

Messages::ToolState Messages::get_tool_state(const std::string& id) const {
    // 1. Check if executing (ephemeral runtime state)
    if (executing_tools_.count(id) > 0) return ToolState::EXECUTING;
    
    // 2. Check if tool result exists
    if (has_tool_result(id)) {
        return is_error_result(id) ? ToolState::FAILED : ToolState::COMPLETED;
    }
    
    // 3. Check if tool call exists in any assistant message
    if (find_assistant_with_tool(id) >= 0) return ToolState::PENDING;
    
    return ToolState::INVALID;
}

bool Messages::all_tools_resolved() const {
    // Find all tool call IDs from assistant messages
    std::vector<std::string> pending_ids = get_pending_tool_ids();
    return pending_ids.empty();
}

bool Messages::has_pending_tools() const {
    return !get_pending_tool_ids().empty();
}

bool Messages::has_executing_tools() const {
    return !executing_tools_.empty();
}

std::vector<std::string> Messages::get_pending_tool_ids() const {
    std::vector<std::string> pending;
    
    for (const auto& msg : messages_) {
        if (msg.value("role", "") == "assistant" && msg.contains("tool_calls")) {
            for (const auto& tc : msg["tool_calls"]) {
                std::string tc_id = tc.value("id", "");
                if (!tc_id.empty()) {
                    // Check if there's a result for this tool
                    if (!has_tool_result(tc_id)) {
                        pending.push_back(tc_id);
                    }
                }
            }
        }
    }
    
    return pending;
}

json Messages::sanitize_assistant_message(const json& msg,
                                            const std::unordered_set<std::string>& tool_result_ids) const {
    if (!msg.is_object() || !msg.contains("tool_calls")) return msg;
    
    json sanitized = msg;
    json valid_calls = json::array();
    
    if (msg["tool_calls"].is_array()) {
        for (const auto& tc : msg["tool_calls"]) {
            std::string tc_id = tc.value("id", "");
            
            bool valid_args = true;
            if (tc.contains("function") && tc["function"].contains("arguments")) {
                const auto& args_val = tc["function"]["arguments"];
                if (args_val.is_string()) {
                    // Use json::accept() for efficient validation without creating parse tree
                    valid_args = json::accept(args_val.get<std::string>());
                }
            }
            
            // CRITICAL FIX: Remove ORPHAN tool_calls (those with no
            // matching tool result). These cause API 400 errors and
            // infinite retry loops. Only check if tool_result_ids was
            // provided (non-empty means we did the collection pass).
            bool has_result = tool_result_ids.empty() || tool_result_ids.count(tc_id) > 0;
            if (!has_result) {
                LOG_CERR(INFO,  "[Messages] Removing orphan tool_call (no result): " << tc_id << '\n');
                continue;
            }

            if (valid_args) {
                valid_calls.push_back(tc);
            } else {
                LOG_CERR(INFO,  "[Messages] Removing invalid args: " << tc_id << '\n');
            }
        }
    }
    
    if (valid_calls.empty()) {
        sanitized.erase("tool_calls");
    } else {
        sanitized["tool_calls"] = valid_calls;
    }
    
    return sanitized;
}

json Messages::sanitize_message_chain(const json& msgs) const {
    if (!msgs.is_array()) return msgs;
    
    json result = json::array();
    
    // First pass: collect valid tool call IDs (those with proper structure)
    std::unordered_set<std::string> valid_tool_ids;
    for (const auto& msg : msgs) {
        if (msg.value("role", "") == "assistant" && msg.contains("tool_calls")) {
            for (const auto& tc : msg["tool_calls"]) {
                std::string tc_id = tc.value("id", "");
                if (!tc_id.empty()) {
                    // Validate arguments
                    bool valid = true;
                    if (tc.contains("function") && tc["function"].contains("arguments")) {
                        const auto& args_val = tc["function"]["arguments"];
                        if (args_val.is_string()) {
                            // Use json::accept() for efficient validation without creating parse tree
                            valid = json::accept(args_val.get<std::string>());
                        }
                    }
                    if (valid) {
                        valid_tool_ids.insert(tc_id);
                    }
                }
            }
        }
    }
    
    // Second pass (NEW): collect tool_call_ids that HAVE matching tool results
    // This is used to detect ORPHAN tool_calls (assistant tool_calls with
    // no matching tool result) - these cause the API to return 400 errors
    // and create infinite retry loops in the agent.
    std::unordered_set<std::string> tool_result_ids;
    for (const auto& msg : msgs) {
        if (msg.value("role", "") == "tool") {
            std::string tc_id = msg.value("tool_call_id", "");
            if (!tc_id.empty()) {
                tool_result_ids.insert(tc_id);
            }
        }
    }

    // Third pass: build sanitized chain
    for (const auto& msg : msgs) {
        std::string role = msg.value("role", "");
        
        if (role == "tool") {
            std::string tc_id = msg.value("tool_call_id", "");
            // Only drop tool results that are truly orphaned:
            // - tool_call_id is empty (malformed)
            // - OR tool_call_id is orphaned (no matching tool call in any assistant message)
            if (tc_id.empty()) {
                LOG_CERR(INFO,  "[Messages] Removed malformed tool result (empty tool_call_id)" << '\n');
                continue;  // Drop tool results with empty/missing tool_call_id
            }
            // Drop orphaned tool results - they cause API errors
            if (valid_tool_ids.find(tc_id) == valid_tool_ids.end()) {
                LOG_CERR(INFO,  "[Messages] Removed orphaned tool result (no matching tool call for id: " << tc_id << ")" << '\n');
                continue;
            }
            // This tool result is valid - has matching tool call
            result.push_back(msg);
        } else if (role == "assistant") {
            // CRITICAL FIX: Pass tool_result_ids so orphan tool_calls (no
            // matching result) are removed - prevents API 400 errors
            json sanitized = sanitize_assistant_message(msg, tool_result_ids);
            if (sanitized.contains("content") || sanitized.contains("tool_calls")) {
                result.push_back(sanitized);
            }
        } else {
            result.push_back(msg);
        }
    }
    
    return result;
}

void Messages::clear_executing_markers() {
    executing_tools_.clear();
}

// ============================================================================
// Private Helper Methods
// ============================================================================

int Messages::find_assistant_with_tool(const std::string& id) const {
    for (size_t i = 0; i < messages_.size(); i++) {
        const auto& msg = messages_[i];
        if (msg.value("role", "") == "assistant" && msg.contains("tool_calls")) {
            for (const auto& tc : msg["tool_calls"]) {
                if (tc.value("id", "") == id) {
                    return static_cast<int>(i);
                }
            }
        }
    }
    return -1;
}

bool Messages::has_tool_result(const std::string& id) const {
    for (const auto& msg : messages_) {
        if (msg.value("role", "") == "tool" && 
            msg.value("tool_call_id", "") == id) {
            return true;
        }
    }
    return false;
}

bool Messages::is_error_result(const std::string& id) const {
    for (const auto& msg : messages_) {
        if (msg.value("role", "") == "tool" && 
            msg.value("tool_call_id", "") == id) {
            // First check the structured status field (preferred)
            std::string status = msg.value("status", "");
            if (status == "error") {
                return true;
            }
            if (status == "success") {
                return false;
            }
            // Fallback: check content for error indicators (backward compatibility)
            // Only check if status is not explicitly set
            std::string content = msg.value("content", "");
            return content.find("Error:") != std::string::npos ||
                   content.find("error") != std::string::npos;
        }
    }
    return false;
}

// ============================================================================
// Helpers
// ============================================================================

size_t Messages::estimate_tokens() const {
    return estimate_tokens_from_json(messages_);
}

size_t Messages::estimate_tokens_from_json(const json& j) const {
    return j.dump().size() / 4;  // Rough estimate: 1 token ≈ 4 bytes
}

void Messages::inject_context_stats(size_t current_tokens, size_t iteration, size_t max_iterations) {
    if (messages_.empty()) return;
    
    json& system = messages_[0];
    if (system.value("role", "") != "system") return;
    
    std::string content = system.value("content", "");
    
    // Update Context Information section
    std::ostringstream stats;
    stats << "\n\n## Context Status\n";
    stats << "- **Token Usage**: " << current_tokens << "/" << config_.max_context_size 
          << " (" << static_cast<int>(get_compression_ratio(current_tokens) * 100) << "%)\n";
    stats << "- **Iterations**: " << iteration << "/" << max_iterations << "\n";
    stats << "- **Messages**: " << messages_.size() << "\n";
    
    // Find or append Context Status section
    size_t ctx_pos = content.find("## Context Status");
    if (ctx_pos != std::string::npos) {
        // Replace existing context status
        size_t end_pos = content.find("\n## ", ctx_pos + 1);
        if (end_pos == std::string::npos) {
            content = content.substr(0, ctx_pos) + stats.str();
        } else {
            content = content.substr(0, ctx_pos) + stats.str() + content.substr(end_pos);
        }
    } else {
        content += stats.str();
    }
    
    system["content"] = content;
}

void Messages::write_debug_file(const std::string& filename, const std::string& content) {
    std::ofstream debug_file("/tmp/" + filename);
    if (debug_file.is_open()) {
        debug_file << content << '\n';
        debug_file.close();
    }
}
