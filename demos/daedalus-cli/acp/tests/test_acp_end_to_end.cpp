/**
 * @file test_acp_end_to_end.cpp
 * @brief End-to-end ACP test: spin up an in-process OmniNet HTTP server
 *        that emits the 11 spec event types via SSE, then connect with
 *        AcpClient and verify dispatch_sse_event() routes them to the
 *        matching IRunEventHandler callbacks.
 *
 * §ACP-7 (2026-07-16): live end-to-end test for ACP SSE structured
 * dispatch. Without this, dispatch_sse_event was only exercised by
 * test_acp_basic which feeds canned bytes through SseParser — never
 * through the actual HTTP/SSE wire path of AcpTransport.
 *
 * The mock server is a real OmniNet HTTP::Server with a registered
 * /v1/runs/<id>/events protocol-switch endpoint that emits the
 * full lifecycle of an ACP run:
 *   1. run.created
 *   2. message.created  (user)
 *   3. message.part     (text/plain "Hello, ")
 *   4. message.part     ("world!\n")
 *   5. message.completed
 *   6. generic          ({"progress":"done"})
 *   7. run.completed
 *
 * Plus an error event on a separate run (server emits the error
 * right after run.created).
 */

#include <atomic>
#include <chrono>
#include <future>
#include <sstream>
#include <thread>

// §ACP-7 (2026-07-16): DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN must be
// defined BEFORE the doctest header is included — it tells doctest to
// emit the main() definition (instead of treating itself as a
// header-only library). Putting it after the include has no effect.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "OmniNet.h"
#include "http/HttpServer.h"
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"
#include "net/Socket.h"

#include <daedalus/net/SseParser.h>

#include "acp/AcpClient.h"
#include "acp/AcpFwd.h"

using namespace std::chrono_literals;

namespace test_acp_e2e {

// ============================================================================
// RecordingHandler — captures every callback fire + the last payload.
// ============================================================================
struct RecordingHandler : acp::IRunEventHandler {
    std::vector<std::string> calls;
    int last_error_code = 0;
    std::string last_error_message;
    nlohmann::json last_generic;
    int run_created = 0;
    int run_in_progress = 0;
    int run_awaiting = 0;
    int run_completed = 0;
    int run_cancelled = 0;
    int run_failed = 0;
    int message_created = 0;
    int message_completed = 0;
    int message_part = 0;
    int generic = 0;
    int error = 0;

