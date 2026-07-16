/**
 * @file src/acp/include/acp/AcpFwd.h
 * @brief Forward declarations for ACP types
 */

#pragma once

#include <string>
#include <vector>
#include <optional>
#include <variant>
#include <chrono>
#include <cstdint>
#include <expected>
#include <uuid.h>
#include <nlohmann/json.hpp>

namespace acp {

// ============================================================================
// Core identifiers
// ============================================================================

using AgentName = std::string;           // RFC 1123 DNS label
using SessionId = std::string;          // UUID v4 string
using RunId = std::string;              // UUID v4 string
using ContentType = std::string;        // MIME type

// ============================================================================
// Error / Result types
// ============================================================================

/// One error returned by the ACP client. Carries enough info for callers
/// to decide whether to retry (network errors), surface to the user
/// (validation errors), or escalate (auth errors).
struct AcpError {
    int code = 0;                       // HTTP status, or 0 for transport-level
    std::string message;                // human-readable; e.g. "HTTP 404", "Connection refused"
    std::optional<std::string> cause;   // exception text or extra context
};

/// Result type for ACP operations. Holds either the success value or an
/// AcpError. Use `.value()`, `.error()`, `.has_value()`, or comparison
/// with `bool`. Pattern matches std::expected<std::expected, AcpError>.
template <typename T>
using AcpResult = std::expected<T, AcpError>;

/// Convenience constructor: build an AcpError with just a message.
inline AcpError make_error(std::string msg,
                           int code = 0,
                           std::optional<std::string> cause = std::nullopt) {
    AcpError e;
    e.code    = code;
    e.message = std::move(msg);
    e.cause   = std::move(cause);
    return e;
}

// ============================================================================
// Enums
// ============================================================================

enum class RunMode {
    Sync,   // Synchronous - wait for completion
    Async, // Asynchronous - return immediately
    Stream // Streaming - return event stream
};

enum class RunStatus {
    Created,     // ACP spec: "created"
    InProgress,  // ACP spec: "in-progress"
    Awaiting,    // ACP spec: "awaiting"
    Completed,   // ACP spec: "completed"
    Cancelled,   // ACP spec: "cancelled"
    Failed       // ACP spec: "failed"
};

// ============================================================================
// Message types
// ============================================================================

struct MessagePart {
    std::optional<std::string> name;
    ContentType content_type = "text/plain";
    std::optional<std::string> content;
    std::optional<std::string> content_encoding;
    std::optional<std::string> content_url;
    std::optional<nlohmann::json> metadata;
};

struct Message {
    std::string role;
    std::vector<MessagePart> parts;
    std::optional<std::string> created_at;
};

// ============================================================================
// Tool definitions
// ============================================================================

struct ToolDefinition {
    std::string name;
    std::optional<std::string> description;
    nlohmann::json parameters;
    std::optional<std::string> provider;
};

using ToolDefinitions = std::vector<ToolDefinition>;

struct ToolCall {
    std::string id;
    ToolDefinition function;
    std::optional<nlohmann::json> tool_input;
    std::optional<nlohmann::json> tool_output;
};

// ============================================================================
// Run types
// ============================================================================

struct RunCreateRequest {
    AgentName agent_name;
    std::vector<Message> input;
    RunMode mode = RunMode::Sync;
    std::optional<SessionId> session_id;
};

struct Run {
    RunId run_id;
    AgentName agent_name;
    std::optional<SessionId> session_id;
    RunStatus status;
    std::optional<std::string> created_at;
    std::optional<std::string> updated_at;
};

// ============================================================================
// Session types
// ============================================================================

struct Session {
    SessionId session_id;
    std::string agent_name;
    std::vector<Message> messages;
    std::optional<std::string> created_at;
};

// ============================================================================
// Agent types
// ============================================================================

struct AgentCapabilities {
    bool streaming = false;
    bool async_mode = false;
    bool input_feeding = false;
};

struct AgentMetadata {
    std::optional<std::string> description;
    std::optional<std::string> version;
    AgentCapabilities capabilities;
    std::optional<nlohmann::json> data;
};

struct AgentManifest {
    AgentName name;
    AgentMetadata metadata;
    ToolDefinitions tools;
};

// ============================================================================
// Event payload structs (forward-looking — NOT in current spec).
//
// The ACP spec (docs/acp-protocol.md / acp-openapi.yaml) defines 11 event
// types, all discriminated by a `type` field on the JSON payload:
//
//   message.created, message.part, message.completed,
//   generic,
//   run.created, run.in-progress, run.awaiting,
//   run.completed, run.failed, run.cancelled,
//   error
//
// None of these need a dedicated struct — the dispatcher reads the
// discriminator and dispatches to the matching IRunEventHandler method.
// The structs below were aspirational and are kept for source
// compatibility with anything that referenced them, but are deprecated.
// §ACP-7 (2026-07-16): use the IRunEventHandler callbacks instead.
// ============================================================================

struct [[deprecated("Use IRunEventHandler callbacks instead — see ACP-7")]] MessageEvent {
    Message message;
};

struct [[deprecated("Not in current ACP spec — use IRunEventHandler")]] ToolCallEvent {
    ToolCall tool_call;
};

struct [[deprecated("Not in current ACP spec — use IRunEventHandler")]] ToolCallOutputEvent {
    std::string tool_call_id;
    nlohmann::json output;
};

struct [[deprecated("Not in current ACP spec — use IRunEventHandler")]] AwaitEvent {
    nlohmann::json await_request;
};

struct [[deprecated("Not in current ACP spec — use IRunEventHandler")]] RunCompletedEvent {
    std::string final_output;
};

struct [[deprecated("Use IRunEventHandler::on_error instead")]] ErrorEvent {
    std::optional<AcpError> error;
};

using RunEventData [[deprecated("Use IRunEventHandler dispatch instead")]] = std::variant<
    MessageEvent,
    ToolCallEvent,
    ToolCallOutputEvent,
    AwaitEvent,
    RunCompletedEvent,
    ErrorEvent
>;

struct [[deprecated("Use IRunEventHandler dispatch instead")]] RunEvent {
    RunId run_id;
    std::string event_type;
    RunEventData data;
    std::optional<std::string> timestamp;
};

// ============================================================================
// Serialization support (declared here, implemented in AcpTypes.cpp)
// ============================================================================

void from_json(const nlohmann::json& j, MessagePart& m);
void to_json(nlohmann::json& j, const MessagePart& m);

void from_json(const nlohmann::json& j, Message& m);
void to_json(nlohmann::json& j, const Message& m);

void from_json(const nlohmann::json& j, Run& r);
void to_json(nlohmann::json& j, const RunCreateRequest& req);

void from_json(const nlohmann::json& j, Session& s);

/// Parse a kebab-case ACP status string ("created", "in-progress",
/// "awaiting", "completed", "cancelled", "failed") into the RunStatus
/// enum. Unknown values map to Created.
RunStatus parse_run_status(const std::string& s);

/// Render a RunStatus as its kebab-case ACP wire string. Inverse of
/// parse_run_status. The returned pointer is to a static literal.
const char* run_status_to_string(RunStatus s);

// Forward declaration; the full definition lives in AcpClient.h.
class IRunEventHandler;

/// §ACP-7 (2026-07-16): dispatch a parsed SSE event payload to the
/// matching IRunEventHandler method. The wire format uses a `type`
/// discriminator on the JSON `data` (the SSE `event:` field is not
/// used by ACP — see docs/acp-openapi.yaml).
///
/// Recognised types (all from the spec):
///   - "message.created"   → on_message_created
///   - "message.part"       → on_message_part
///   - "message.completed"  → on_message_completed
///   - "generic"            → on_generic
///   - "run.created"        → on_run_created
///   - "run.in-progress"    → on_run_in_progress
///   - "run.awaiting"       → on_run_awaiting
///   - "run.completed"      → on_run_completed
///   - "run.failed"         → on_run_failed
///   - "run.cancelled"      → on_run_cancelled
///   - "error"              → on_error
///
/// Unknown types fall back to on_generic (with the parsed JSON so
/// callers can inspect the structure).
///
/// Malformed payloads (parse failure, missing `type`) are silently
/// dropped — they don't reach any callback. Callers should treat
/// a stream with no callbacks as "transport error" if they expected
/// at least a `run.created`.
void dispatch_sse_event(const RunId& run_id,
                        const std::string& event_data_json,
                        IRunEventHandler& handler);

} // namespace acp
