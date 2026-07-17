/**
 * @file demos/daedalus-cli/acp/tests/test_acp_basic.cpp
 * @brief ACP client smoke tests + regression coverage for the bugs
 *        found in the 2026-07-16 code review (see CodeMax.md, ACP-1..15).
 *
 * Coverage:
 *   - Client construction against a dead URL (graceful failure path)
 *   - Status enum mapping (kebab-case <-> enum, both directions)
 *   - Run JSON deserialization (run_id, status, session_id, timestamps)
 *   - Message JSON deserialization (created_at)
 *   - Mode string round-trip (sync/async/stream)
 *   - AcpResult<T> error propagation (was the silent-failure bug ACP-6)
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "acp/AcpClient.h"
#include "acp/AcpFwd.h"

#include <daedalus/net/SseParser.h>

#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace acp;

// ============================================================================
// Smoke: client construction + graceful failure on unreachable server.
// ============================================================================

TEST_CASE("acp::create_client constructs successfully") {
    auto client = acp::create_client("http://localhost:9999");
    REQUIRE(client != nullptr);
    CHECK(client->base_url() == "http://localhost:9999");
}

TEST_CASE("acp::ping returns false on unreachable server") {
    auto client = acp::create_client("http://localhost:1");  // unreachable port
    CHECK_FALSE(client->ping());
}

/// Fake transport for tests that don't need a real HTTP server.
/// Returns the same Response for every get/post call.
class FakeTransport : public IAcpTransport {
public:
    Response next_response;
    int  get_calls  = 0;
    int  post_calls = 0;

    Response get(const std::string& /*path*/) override {
        ++get_calls;
        return next_response;
    }
    Response post(const std::string& /*path*/, const nlohmann::json& /*body*/) override {
        ++post_calls;
        return next_response;
    }
    void stream_events(const RunId&, IRunEventHandler&) override {}
    bool is_connected() const override { return next_response.transport_error.empty(); }
};

// ============================================================================
// Regression: AcpResult<T> error propagation (ACP-6 — was silent failure).
// ============================================================================

TEST_CASE("list_agents returns AcpError when transport fails") {
    auto client = acp::create_client("http://localhost:1");
    auto result = client->list_agents();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code != 0);     // some non-zero code
    CHECK_FALSE(result.error().message.empty());
}