    void on_message_created(const acp::RunId&, acp::Message) override {
        calls.push_back("message_created");
        ++message_created;
    }
    void on_message_part(const acp::RunId&, acp::MessagePart) override {
        calls.push_back("message_part");
        ++message_part;
    }
    void on_message_completed(const acp::RunId&, acp::Message) override {
        calls.push_back("message_completed");
        ++message_completed;
    }
    void on_run_created(const acp::RunId&, acp::Run) override {
        calls.push_back("run_created");
        ++run_created;
    }
    void on_run_in_progress(const acp::RunId&, acp::Run) override {
        calls.push_back("run_in_progress");
        ++run_in_progress;
    }
    void on_run_awaiting(const acp::RunId&, acp::Run) override {
        calls.push_back("run_awaiting");
        ++run_awaiting;
    }
    void on_run_completed(const acp::RunId&, acp::Run) override {
        calls.push_back("run_completed");
        ++run_completed;
    }
    void on_run_cancelled(const acp::RunId&, acp::Run) override {
        calls.push_back("run_cancelled");
        ++run_cancelled;
    }
    void on_run_failed(const acp::RunId&, acp::Run) override {
        calls.push_back("run_failed");
        ++run_failed;
    }
    void on_error(const acp::RunId&, acp::AcpError err) override {
        calls.push_back("error");
        ++error;
        last_error_code    = err.code;
        last_error_message = err.message;
    }
    void on_generic(const acp::RunId&, const nlohmann::json& data) override {
        calls.push_back("generic");
        ++generic;
        last_generic = data;
    }
};

// ============================================================================
// Tiny mock ACP server.
// ============================================================================
//
// We register a single SSE endpoint at /v1/runs/<id>/events that emits
// the spec event sequence as SSE data lines, then closes the stream
// cleanly (chunked terminator). One server, one connection, full lifecycle.
// Each frame is wrapped in chunked transfer encoding
// (`<hex size>\r\n<data>\r\n`) because the response sets
// Transfer-Encoding: chunked — without it the client decoder fails
// to find any chunk size and reports EOF immediately, which is why an
// earlier version of this test saw calls.size()==0 for every test.
// This matches SSEHandler::writeChunked()'s on-the-wire format.

static void emit_run_lifecycle(OmniNet::Socket& sock,
                               const std::string& run_id) {
    auto send_chunked = [&](const std::string& sse_payload) {
        // §ACP-7 e2e test: the response sets Transfer-Encoding:
        // chunked, so each payload must be framed as
        //   <hex size>\r\n
        //   <SSE bytes>\r\n
        // §ACP-7 chunked-encoding: WITHOUT this wrapping the
        // chunked decoder on the client side fails to find any
        // chunk size and reports EOF immediately, which is why
        // an earlier version of this test saw calls.size()==0.
        std::ostringstream oss;
        oss << std::hex << sse_payload.size() << "\r\n";
        std::string header = oss.str();
        sock.sendAll(std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(header.data()), header.size()));
        sock.sendAll(std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(sse_payload.data()),
            sse_payload.size()));
        sock.sendAll(std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>("\r\n"), 2));
        std::fprintf(stderr,
            "[mock-server] sent %zu bytes chunked for: %.40s...\n",
            sse_payload.size(), sse_payload.c_str());
    };

    auto send = [&](const std::string& json) {
        send_chunked("data: " + json + "\n\n");
    };

    // 1. run.created
    send(R"({"type":"run.created","run":{"run_id":")" + run_id +
         R"(","agent_name":"test","status":"created"}})");
    // 2. message.created (user)
    send(R"({"type":"message.created","message":{"role":"user","parts":[]}})");
    // 3. message.part (text "Hello, ")
    send(R"({"type":"message.part","part":{"content_type":"text/plain","content":"Hello, "}})");
    // 4. message.part (text "world!\n")
    send(R"({"type":"message.part","part":{"content_type":"text/plain","content":"world!\n"}})");
    // 5. message.completed (agent)
    send(R"({"type":"message.completed","message":{"role":"agent","parts":[]}})");
    // 6. generic (progress update)
    send(R"({"type":"generic","generic":{"progress":"done"}})");
    // 7. run.completed
    send(R"({"type":"run.completed","run":{"run_id":")" + run_id +
         R"(","agent_name":"test","status":"completed"}})");
    // 8. Chunked terminator — 0\r\n\r\n
    sock.sendAll(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>("0\r\n\r\n"), 5));
    std::fprintf(stderr, "[mock-server] lifecycle complete for %s\n",
                 run_id.c_str());
}

// ============================================================================
// Tests
// ============================================================================

