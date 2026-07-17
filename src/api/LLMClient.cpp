#include "api/LLMClient.h"
#include "core/version.h"
#include "core/FileIO.h"
#include "core/utils.h"
#include "core/ImageProcessor.h"
#include "core/SkillManager.h"
#include "core/Logger.h"
#include "core/SystemPromptBuilder.h"
#include "tools/ToolHandler.h"
#include "tools/ToolFactory.h"
#include "tools/BashTool.h"
#include "core/ThreadPool.h"
#include "core/Utf8Utils.h"  // §B11/B28: shared utf8_sanitize_replace_invalid
#include "daedalus/default_paths.h"   // §Phase 8.8 — resolve_agent_dir()
#include "daedalus/types/AgentState.h"  // §State Machine — agent_state_from()
#include "mcp/McpServerManager.h"  // §Phase 11 — injected via ctor

#include <OmniNet.h>
#include <http/HttpClient.h>
#include <http/HttpResponse.h>

#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <future>
#include <filesystem>
#include <set>
#include <sys/select.h>
#include <iconv.h>
#include <pwd.h>
#include <ctime>

// §Phase 5.1: helper macro that converts LOG_CERR(INFO,  ...); style
// statements into LOG_INFO/LOG_WARNING/LOG_ERROR via the existing
// Logger. Same formatted text routed through the Logger instead
// of writing to stderr directly.
#define LOG_CERR(level, expr) do {                          \
        std::ostringstream _daedalus_msg;                   \
        _daedalus_msg << expr;                               \
        LOG_##level(_daedalus_msg.str());                    \
    } while (0)

// Replaces invalid UTF-8 bytes with a replacement character.
// §B11/B28: state-machine fallback uses shared utf8_sanitize_replace_invalid
// from Utf8Utils.h (which also fixes the B12 truncation bug).
static std::string sanitize_utf8(const std::string& input) {
    // Try iconv conversion to ensure valid UTF-8
    iconv_t cd = iconv_open("UTF-8", "UTF-8");
    if (cd != (iconv_t)-1) {
        char* in_ptr = const_cast<char*>(input.data());
        size_t in_left = input.size();
        std::string output(input.size() * 4, '\0');
        char* out_ptr = &output[0];
        size_t out_left = output.size();
        size_t conv_result = iconv(cd, &in_ptr, &in_left, &out_ptr, &out_left);
        iconv_close(cd);
        if (conv_result != (size_t)-1 && in_left == 0) {
            return output.c_str();  // conversion succeeded
        }
        // Fall through to state-machine path
    }
    // State-machine fallback via shared utility (fixes B12 truncation bug)
    return utf8_sanitize_replace_invalid(input);
}

// §Phase 5.1: TerminalGuard removed. Library no longer touches the
// host's terminal state — no tcgetattr, no tcsetattr, no raw mode,
// no isatty. Hosts that want ESC/Ctrl-C capture wire it themselves
// and call Agent::cancel() (or LLMClient::cancel()) on detection.

// Helper functions (moved from main.cpp)

// §NC-8 (2026-07-16): WriteBuffer / write_callback removed. The
// sync fallback path now uses OmniNet::HTTP::Client::send_blocking,
// which fills response_buffer directly — no manual write callback.

// ============================================================================
// Error Handling and Retry Helpers
// ============================================================================

/**
 * @brief Error category classification for API errors
 */
enum class ErrorCategory {
    TRANSIENT,  // Retryable errors (rate limit, internal errors, etc.)
    FATAL,      // Non-retryable errors (auth, permissions, etc.)
    UNKNOWN     // Default for unrecognized errors
};

/**
 * @brief Classify an API error type as transient or fatal
 */
static ErrorCategory classify_api_error(const std::string& error_type, const std::string& error_message = "") {
    // Fatal errors - don't retry (these won't be fixed by retrying)
    if (error_type == "authentication_error" ||
        error_type == "invalid_request" ||
        error_type == "permission_error" ||
        error_type == "invalid_api_key" ||
        error_type == "context_length_exceeded") {
        return ErrorCategory::FATAL;
    }
    
    // MiniMax rejects multiple "role": "system" - this is fatal, don't retry
    if (error_type == "bad_request_error" && 
        error_message.find("invalid message role") != std::string::npos) {
        return ErrorCategory::FATAL;
    }
    
    // "tool call result does not follow tool call" - FATAL, don't retry
    // This means messages got out of sync - retrying won't fix it
    if (error_type == "bad_request_error" && 
        error_message.find("tool call result does not follow tool call") != std::string::npos) {
        return ErrorCategory::FATAL;
    }
    
    // All other bad_request_error - also FATAL (don't retry on client errors)
    if (error_type == "bad_request_error") {
        return ErrorCategory::FATAL;
    }
    
    // Transient errors - retry with backoff (these are server-side issues)
    if (error_type == "rate_limit" ||
        error_type == "rate_limit_error" ||
        error_type == "internal_error" ||
        error_type == "service_unavailable" ||
        error_type == "timeout" ||
        error_type == "server_error" ||
        error_type == "internal_server_error" ||
        error_type == "service_overloaded" ||
        error_type == "empty_response") {
        return ErrorCategory::TRANSIENT;
    }
    
    // Unknown - treat as FATAL by default to avoid polluting messages with retries
    return ErrorCategory::FATAL;
}

// §B9 refactor: extract "find last tool_call" walk into a single helper.
// Used by both the FATAL error path (with args for debugging) and the
// retry-fatal path (id + name only).
struct LastToolCall {
    std::string id;
    std::string name;
    std::string args;  // optional; empty if not requested
};

static LastToolCall find_last_tool_call(const json& messages,
                                        bool want_args = false) {
    LastToolCall out;
    for (int i = static_cast<int>(messages.size()) - 1; i >= 0; i--) {
        if (!messages[i].is_object()) continue;
        if (messages[i].value("role", "") != "assistant") continue;
        if (!messages[i].contains("tool_calls") ||
            !messages[i]["tool_calls"].is_array()) continue;
        auto& tcs = messages[i]["tool_calls"];
        if (tcs.empty() || !tcs[tcs.size() - 1].is_object()) continue;
        auto& last_tc = tcs[tcs.size() - 1];
        out.id = last_tc.value("id", "UNKNOWN");
        if (last_tc.contains("function") && last_tc["function"].is_object()) {
            out.name = last_tc["function"].value("name", "UNKNOWN");
            if (want_args &&
                last_tc["function"].contains("arguments") &&
                last_tc["function"]["arguments"].is_string()) {
                out.args = last_tc["function"]["arguments"].get<std::string>();
                if (out.args.length() > 200) {
                    out.args = out.args.substr(0, 200) + "...";
                }
            }
        }
        break;
    }
    return out;
}



// Implementation of LLMClient methods
LLMClient::LLMClient(const AppConfig& config, daedalus::SettingsStore* settings_store,
                       mcp::McpServerManager* mcp_manager) :
    config_(config),
    file_io_(std::make_unique<FileIO>(config.target_directory)),
    settings_store_(settings_store),
    iteration_count_(0),   // §Phase 11: order follows declaration; sink_ defaulted
    mcp_manager_(mcp_manager)   // §Phase 11: injected; nullptr = MCP disabled
{
    // Load persisted command approvals
    ToolHandler::load_persisted_approvals(config.target_directory);
    // Initialize current working directory tracker with project root
    ToolHandler::init_current_working_directory(config.target_directory);
    
    // Initialize Session Memory (if not disabled)
    if (!config_.memory_disabled) {
        // §Phase 8.8: SessionMemory takes the resolved per-agent
        // namespace dir. Empty agent_name yields the legacy
        // `<project>/.daedalus/memory/` — same as before.
        session_memory_ = std::make_unique<SessionMemory>(
            daedalus::resolve_agent_dir(config.target_directory,
                                       config.agent_name));
        session_memory_->on_session_start();
    }
    
    // Initialize ToolCallManager for tracking tool execution state (now via Messages)
    // Note: messages_ is used for tool call tracking via Messages::add_tool_calls etc.
    
    // Set up BashTool callback for build event tracking
    BashTool::set_build_callback([this](const std::string& command, bool success) {
        if (session_memory_) {
            session_memory_->on_build_complete(command, success);
        }
    });
    
    // Initialize memory tools with SessionMemory pointer
    ToolHandler::init_memory_tools(session_memory_.get());
    
    // §v0.5.0: load settings from SettingsStore (no is_loaded() check needed — SQLite is always ready)
    if (settings_store) {
        max_iterations_ = settings_store->get_max_agent_iterations();
        max_tool_result_size_ = settings_store->get_max_tool_result_size();
        max_request_size_ = settings_store->get_max_request_size();
        max_response_size_ = settings_store->get_max_response_size();

        // Apply auto_approve setting (already displayed at startup if enabled)
        if (settings_store->get_auto_approve()) {
            ToolHandler::set_auto_mode(true);
        }
        
        // Initialize Messages with settings
        MessageCompressionConfig messages_config;
        // §Phase 8.5+: use the *selected* model's max_context_size, not the
        // settings.json default. Previously this used get_max_context_size()
        // which always returns the default model's context, causing the
        // Token Usage progress bar to show e.g. 500000 (m3's) when running
        // m2.7 (which actually has 200000).
        if (!config_.model_name.empty()) {
            messages_config.max_context_size =
                settings_store->get_max_context_size_for_model(config_.model_name);
        } else {
            messages_config.max_context_size =
                settings_store->get_max_context_size_for_model(config_.model);
        }
        messages_config.model = config_.model;
        messages_config.max_compress_output = 1500;
        // Compression timeout. First attempt is 30s, retries grow to 60s and 120s
        // (see Messages::call_llm_for_summary).
        messages_config.compression_timeout = 30;
        messages_config.debug_output = true; // Enable for debugging
        messages_config.api_url = config_.base_url + config_.chat_endpoint;
        messages_config.api_key = config_.api_key;
        messages_config.temperature = config_.temperature;  // §Phase 8.7

        // §Phase 9.10: wire compression token callback so the main agent's
        // token bar stays accurate between streaming turns (SSE is always null).
        messages_config.on_compression_tokens =
            [this](int pt, int ct, int tt) {
                report_compression_token_usage(pt, ct, tt);
            };

        // §Phase 8.8: resolve the per-agent namespace dir. Empty
        // agent_name yields the legacy `<project>/.daedalus/` — zero
        // behaviour change for existing users.
        messages_ = std::make_unique<Messages>(
            daedalus::resolve_agent_dir(config.target_directory, config.agent_name),
            messages_config);
    } else {
        // Settings not loaded - create Messages with defaults for compression functionality
        MessageCompressionConfig messages_config;
        messages_config.max_context_size = 200000;
        messages_config.debug_output = true;
        messages_config.temperature = config_.temperature;  // §Phase 8.7
        messages_config.on_compression_tokens =
            [this](int pt, int ct, int tt) {
                report_compression_token_usage(pt, ct, tt);
            };
        messages_ = std::make_unique<Messages>(
            daedalus::resolve_agent_dir(config.target_directory, config.agent_name),
            messages_config);
    }

    // §18/§18.9 — at startup, refresh the MCP tool surface from the
    // configured servers (if any). Failures are isolated per server by
    // the Hub's circuit breaker — we never abort startup because of MCP.
    refresh_mcp_tools();

    // §18.9.C — soft-cap warning. If the combined tool schema exceeds
    // mcp.tool_schema_soft_cap_percent of the configured context window,
    // log a warning so the user can prune servers.json. Default 5% is
    // a conservative bound; well-tuned configs sit well below.
    // §Phase 11: guard with mcp_manager_ (nullptr when mcp_enabled=false).
    // §v0.5.0: soft-cap check uses settings_store_ (nullptr is safe — check guards it)
    if (mcp_manager_ && settings_store_) {
        const size_t est_tokens = mcp_manager_->estimated_token_count();
        if (est_tokens > 0) {
            const int cap_pct = settings_store_->get_mcp_tool_schema_soft_cap_percent();
            const size_t max_ctx = static_cast<size_t>(get_max_context_size());
            const size_t cap_tokens = (max_ctx * static_cast<size_t>(cap_pct)) / 100;
            if (est_tokens > cap_tokens) {
                std::ostringstream w;
                w << "[MCP] WARNING: tool schemas consume ~"
                  << est_tokens << " tokens ("
                  << cap_pct << "% cap is "
                  << cap_tokens << "). Consider pruning servers.json.";
                if (sink_) sink_->on_log(daedalus::LogLevel::Warn, w.str());
                else LOG_WARNING(w.str());
            }
        }
    }
}

void LLMClient::refresh_mcp_tools() {
    // §18.9 — refresh via the singleton ToolFactory (which lives in
    // tools/ToolFactory.cpp). The factory iterates McpServerManager and
    // registers one McpProxiedTool per discovered tool. We deliberately
    // keep the LLMClient thin: no transport knowledge.
    auto factory = ToolHandler::get_tool_factory();
    if (factory) factory->refresh_mcp_tools();
    mcp_tools_refresh_pending_ = false;
}