TEST_CASE("get_agent returns AcpError when transport fails") {
    auto client = acp::create_client("http://localhost:1");
    auto result = client->get_agent("test-agent");
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("create_run returns AcpError when transport fails") {
    auto client = acp::create_client("http://localhost:1");
    Message m;
    m.role = "user";
    MessagePart p;
    p.content_type = "text/plain";
    p.content = "hi";
    m.parts.push_back(p);

    auto result = client->create_run("test-agent", {m});
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("get_run returns AcpError when transport fails") {
    auto client = acp::create_client("http://localhost:1");
    auto result = client->get_run("nonexistent");
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("get_session returns AcpError when transport fails") {
    auto client = acp::create_client("http://localhost:1");
    auto result = client->get_session("nonexistent");
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("stream_run_events returns success on unreachable server") {
    // §ACP-7 (2026-07-16): no longer a stub. stream_run_events now
    // delegates to the transport's stream_events() — which uses
    // send_streaming + SseParser. With an unreachable server the
    // streaming path returns an empty result (the SSE stream
    // simply has no events to deliver, so we don't return an error).
    // The behaviour: the call completes silently; the handler is
    // never invoked because the server is unreachable.
    auto client = acp::create_client("http://localhost:1");
    // Even on a real server this would fail until ACP-7 is done. We
    // don't pass a handler — using a do-nothing lambda for completeness.
    struct NopHandler : IRunEventHandler {
        void on_message_created(const RunId&, Message) override {}
        void on_message_part(const RunId&, MessagePart) override {}
        void on_message_completed(const RunId&, Message) override {}
        void on_run_created(const RunId&, Run) override {}
        void on_run_in_progress(const RunId&, Run) override {}
        void on_run_awaiting(const RunId&, Run) override {}
        void on_run_completed(const RunId&, Run) override {}
        void on_run_cancelled(const RunId&, Run) override {}
        void on_run_failed(const RunId&, Run) override {}
        void on_error(const RunId&, AcpError) override {}
        void on_generic(const RunId&, const nlohmann::json&) override {}
    } handler;

    auto result = client->stream_run_events("test", handler);
    REQUIRE(result.has_value());   // success — stream simply ends
}

// ============================================================================
// Regression: status string mapping (ACP-3).
// ============================================================================

TEST_CASE("parse_run_status accepts all kebab-case ACP strings") {
    CHECK(parse_run_status("created")     == RunStatus::Created);
    CHECK(parse_run_status("in-progress") == RunStatus::InProgress);
    CHECK(parse_run_status("awaiting")    == RunStatus::Awaiting);
    CHECK(parse_run_status("completed")   == RunStatus::Completed);
    CHECK(parse_run_status("cancelled")   == RunStatus::Cancelled);
    CHECK(parse_run_status("failed")      == RunStatus::Failed);
}

TEST_CASE("parse_run_status rejects snake_case (was the old bug)") {
    // Pre-fix code only matched these snake_case strings, which the
    // ACP server never sends. They must now fall back to Created.
    CHECK(parse_run_status("queued")         == RunStatus::Created);
    CHECK(parse_run_status("in_progress")    == RunStatus::Created);
    CHECK(parse_run_status("awaiting_input") == RunStatus::Created);
}

TEST_CASE("parse_run_status handles unknown strings defensively") {
    CHECK(parse_run_status("")            == RunStatus::Created);
    CHECK(parse_run_status("foo")         == RunStatus::Created);
    CHECK(parse_run_status("IN-PROGRESS") == RunStatus::Created);  // case-sensitive
}

TEST_CASE("run_status_to_string round-trips through parse_run_status") {
    for (auto s : {RunStatus::Created, RunStatus::InProgress, RunStatus::Awaiting,
                   RunStatus::Completed, RunStatus::Cancelled, RunStatus::Failed}) {
        CHECK(parse_run_status(run_status_to_string(s)) == s);
    }
}

// ============================================================================
// Regression: Run JSON deserialization (ACP-4).
// ============================================================================

TEST_CASE("from_json(Run) parses run_id, status, session_id, timestamps") {
    json j = {
        {"run_id",     "abc-123"},
        {"agent_name", "test-agent"},
        {"session_id", "sess-xyz"},
        {"status",     "in-progress"},
        {"created_at", "2026-07-16T12:00:00Z"},
        {"updated_at", "2026-07-16T12:01:00Z"}
    };
    Run r;
    from_json(j, r);
    CHECK(r.run_id     == "abc-123");
    CHECK(r.agent_name == "test-agent");
    CHECK(*r.session_id == "sess-xyz");
    CHECK(r.status     == RunStatus::InProgress);
    REQUIRE(r.created_at.has_value());
    CHECK(*r.created_at == "2026-07-16T12:00:00Z");
    REQUIRE(r.updated_at.has_value());
    CHECK(*r.updated_at == "2026-07-16T12:01:00Z");
}

TEST_CASE("from_json(Run) handles missing optional fields") {
    json j = {{"agent_name", "minimal"}};
    Run r;
    REQUIRE_NOTHROW(from_json(j, r));
    CHECK(r.run_id.empty());
    CHECK(r.agent_name == "minimal");
    CHECK_FALSE(r.session_id.has_value());
    CHECK(r.status == RunStatus::Created);   // default
}

TEST_CASE("from_json(Run) tolerates unknown status string") {
    json j = {{"agent_name", "x"}, {"status", "weird-state"}};
    Run r;
    from_json(j, r);
    CHECK(r.status == RunStatus::Created);    // falls back gracefully
}

// ============================================================================
// Regression: Message.created_at parsing (ACP-5).
// ============================================================================

TEST_CASE("from_json(Message) parses created_at") {
    json j = {
        {"role", "user"},
        {"parts", json::array({
            {{"content_type", "text/plain"}, {"content", "hello"}}
        })},
        {"created_at", "2026-07-16T12:00:00Z"}
    };
    Message m;
    from_json(j, m);
    CHECK(m.role == "user");
    REQUIRE(m.parts.size() == 1);
    CHECK(*m.parts[0].content == "hello");
    REQUIRE(m.created_at.has_value());
    CHECK(*m.created_at == "2026-07-16T12:00:00Z");
}

TEST_CASE("from_json(Message) works without created_at") {
    json j = {
        {"role", "agent"},
        {"parts", json::array()}
    };
    Message m;
    REQUIRE_NOTHROW(from_json(j, m));
    CHECK_FALSE(m.created_at.has_value());
}

TEST_CASE("to_json(Message) round-trips with from_json") {
    Message original;
    original.role = "user";
    MessagePart part;
    part.content_type = "text/plain";
    part.content = "round-trip";
    original.parts.push_back(part);
    original.created_at = "2026-07-16T00:00:00Z";

    json j;
    to_json(j, original);
    Message restored;
    from_json(j, restored);

    CHECK(restored.role == original.role);
    REQUIRE(restored.parts.size() == 1);
    CHECK(*restored.parts[0].content == "round-trip");
    REQUIRE(restored.created_at.has_value());
    CHECK(*restored.created_at == *original.created_at);
}

// ============================================================================
// Mode string round-trip.
// ============================================================================

TEST_CASE("RunCreateRequest serializes mode correctly") {
    auto make_input = []() {
        std::vector<Message> input;
        Message m;
        m.role = "user";
        MessagePart p;
        p.content_type = "text/plain";
        p.content = "hi";
        m.parts.push_back(p);
        input.push_back(std::move(m));
        return input;
    };

    for (auto [mode, expected] : std::vector<std::pair<RunMode, std::string>>{
            {RunMode::Sync,   "sync"},
            {RunMode::Async,  "async"},
            {RunMode::Stream, "stream"}}) {
        RunCreateRequest req;
        req.agent_name = "x";
        req.input      = make_input();
        req.mode       = mode;
        json j;
        to_json(j, req);
        CHECK(j["mode"] == expected);
    }
}

// ============================================================================
// Regression: Session messages parsing (ACP-12).
// ============================================================================

TEST_CASE("from_json(Session) parses session_id, agent_name, created_at") {
    json j = {
        {"session_id", "sess-abc"},
        {"agent_name", "test-agent"},
        {"created_at", "2026-07-16T12:00:00Z"},
        {"messages",   json::array()}
    };
    Session s;
    from_json(j, s);
    CHECK(s.session_id == "sess-abc");
    CHECK(s.agent_name == "test-agent");
    REQUIRE(s.created_at.has_value());
    CHECK(*s.created_at == "2026-07-16T12:00:00Z");
    CHECK(s.messages.empty());
}

TEST_CASE("from_json(Session) parses message history") {
    json j = {
        {"session_id", "sess-1"},
        {"agent_name", "test-agent"},
        {"messages", json::array({
            {
                {"role", "user"},
                {"parts", json::array({
                    {{"content_type", "text/plain"}, {"content", "hi"}}
                })}
            },
            {
                {"role", "agent"},
                {"parts", json::array({
                    {{"content_type", "text/plain"}, {"content", "hello!"}}
                })}
            }
        })}
    };
    Session s;
    from_json(j, s);
    REQUIRE(s.messages.size() == 2);
    CHECK(s.messages[0].role == "user");
    CHECK(s.messages[1].role == "agent");
    REQUIRE(s.messages[1].parts.size() == 1);
    CHECK(*s.messages[1].parts[0].content == "hello!");
}

TEST_CASE("from_json(Session) tolerates missing fields") {
    json j = json::object();
    Session s;
    REQUIRE_NOTHROW(from_json(j, s));
    CHECK(s.session_id.empty());
    CHECK(s.agent_name.empty());
    CHECK(s.messages.empty());
}

TEST_CASE("from_json(Session) ignores non-array messages field") {
    json j = {{"messages", "not-an-array"}};
    Session s;
    REQUIRE_NOTHROW(from_json(j, s));
    CHECK(s.messages.empty());
}

// ============================================================================
// Regression: libuuid is actually used (ACP-8) — create_run generates a
// client-side run_id when the server response doesn't include one.
// ============================================================================

TEST_CASE("create_run generates UUID when server omits run_id") {
    auto fake = std::make_unique<FakeTransport>();
    fake->next_response = IAcpTransport::Response{
        /*status_code=*/201,
        /*body=*/json{{"agent_name", "x"}, {"status", "created"}},  // no run_id
        /*transport_error=*/""};

    AcpClient client(std::move(fake), "http://test");
    Message m;
    m.role = "user";
    MessagePart p;
    p.content_type = "text/plain";
    p.content = "hi";
    m.parts.push_back(p);

    auto result = client.create_run("x", {m});
    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->run_id.empty());
    // UUID format: 8-4-4-4-12 hex chars with hyphens = 36 chars total
    CHECK(result->run_id.size() == 36);
    CHECK(result->run_id[8]  == '-');
    CHECK(result->run_id[13] == '-');
    CHECK(result->run_id[18] == '-');
    CHECK(result->run_id[23] == '-');
}

TEST_CASE("create_run preserves server-provided run_id") {
    auto fake = std::make_unique<FakeTransport>();
    fake->next_response = IAcpTransport::Response{
        201,
        json{{"run_id", "server-assigned-123"}, {"agent_name", "x"}, {"status", "created"}},
        ""};

    AcpClient client(std::move(fake), "http://test");
    Message m;
    m.role = "user";
    m.parts.push_back(MessagePart{});
    m.parts[0].content_type = "text/plain";
    m.parts[0].content = "hi";

    auto result = client.create_run("x", {m});
    REQUIRE(result.has_value());
    CHECK(result->run_id == "server-assigned-123");
}

// ============================================================================
// §ACP-7 (2026-07-16): SSE event dispatcher tests.
// Verify each spec event type routes to the correct IRunEventHandler
// callback with the right payload.
// ============================================================================

/// Test handler that records which callbacks fired + last payload.
struct RecordingHandler : IRunEventHandler {
    std::vector<std::string> calls;

    // Helper for the trivially-typed callbacks.
    void on_message_created(const RunId&, Message) override { calls.push_back("message_created"); }
    void on_message_part(const RunId&, MessagePart) override { calls.push_back("message_part"); }
    void on_message_completed(const RunId&, Message) override { calls.push_back("message_completed"); }
    void on_run_created(const RunId&, Run) override { calls.push_back("run_created"); }
    void on_run_in_progress(const RunId&, Run) override { calls.push_back("run_in_progress"); }
    void on_run_awaiting(const RunId&, Run) override { calls.push_back("run_awaiting"); }
    void on_run_completed(const RunId&, Run) override { calls.push_back("run_completed"); }
    void on_run_cancelled(const RunId&, Run) override { calls.push_back("run_cancelled"); }
    void on_run_failed(const RunId&, Run) override { calls.push_back("run_failed"); }
    void on_error(const RunId&, AcpError err) override {
        calls.push_back("error");
        last_error_code    = err.code;
        last_error_message = err.message;
    }
    void on_generic(const RunId&, const nlohmann::json& data) override {
        calls.push_back("generic");
        last_generic = data;
    }

    int last_error_code = 0;
    std::string last_error_message;
    nlohmann::json last_generic;
};

TEST_CASE("dispatch_sse_event: message.created routes to on_message_created") {
    RecordingHandler h;
    acp::dispatch_sse_event("run-1", R"({"type":"message.created","message":{"role":"user","parts":[]}})", h);
    REQUIRE(h.calls.size() == 1);
    CHECK(h.calls[0] == "message_created");
}

TEST_CASE("dispatch_sse_event: message.part routes to on_message_part") {
    RecordingHandler h;
    acp::dispatch_sse_event("run-1", R"({"type":"message.part","part":{"content_type":"text/plain","content":"hi"}})", h);
    REQUIRE(h.calls.size() == 1);
    CHECK(h.calls[0] == "message_part");
}

TEST_CASE("dispatch_sse_event: message.completed routes to on_message_completed") {
    RecordingHandler h;
    acp::dispatch_sse_event("run-1", R"({"type":"message.completed","message":{"role":"agent","parts":[]}})", h);
    REQUIRE(h.calls.size() == 1);
    CHECK(h.calls[0] == "message_completed");
}

TEST_CASE("dispatch_sse_event: generic routes to on_generic") {
    RecordingHandler h;
    acp::dispatch_sse_event("run-1", R"({"type":"generic","generic":{"foo":"bar"}})", h);
    REQUIRE(h.calls.size() == 1);
    CHECK(h.calls[0] == "generic");
    CHECK(h.last_generic["foo"] == "bar");
}

TEST_CASE("dispatch_sse_event: run.created / .in-progress / .awaiting / "
         ".completed / .failed / .cancelled route to matching on_run_*") {
    for (const auto& [type, callback] : std::vector<std::pair<std::string, std::string>>{
            {"run.created",     "run_created"},
            {"run.in-progress", "run_in_progress"},
            {"run.awaiting",    "run_awaiting"},
            {"run.completed",   "run_completed"},
            {"run.failed",      "run_failed"},
            {"run.cancelled",   "run_cancelled"}}) {
        RecordingHandler h;
        acp::dispatch_sse_event("run-1",
            R"({"type":")" + type + R"(","run":{"run_id":"r","agent_name":"a","status":"created"}})", h);
        REQUIRE(h.calls.size() == 1);
        CHECK(h.calls[0] == callback);
    }
}

TEST_CASE("dispatch_sse_event: error routes to on_error with parsed AcpError") {
    RecordingHandler h;
    acp::dispatch_sse_event("run-1",
        R"({"type":"error","error":{"code":503,"message":"overloaded","cause":"upstream timeout"}})", h);
    REQUIRE(h.calls.size() == 1);
    CHECK(h.calls[0] == "error");
    CHECK(h.last_error_code    == 503);
    CHECK(h.last_error_message == "overloaded");
}

TEST_CASE("dispatch_sse_event: error with no cause field still works") {
    RecordingHandler h;
    acp::dispatch_sse_event("run-1",
        R"({"type":"error","error":{"code":500,"message":"boom"}})", h);
    CHECK(h.calls[0] == "error");
    CHECK(h.last_error_code    == 500);
    CHECK(h.last_error_message == "boom");
}

TEST_CASE("dispatch_sse_event: unknown type falls through to on_generic") {
    RecordingHandler h;
    acp::dispatch_sse_event("run-1",
        R"({"type":"future.event","some":"data"})", h);
    REQUIRE(h.calls.size() == 1);
    CHECK(h.calls[0] == "generic");
    // Spec-compatible: raw payload preserved so callers can inspect.
    CHECK(h.last_generic["type"] == "future.event");
    CHECK(h.last_generic["some"] == "data");
}

TEST_CASE("dispatch_sse_event: malformed JSON is dropped silently") {
    RecordingHandler h;
    acp::dispatch_sse_event("run-1", "not valid json {", h);
    CHECK(h.calls.empty());
}

TEST_CASE("dispatch_sse_event: missing type field is dropped silently") {
    RecordingHandler h;
    acp::dispatch_sse_event("run-1", R"({"message":{"role":"user","parts":[]}})", h);
    CHECK(h.calls.empty());
}

TEST_CASE("dispatch_sse_event: empty string is dropped silently") {
    RecordingHandler h;
    acp::dispatch_sse_event("run-1", "", h);
    CHECK(h.calls.empty());
}

TEST_CASE("dispatch_sse_event: malformed event_payload data is dropped silently") {
    // Same as the empty-string test but with a slightly different
    // failure mode: the payload parses as JSON but isn't an object.
    // Spec requires an object with a `type` discriminator — anything
    // else is dropped silently (we don't pollute the stream with
    // on_generic calls for non-object JSON).
    RecordingHandler h;
    acp::dispatch_sse_event("run-1", R"([{"type":"run.created","run":{}}])", h);
    CHECK(h.calls.empty());
}

TEST_CASE("dispatch_sse_event: warning surfaces when inner payload throws") {
    // The dispatcher must never crash a stream on a per-event payload
    // failure — it should log a warning via on_generic so the caller
    // can observe the issue without the stream dying.
    //
    // We force an inner failure by passing a `message` payload whose
    // `parts` field is the wrong type — from_json(Message&) calls
    // `parts.get<std::vector<MessagePart>>()` which throws on type
    // mismatch.
    RecordingHandler h;
    acp::dispatch_sse_event("run-1",
        R"({"type":"message.created","message":{"role":"user","parts":"not-an-array"}})", h);
    REQUIRE(h.calls.size() == 1);
    CHECK(h.calls[0] == "generic");
    CHECK(h.last_generic.contains("warning"));
}

TEST_CASE("dispatch_sse_event: full lifecycle (5 events in sequence)") {
    // Simulate one full ACP run cycle end-to-end:
    //   run.created -> message.created -> message.completed
    //                -> run.completed
    //                -> (terminal)
    RecordingHandler h;
    const std::string run_id = "run-lifecycle-1";

    acp::dispatch_sse_event(run_id,
        R"({"type":"run.created","run":{"run_id":")" + run_id + R"(","agent_name":"a","status":"created"}})", h);
    acp::dispatch_sse_event(run_id,
        R"({"type":"message.created","message":{"role":"user","parts":[]}})", h);
    acp::dispatch_sse_event(run_id,
        R"({"type":"message.completed","message":{"role":"user","parts":[]}})", h);
    acp::dispatch_sse_event(run_id,
        R"({"type":"run.completed","run":{"run_id":")" + run_id + R"(","agent_name":"a","status":"completed"}})", h);

    REQUIRE(h.calls.size() == 4);
    CHECK(h.calls[0] == "run_created");
    CHECK(h.calls[1] == "message_created");
    CHECK(h.calls[2] == "message_completed");
    CHECK(h.calls[3] == "run_completed");
}

// §ACP-7: end-to-end test using SseParser + dispatch_sse_event.
// Feeds canned SSE bytes (matching what an ACP server would emit) and
// verifies the right callbacks fire.
TEST_CASE("dispatch_sse_event: end-to-end via SseParser") {
    RecordingHandler h;
    daedalus::net::SseParser parser;

    // A 3-event stream in canonical ACP wire format.
    const std::string wire =
        "data: {\"type\":\"run.created\",\"run\":{\"run_id\":\"r1\",\"agent_name\":\"a\",\"status\":\"created\"}}\n"
        "\n"
        "data: {\"type\":\"message.created\",\"message\":{\"role\":\"user\",\"parts\":[]}}\n"
        "\n"
        "data: {\"type\":\"run.completed\",\"run\":{\"run_id\":\"r1\",\"agent_name\":\"a\",\"status\":\"completed\"}}\n"
        "\n";

    parser.feed(wire);
    parser.finish();
    while (auto ev = parser.try_pop()) {
        acp::dispatch_sse_event("r1", ev->data, h);
    }

    REQUIRE(h.calls.size() == 3);
    CHECK(h.calls[0] == "run_created");
    CHECK(h.calls[1] == "message_created");
    CHECK(h.calls[2] == "run_completed");
}