TEST_CASE("ACP end-to-end: full lifecycle emits 7 IRunEventHandler callbacks") {
    OmniNet::init();

    // Pick a free port. The OS won't reuse immediately after close.
    // 18700 + (pid % 100) — ephemeral range, unlikely to clash.
    const uint16_t port = static_cast<uint16_t>(18700 + (getpid() % 100));
    const std::string run_id = "e2e-run-1";

    OmniNet::HTTP::Server::Config scfg;
    scfg.address = "127.0.0.1";
    scfg.port    = port;
    scfg.workerCount = 2;
    OmniNet::HTTP::Server server(scfg);

    // Register the SSE endpoint.
    server.registerProtocolSwitch(
        "/v1/runs/e2e-run-1/events",
        {
            .on_upgrade = [](const OmniNet::HTTP::Request&,
                             OmniNet::HTTP::Response& resp) {
                resp.setStatus(OmniNet::HTTP::StatusCode::OK);
                resp.setContentType("text/event-stream");
                resp.setHeader("Cache-Control", "no-cache");
                resp.setTransferEncoding("chunked");
                resp.markStreaming();
            },
            .on_post_upgrade = [run_id](std::unique_ptr<OmniNet::Socket> sock,
                                         const OmniNet::HTTP::Request&) {
                // Detach; emit lifecycle then close.
                std::thread([sock = std::move(sock), run_id]() mutable {
                    if (!sock || !sock->isOpen()) return;
                    emit_run_lifecycle(*sock, run_id);
                }).detach();
            }
        });

    server.startAsync();

    // Give the server time to bind + for the test thread to settle.
    // Without this, the very-fast test thread races the server
    // accept-loop setup; we observed the client connecting to a
    // not-yet-bound socket.
    std::this_thread::sleep_for(500ms);

    // Connect an ACP client.
    auto client = acp::create_client("http://127.0.0.1:" + std::to_string(port));
    RecordingHandler h;

    auto result = client->stream_run_events(run_id, h);
    REQUIRE(result.has_value());

    // Expected sequence (cumulative callbacks fired by the 7 events):
    //   run.created, message.created, message.part, message.part,
    //   message.completed, generic, run.completed
    REQUIRE(h.calls.size() == 7);
    CHECK(h.calls[0] == "run_created");
    CHECK(h.calls[1] == "message_created");
    CHECK(h.calls[2] == "message_part");
    CHECK(h.calls[3] == "message_part");
    CHECK(h.calls[4] == "message_completed");
    CHECK(h.calls[5] == "generic");
    CHECK(h.calls[6] == "run_completed");

    CHECK(h.run_created      == 1);
    CHECK(h.message_created  == 1);
    CHECK(h.message_part     == 2);
    CHECK(h.message_completed== 1);
    CHECK(h.generic          == 1);
    CHECK(h.run_completed    == 1);
    CHECK(h.run_failed       == 0);
    CHECK(h.run_cancelled    == 0);
    CHECK(h.run_awaiting     == 0);
    CHECK(h.error            == 0);

    // Last generic should carry the {"progress":"done"} payload.
    CHECK(h.last_generic.contains("progress"));
    CHECK(h.last_generic["progress"] == "done");

    server.stop();
    OmniNet::cleanup();
}

TEST_CASE("ACP end-to-end: error event routes to on_error") {
    OmniNet::init();

    const uint16_t port = static_cast<uint16_t>(18800 + (getpid() % 100));
    const std::string run_id = "e2e-run-err";

    OmniNet::HTTP::Server::Config scfg;
    scfg.address = "127.0.0.1";
    scfg.port    = port;
    scfg.workerCount = 2;
    OmniNet::HTTP::Server server(scfg);

    server.registerProtocolSwitch(
        "/v1/runs/e2e-run-err/events",
        {
            .on_upgrade = [](const OmniNet::HTTP::Request&,
                             OmniNet::HTTP::Response& resp) {
                resp.setStatus(OmniNet::HTTP::StatusCode::OK);
                resp.setContentType("text/event-stream");
                resp.setTransferEncoding("chunked");
                resp.markStreaming();
            },
                        .on_post_upgrade = [run_id](std::unique_ptr<OmniNet::Socket> sock,
                                         const OmniNet::HTTP::Request&) {
                std::thread([sock = std::move(sock), run_id]() mutable {
                    if (!sock || !sock->isOpen()) return;
                    // §ACP-7: chunked-encoded SSE (per
                    // Transfer-Encoding: chunked the response sets).
                    auto send = [&](const std::string& payload) {
                        std::string sse = "data: " + payload + "\n\n";
                        std::ostringstream oss;
                        oss << std::hex << sse.size() << "\r\n";
                        std::string hdr = oss.str();
                        sock->sendAll(std::span<const uint8_t>(
                            reinterpret_cast<const uint8_t*>(hdr.data()),
                            hdr.size()));
                        sock->sendAll(std::span<const uint8_t>(
                            reinterpret_cast<const uint8_t*>(sse.data()),
                            sse.size()));
                        sock->sendAll(std::span<const uint8_t>(
                            reinterpret_cast<const uint8_t*>("\r\n"), 2));
                    };
                    // run.created then error
                    send(R"({"type":"run.created","run":{"run_id":")" + run_id +
                         R"(","agent_name":"test","status":"created"}})");
                    send(R"({"type":"error","error":{"code":503,"message":"upstream timeout"}})");
                    sock->sendAll(std::span<const uint8_t>(
                        reinterpret_cast<const uint8_t*>("0\r\n\r\n"), 5));
                }).detach();
            }
        });

    server.startAsync();
    std::this_thread::sleep_for(500ms);

    auto client = acp::create_client("http://127.0.0.1:" + std::to_string(port));
    RecordingHandler h;

    auto result = client->stream_run_events(run_id, h);
    REQUIRE(result.has_value());

    // Two callbacks: run.created, error.
    REQUIRE(h.calls.size() == 2);
    CHECK(h.calls[0] == "run_created");
    CHECK(h.calls[1] == "error");
    CHECK(h.last_error_code    == 503);
    CHECK(h.last_error_message == "upstream timeout");

    server.stop();
    OmniNet::cleanup();
}

