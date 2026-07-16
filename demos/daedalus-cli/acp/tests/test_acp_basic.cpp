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

TEST_CASE("stream_run_events returns AcpError 501 (not implemented yet)") {
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
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == 501);
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