// §Phase 5.1: ESCKeyDetector removed. The library no longer polls
// stdin for ESC bytes. Hosts that want ESC-cancel capture it in
// their own thread and call Agent::cancel() / LLMClient::cancel().

std::future<std::string> LLMClient::make_api_request_async(
    const std::string& url,
    const std::string& payload,
    std::stop_token stop_token,
    std::function<void(std::string_view)> streaming_callback
) {
    using daedalus::net::HttpRequest;
    using daedalus::net::HttpSession;
    using daedalus::net::HttpError;
    using daedalus::net::to_string;

    // Get server error settings
    daedalus::ServerErrorSettings server_error_settings;
    try {
        if (settings_store_) {
            server_error_settings = settings_store_->get_server_error_settings();
        }
    } catch (...) {
        server_error_settings.enabled = true;
        server_error_settings.initial_wait_seconds = 30;
        server_error_settings.max_wait_seconds = 300;
        server_error_settings.backoff_multiplier = 2.0;
        server_error_settings.max_retries_per_call = 3;
    }

    // Capture sink for error routing from the worker thread.
    std::weak_ptr<daedalus::AgentSink> weak_sink = sink_;
    auto log_err = [weak_sink](std::string msg) {
        if (auto s = weak_sink.lock()) s->on_log(daedalus::LogLevel::Error, msg);
        else LOG_ERROR(msg);
    };

    // Submit the entire retry loop to the ThreadPool so it doesn't block
    // the calling thread.
    return daedalus::concurrent::ThreadPool::get_instance().submit(
        daedalus::concurrent::TaskPriority::HIGH,
        [this, url, payload, stop_token, server_error_settings,
         streaming_callback, weak_sink, log_err]() mutable -> std::string {
            int max_retries = server_error_settings.max_retries_per_call;
            int wait_time = server_error_settings.initial_wait_seconds;
            bool last_attempt_was_transport = false;  // §B14 fix
            std::string last_body;

            for (int attempt = 1; attempt <= max_retries; ++attempt) {
                if (stop_token.stop_requested()) {
                    throw std::runtime_error("API request cancelled");
                }

                HttpRequest req;
                req.url = url;
                req.method = "POST";
                req.body = payload;
                req.headers.push_back("Content-Type: application/json");
                if (streaming_callback) {
                    req.headers.push_back("Accept: text/event-stream");
                    // Streaming: pass per-chunk callback to HttpSession.
                    // Guard against stop_requested() so in-flight chunks from the
                    // kernel buffer don't fire one more on_chunk after cancellation.
                    auto safe_callback = [&streaming_callback,
                                          &stop_token](std::string_view chunk) {
                        if (stop_token.stop_requested()) return;
                        streaming_callback(chunk);
                    };
                    req.stream_callback = safe_callback;
                } else {
                    req.headers.push_back("Accept: application/json");
                }
                if (!config_.api_key.empty()) {
                    req.bearer_token = config_.api_key;
                }
                req.timeout_seconds = 900;  // 15 minutes
                req.follow_redirects = true;

                // Submit via HttpSession (async, non-blocking)
                auto fut = HttpSession::get_instance().submit_cancellable(req, stop_token);
                auto result = fut.get();

                if (result) {
                    last_body = std::move(result->body);
                    long code = result->status_code;
                    int  retry_after = result->retry_after_seconds;
                    if (code >= 200 && code < 300) {
                        return last_body;  // Success
                    } else if (code == 401) {
                        // Auth failure: never retry. Surface the error.
                        log_err("[API] HTTP " + std::to_string(code) + " (auth failed)");
                        return last_body;
                    } else if (code == 429) {
                        // §B14 fix: rate-limited. Honour Retry-After if present;
                        // otherwise fall through to the exponential-backoff path.
                        log_err("[API] HTTP 429 (rate limited)");
                        if (retry_after > 0) {
                            wait_time = retry_after;
                            log_err("[API] Honoring Retry-After: " +
                                    std::to_string(retry_after) + "s");
                        }
                        // Continue to retry block below.
                    } else if (code >= 400 && code < 500) {
                        // 4xx (other than 401/429): client error, no point retrying.
                        log_err("[API] HTTP " + std::to_string(code) + " (client error)");
                        return last_body;
                    } else {
                        log_err("[API] HTTP " + std::to_string(code) + " (server error)");
                    }
                } else {
                    HttpError err = result.error();
                    if (err == HttpError::Cancelled) {
                        throw std::runtime_error("API request cancelled");
                    } else if (err == HttpError::Timeout) {
                        log_err("[API] request timed out (attempt " + std::to_string(attempt) + ")");
                    } else {
                        log_err("[API] request failed (attempt " + std::to_string(attempt) + "): " + to_string(err));
                    }
                    // §B14 fix: transport errors (timeout / network) should retry
                    // regardless of server_error_settings.enabled — a single blip
                    // shouldn't drop the whole turn. The setting gates HTTP 5xx / 429.
                    last_attempt_was_transport = true;
                }

                // Wait before retry (exponential backoff, cancellable)
                // §B14 fix: split retry gate. Transport errors retry always
                // (subject to max_retries + stop_token). HTTP 5xx / 429 retry
                // only when server_error_settings.enabled is true.
                const bool is_transport_retry = last_attempt_was_transport;
                const bool is_http_retry = !last_attempt_was_transport;
                const bool http_retry_allowed =
                    is_http_retry && server_error_settings.enabled;
                const bool should_retry =
                    attempt < max_retries &&
                    !stop_token.stop_requested() &&
                    (is_transport_retry || http_retry_allowed);
                if (should_retry) {
                    std::ostringstream w;
                    w << "Waiting " << wait_time << "s before retry...";
                    if (auto s = weak_sink.lock()) s->on_log(daedalus::LogLevel::Info, w.str());
                    for (int i = 0; i < wait_time * 10; ++i) {
                        if (stop_token.stop_requested()) {
                            throw std::runtime_error("API request cancelled during retry wait");
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                    wait_time = std::min(
                        static_cast<int>(wait_time * server_error_settings.backoff_multiplier),
                        server_error_settings.max_wait_seconds
                    );
                    last_attempt_was_transport = false;  // reset for next attempt
                }
            }

            if (!last_body.empty()) return last_body;
            throw std::runtime_error("API request failed after " +
                                     std::to_string(max_retries) + " attempts");
        });
}

// §NC-8 (2026-07-16): make_http_request_with_retry rewritten on
// OmniNet::HTTP::Client. The CURL* parameter is dropped — callers
// no longer need to manage a curl handle.
//
// Retry policy is preserved:
//   - 2xx → return body
//   - 401 → log + return body (no retry, auth won't fix itself)
//   - 4xx (other) → log + return body (no retry, client error)
//   - 5xx → log + exponential backoff via server_error_settings
//   - Transport error → log + exponential backoff (if enabled)
std::string LLMClient::make_http_request_with_retry(
    const std::string& url, const std::string& payload, int max_retries)
{
    std::string response_buffer;

    // Get server error settings for configurable retry behavior
    daedalus::ServerErrorSettings server_error_settings;
    try {
        if (settings_store_) {
            server_error_settings = settings_store_->get_server_error_settings();
            max_retries = server_error_settings.max_retries_per_call;
        }
    } catch (...) {
        // Use defaults if settings not available
        server_error_settings.enabled = true;
        server_error_settings.initial_wait_seconds = 30;
        server_error_settings.max_wait_seconds = 300;
        server_error_settings.backoff_multiplier = 2.0;
        server_error_settings.max_retries_per_call = 3;
    }

    int attempt = 0;
    int wait_time = server_error_settings.initial_wait_seconds;

    while (attempt < max_retries) {
        attempt++;

        response_buffer.clear();

        // Build the OmniNet request.
        OmniNet::HTTP::ClientRequest req;
        req.url(url);
        req.method("POST");
        req.body(payload);
        req.header("Content-Type", "application/json");
        req.header("Accept",       "application/json");
        if (!config_.api_key.empty()) {
            req.header("Authorization",
                       std::string("Authorization: Bearer ") + config_.api_key);
        }

        OmniNet::HTTP::ClientConfig cfg;
        cfg.connect_timeout = std::chrono::seconds(10);
        cfg.request_timeout = std::chrono::seconds(900);  // 15 minutes
        cfg.follow_redirects = true;
        cfg.verify_tls       = true;

        // Execute synchronously (this is the sync fallback path used
        // when SSE parsing fails).
        auto result = OmniNet::HTTP::Client::get_instance().send_blocking(req, cfg);

        if (result.has_value()) {
            const auto& resp = result.value();
            response_buffer = resp.bodyAsString();
            // Enforce response size cap (matches old WriteBuffer::max_size).
            if (response_buffer.size() > max_response_size_) {
                response_buffer.resize(max_response_size_);
                std::ostringstream trunc;
                trunc << "Response was truncated to " << max_response_size_ << " bytes";
                if (sink_) sink_->on_log(daedalus::LogLevel::Warn, trunc.str());
            }

            const long code = resp.statusCode();
            if (code >= 200 && code < 300) {
                break; // Success
            } else if (code == 401) {
                // Don't retry on authentication errors - they won't succeed
                if (sink_) {
                    std::ostringstream e401;
                    e401 << "HTTP " << code
                         << " (authentication failed, not retrying)\n"
                         << "Response body: " << response_buffer;
                    sink_->on_log(daedalus::LogLevel::Error, e401.str());
                } else {
                    LOG_ERROR("HTTP error " + std::to_string(code)
                              + " (authentication failed, not retrying). "
                              + "Response body: " + response_buffer);
                }
                break;
            } else if (code >= 400 && code < 500) {
                // Client error - no point retrying
                if (sink_) {
                    std::ostringstream e4xx;
                    e4xx << "HTTP " << code
                         << " (client error, not retrying)\n"
                         << "Response body: " << response_buffer;
                    sink_->on_log(daedalus::LogLevel::Error, e4xx.str());
                } else {
                    LOG_ERROR("HTTP error " + std::to_string(code)
                              + " (client error, not retrying). "
                              + "Response body: " + response_buffer);
                }
                break;
            } else if (code >= 500) {
                // Server error - retry with exponential backoff if enabled
                if (sink_) {
                    std::ostringstream e5xx;
                    e5xx << "HTTP " << code << " (server error";
                    if (server_error_settings.enabled) {
                        e5xx << ", retrying in " << wait_time << "s";
                    }
                    e5xx << ")\nResponse body (first 500 chars): "
                          << response_buffer.substr(0, 500);
                    sink_->on_log(daedalus::LogLevel::Warn, e5xx.str());
                } else {
                    std::ostringstream msg;
                    msg << "HTTP " << code << " (server error";
                    if (server_error_settings.enabled) {
                        msg << ", retrying in " << wait_time << "s";
                    }
                    msg << "). Response body (first 500 chars): "
                        << response_buffer.substr(0, 500);
                    LOG_WARNING(msg.str());
                }
            }
        } else {
            // Transport-level failure (OmniNet::HTTP::ClientError).
            const char* err_name = OmniNet::HTTP::clientErrorName(result.error());
            if (sink_) {
                std::ostringstream msg;
                msg << "transport error on attempt " << attempt
                    << ": " << err_name;
                if (server_error_settings.enabled) {
                    msg << " (retrying in " << wait_time << "s)";
                }
                sink_->on_log(daedalus::LogLevel::Error, msg.str());
            } else {
                std::ostringstream msg;
                msg << "transport error on attempt " << attempt
                    << ": " << err_name;
                if (server_error_settings.enabled) {
                    msg << " (retrying in " << wait_time << "s)";
                }
                LOG_ERROR(msg.str());
            }
        }

        // If we're not done yet, wait before retrying with exponential backoff
        if (attempt < max_retries && server_error_settings.enabled) {
            std::ostringstream wait;
            wait << "Waiting " << wait_time << " seconds before retry...";
            if (sink_) sink_->on_log(daedalus::LogLevel::Info, wait.str());
            std::this_thread::sleep_for(std::chrono::seconds(wait_time));
            // Exponential backoff: multiply by backoff_multiplier, cap at max_wait
            wait_time = std::min(static_cast<int>(wait_time * server_error_settings.backoff_multiplier),
                                 server_error_settings.max_wait_seconds);
        }
    }

    if (attempt == max_retries && !response_buffer.empty()) {
        // We exhausted retries but still got a response, return it
        return response_buffer;
    } else if (attempt == max_retries) {
        throw std::runtime_error("API request failed after "
                                 + std::to_string(max_retries) + " attempts");
    }

    // Extract and store token usage (via shared helper)
    try {
        update_token_usage(json::parse(response_buffer));
    } catch (...) {
        // Ignore parsing errors - usage is best-effort
    }

    return response_buffer;
}



// Extract and store token usage from a parsed LLM response.
// This MUST be called after every successful API response so the
// last_total_tokens_ / last_prompt_tokens_ / last_completion_tokens_
// members are up to date for:
//   1. The token usage bar in the status display
//   2. The 90% compression threshold check
//   3. The session memory stats
//   4. The metrics tracked by Metrics class
//
// Without this, the user sees "0/1000000 (0.0%)" even though the LLM
// is responding - because we never capture the usage field from responses.
void LLMClient::update_token_usage(const json& response_json) {
    // §Phase 9.10: MiniMax SSE API returns "usage": null on every chunk
    // (confirmed by /tmp/test_sse_raw.py). The non-streaming path gets real
    // counts. Log a warning so operators know why token counts are 0 during
    // SSE streaming — this is expected behaviour, not a bug.
    // §B32: suppress duplicate warnings per turn (multiple SSE chunks without usage).
    if (!response_json.contains("usage")) {
        if (!warned_no_usage_this_turn_) {
            if (sink_) sink_->on_log(daedalus::LogLevel::Warn,
                "[LLM] No 'usage' field in response (SSE stream? — token counts are 0)");
            warned_no_usage_this_turn_ = true;
        }
        return;
    }
    if (response_json["usage"].is_null()) {
        if (!warned_no_usage_this_turn_) {
            if (sink_) sink_->on_log(daedalus::LogLevel::Warn,
                "[LLM] 'usage' is null in SSE stream — token counts unavailable");
            warned_no_usage_this_turn_ = true;
        }
        return;
    }
    try {
        const auto& usage = response_json["usage"];
        last_prompt_tokens_ = usage.value("prompt_tokens", 0);
        last_completion_tokens_ = usage.value("completion_tokens", 0);
        last_total_tokens_ = usage.value("total_tokens", 0);
    } catch (...) {
        // Ignore parsing errors - usage is best-effort
    }
}

// ===================================================================
// §Phase 3 - MiniMax SSE streaming parser (file scope).
// Defined here so run_one_turn can instantiate it and pass a callback
// to make_api_request_async.
//
// MiniMax SSE line format:
//   data: [{"id":"...","choices":[{"delta":{"content":"..."},"index":0}]}]
//   data: [DONE]
// ===================================================================
class MiniMaxSseParser {
public:
    explicit MiniMaxSseParser(std::weak_ptr<daedalus::AgentSink> sink)
        : weak_sink_(std::move(sink)) {}

    /// Called for each raw HTTP chunk; SSE-lines are buffered and parsed.
    void on_chunk(std::string_view data) {
        buffer_.append(data.data(), data.size());
        size_t pos = 0;
        while ((pos = buffer_.find('\n', pos)) != std::string::npos) {
            std::string line = buffer_.substr(0, pos);
            buffer_.erase(0, pos + 1);
            pos = 0;
            parse_line(line);
        }
    }

    /// Flush any incomplete line after the stream ends.
    void finish() {
        if (!buffer_.empty()) {
            parse_line(buffer_);
            buffer_.clear();
        }
    }

    /// Returns all text accumulated from delta events. Used for display.
    const std::string& text() const noexcept { return accumulated_text_; }

    /// Returns the final SSE event as a JSON object. Contains the complete
    /// response metadata (id, model, usage, finish_reason) needed for
    /// token tracking and downstream JSON parsing.
    /// Returns a null json::object() if no valid event was received.
    const json& final_event() const noexcept { return final_event_; }

    /// Reconstruct the assistant's message in the standard non-streaming
    /// format.  When the server streams `delta`-shaped chunks, the
    /// downstream code that consumes `choices[0].message` breaks unless we
    /// synthesise the equivalent object from accumulated fragments:
    ///   {
    ///     "role": "assistant",
    ///     "content": "<accumulated text>",
    ///     "tool_calls": [<accumulated tool calls>],     // if any
    ///     "audio_content": ""                            // MiniMax quirk
    ///   }
    /// `final_event_` is *not* used here — it is the LAST SSE chunk, which
    /// carries the final `finish_reason` and `usage` but only the trailing
    /// `delta` text fragment.  All reassembly has to come from the deltas
    /// we buffered as they arrived.
    json synthesized_message() const {
        json m;
        m["role"] = accumulated_role_.empty() ? "assistant" : accumulated_role_;
        m["content"] = accumulated_text_;
        m["audio_content"] = "";
        if (!accumulated_tool_calls_.empty()) {
            m["tool_calls"] = accumulated_tool_calls_;
        }
        if (!accumulated_finish_reason_.empty()) {
            m["finish_reason"] = accumulated_finish_reason_;
        }
        return m;
    }

    /// §Phase 9.11: Did the SSE stream emit any text via `on_assistant_delta`
    /// during this turn?  Used by the downstream pipeline to decide whether
    /// to re-emit the synthesized message's content (which would duplicate
    /// every fragment the user already saw streaming live).
    ///   - Returns `true` if at least one text delta was fired.
    ///   - Returns `false` if the stream had no text content (tool-only
    ///     response, or empty response, or non-streaming path).
    bool had_text_deltas() const noexcept { return !accumulated_text_.empty(); }

private:
    void parse_line(const std::string& line) {
        constexpr std::string_view PREFIX = "data: ";
        if (line.rfind(PREFIX, 0) != 0) return;
        std::string json_str = line.substr(PREFIX.size());
        if (json_str == "[DONE]" || json_str == "DONE") return;
        try {
            json parsed = json::parse(json_str);

            // Normalise into a list of top-level objects.
            // MiniMax streams: `data: {"id":"...","choices":[{...delta...}]}` (one object).
            // Some OpenAI-compatible endpoints stream: `data: [{...}]` (array of one).
            // We must support both.  If `parsed` is already an array, iterate it.
            // If it's an object, treat it as a one-element list.
            // If neither (string, number, etc.), skip silently.
            std::vector<json> items;
            if (parsed.is_array()) {
                for (const auto& el : parsed) {
                    if (el.is_object()) items.push_back(el);
                }
            } else if (parsed.is_object()) {
                items.push_back(parsed);
            }
            // Anything else (string, primitive) -- skip.

            for (const auto& item : items) {
                // Guard: item must be a JSON object (not an array or primitive).
                if (!item.is_object()) continue;

                // Capture the complete item for final_event_ -- it carries the
                // final `usage` (token counts) and `id`/`model` metadata that
                // aren't present on every chunk.
                final_event_ = item;

                for (const auto& choice : item.value("choices", json::array())) {
                    const auto& delta = choice.value("delta", json::object());

                    // Role (typically "assistant"; MiniMax also puts name here).
                    if (delta.contains("role") && delta["role"].is_string()) {
                        accumulated_role_ = delta["role"].get<std::string>();
                    }

                    // Text content streaming — the basic case.
                    const json& content = delta.value("content", json());
                    if (content.is_string() && !content.get<std::string>().empty()) {
                        const std::string& s = content.get_ref<const std::string&>();
                        accumulated_text_ += s;
                        if (auto w = weak_sink_.lock()) w->on_assistant_delta(s);
                    } else if (content.is_array()) {
                        // MiniMax sometimes sends content as an array of parts.
                        for (const auto& part : content) {
                            if (part.is_string()) {
                                const std::string& s = part.get_ref<const std::string&>();
                                accumulated_text_ += s;
                                if (auto w = weak_sink_.lock()) w->on_assistant_delta(s);
                            } else if (part.is_object() && part.value("type", "") == "text") {
                                const std::string& txt = part.value("text", std::string{});
                                if (!txt.empty()) {
                                    accumulated_text_ += txt;
                                    if (auto w = weak_sink_.lock()) w->on_assistant_delta(txt);
                                }
                            }
                        }
                    }

                    // Tool calls — the part Phase 9.8 missed.
                    // MiniMax streams the whole tool_call object in one chunk
                    // (id, name, complete arguments JSON string).
                    // OpenAI spec streams arguments char-by-char; we handle
                    // both by keying on `index` for accumulation.
                    if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                        for (const auto& tc : delta["tool_calls"]) {
                            if (!tc.is_object()) continue;
                            int tc_index = tc.value("index", static_cast<int>(accumulated_tool_calls_.size()));
                            // Grow the list to fit this index.
                            while (static_cast<int>(accumulated_tool_calls_.size()) <= tc_index) {
                                accumulated_tool_calls_.push_back(json::object());
                            }
                            auto& slot = accumulated_tool_calls_[tc_index];
                            // Copy known fields onto the slot.  Streaming
                            // arguments arrive as fragments -- always concatenate
                            // strings rather than overwriting.
                            if (tc.contains("id") && tc["id"].is_string()) {
                                slot["id"] = tc["id"];
                            } else if (!slot.contains("id")) {
                                slot["id"] = "";
                            }
                            if (tc.contains("type") && tc["type"].is_string()) {
                                slot["type"] = tc["type"];
                            } else if (!slot.contains("type")) {
                                slot["type"] = "function";
                            }
                            if (tc.contains("function") && tc["function"].is_object()) {
                                if (!slot.contains("function")) slot["function"] = json::object();
                                if (tc["function"].contains("name") && tc["function"]["name"].is_string()) {
                                    slot["function"]["name"] = tc["function"]["name"];
                                } else if (!slot["function"].contains("name")) {
                                    slot["function"]["name"] = "";
                                }
                                if (tc["function"].contains("arguments")) {
                                    if (tc["function"]["arguments"].is_string()) {
                                        // Streaming fragment -- append.
                                        std::string prev = slot["function"].value("arguments", std::string{});
                                        slot["function"]["arguments"] = prev + tc["function"]["arguments"].get<std::string>();
                                    } else if (tc["function"]["arguments"].is_object()) {
                                        // Already-parsed object (rare).  Keep as JSON string.
                                        slot["function"]["arguments"] = tc["function"]["arguments"].dump();
                                    }
                                } else if (!slot["function"].contains("arguments")) {
                                    slot["function"]["arguments"] = "";
                                }
                            } else if (!slot.contains("function")) {
                                slot["function"] = json::object();
                            }
                        }
                    }

                    // finish_reason comes on the LAST chunk for tool_calls /
                    // stop decisions. Capture it once.
                    if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
                        if (choice["finish_reason"].is_string()) {
                            accumulated_finish_reason_ = choice["finish_reason"].get<std::string>();
                        }
                    }
                }
            }
        } catch (const json::parse_error&) {
            // Malformed SSE line -- skip silently
        }
    }

    std::weak_ptr<daedalus::AgentSink> weak_sink_;
    std::string buffer_;
    std::string accumulated_text_;           /// Complete assistant text from deltas.
    std::string accumulated_role_;           /// "assistant" usually; set from delta.role.
    std::string accumulated_finish_reason_;  /// "stop", "tool_calls", "length" — set once.
    json accumulated_tool_calls_ = json::array();  /// Rebuilt by tool_calls streaming.
    json final_event_;                       /// Last SSE event with usage/finish_reason.
};