TEST_CASE("ACP end-to-end: run.failed and run.cancelled route correctly") {
    OmniNet::init();

    const uint16_t port = static_cast<uint16_t>(18900 + (getpid() % 100));

    OmniNet::HTTP::Server::Config scfg;
    scfg.address = "127.0.0.1";
    scfg.port    = port;
    scfg.workerCount = 2;
    OmniNet::HTTP::Server server(scfg);

    // Two endpoints in one test, distinguished by path.
    auto register_failed = [&](const std::string& path, const std::string& run_id,
                               const std::string& fail_event_type) {
        server.registerProtocolSwitch(path, {
            .on_upgrade = [](const OmniNet::HTTP::Request&,
                             OmniNet::HTTP::Response& resp) {
                resp.setStatus(OmniNet::HTTP::StatusCode::OK);
                resp.setContentType("text/event-stream");
                resp.setTransferEncoding("chunked");
                resp.markStreaming();
            },
            .on_post_upgrade = [run_id, fail_event_type](
                    std::unique_ptr<OmniNet::Socket> sock,
                    const OmniNet::HTTP::Request&) {
                std::thread([sock = std::move(sock), run_id, fail_event_type]() mutable {
                    if (!sock || !sock->isOpen()) return;
                    // §ACP-7: chunked-encoded SSE.
                    auto send = [&](const std::string& payload) {
                        std::string sse = "data: " + payload + "\n\n";
                        std::ostringstream oss;
                        oss << std::hex << sse.size() << "\r\n";
                        std::string hdr = oss.str();
                        sock->sendAll(std::span<const uint8_t>(
                            reinterpret_cast<const uint8_t*>(hdr.data()),
                            hdr.size()));
                        sock->sendAll(std::span<const uint8_t>(
                            reinterpret_cast<const uint8_t*>(sse.data()),
                            sse.size()));
                        sock->sendAll(std::span<const uint8_t>(
                            reinterpret_cast<const uint8_t*>("\r\n"), 2));
                    };
                    send(R"({"type":"run.created","run":{"run_id":")" + run_id +
                         R"(","agent_name":"test","status":"created"}})");
                    send(R"({"type":")" + fail_event_type +
                         R"(","run":{"run_id":")" + run_id +
                         R"(","agent_name":"test","status":"failed"}})");
                    sock->sendAll(std::span<const uint8_t>(
                        reinterpret_cast<const uint8_t*>("0\r\n\r\n"), 5));
                }).detach();
            }
        });
    };

    register_failed("/v1/runs/e2e-fail/events",   "e2e-fail",   "run.failed");
    register_failed("/v1/runs/e2e-cancel/events", "e2e-cancel", "run.cancelled");

    server.startAsync();
    std::this_thread::sleep_for(500ms);

    {
        auto client = acp::create_client("http://127.0.0.1:" + std::to_string(port));
        RecordingHandler h;
        client->stream_run_events("e2e-fail", h);
        REQUIRE(h.calls.size() == 2);
        CHECK(h.calls[0] == "run_created");
        CHECK(h.calls[1] == "run_failed");
        CHECK(h.run_failed == 1);
    }

    {
        auto client = acp::create_client("http://127.0.0.1:" + std::to_string(port));
        RecordingHandler h;
        client->stream_run_events("e2e-cancel", h);
        REQUIRE(h.calls.size() == 2);
        CHECK(h.calls[0] == "run_created");
        CHECK(h.calls[1] == "run_cancelled");
        CHECK(h.run_cancelled == 1);
    }

    server.stop();
    OmniNet::cleanup();
}

}  // namespace test_acp_e2e
