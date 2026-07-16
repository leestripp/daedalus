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
// Event types
// ============================================================================

struct AcpError {
    int code;
    std::string message;
    std::optional<std::string> cause_message;
};

struct MessageEvent {
    Message message;
};

struct ToolCallEvent {
    ToolCall tool_call;
};

struct ToolCallOutputEvent {
    std::string tool_call_id;
    nlohmann::json output;
};

struct AwaitEvent {
    nlohmann::json await_request;
};

struct RunCompletedEvent {
    std::string final_output;
};

struct ErrorEvent {
    std::optional<AcpError> error;
};

using RunEventData = std::variant<
    MessageEvent,
    ToolCallEvent,
    ToolCallOutputEvent,
    AwaitEvent,
    RunCompletedEvent,
    ErrorEvent
>;

struct RunEvent {
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

} // namespace acp