// // Helper method to get state name as string
std::string LLMClient::get_state_name(AgentState state) const {
    switch (state) {
        case AgentState::UNDERSTAND: return "UNDERSTAND";
        case AgentState::PLANNING: return "PLANNING";
        case AgentState::ACT: return "ACT";
        case AgentState::BUILD_TEST: return "BUILD_TEST";
        case AgentState::DONE: return "DONE";
        case AgentState::CANCELLED: return "CANCELLED";
        default: return "UNKNOWN";
    }
}

// Helper method to extract error message from raw API response
std::string LLMClient::extract_error_message(const std::string& response) const {
    // Try to find JSON error object in the response
    try {
        json j = json::parse(response);
        if (j.contains("error") && j["error"].is_object()) {
            return j["error"].value("message", "Unknown error");
        }
    } catch (...) {
        // If JSON parsing fails, try to extract from raw text
        size_t msg_pos = response.find("\"message\":");
        if (msg_pos != std::string::npos) {
            size_t start = response.find("\"", msg_pos + 9);
            size_t end = response.find("\"", start + 1);
            if (start != std::string::npos && end != std::string::npos) {
                return response.substr(start + 1, end - start - 1);
            }
        }
    }
    return "";
}

// Run a single agent cycle (one LLM call + tool execution)
// §NC-8 (2026-07-16): CURL* parameter dropped. The sync fallback
// path now uses OmniNet::HTTP::Client internally.
AgentState LLMClient::run_agent_cycle(const std::string& api_url,
                                          json& messages, const json& tool_defs,
                                          bool& compress_after_done) {
    warned_no_usage_this_turn_ = false;  // §B32: reset per-turn

    // Build the request payload
    json request_payload = {
        {"model",     config_.model},
        {"messages",  messages},
        {"tools",     tool_defs},
        {"stream",    config_.stream},  // §Phase 9.8: true = SSE tokens, false = buffered JSON
        {"temperature", config_.temperature}
    };

    // §Phase 9.10: When streaming, always request usage stats via
    // `stream_options: {include_usage: true}`.  MiniMax (confirmed by live
    // test on 2026-07-11) sends a separate final chunk with
    // `{"choices":[],"usage":{"total_tokens":N,"prompt_tokens":M,
    // "completion_tokens":K}}` only when this flag is set.  Without it,
    // every chunk has `"usage": null` and token counts are 0 for the whole
    // session — making context-compression triggering impossible.
    // OpenAI added this in May 2024; MiniMax supports it identically.
    if (config_.stream) {
        request_payload["stream_options"] = {{"include_usage", true}};
    }
    
    // Validate JSON serialization before sending
    std::string payload_str;
    try {
        payload_str = request_payload.dump();
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to serialize request payload: " + std::string(e.what()));
        // Try to identify which message is problematic
        for (size_t i = 0; i < messages.size(); i++) {
            try {
                std::string msg_str = messages[i].dump();
            } catch (const std::exception& e2) {
                LOG_ERROR("Problematic message at index " + std::to_string(i) + ": " + e2.what());
                LOG_ERROR("Message content (first 200 chars): " + messages[i].dump().substr(0, 200));
            }
        }
        return AgentState::DONE;
    }
    // Verify payload can be parsed back (catch JSON corruption)
    try {
        json parsed_verify = json::parse(payload_str);
        if (!parsed_verify.contains("messages") || !parsed_verify["messages"].is_array()) {
            LOG_ERROR("JSON validation failed: messages array missing or invalid");
            return AgentState::DONE;
        }
        
        // Validate each tool_call's arguments are valid JSON
        auto& msgs = parsed_verify["messages"];
        for (size_t i = 0; i < msgs.size(); i++) {
            auto& msg = msgs[i];
            if (msg.value("role", "") == "assistant" && msg.contains("tool_calls")) {
                for (size_t j = 0; j < msg["tool_calls"].size(); j++) {
                    auto& tc = msg["tool_calls"][j];
                    std::string tc_id = tc.value("id", "MISSING");
                    
                    if (tc.contains("function") && tc["function"].contains("arguments")) {
                        std::string args_str = tc["function"]["arguments"];
                        try {
                            // This will throw if args_str is not valid JSON
                            json args_parsed = json::parse(args_str);
                        } catch (const json::parse_error& e) {
                            LOG_ERROR("INVALID arguments JSON at msg[" + std::to_string(i) + "][" + std::to_string(j) + "] id=" + tc_id);
                            LOG_ERROR("args_len=" + std::to_string(args_str.length()) + " chars");
                            LOG_ERROR("parse error at char " + std::to_string(e.byte) + ": " + e.what());
                            
                            // Show the last 100 chars to see what went wrong
                            std::string last_100 = args_str.length() > 100 ? args_str.substr(args_str.length() - 100) : args_str;
                            LOG_ERROR("last 100 chars: " + last_100);
                            
                            // Mark this tool_call as having invalid arguments
                            // We'll add an error tool result instead of executing it
                            tc["_invalid_args"] = true;
                            tc["_parse_error"] = e.what();
                        }
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR("JSON parse verification failed: " + std::string(e.what()));
        LOG_ERROR("Payload (first 500 chars): " + payload_str.substr(0, 500));
        return AgentState::DONE;
    }
    
    // Use Messages class to sanitize messages before API call
    // This validates tool_calls and removes broken ones, tracking state to prevent cascading errors
    // IMPORTANT: sanitize_message_chain returns a NEW json (by value), not a reference.
    // We must use set_messages to update Messages class, otherwise the local copy
    // shadows the reference and tool results never get synced back!
    json sanitized = messages_->sanitize_message_chain(messages);
    messages_->set_messages(sanitized);
    messages = messages_->get_messages();  // Re-get reference after update
    
    // Rebuild payload after sanitization
    request_payload["messages"] = messages;
    payload_str = request_payload.dump();
    
    // Check for tool_calls in messages and verify their IDs are valid strings
    for (size_t i = 0; i < messages.size(); i++) {
        const auto& msg = messages[i];
        if (msg.value("role", "") == "assistant" && msg.contains("tool_calls")) {
            for (const auto& tc : msg["tool_calls"]) {
                std::string tc_id = tc.value("id", "");
                if (tc_id.empty() || tc_id.find('\0') != std::string::npos) {
                    LOG_ERROR("Invalid tool_call_id at message " + std::to_string(i) + ": empty or contains null");
                    return AgentState::DONE;
                }
                // Check for control characters that might break JSON
                for (char c : tc_id) {
                    if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') {
                        LOG_ERROR("Invalid control char in tool_call_id at message " + std::to_string(i));
                        return AgentState::DONE;
                    }
                }
            }
        }
        if (msg.value("role", "") == "tool") {
            std::string tc_id = msg.value("tool_call_id", "");
            if (tc_id.empty() || tc_id.find('\0') != std::string::npos) {
                LOG_ERROR("Invalid tool_call_id in tool result at message " + std::to_string(i));
                return AgentState::DONE;
            }
        }
    }

    // Check if request payload exceeds max size and truncate if needed
    messages_->truncate_if_needed(max_request_size_);
    
    // Rebuild payload after truncation
    request_payload["messages"] = messages;

    // Reset cancellation flag

    // Make the API call with retry logic - run in thread for ESC cancellation support
    std::string response_body;

    // Make the API call asynchronously via HttpSession
    // Benefits: HTTP keep-alive, connection reuse, true async I/O,
    // §Phase 5.1: the cancel source is a member of LLMClient
    // (cancel_source_) so Agent::cancel() can fire it from any
    // thread. Polled below in the wait loop.
    std::stop_token cancel_token = cancel_source_.get_token();

    // §Phase 9.8: SSE streaming — only wired when config_.stream is true.
    // When false the existing buffered JSON path is used unchanged.
    MiniMaxSseParser sse_parser{std::weak_ptr<daedalus::AgentSink>(sink_)};
    std::function<void(std::string_view)> streaming_callback;
    if (config_.stream) {
        // Capture sse_parser by reference — it lives on run_agent_cycle's
        // stack and the async task fires its callbacks while we are still
        // in the wait loop below, so the reference is always valid.
        streaming_callback = [&sse_parser](std::string_view chunk) {
            sse_parser.on_chunk(chunk);
        };
    }
    auto api_future = make_api_request_async(
        api_url, request_payload.dump(), cancel_token, streaming_callback);

    // §Phase 5.1: wait for the API response. The host may call
    // Agent::cancel() (which fires cancel_source_) at any time; we
    // exit the wait loop on cancel_token.stop_requested(). Library
    // no longer reads stdin / sets raw mode / captures ESC bytes.
    while (true) {
        auto status = api_future.wait_for(std::chrono::milliseconds(100));
        if (cancel_token.stop_requested()) {
            std::ostringstream msg;
            msg << "\n[INTERRUPTED] Agent execution halted by user.";
            if (sink_) sink_->on_log(daedalus::LogLevel::Info, msg.str());
            messages = messages_->sanitize_message_chain(messages);
            messages_->clear_executing_markers();
            return AgentState::CANCELLED;
        }
        if (status != std::future_status::timeout) {
            break;
        }
    }

    // Flush any remaining SSE data from the buffer.
    sse_parser.finish();

    // Get the result from the API call.
    // With config_.stream=true, response_body may be empty (HttpSession
    // takes the streaming branch and doesn't accumulate when stream_callback
    // is set). In that case sse_parser.final_event() carries the data.
    // With config_.stream=false, response_body holds the full JSON and
    // sse_parser.final_event() is always null.
    const int max_retries = 3;
    bool parse_success = false;
    json response_json;

    // Load server_error_settings to gate the SSE retry loop (B27)
    daedalus::ServerErrorSettings server_error_settings;
    try {
        if (settings_store_) {
            server_error_settings = settings_store_->get_server_error_settings();
        }
    } catch (...) {
        server_error_settings.enabled = true;
    }

    for (int retry = 1; retry <= max_retries; retry++) {
        if (retry == 1 && !sse_parser.final_event().is_null()) {
            // First attempt: use the JSON from SSE streaming (has complete
            // usage + finish_reason from the final SSE event). Fall through
            // to token update and tool extraction below.
            response_json = sse_parser.final_event();
            parse_success = true;
            break;
        }

        // §Phase 9.8: if streaming was on but the SSE parser got no final
        // event (e.g. server doesn't support SSE, or the Accept header wasn't
        // honoured), fall back to a synchronous non-streaming request for this
        // retry instead of trying to json::parse an empty response_body.
        if (retry >= 2 && config_.stream && sse_parser.final_event().is_null()) {
            try {
                response_body = make_http_request_with_retry( api_url,
                    request_payload.dump());
            } catch (const std::exception& api_e) {
                std::ostringstream api_retry;
                api_retry << "API request also failed: " << api_e.what();
                if (sink_) sink_->on_log(daedalus::LogLevel::Error, api_retry.str());
            }
        }

        try {
            response_json = json::parse(response_body);
            parse_success = true;
            break;  // Success, continue processing
        } catch (const json::parse_error& e) {
            if (!server_error_settings.enabled) {
                // User disabled server-error retry via settings store (B27)
                break;
            }
            if (retry < max_retries) {
                int delay = 1 << (retry - 1);  // 1s, 2s, 4s...
                std::ostringstream retry_msg;
                retry_msg << "JSON parse failed (attempt " << retry << "/" << max_retries
                          << "), waiting " << delay << "s...";
                if (sink_) sink_->on_log(daedalus::LogLevel::Warn, retry_msg.str());
                std::this_thread::sleep_for(std::chrono::seconds(delay));

                // Try to get a fresh response from the API (non-streaming fallback)
                try {
                    response_body = make_http_request_with_retry( api_url,
                        request_payload.dump());
                } catch (const std::exception& api_e) {
                    std::ostringstream api_retry;
                    api_retry << "API request also failed: " << api_e.what();
                    if (sink_) sink_->on_log(daedalus::LogLevel::Error, api_retry.str());
                }
            } else {
                // All retries exhausted
                std::string error_msg = extract_error_message(response_body);
                if (!error_msg.empty()) {
                    if (sink_) sink_->on_log(daedalus::LogLevel::Error, error_msg);
                    LOG_ERROR("JSON parse error with API message: " + error_msg);
                } else {
                    std::ostringstream parse_err;
                    parse_err << "Failed to parse JSON response after " << max_retries
                              << " attempts: " << e.what();
                    if (sink_) sink_->on_log(daedalus::LogLevel::Error, parse_err.str());
                    LOG_ERROR("Response was: " + response_body.substr(0, 500));
                    LOG_ERROR("JSON parse failed: " + std::string(e.what()));
                }
                return AgentState::DONE;
            }
        }
    }
    
    if (!parse_success) {
        return AgentState::DONE;
    }

    // Track token usage from this response (for stats display + compression threshold)
    update_token_usage(response_json);

    // Check for API errors
    if (response_json.contains("error")) {
        auto& error = response_json["error"];
        std::string error_message = error.value("message", "Unknown error");
        std::string error_type = error.value("type", "unknown");

        // Remove provider-specific text from the error message (URLs, help text)
        // Find the first period followed by space, or " at ", or " You " and cut there
        size_t cut_pos = error_message.find(". You ");
        if (cut_pos != std::string::npos) {
            error_message = error_message.substr(0, cut_pos);
        } else {
            cut_pos = error_message.find(" at ");
            if (cut_pos != std::string::npos) {
                error_message = error_message.substr(0, cut_pos);
            }
        }

        // §B9: extract the "find last tool_call" walk once and reuse it.
        LastToolCall last = find_last_tool_call(messages, /*want_args=*/true);
        const std::string& last_tool_call_id = last.id;
        const std::string& last_tool_name   = last.name;
        const std::string& last_tool_args   = last.args;

        // Classify error and decide whether to retry
        ErrorCategory category = classify_api_error(error_type, error_message);
        
        if (category == ErrorCategory::FATAL) {
            // Fatal error - add error message to conversation and continue
            {
            std::ostringstream api_err;
            api_err << error_type << ": " << error_message;
            if (sink_) sink_->on_log(daedalus::LogLevel::Error, api_err.str());
        }
            LOG_DEBUG("Last tool call: id=" + last_tool_call_id + " name=" + last_tool_name);
            LOG_DEBUG("Args: " + last_tool_args);
            LOG_ERROR("API error [FATAL, " + error_type + "]: " + error_message + " | Tool: " + last_tool_name + " (id=" + last_tool_call_id + ")");

            // CRITICAL FIX: Add a tool result for the failed call so the
            // conversation state stays valid. Without this, the assistant's
            // tool_call is orphaned (no matching tool result) and the LLM
            // gets stuck retrying the same call in an infinite loop.
            if (!last_tool_call_id.empty() && last_tool_call_id != "UNKNOWN") {
                messages.push_back({
                    {"role", "tool"},
                    {"tool_call_id", last_tool_call_id},
                    {"content", "[API ERROR] This tool call was REJECTED by the API server with: " + error_type + " - " + error_message + ".\n\nThe arguments you provided were:\n" + last_tool_args + "\n\nYou MUST try a DIFFERENT approach - do NOT retry this exact call. Read the tool description and the error carefully, then use different arguments or a different tool."}
                });
            }

            // Also add a user message for additional context
            messages.push_back({
                {"role", "user"},
                {"content", "[SYSTEM ERROR] Your last tool call (" + last_tool_name + ") was rejected by the API: " + error_message + ". Please try a different approach - do NOT repeat the same call."}
            });

            // Return ACT state to continue the agent loop
            return AgentState::ACT;
        }
        
        // Transient error - retry with backoff
        std::ostringstream retry_msg2;
        retry_msg2 << error_type << ": " << error_message << " - will retry...";
        if (sink_) sink_->on_log(daedalus::LogLevel::Warn, retry_msg2.str());
        LOG_ERROR("API error [TRANSIENT, " + error_type + "]: " + error_message);
        
        bool retry_success = false;
        for (int retry = 1; retry <= max_retries; retry++) {
            int delay = 1 << (retry - 1);  // 1s, 2s, 4s...
            std::ostringstream wait_msg;
            wait_msg << "Waiting " << delay << "s before retry " << retry << "/" << max_retries << "...";
            if (sink_) sink_->on_log(daedalus::LogLevel::Info, wait_msg.str());
            std::this_thread::sleep_for(std::chrono::seconds(delay));
            
            try {
                response_body = make_http_request_with_retry( api_url, request_payload.dump());
                response_json = json::parse(response_body);
                
                // Check if this response is valid
                if (!response_json.contains("error") && 
                    response_json.contains("choices") && 
                    !response_json["choices"].empty()) {
                    LOG_INFO("API call succeeded on retry " + std::to_string(retry));
                    retry_success = true;
                    break;
                }
                
                // Check for error again
                if (response_json.contains("error")) {
                    auto& retry_error = response_json["error"];
                    std::string retry_error_type = retry_error.value("type", "unknown");
                    std::string retry_error_msg = retry_error.value("message", "Unknown error");
                    if (classify_api_error(retry_error_type, retry_error_msg) == ErrorCategory::FATAL) {
                        LOG_ERROR("Fatal error during retry: " + retry_error_type);
                        // §B9: reuse the shared helper.
                        LastToolCall last_retry = find_last_tool_call(messages);
                        const std::string& last_tool_id   = last_retry.id;
                        const std::string& last_tool_name = last_retry.name;
                        // CRITICAL FIX: Add a tool result for the failed call
                        // to keep conversation state valid (no orphan tool_calls)
                        if (!last_tool_id.empty() && last_tool_id != "UNKNOWN") {
                            messages.push_back({
                                {"role", "tool"},
                                {"tool_call_id", last_tool_id},
                                {"content", "[API ERROR] This tool call was REJECTED by the API server with: " + retry_error_type + " - " + retry_error_msg + ".\n\nYou MUST try a DIFFERENT approach - do NOT retry this exact call."}
                            });
                        }
                        messages.push_back({
                            {"role", "user"},
                            {"content", "[SYSTEM ERROR] API error: " + retry_error_type + " - " + retry_error_msg + "\n\nLast tool: " + last_tool_name + " (id=" + last_tool_id + ")\n\nPlease try a DIFFERENT approach - do NOT repeat the same call."}
                        });
                        return AgentState::ACT;
                    }
                    std::ostringstream persist_msg;
                    persist_msg << "Error persisted: " << retry_error_type;
                    if (sink_) sink_->on_log(daedalus::LogLevel::Error, persist_msg.str());
                }
            } catch (const std::exception& e) {
                std::ostringstream exc_msg;
                exc_msg << "Exception during retry: " << e.what();
                if (sink_) sink_->on_log(daedalus::LogLevel::Error, exc_msg.str());
            }
        }
        
        if (!retry_success) {
            std::ostringstream failed_msg;
            failed_msg << "All API retries failed. Adding error to conversation and continuing.";
            if (sink_) sink_->on_log(daedalus::LogLevel::Error, failed_msg.str());
            // CRITICAL FIX: Add a tool result for the failed call
            // to keep conversation state valid (no orphan tool_calls)
            if (!last_tool_call_id.empty() && last_tool_call_id != "UNKNOWN") {
                messages.push_back({
                    {"role", "tool"},
                    {"tool_call_id", last_tool_call_id},
                    {"content", "[API ERROR] All " + std::to_string(max_retries) + " retries failed.\n\nYou MUST try a DIFFERENT approach - do NOT retry this exact call."}
                });
            }
            messages.push_back({
                {"role", "user"},
                {"content", "[SYSTEM ERROR] API error after " + std::to_string(max_retries) + " retries. Please try a DIFFERENT approach - do NOT repeat the same tool call."}
            });
            // Return ACT state to continue the agent loop
            return AgentState::ACT;
        }
        // If retry_success is true, we have a valid response_json, continue processing
    }

    // Check for valid response
    // §Phase 9.10: when streaming with `stream_options: {include_usage: true}`,
    // MiniMax (and OpenAI) send a separate final SSE chunk whose ONLY payload
    // is the `usage` object — `choices` is `[]` and there is no `message`.
    // That IS a valid response: the assistant message has already been
    // accumulated by `MiniMaxSseParser` from the earlier delta chunks.  Skip
    // the "no choices" retry path in that case.
    if (!response_json.contains("choices") || response_json["choices"].empty()) {
        bool is_usage_only_chunk =
            config_.stream &&
            response_json.contains("usage") &&
            !response_json["usage"].is_null();
        if (is_usage_only_chunk) {
            // §Phase 9.10: usage-only final SSE chunk.  Build a synthetic
            // single-choice response so the rest of the pipeline (which
            // reads `response_json["choices"][0]`) continues to work.
            // `usage` data is preserved as-is for `update_token_usage`.
            json synth_message = sse_parser.synthesized_message();
            response_json["choices"] = json::array({
                {{"index", 0},
                 {"message", synth_message},
                 {"finish_reason", "stop"}}
            });
            if (sink_) sink_->on_log(daedalus::LogLevel::Info,
                "[LLM] Stream finished with usage-only final chunk "
                "(real token counts available)");
            // Fall through to message/tool-call processing below.
        }
        else
        {
        LOG_ERROR("No choices in response - will retry API call...");
        
        // Retry with exponential backoff
        bool api_success = false;
        
        for (int retry = 1; retry <= max_retries; retry++) {
            int delay = 1 << (retry - 1);  // 1s, 2s, 4s...
            std::ostringstream nochoices_msg;
            nochoices_msg << "No choices - Attempt " << retry << "/" << max_retries
                           << " (waiting " << delay << "s)...";
            if (sink_) sink_->on_log(daedalus::LogLevel::Warn, nochoices_msg.str());
            std::this_thread::sleep_for(std::chrono::seconds(delay));
            
            try {
                response_body = make_http_request_with_retry( api_url, request_payload.dump());
                response_json = json::parse(response_body);
                
                if (response_json.contains("choices") && !response_json["choices"].empty()) {
                    LOG_INFO("Got valid response on retry " + std::to_string(retry));
                    api_success = true;
                    break;
                }
                
                // Check if we got an error instead
                if (response_json.contains("error")) {
                    auto& retry_error = response_json["error"];
                    std::string retry_error_type = retry_error.value("type", "unknown");
                    std::string retry_error_msg = retry_error.value("message", "Unknown error");
                    std::ostringstream got_err_msg;
                    got_err_msg << "Got error: " << retry_error_type << ": " << retry_error_msg;
                    if (sink_) sink_->on_log(daedalus::LogLevel::Warn, got_err_msg.str());
                    
                    if (classify_api_error(retry_error_type) == ErrorCategory::FATAL) {
                        LOG_ERROR("Fatal error: " + retry_error_type);
                        // Find the last tool call for debugging
                        std::string last_tool_name = "UNKNOWN";
                        std::string last_tool_id = "UNKNOWN";
                        try {
                            for (int i = static_cast<int>(messages.size()) - 1; i >= 0; i--) {
                                if (!messages[i].is_object()) continue;
                                if (messages[i].value("role", "") != "assistant") continue;
                                if (!messages[i].contains("tool_calls") || !messages[i]["tool_calls"].is_array()) continue;
                                auto& tcs = messages[i]["tool_calls"];
                                if (tcs.empty() || !tcs[tcs.size()-1].is_object()) continue;
                                if (!tcs[tcs.size()-1].contains("function") || !tcs[tcs.size()-1]["function"].is_object()) continue;
                                last_tool_id = tcs[tcs.size()-1].value("id", "UNKNOWN");
                                last_tool_name = tcs[tcs.size()-1]["function"].value("name", "UNKNOWN");
                                break;
                            }
                        } catch (const std::exception& e) {
                            LOG_DEBUG(std::string("Error extracting tool call: ") + e.what());
                        }
                        // Add error message and RETURN immediately
                        messages.push_back({
                            {"role", "user"},
                            {"content", "[SYSTEM ERROR] No choices + got error: " + retry_error_type + " - " + retry_error_msg + "\n\nLast tool: " + last_tool_name + " (id=" + last_tool_id + ")\n\nPlease re-read the file and try again with corrected parameters."}
                        });
                        return AgentState::ACT;
                    }
                }
            } catch (const std::exception& e) {
                std::ostringstream exc2_msg;
                exc2_msg << "Exception during retry: " << e.what();
                if (sink_) sink_->on_log(daedalus::LogLevel::Error, exc2_msg.str());
            }
        }
        
        if (!api_success) {
            LOG_ERROR("All API retries failed. Adding error to conversation and continuing.");
            // CRITICAL FIX: Find the last tool_call_id and add a tool result
            // to keep conversation state valid (no orphan tool_calls)
            std::string orphan_tc_id = "UNKNOWN";
            try {
                for (int i = static_cast<int>(messages.size()) - 1; i >= 0; i--) {
                    if (!messages[i].is_object()) continue;
                    if (messages[i].value("role", "") != "assistant") continue;
                    if (!messages[i].contains("tool_calls") || !messages[i]["tool_calls"].is_array()) continue;
                    auto& tcs = messages[i]["tool_calls"];
                    if (tcs.empty() || !tcs[tcs.size()-1].is_object()) continue;
                    orphan_tc_id = tcs[tcs.size()-1].value("id", "UNKNOWN");
                    break;
                }
            } catch (...) {}
            if (!orphan_tc_id.empty() && orphan_tc_id != "UNKNOWN") {
                messages.push_back({
                    {"role", "tool"},
                    {"tool_call_id", orphan_tc_id},
                    {"content", "[API ERROR] All " + std::to_string(max_retries) + " retries failed - API returned empty response.\n\nYou MUST try a DIFFERENT approach - do NOT retry this exact call."}
                });
            }
            messages.push_back({
                {"role", "user"},
                {"content", "[SYSTEM ERROR] API returned empty response after " + std::to_string(max_retries) + " retries. Please try a DIFFERENT approach - do NOT repeat the same tool call."}
            });
            // Return ACT state to continue the agent loop
            return AgentState::ACT;
        }
        // If api_success is true, we have a valid response_json, continue processing
        }
    }

    json choice = response_json["choices"][0];
    json message = choice["message"];

    // §Phase 9.8: when we streamed via SSE the response_json is the LAST
    // SSE chunk, which only has `choices[0].delta`, NOT `choices[0].message`.
    // Reconstruct the standard non-streaming message from the SSE parser's
    // accumulated role/content/tool_calls/finish_reason fragments so the
    // tool-call processing below continues to work unchanged.
    if (!message.is_object() && config_.stream) {
        message = sse_parser.synthesized_message();
        // Wrap it back into a choice so the rest of the pipeline reads it
        // uniformly.  `response_json` itself keeps its full metadata
        // (usage, model, id, ...) for token tracking.
        response_json["choices"][0]["message"] = message;
    }

    // Add the assistant's message to history
    messages.push_back(message);

    // Check for tool calls
    if (message.contains("tool_calls")) {
        json tool_calls = message["tool_calls"];
        
        // NOTE: tool_calls are already in the message object that was pushed above,
        // and messages is a reference to messages_->get_messages(), so the tool_calls
        // are already properly stored. No need to call add_tool_calls() again.
        // The mark_tool_executing() call below tracks ephemeral runtime state only.

        // Process each tool call and prepare results
        json tool_results = json::array();

        for (const auto& tool_call : tool_calls) {
            std::string function_name = tool_call["function"]["name"].get<std::string>();
            std::string call_id = tool_call["id"].get<std::string>();
            
            // Mark as EXECUTING in Messages (ephemeral runtime state)
            messages_->mark_tool_executing(call_id);
            
            // OUTPUT: Print tool header BEFORE execution
            // Handle arguments - can be string (JSON) or object
            json args_for_display = json::object();
            std::string action_desc;
            
            if (tool_call["function"].contains("arguments")) {
                auto& args = tool_call["function"]["arguments"];
                if (args.is_object()) {
                    // Arguments is already an object
                    args_for_display = args;
                    // Extract first string value from args as action description
                    for (auto& [key, val] : args.items()) {
                        if (val.is_string() && !val.get<std::string>().empty()) {
                            action_desc = val.get<std::string>();
                            if (action_desc.length() > 50) {
                                action_desc = action_desc.substr(0, 47) + "...";
                            }
                            break;
                        }
                    }
                } else if (args.is_string()) {
                    // Arguments is a JSON string - parse it
                    try {
                        args_for_display = json::parse(args.get<std::string>());
                    } catch (...) {
                        // Failed to parse, keep empty object
                    }
                }
            }
            // §Phase 5.1: render the tool header as a string and
            // route it through the sink instead of std::cout.
            if (sink_) sink_->on_log(daedalus::LogLevel::Info,
                format_tool_header_before(function_name, action_desc, args_for_display));

            // §Phase 9.6: notify the host that a tool is about to run.
            // This was missing from the library — AgentSink::on_tool_call
            // was declared and overridden by every demo's sink, but the
            // library never invoked it. Hosts could only see tool
            // activity via on_log() breadcrumbs or by parsing the
            // transcript. See Agent.md §Phase 9.6.
            // §Phase D1: correlate tool_call/result/progress events via call_id.
            // Sinks that don't care (e.g. ConsoleAgentSink) override only
            // the 2-arg versions — the 3-arg defaults above discard the
            // id and forward. WebSseSink overrides 3-arg and uses the
            // id as the DOM key for its per-tool card.
            if (sink_) sink_->on_tool_call(function_name, call_id,
                                           args_for_display);

            // Check if this tool_call has invalid arguments (marked during validation)
            if (tool_call.contains("_invalid_args") && tool_call["_invalid_args"] == true) {
                LOG_ERROR("Skipping tool_call " + call_id + " due to invalid arguments");
                std::string parse_error = tool_call.value("_parse_error", "unknown parse error");
                // Add error tool result - state derives from message chain
                tool_results.push_back({
                    {"role", "tool"},
                    {"tool_call_id", call_id},
                    {"content", "[ERROR] Invalid arguments JSON: " + parse_error},
                    {"status", "error"}
                });
                continue;  // Skip execution, go to next tool call
            }

            // Execute tool once - no retries. If it fails, the LLM will fix it in the next response.
            json result;
            try {
                // Wire streaming progress through the sink. Tools that
                // opt into the 3-arg execute() overload (e.g. bash,
                // network, MCP) will call this between on_tool_call
                // and on_tool_result, giving hosts live feedback for
                // long-running operations.
                ToolProgressFn progress_cb = nullptr;
                if (sink_) {
                    progress_cb = [this, function_name, call_id](const json& p) {
                        sink_->on_tool_progress(function_name, call_id, p);
                    };
                }
                result = ToolHandler::execute_tool_call(
                    tool_call, *file_io_, progress_cb);
                result["tool_name"] = function_name;

                std::string status = result.value("status", "unknown");
                LOG_INFO("Tool executed: " + function_name + " | Status: " + status);

                // §Phase 9.6: notify the host of the tool's outcome.
                // Same shape hosts already consume via Agent::execute_tool().
                if (sink_) sink_->on_tool_result(function_name, call_id, result);

                // §State Machine: when the agent explicitly drives its
                // own state via the `set_status` tool, fire
                // on_state_change so the host UI / SSE stream see the
                // transition immediately (e.g. webui chip row updates).
                // Only fires when the host wired a sink — old behaviour
                // is preserved when no sink is attached.
                if (status == "set_state" && sink_ && result.contains("state")
                    && result["state"].is_string()) {
                    bool ok = false;
                    auto new_state = daedalus::agent_state_from(
                        result["state"].get<std::string>(), &ok);
                    if (ok) {
                        // §B23 fix: track previous and current public state so we
                        // can emit the correct on_state_change(prev, current) transition.
                        auto prev = previous_public_state_;
                        current_public_state_ = static_cast<int>(new_state);
                        previous_public_state_ = current_public_state_;
                        sink_->on_state_change(prev, current_public_state_);
                        LOG_INFO("State machine: set_status → "
                                 + std::string(daedalus::to_string(new_state)));
                    }
                }

                // Check if tool returned error status.
                // Tools return the message under either "message" (the
                // daedalus-wide convention) or "error" (a few older tools).
                // Check both so log lines are useful.
                if (status == "error") {
                    const std::string msg = result.value("message",
                                              result.value("error", "unknown error"));
                    LOG_ERROR("Tool " + function_name + " failed: " + msg);
                } else {
                    // Success (or non-error status like "compress_context")
                    if (sink_) sink_->on_log(daedalus::LogLevel::Info,
                        format_tool_result(result));
                }

                // OUTPUT: separator after result
                if (sink_) sink_->on_log(daedalus::LogLevel::Info,
                    format_tool_separator());

                // OUTPUT: token usage stats
                size_t msg_count = messages_ ? messages_->get_messages().size() : 0;
                if (sink_) sink_->on_log(daedalus::LogLevel::Info,
                    format_token_usage(last_total_tokens_, get_max_context_size_value(),
                                       msg_count, iteration_count_, max_iterations_));
                
            } catch (const std::exception& e) {
                result = json{{"status", "error"}, {"message", std::string(e.what())}};
                LOG_ERROR("Tool " + function_name + " exception: " + std::string(e.what()));
                LOG_ERROR("Tool execution failed [" + function_name + "]: " + std::string(e.what()));

                // §Phase 9.6: also notify on the exception path, so the
                // sink sees one on_tool_result per on_tool_call even
                // when the tool throws.
                if (sink_) {
                    result["tool_name"] = function_name;
                    sink_->on_tool_result(function_name, call_id, result);
                }
            }
            
            // Update Messages tool state based on execution result
            // In the single-source-of-truth architecture, we don't track state separately.
            // Tool state is derived from the message chain. The call to add_tool_result()
            // below adds the tool result message which will make get_tool_state() return
            // COMPLETED or FAILED accordingly.
            // Note: mark_tool_executing() was already called at line 1188 BEFORE execution.
            // The tool result is added to tool_results array below and then inserted
            // into messages. For errors, we prepend "Error:" to the content so
            // is_error_result() detects it when checking state.
            
            // Determine error status and build error prefix for content
            bool is_error = (result.value("status", "") == "error");
            std::string error_prefix;
            if (is_error) {
                std::string error_msg = result.value("message", "Unknown error");
                error_prefix = "Error: " + error_msg + "\n\n";
            }
            
            // Tool result will be added to tool_results array below with content
            // After tool completes, clear the EXECUTING marker (ephemeral state)
            messages_->clear_executing(call_id);

            // Add tool result to message history (truncate if too large)
            // Special handling for read_image: strip the large data_uri to save context space
            // The LLM already received the data_uri in this response and can include images in
            // its next message using ![alt](data:...) syntax
            json result_for_storage = result;
            if (function_name == "read_image" && result.contains("data_uri")) {
                // Replace the large data_uri with just a reference to save memory
                std::string data_uri = result["data_uri"].get<std::string>();
                size_t data_size = data_uri.length();
                result_for_storage["data_uri"] = "[IMAGE_DATA_REMOVED: " + result.value("path", "unknown") + 
                    " (" + std::to_string(data_size) + " bytes) - Use the path/reference in your next message to include this image]";
                result_for_storage["image_data_removed"] = true;
            }
            
            // Sanitize result content to ensure valid UTF-8 before JSON serialization
            std::string result_content;
            bool needs_sanitize = false;
            try {
                result_content = result_for_storage.dump();
                // Verify it's valid JSON by parsing it back
                try {
                    [[maybe_unused]] auto parsed = json::parse(result_content);
                } catch (const json::exception&) {
                    // If parsing fails, trigger sanitization
                    needs_sanitize = true;
                }
            } catch (...) {
                // Any exception means we need to sanitize
                needs_sanitize = true;
            }
            
            if (needs_sanitize) {
                // If JSON serialization fails, sanitize and retry
                if (sink_) sink_->on_log(daedalus::LogLevel::Warn,
                    std::string("Tool result contains invalid UTF-8, sanitizing..."));
                
                // Function to recursively sanitize ALL strings in JSON
                // (both KEYS and VALUES - keys can have invalid UTF-8 too!)
                std::function<void(json&)> sanitize_json = [&](json& j) {
                    if (j.is_object()) {
                        // Build new object with sanitized keys
                        json sanitized = json::object();
                        for (auto it = j.begin(); it != j.end(); ++it) {
                            std::string clean_key = sanitize_utf8(it.key());
                            if (it.value().is_string()) {
                                sanitized[clean_key] = sanitize_utf8(it.value().get<std::string>());
                            } else if (it.value().is_object() || it.value().is_array()) {
                                sanitize_json(it.value());
                                sanitized[clean_key] = it.value();
                            } else {
                                sanitized[clean_key] = it.value();
                            }
                        }
                        j = std::move(sanitized);
                    } else if (j.is_array()) {
                        for (auto& item : j) {
                            if (item.is_string()) {
                                item = sanitize_utf8(item.get<std::string>());
                            } else if (item.is_object() || item.is_array()) {
                                sanitize_json(item);
                            }
                        }
                    }
                };

                sanitize_json(result_for_storage);
                // Use error_handler_t::replace as a safety net for any UTF-8
                // bytes that might have slipped through (e.g. in non-string values)
                result_content = result_for_storage.dump(-1, ' ', false, json::error_handler_t::replace);
            }
            
            if (result_content.size() > max_tool_result_size_) {
                std::ostringstream trunc_msg;
                trunc_msg << "Tool result truncated from " << result_content.size()
                          << " to " << max_tool_result_size_ << " bytes";
                if (sink_) sink_->on_log(daedalus::LogLevel::Warn, trunc_msg.str());
                
                // Truncate middle: keep start and end, remove middle
                size_t total_size = result_content.size();
                size_t max_size = max_tool_result_size_;
                
                // Reserve space for the truncation message
                const size_t truncation_msg_size = 60;
                size_t keep_each = (max_size > truncation_msg_size) ? (max_size - truncation_msg_size) / 2 : max_size / 2;
                
                std::string start_part = result_content.substr(0, keep_each);
                std::string end_part;
                if (total_size > keep_each) {
                    end_part = result_content.substr(total_size - keep_each, keep_each);
                }
                
                std::ostringstream truncated;
                truncated << start_part << "\n\n[...] " << (total_size - 2 * keep_each) 
                          << " bytes removed [...]\n\n" << end_part;
                result_content = truncated.str();
            }
            tool_results.push_back({
                {"role", "tool"},
                {"tool_call_id", call_id},
                {"content", error_prefix + result_content},
                {"status", is_error ? "error" : "success"}
            });
            
            // DEBUG: Verify tool_result has correct tool_call_id

            // Check if this is a stop tool call - agent is done
            // Note: Summary is already printed by print_tool_result() above
            if (function_name == "stop") {
                messages.insert(messages.end(), tool_results.begin(), tool_results.end());
                return AgentState::DONE;
            }

            // Check if agent requested context compression
            // Set flag to compress at end of agent cycle, not immediately
            if (function_name == "compress_context" && result.value("status", "") == "compress_context") {
                // Set flag to compress after agent completes
                compress_after_done = true;
            }

        }

        // Add tool results to message history
        messages.insert(messages.end(), tool_results.begin(), tool_results.end());

        // Determine next state based on tool types used
        // If execute_bash was called, we just completed BUILD_TEST
        bool has_build_test = false;
        for (const auto& tool_call : tool_calls) {
            std::string fn = tool_call["function"]["name"].get<std::string>();
            if (fn == "execute_bash") {
                has_build_test = true;
                break;
            }
        }

        // Agent decides next state - continue in ACT or go to BUILD_TEST
        return has_build_test ? AgentState::BUILD_TEST : AgentState::ACT;
    }

    // If no tool calls, the agent provided a text response
    // This could mean it's done, answering a question, or thinking - display the response
    if (message.contains("content")) {
        std::string content = message["content"].get<std::string>();
        if (!content.empty()) {
            // Remove thinking tags like <answer> or ```answer blocks if present
            size_t think_start = content.find("<answer>");
            if (think_start != std::string::npos) {
                size_t think_end = content.find("</answer>");
                if (think_end != std::string::npos) {
                    content = content.substr(think_start + 8, think_end - think_start - 8);
                }
            }
            
            // Also handle ```answer markdown blocks
            size_t md_start = content.find("```answer");
            if (md_start != std::string::npos) {
                size_t md_end = content.find("```", md_start + 8);
                if (md_end != std::string::npos) {
                    content = content.substr(md_start + 8, md_end - md_start - 8);
                }
            }

            // Final text response — emit through sink as a single delta
            // so the host can display it consistently with streamed output.
            // §Phase 5.1: no sink attached is a programmer error —
            // the library will never reach for std::cout. The host
            // must wire a sink to receive assistant output.
            // §Phase 9.11: skip the emit if the SSE stream already
            // delivered the text via deltas (otherwise the host sees
            // every fragment twice: once live-streamed, once here).
            if (sink_ && !sse_parser.had_text_deltas()) {
                sink_->on_assistant_delta("\n" + content + "\n");
            } else if (sink_) {
                // Already streamed live; nothing to re-emit.  Note this
                // in the sink log so operators can confirm dedup behaviour.
                // (Silent success is fine — this is the expected path.)
            } else {
                LOG_WARNING("Assistant produced content but no AgentSink "
                             "is attached. Provide a sink to receive output.");
            }
        }
    }

    // When agent just responds with text (no tools), task is complete
    // The user will be prompted to continue or quit when DONE state is handled
    return AgentState::DONE;
}

void LLMClient::display_token_stats(int message_count) const {
    // Calculate token usage percentage
    float token_percent = 0.0f;
    bool at_threshold = false;
    if (get_max_context_size_value() > 0) {
        token_percent = (static_cast<float>(last_total_tokens_) / get_max_context_size_value()) * 100.0f;
        at_threshold = messages_ && messages_->should_compress(last_total_tokens_);
    }
    
    // NAG the user if at 90% threshold - system handles compression automatically
    if (at_threshold) {
        {

            std::ostringstream msg;

            msg << "\n🚨 **Context at 90% threshold** - SYSTEM will auto-compress\n"

                << "   No action needed - system handles compression automatically\n\n";

            LOG_INFO(msg.str());

        }
    }
    
    // Calculate iteration percentage
    float iter_percent = 0.0f;
    if (max_iterations_ > 0) {
        iter_percent = (static_cast<float>(iteration_count_) / max_iterations_) * 100.0f;
    }
    
    // Generate progress bar (30 characters)
    const int bar_width = 30;
    int filled = static_cast<int>((token_percent / 100.0f) * bar_width);
    if (filled > bar_width) filled = bar_width;
    if (filled < 0) filled = 0;
    
    std::ostringstream bar;
    bar << "[";
    for (int i = 0; i < bar_width; i++) {
        if (i < filled) {
            bar << "█";
        } else {
            bar << "░";
        }
    }
    bar << "]";
    
    // §Phase 5.1: render via ostringstream + LOG_INFO.
    std::ostringstream msg;
    msg << "\n  📈 Token Usage: " << last_total_tokens_ << "/" << get_max_context_size_value()
        << " (" << std::fixed << std::setprecision(1) << token_percent << "%)\n"
        << "  " << bar.str() << "\n"
        << "  💬 Messages: " << message_count << "\n"
        << "  🔄 Iterations: " << iteration_count_ << "/" << max_iterations_
        << " (" << std::fixed << std::setprecision(1) << iter_percent << "%)\n";
    LOG_INFO(msg.str());
}

void LLMClient::display_subagent_stats(int message_count) const {
    // Simple single-line output for sub-agents (one-shot execution)
    float token_percent = 0.0f;
    bool at_threshold = false;
    if (get_max_context_size_value() > 0) {
        token_percent = (static_cast<float>(last_total_tokens_) / get_max_context_size_value()) * 100.0f;
        at_threshold = messages_ && messages_->should_compress(last_total_tokens_);
    }
    
    // NAG the user if at 90% threshold - system handles compression automatically
    if (at_threshold) {
        {

            std::ostringstream msg;

            msg << "\n🚨 **Context at 90% threshold** - SYSTEM will auto-compress\n"

                << "   No action needed - system handles compression automatically\n\n";

            LOG_INFO(msg.str());

        }
    }
    
    std::ostringstream msg;
    msg << "\n  [SubAgent] Token: " << last_total_tokens_ << "/" << get_max_context_size_value()
        << " (" << std::fixed << std::setprecision(1) << token_percent << "%)"
        << " | Messages: " << message_count << "\n";
    LOG_INFO(msg.str());
}

void LLMClient::display_manager_stats(int message_count) const {
    // Calculate token usage percentage
    float token_percent = 0.0f;
    bool at_threshold = false;
    if (get_max_context_size_value() > 0) {
        token_percent = (static_cast<float>(last_total_tokens_) / get_max_context_size_value()) * 100.0f;
        at_threshold = messages_ && messages_->should_compress(last_total_tokens_);
    }
    
    // NAG the user if at 90% threshold - system handles compression automatically
    if (at_threshold) {
        {

            std::ostringstream msg;

            msg << "\n🚨 **Context at 90% threshold** - SYSTEM will auto-compress\n"

                << "   No action needed - system handles compression automatically\n\n";

            LOG_INFO(msg.str());

        }
    }
    
    // Calculate iteration percentage
    float iter_percent = 0.0f;
    if (max_iterations_ > 0) {
        iter_percent = (static_cast<float>(iteration_count_) / max_iterations_) * 100.0f;
    }
    
    // Generate progress bar (30 characters)
    const int bar_width = 30;
    int filled = static_cast<int>((token_percent / 100.0f) * bar_width);
    if (filled > bar_width) filled = bar_width;
    if (filled < 0) filled = 0;
    
    std::ostringstream bar;
    bar << "[";
    for (int i = 0; i < bar_width; i++) {
        if (i < filled) {
            bar << "█";
        } else {
            bar << "░";
        }
    }
    bar << "]";
    
    // Print the stats line
    // §Phase 5.1: route via Logger instead of std::cout.
    std::ostringstream msg;
    msg << "\n  [Manager] Token Usage: " << last_total_tokens_ << "/" << get_max_context_size_value()
        << " (" << std::fixed << std::setprecision(1) << token_percent << "%)\n"
        << "  " << bar.str() << "\n"
        << "  Messages: " << message_count << "\n"
        << "  Iterations: " << iteration_count_ << "/" << max_iterations_
        << " (" << std::fixed << std::setprecision(1) << iter_percent << "%)\n";
    LOG_INFO(msg.str());
}

void LLMClient::inject_context_stats(json& messages) {
    // Build context statistics string
    std::ostringstream stats;

    // Token usage
    float token_percent = 0.0f;
    bool at_threshold = false;
    if (get_max_context_size_value() > 0) {
        token_percent = (static_cast<float>(last_total_tokens_) / get_max_context_size_value()) * 100.0f;
        at_threshold = messages_ && messages_->should_compress(last_total_tokens_);
    }

    // Add urgent warning if at 90% threshold - system handles compression automatically
    if (at_threshold) {
        stats << "\n🚨 **Context at 90% threshold** - SYSTEM will auto-compress\n\n";
        stats << "**No action needed** - system handles compression automatically\n";
        stats << "- Do NOT call any compression tool\n";
        stats << "- Just continue your work - system will compress when ready\n\n";
    }

    stats << "# Context Status\n";
    stats << "<!-- This section is auto-updated each iteration -->\n";
    stats << "| Metric | Value |\n";
    stats << "|--------|-------|\n";
    stats << "| Token Usage | " << last_total_tokens_ << "/" << get_max_context_size_value()
          << " (" << std::fixed << std::setprecision(1) << token_percent << "%) |\n";
    stats << "| Messages | " << messages.size() << " |\n";
    stats << "| Iterations | " << iteration_count_ << "/" << max_iterations_ << " |\n";

    // Compression status
    if (messages_) {
        float compress_ratio = messages_->get_compression_ratio(last_total_tokens_);
        stats << "| Context Pressure | " << std::fixed << std::setprecision(1) << (compress_ratio * 100) << "% |\n";
    }

    // Session memory stats
    if (session_memory_) {
        auto mem_stats = session_memory_->get_stats();
        stats << "| Session Memory | " << mem_stats.resolved_errors << "/" << mem_stats.total_errors << " errors resolved |\n";
        stats << "| | " << mem_stats.total_builds << " builds, " << mem_stats.total_sessions << " sessions |\n";
    }

    // Active skills
    SkillManager* skill_manager = SkillManager::get_instance();
    if (skill_manager) {
        auto active = skill_manager->get_active_skills();
        stats << "| Active Skills | " << active.size() << " |\n";
        if (!active.empty()) {
            stats << "| | ";
            for (size_t i = 0; i < active.size() && i < 5; i++) {
                if (i > 0) stats << ", ";
                stats << active[i].metadata.name;
            }
            if (active.size() > 5) stats << " (+" << (active.size() - 5) << " more)";
            stats << " |\n";
        }
    }

    // §B8 fix: terminator sentinel. The block always ends with this
    // unique line so we can find and erase exactly what we wrote
    // instead of walking forward to the next same-level heading
    // (which over-truncated the trailing tail of the system prompt
    // when the stats block was the LAST section).
    constexpr const char* kEndSentinel = "<!-- /daedalus-context-status -->\n";
    stats << kEndSentinel;

    std::string context_stats = stats.str();

    // Update or add the context stats to the system message
    if (messages.empty()) {
        return;  // No messages yet
    }

    json& system_msg = messages[0];
    if (system_msg.value("role", "") != "system") {
        return;  // First message is not system
    }

    std::string content = system_msg.value("content", "");

    // §B8 fix: locate the previous stats block by its sentinel and
    // erase exactly that block (from `# Context Status` to the
    // sentinel, inclusive). If the sentinel is missing — e.g. a
    // leftover from an earlier pre-fix write — fall back to the old
    // heading-based search so we don't leak the stats forever.
    size_t context_start = content.find("# Context Status");
    bool is_h2 = false;
    if (context_start == std::string::npos) {
        context_start = content.find("## Context Status");
        is_h2 = true;
    }
    if (context_start != std::string::npos) {
        // Try the new sentinel first (the precise block we wrote).
        size_t sentinel_pos = content.find(kEndSentinel, context_start);
        size_t context_end;
        if (sentinel_pos != std::string::npos) {
            // Sentinel found — erase inclusive of it.
            context_end = sentinel_pos + std::strlen(kEndSentinel);
        } else {
            // Fallback: pre-fix compatibility. Find the next same-level
            // heading or the next blank line, but never erase past
            // the end of the content (the previous code did that
            // and dropped the trailing tail of the prompt).
            std::string end_marker = is_h2 ? "\n## " : "\n# ";
            size_t next_heading = content.find(end_marker, context_start + 1);
            if (next_heading != std::string::npos) {
                context_end = next_heading;
            } else {
                // No same-level heading after. Erase to the next
                // blank line (the stats block always ends with one).
                // If none, just erase to end of string.
                size_t blank = content.find("\n\n", context_start);
                context_end = (blank != std::string::npos) ? blank + 2
                                                           : content.size();
            }
        }
        content.erase(context_start, context_end - context_start);
    }

    // Append the new context stats before the first "\n# " or at the end
    size_t first_heading = content.find("\n# ");
    if (first_heading != std::string::npos) {
        content.insert(first_heading, context_stats);
    } else {
        content += context_stats;
    }

    system_msg["content"] = content;
}

// Format: ![alt text](path_or_url)
// Returns a vector of pairs: (image_path_or_url, position in string)
static std::vector<std::pair<std::string, size_t>> find_image_references(const std::string& input) {
    std::vector<std::pair<std::string, size_t>> references;
    size_t pos = 0;
    while (true) {
        size_t start = input.find("![", pos);
        if (start == std::string::npos) break;
        size_t link_start = input.find("](", start);
        if (link_start == std::string::npos) break;
        size_t link_end = input.find(")", link_start + 2);
        if (link_end == std::string::npos) break;
        std::string url = input.substr(link_start + 2, link_end - link_start - 2);
        size_t url_start = url.find_first_not_of(" \t");
        size_t url_end = url.find_last_not_of(" \t");
        if (url_start != std::string::npos) {
            url = url.substr(url_start, url_end - url_start + 1);
            references.push_back({url, start});
        }
        pos = link_end + 1;
    }
    return references;
}

json LLMClient::build_multimodal_message(const std::string& user_input) {
    // Find image references in the input
    auto image_refs = find_image_references(user_input);
    
    // If no image references, return simple text message
    if (image_refs.empty()) {
        return json::object({
            {"role", "user"},
            {"content", user_input}
        });
    }
    
    // Build multi-modal content array
    json content = json::array();
    
    size_t current_pos = 0;
    
    for (const auto& ref : image_refs) {
        const std::string& image_path = ref.first;
        size_t image_pos = ref.second;
        
        // Add text before this image (if any)
        if (image_pos > current_pos) {
            std::string text_before = user_input.substr(current_pos, image_pos - current_pos);
            // Also remove the ![alt](url) syntax from text
            // Find the matching closing paren and remove the whole syntax
            size_t syntax_start = text_before.rfind("![");
            if (syntax_start != std::string::npos) {
                text_before = text_before.substr(0, syntax_start);
            }
            
            // Clean up whitespace
            text_before = trim(text_before);
            if (!text_before.empty()) {
                content.push_back({
                    {"type", "text"},
                    {"text", text_before}
                });
            }
        }
        
        // Process the image
        std::string data_uri;
        
        // Check if it's a URL
        if (image_path.substr(0, 7) == "http://" || image_path.substr(0, 8) == "https://") {
            data_uri = download_image_as_base64(image_path);
        } else if (image_path.substr(0, 5) == "data:") {
            data_uri = image_path;
        } else {
            // It's a local file
            data_uri = ImageProcessor::load_local_image(image_path, config_.target_directory.string());
        }
        
        if (!data_uri.empty()) {
            content.push_back({
                {"type", "image_url"},
                {"image_url", {
                    {"url", data_uri}
                }}
            });
        } else {
            // Image failed to load, include text note
            content.push_back({
                {"type", "text"},
                {"text", "[Image not found: " + image_path + "]"}
            });
        }
        
        // Update position to after the image reference
        // Find the end of ![...](...)
        size_t search_pos = image_pos;
        while (search_pos < user_input.size() && user_input[search_pos] != ')') {
            search_pos++;
        }
        current_pos = search_pos + 1;
    }
    
    // Add any remaining text after the last image
    if (current_pos < user_input.size()) {
        std::string remaining = user_input.substr(current_pos);
        remaining = trim(remaining);
        
        // Remove any remaining image syntax
        size_t syntax_pos = remaining.find("![");
        if (syntax_pos != std::string::npos) {
            remaining = remaining.substr(0, syntax_pos);
            remaining = trim(remaining);
        }
        
        if (!remaining.empty()) {
            content.push_back({
                {"type", "text"},
                {"text", remaining}
            });
        }
    }
    
    // If content array is empty (all images failed or no text), return error
    if (content.empty()) {
        return json::object({
            {"role", "user"},
            {"content", user_input}
        });
    }
    
    return json::object({
        {"role", "user"},
        {"content", content}
    });
}

// Manager mode system prompt - makes the agent act as a manager
// This is a STRICT override that tells the agent to not do work itself,
// but instead spawn sub-agents and wait for them
void LLMClient::initialize() {
    // Display startup banner — §Phase 3: routed through sink.
    if (sink_) {
        std::ostringstream banner;
        banner << "╔═══════════════════════════════════════════════════════════════════\n"
               << "║  daedalus v" << VERSION_STRING << " - Autonomous Coding Agent\n"
               << "║  Base URL: " << config_.base_url << "\n"
               << "║  Chat path: " << config_.chat_endpoint << "\n"
               << "║  Model:    " << config_.model << "\n"
               << "╚═══════════════════════════════════════════════════════════════════";
        sink_->on_log(daedalus::LogLevel::Info, banner.str());
    } else {
        // §Phase 5.1: no sink fallback. Banner goes through the
        // Logger, not std::cout.
        std::ostringstream banner;
        banner << "╔═══════════════════════════════════════════════════════════════════\n"
               << "║  daedalus v" << VERSION_STRING << " - Autonomous Coding Agent\n"
               << "║  Base URL: " << config_.base_url << "\n"
               << "║  Chat path: " << config_.chat_endpoint << "\n"
               << "║  Model:    " << config_.model << "\n"
               << "╚═══════════════════════════════════════════════════════════════════";
        LOG_INFO(banner.str());
    }

    // Load persisted messages from previous session.
    if (messages_ && messages_->load()) {
        std::ostringstream persist;
        persist << "[PERSIST] Restored " << messages_->size()
                << " messages from previous session";
        if (sink_) sink_->on_log(daedalus::LogLevel::Info, persist.str());
        else LOG_INFO(persist.str());
        LOG_INFO("Successfully restored " + std::to_string(messages_->size())
                 + " persisted messages");
    } else {
        LOG_DEBUG("No messages file found or load failed, starting fresh session");
    }
}

void LLMClient::cleanup() {
    // Session end — save memory.
    if (session_memory_) {
        std::string summary = "Session with " + std::to_string(iteration_count_) + " iterations";
        session_memory_->on_session_end(summary, {}, iteration_count_);
    }

    // Save persistent messages for next session.
    if (messages_) {
        messages_->save();
    }
}

// §Phase 5.1: cancel an in-flight turn. Fires cancel_source_ which
// the API wait loop and the per-iteration check both consult. Hosts
// typically call this from Agent::cancel() (which we wire up in
// AgentImpl::cancel). Safe to call from any thread. Idempotent —
// multiple calls just leave cancel_source_ in the requested state.
void LLMClient::cancel() noexcept {
    if (cancel_source_.stop_possible()) {
        cancel_source_.request_stop();
    }
}

    // ===================================================================
    // §Phase 3 - SSE streaming via sink
    //
    // MiniMax SSE line format:
    //   data: [{"id":"...","choices":[{"delta":{"content":"..."},"index":0}]}]
    //   data: [DONE]
    //
    // We accumulate a line buffer and parse each complete "data: " line.
    // Accumulated assistant text goes to sink_->on_assistant_delta(text).
    // ===================================================================

AgentState LLMClient::run_one_turn(const std::string& user_message) {
    std::string api_url = config_.base_url + config_.chat_endpoint;

    // Get current user and datetime for system prompt
    std::string username = "unknown";
    struct passwd* pw = getpwuid(getuid());
    if (pw && pw->pw_name) {
        username = pw->pw_name;
    }

    // Get current date/time in human-readable format
    std::time_t now = std::time(nullptr);
    std::tm* local_tm = std::localtime(&now);
    std::ostringstream datetime;
    datetime << std::put_time(local_tm, "%Y-%m-%d %H:%M:%S %Z");

    // Build header with user and datetime
    std::string prompt_header = "# Session Info\n";
    prompt_header += "- **User**: " + username + "\n";
    prompt_header += "- **Date/Time**: " + datetime.str() + "\n\n";

    // Build base system instruction using SystemPromptBuilder
    // §Phase 11: removed AgentMode from SystemPromptOptions.
    // §Phase 14: SystemPromptBuilder::compose() is now a thin composer.
    // The host's full system_prompt (from AgentConfig) is taken verbatim
    // as the first section; the library appends ONLY the dynamic adders
    // (MCP server status, tool definitions, skills catalog, session
    // memory, runtime stats). No identity / rules / workflow prose
    // lives in the library.
    //
    // §Phase 14 fix: refresh_mcp_tools() MUST happen BEFORE compose()
    // so the MCP status prose and tool_definitions both reflect the
    // live tool surface. (Previously compose() ran first, then
    // refresh_mcp_tools() updated the JSON schema AFTER system_instruction
    // was already built — the MCP server status prose was never added
    // to the prompt at all.)
    refresh_mcp_tools();

    SystemPromptOptions options;
    options.system_prompt = config_.system_prompt;  // verbatim, ALWAYS first

    // Optional system_info note (host's policy on tool discovery).
    if (!config_.system_info_note.empty()) {
        options.system_info_note = config_.system_info_note;
    }

    // MCP server status prose. The MCP **tools** also appear in the
    // JSON schema block below; this is the human-readable "what servers
    // are connected and what they offer" summary.
    if (mcp_manager_) {
        const auto connected = mcp_manager_->list_connected_servers();
        if (!connected.empty()) {
            std::ostringstream mcp_block;
            for (const auto& server_name : connected) {
                mcp_block << "- **" << server_name << "**";
                // Pull this server's tools from get_all_tools().
                // McpProxiedTool prefixes tool names with the server
                // name, so we filter by prefix.
                const auto prefix = server_name + "__";
                bool first_tool = true;
                for (const auto& t : mcp_manager_->get_all_tools()) {
                    if (t.name.rfind(prefix, 0) == 0) {
                        if (first_tool) {
                            mcp_block << " — tools: ";
                            first_tool = false;
                        } else {
                            mcp_block << ", ";
                        }
                        mcp_block << t.name.substr(prefix.size());
                    }
                }
                mcp_block << "\n";
            }
            options.mcp_servers = mcp_block.str();
        }
    }

    // Live tool JSON schema (post-MCP-refresh). Empty if no tools are
    // registered (degenerate — the agent has nothing to call).
    {
        std::string defs = ToolHandler::get_tool_definitions();
        if (!defs.empty() && defs != "[]") {
            options.tool_definitions = std::move(defs);
        }
    }

    // Session memory fact dump.
    if (session_memory_ && !config_.memory_disabled) {
        options.session_memory = session_memory_->get_system_prompt_addition();
    }

    // Skills: active = summary table only (full content is fetched on
    // demand via the get_skill_context tool). Available catalog = name +
    // description, with active skills filtered out so each skill appears
    // in exactly one of the two sections.
    if (SkillManager* skill_manager = SkillManager::get_instance()) {
        // Active skills — use the §Phase 15 filtered view (excludes
        // host-disabled skills even if they were previously activated).
        const auto active_skills = skill_manager->get_visible_active_skills();
        if (!active_skills.empty()) {
            std::ostringstream active_block;
            active_block << "| Skill | Description |\n";
            active_block << "|-------|-------------|\n";
            for (const auto& s : active_skills) {
                active_block << "| " << s.metadata.name
                             << " | " << s.metadata.description << " |\n";
            }
            active_block << "\nFull content available via `get_skill_context` tool.\n";
            options.active_skills = active_block.str();
        }

        // Available skills — use the §Phase 15 filtered view (only
        // non-disabled skills appear). Already excludes active skills.
        // (§Phase 14 fix: was duplicating them in both sections.)
        const auto visible_skills = skill_manager->get_visible_cached_skills();
        if (visible_skills.size() <= 50) {
            std::ostringstream cat;
            int listed = 0;
            for (const auto& [name, skill] : visible_skills) {
                if (skill_manager->is_skill_active(name)) continue;
                cat << "- **" << name << "**: " << skill.metadata.description << "\n";
                ++listed;
            }
            if (listed > 0) {
                options.available_skills = cat.str();
            }
        }
    }

    std::string system_instruction = prompt_header + SystemPromptBuilder::compose(options);

    // §NC-8 (2026-07-16): CURL handle removed. The sync fallback path
    // inside run_agent_cycle now uses OmniNet::HTTP::Client directly;
    // the async path always used HttpSession (now also OmniNet).

    json& messages = messages_->get_messages();

    // Add the user message provided by the host (the *one* input we accept).
    if (messages.empty()) {
        messages.push_back({{"role", "system"}, {"content", system_instruction}});
    }
    if (!user_message.empty()) {
        // §B19 fix: refuse to push a user message after a dangling tool result.
        // If the last message is a tool result, the previous turn ended mid-cycle
        // (e.g. user pressed Ctrl-C). The next run_one_turn would push a user
        // message after the tool result, causing the API to reject with
        // "tool call result does not follow tool call".
        if (!messages.empty() && messages.back().value("role", "") == "tool") {
            LOG_WARNING("run_one_turn: last message is a dangling tool result; "
                         "refusing to append user message. "
                         "Host should reload the session or handle this state.");
            if (sink_) {
                sink_->on_log(daedalus::LogLevel::Warn,
                              "run_one_turn: dangling tool result at end of "
                              "messages; cannot append user message.");
            }
        } else {
            messages.push_back({{"role", "user"}, {"content", user_message}});
        }
    }

    // Reset per-turn state
    iteration_count_ = 0;
    last_total_tokens_ = 0;

    bool agent_running = true;
    bool compress_after_done = false;
    AgentState final_state = AgentState::DONE;

    // Live tool definitions (re-fetched when MCP dirty flag flips).
    json tool_defs = json::parse(ToolHandler::get_tool_definitions());

    while (agent_running) {
        // §Phase 5.1: cancel via cancel_source_ (fired by
        // Agent::cancel()). Library no longer polls stdin.
        if (cancel_source_.get_token().stop_requested()) {
            std::ostringstream msg;
            msg << "\n[INTERRUPTED] Agent execution halted by user.";
            if (sink_) sink_->on_log(daedalus::LogLevel::Info, msg.str());
            final_state = AgentState::CANCELLED;
            break;
        }

        // §Phase 5.1: route the max-iterations diagnostic through the
        // Logger / sink instead of std::cout.
        if (iteration_count_ >= max_iterations_) {
            std::ostringstream msg;
            msg << "\n[WARNING] Maximum iterations (" << max_iterations_
                << ") reached. Stopping agent to prevent infinite loops.";
            if (sink_) sink_->on_log(daedalus::LogLevel::Warn, msg.str());
            break;
        }

        iteration_count_++;
        inject_context_stats(messages);

        if (mcp_tools_refresh_pending_ ||
            (mcp_manager_ && mcp_manager_->mcp_tools_dirty())) {
            refresh_mcp_tools();
            tool_defs = json::parse(ToolHandler::get_tool_definitions());
        }

        AgentState result_state = run_agent_cycle(api_url, messages, tool_defs, compress_after_done);

        // Auto-compress at 90% threshold OR if the agent explicitly requested it
        // via the compress_context tool (B7 fix)
        bool at_threshold = messages_ && messages_->should_compress(last_total_tokens_);
        if (at_threshold || compress_after_done) {
            if (compress_after_done) {
                LOG_INFO("[COMPRESS] Agent requested context compression...");
                compress_after_done = false;
            } else {
                LOG_INFO("[COMPRESS] Auto-triggering context compression at 90% threshold...");
            }
            if (messages_) {
                const size_t before_tokens = last_total_tokens_;
                messages_->set_messages(messages);
                auto compressed = messages_->compress();
                if (compressed) {
                    messages_->replace_messages(*compressed);
                    messages = messages_->get_messages();
                    const size_t after_tokens = messages_->estimate_tokens();
                    if (sink_) sink_->on_compaction(before_tokens, after_tokens);
                    LOG_INFO("[CONTEXT] Compressed successfully. Continuing...");
                } else {
                    LOG_ERROR("[COMPRESS] Failed - first half already deleted");
                }
            }
            iteration_count_ = 0;
            last_total_tokens_ = 0;
            continue;
        }

        // §Phase 5.1: route through Logger.
        if (result_state == AgentState::CANCELLED) {
            LOG_INFO("[AGENT] Execution cancelled by user.");
            final_state = AgentState::CANCELLED;
            break;
        }
        if (result_state == AgentState::ACT || result_state == AgentState::BUILD_TEST) {
            continue;
        }

        LOG_INFO("✓ Task Complete");
        final_state = AgentState::DONE;
        break;
    }

    return final_state;
}
