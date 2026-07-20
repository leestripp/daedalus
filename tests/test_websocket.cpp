// tests/test_websocket.cpp
//
// Unit tests for the four WebSocket tools + WsRegistry:
//
//   WebSocketConnectTool  (websocket_connect)
//   WebSocketSendTool     (websocket_send)
//   WebSocketRecvTool     (websocket_recv)
//   WebSocketCloseTool    (websocket_close)
//
// Strategy:
//   - Spin up a tiny in-process OmniNet WebSocket echo server on
//     127.0.0.1:19443 (fixed port; well outside default http range).
//   - Connect via the tools, send text + binary, verify echo back.
//   - Test URL validation, unknown connection_id, etc.
//
// NOTE: OmniNet 0.7.4's WebSocket client ctor only supports ws:// (no TLS).
// wss:// URLs are rejected with a clear error in our tool layer.

#include "NetworkTool.h"
#include "core/FileIO.h"

#include "websocket/WebSocket.h"
#include "http/HttpServer.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

using json = nlohmann::json;

static int failures = 0;
#define EXPECT(cond, msg) do { \
    if (!(cond)) { ++failures; std::cerr << "  FAIL: " << msg << std::endl; } \
    else         { std::cerr << "  OK:   " << msg << std::endl; } \
} while (0)

// ---------- Mini echo server ----------

class EchoServer {
public:
    EchoServer() : running_(false), port_(19443) {
        OmniNet::HTTP::Server::Config cfg;
        cfg.address = "127.0.0.1";
        cfg.port = port_;
        cfg.workerCount = 2;
        cfg.requestTimeout = std::chrono::seconds(60);

        // OmniNet's WebSocket::Server wraps HttpServer and uses
        // HttpServer::start() which is blocking. Run it on its own
        // thread so we can stop it cleanly later.
        server_thread_ = std::thread([this, cfg]() mutable {
            server_ = std::make_unique<OmniNet::WebSocket::Server>(cfg);
            server_->on("/echo", [](OmniNet::WebSocket::Connection& ws) {
                std::cerr << "[echo] handler called\n";
                // OmniNet's API: register callbacks (handler returns
                // immediately, then a background readLoop dispatches
                // frames). NOT a blocking receive loop.
                ws.onText([&ws](std::string text) {
                    std::cerr << "[echo] got text: " << text << "\n";
                    ws.sendText(text);
                });
                ws.onBinary([&ws](std::span<const uint8_t> data) {
                    std::cerr << "[echo] got binary len=" << data.size() << "\n";
                    ws.sendBinary(data);
                });
                ws.onClose([](auto, auto) {
                    std::cerr << "[echo] peer closed\n";
                });
            });
            // Blocking start() — runs the accept loop on this thread.
            server_->start();
        });
        running_.store(true);
        // Poll until the listening socket is actually bound (server thread
        // may take a moment after start() is called).
        for (int i = 0; i < 50; ++i) {  // up to ~5s
            int s = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(port_);
            int rc = ::connect(s, (sockaddr*)&addr, sizeof(addr));
            ::close(s);
            if (rc == 0) return;  // bound and accepting
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    ~EchoServer() {
        running_.store(false);
        if (server_) server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
    }

    int port() const { return port_; }
    const std::string& url() const { return url_; }

private:
    std::unique_ptr<OmniNet::WebSocket::Server> server_;
    std::thread server_thread_;
    std::atomic<bool> running_;
    uint16_t port_;
    std::string url_{"ws://127.0.0.1:19443/echo"};
};

// ---------- Test groups ----------

static void test_url_validation() {
    std::cerr << "test_url_validation\n";
    FileIO fileio(std::filesystem::current_path());
    WebSocketConnectTool tool;

    // missing url
    auto r0 = tool.execute(json::object(), fileio);
    EXPECT(r0.value("status", "") == "error", "missing url -> error");

    // non-ws scheme
    auto r1 = tool.execute(json{{"url", "http://example.com/"}}, fileio);
    EXPECT(r1.value("status", "") == "error", "http:// rejected");
    EXPECT(r1.value("message", "").find("ws") != std::string::npos,
           "error mentions ws/wss");

    auto r2 = tool.execute(json{{"url", "ftp://example.com/"}}, fileio);
    EXPECT(r2.value("status", "") == "error", "ftp:// rejected");

        // wss:// now supported (OmniNet 0.7.6). Use a port that's never
    // listening so the handshake fails cleanly (not with FD_SET abort).
    auto r3 = tool.execute(json{{"url", "wss://127.0.0.1:1/"}, {"timeout_seconds", 2}}, fileio);
    EXPECT(r3.value("status", "") == "error", "wss:// to dead port -> error");
    EXPECT(r3.value("message", "").size() > 0, "wss:// error message non-empty");
}

static void test_tool_definitions() {
    std::cerr << "test_tool_definitions\n";
    {
        WebSocketConnectTool tool;
        auto def = tool.get_definition();
        EXPECT(def["function"]["name"] == "websocket_connect", "connect name");
        EXPECT(def["function"]["parameters"]["required"][0] == "url",
               "required=[url]");
    }
    {
        WebSocketSendTool tool;
        auto def = tool.get_definition();
        EXPECT(def["function"]["name"] == "websocket_send", "send name");
        auto req = def["function"]["parameters"]["required"];
        bool has_conn = false;
        for (auto& r : req) if (r == "connection_id") has_conn = true;
        EXPECT(has_conn, "send required=[connection_id]");
    }
    {
        WebSocketRecvTool tool;
        auto def = tool.get_definition();
        EXPECT(def["function"]["name"] == "websocket_recv", "recv name");
        EXPECT(def["function"]["parameters"]["properties"].contains("timeout_seconds"),
               "recv has timeout_seconds");
    }
    {
        WebSocketCloseTool tool;
        auto def = tool.get_definition();
        EXPECT(def["function"]["name"] == "websocket_close", "close name");
        auto req = def["function"]["parameters"]["required"];
        bool has_conn = false;
        for (auto& r : req) if (r == "connection_id") has_conn = true;
        EXPECT(has_conn, "close required=[connection_id]");
    }
}

static void test_unknown_connection_id() {
    std::cerr << "test_unknown_connection_id\n";
    FileIO fileio(std::filesystem::current_path());
    WebSocketSendTool send;
    WebSocketRecvTool recv;
    WebSocketCloseTool close;

    auto r1 = send.execute(json{{"connection_id", "ws_does_not_exist"},
                                {"message", "hi"}}, fileio);
    EXPECT(r1.value("status", "") == "error", "send on unknown id -> error");

    auto r2 = recv.execute(json{{"connection_id", "ws_does_not_exist"}}, fileio);
    EXPECT(r2.value("status", "") == "error", "recv on unknown id -> error");

    auto r3 = close.execute(json{{"connection_id", "ws_does_not_exist"}}, fileio);
    EXPECT(r3.value("status", "") == "error", "close on unknown id -> error");
}

static void test_send_missing_payload() {
    std::cerr << "test_send_missing_payload\n";
    FileIO fileio(std::filesystem::current_path());
    WebSocketSendTool send;

    // Need a real connection to test "exactly one of" — but since the
    // validation runs first, we can test it on any bogus id. The
    // validation error should be returned before the lookup error.
    auto r1 = send.execute(json{{"connection_id", "ws_x"}}, fileio);
    EXPECT(r1.value("status", "") == "error", "no payload -> error");
    EXPECT(r1.value("message", "").find("message") != std::string::npos
        || r1.value("message", "").find("base64") != std::string::npos,
           "error mentions message/base64");

    auto r2 = send.execute(json{{"connection_id", "ws_x"},
                                {"message", "hi"},
                                {"message_base64", "aGVsbG8="}}, fileio);
    EXPECT(r2.value("status", "") == "error", "both payload -> error");
}

static void test_connect_to_real_server_and_echo() {
    std::cerr << "test_connect_to_real_server_and_echo\n";
    EchoServer srv;
    FileIO fileio(std::filesystem::current_path());

    WebSocketConnectTool connect;
    WebSocketSendTool send;
    WebSocketRecvTool recv;
    WebSocketCloseTool close;

    // 1. Connect
    auto r0 = connect.execute(json{{"url", srv.url()}, {"timeout_seconds", 5}}, fileio);
    EXPECT(r0.value("status", "") == "success", "connect success");
    EXPECT(r0.contains("connection_id"), "connection_id present");
    EXPECT(r0.value("connection_id", "").rfind("ws_0x", 0) == 0,
           "connection_id format ws_0x...");
    std::string conn_id = r0.value("connection_id", "");
    std::cerr << "  info: conn_id = " << conn_id << "\n";

    // 2. Send text
    auto r1 = send.execute(json{{"connection_id", conn_id},
                               {"message", "hello ws"}}, fileio);
    EXPECT(r1.value("status", "") == "success", "send text success");
    EXPECT(r1.value("bytes_sent", 0) == 8, "sent 8 bytes");

    // 3. Receive echo
    auto r2 = recv.execute(json{{"connection_id", conn_id},
                               {"timeout_seconds", 5}}, fileio);
    EXPECT(r2.value("status", "") == "success", "recv text success");
    EXPECT(r2.value("type", "") == "text", "type=text");
    EXPECT(r2.value("payload", "") == "hello ws", "payload echoed");

    // 4. Send binary (base64 of "deadbeef")
    // "deadbeef" -> base64 "ZGVhZGJlZWY=" (8 bytes decoded)
    auto r3 = send.execute(json{{"connection_id", conn_id},
                               {"message_base64", "ZGVhZGJlZWY="}}, fileio);
    EXPECT(r3.value("status", "") == "success", "send binary success");
    EXPECT(r3.value("type", "") == "binary", "type=binary");
    EXPECT(r3.value("bytes_sent", 0) == 8, "sent 8 bytes");

    // 5. Receive binary echo
    auto r4 = recv.execute(json{{"connection_id", conn_id},
                               {"timeout_seconds", 5}}, fileio);
    EXPECT(r4.value("status", "") == "success", "recv binary success");
    EXPECT(r4.value("type", "") == "binary", "type=binary");
    EXPECT(r4.value("payload_base64", "") == "ZGVhZGJlZWY=", "base64 echo matches");
    EXPECT(r4.value("bytes", 0) == 8, "8 bytes");

    // 6. Close cleanly
    auto r5 = close.execute(json{{"connection_id", conn_id},
                                {"code", 1000},
                                {"reason", "test done"}}, fileio);
    EXPECT(r5.value("status", "") == "success", "close success");

    // 7. After close, the registry should not have this id
    auto r6 = send.execute(json{{"connection_id", conn_id},
                               {"message", "after close"}}, fileio);
    EXPECT(r6.value("status", "") == "error", "send after close -> error");
}

static void test_invalid_close_code() {
    std::cerr << "test_invalid_close_code\n";
    FileIO fileio(std::filesystem::current_path());
    WebSocketCloseTool close;

    // code < 1000 is invalid per RFC 6455
    auto r1 = close.execute(json{{"connection_id", "ws_x"}, {"code", 999}}, fileio);
    EXPECT(r1.value("status", "") == "error", "code=999 -> error");

    auto r2 = close.execute(json{{"connection_id", "ws_x"}, {"code", 5000}}, fileio);
    EXPECT(r2.value("status", "") == "error", "code=5000 -> error");
}

// Regression: WsRegistry::get() used to return a raw Entry* that the
// reaper could destroy underneath the caller. Now it returns shared_ptr
// so the entry stays alive while held. This test verifies the contract:
//   1. get() returns a shared_ptr with non-zero refcount.
//   2. close() removes from the registry map (a second get() returns null).
//   3. The shared_ptr we kept still points to a valid Entry whose ws
//      hasn't been destroyed (no use-after-free).
static void test_registry_get_returns_shared_ptr() {
    std::cerr << "test_registry_get_returns_shared_ptr\n";
    EchoServer srv;
    FileIO fileio(std::filesystem::current_path());

    WebSocketConnectTool connect;
    auto r0 = connect.execute(json{{"url", srv.url()}, {"timeout_seconds", 5}}, fileio);
    EXPECT(r0.value("status", "") == "success", "connect for registry test");
    std::string conn_id = r0.value("connection_id", "");
    if (r0.value("status", "") != "success") return;

    // Grab a shared_ptr to the entry (the API now returns this).
    auto entry = WsRegistry::instance().get(conn_id);
    EXPECT(entry != nullptr, "get() returns non-null shared_ptr");
    EXPECT(entry.use_count() >= 2,
           "shared_ptr refcount >= 2 (held by registry + caller)");
    EXPECT(entry->ws != nullptr, "entry has a Connection");

    // Close via the tool — removes the entry from the registry map.
    WebSocketCloseTool closer;
    auto r1 = closer.execute(json{{"connection_id", conn_id}}, fileio);
    EXPECT(r1.value("status", "") == "success", "close succeeded");

    // Registry map no longer has this id.
    EXPECT(WsRegistry::instance().get(conn_id) == nullptr,
           "second get() returns null after close");

    // But our local shared_ptr is still valid — the Entry is alive
    // because refcount is still 1 (held by us). If the old raw-pointer
    // code were still in place, this would be a use-after-free.
    EXPECT(entry.use_count() >= 1,
           "local shared_ptr still has refcount >= 1 after close");
    EXPECT(entry->ws != nullptr,
           "local shared_ptr's ws still non-null (no UAF)");
    EXPECT(entry->url == srv.url(),
           "local shared_ptr's url field still readable");
}

// ---------- main ----------

int main() {
    std::cerr << "=== WebSocket tool tests ===\n";
    test_url_validation();
    test_tool_definitions();
    test_unknown_connection_id();
    test_send_missing_payload();
    test_connect_to_real_server_and_echo();
    test_invalid_close_code();
    test_registry_get_returns_shared_ptr();
    // Note: a live wss:// round-trip test would require a public wss
    // endpoint that resolves + allows unauthenticated echo (e.g. echo.websocket.events,
    // which doesn't resolve in this build environment). The ws:// loopback
    // test above proves the same code path; the wss:// TLS handshake uses
    // the same TLSSocket that http_request_tests and network_diagnostics_tests
    // exercise against https://api.github.com (see ctest with
    // OMNINET_HTTPS_TEST_URL set).
    std::cerr << "\nResults: " << (failures == 0 ? "all passed" : "FAILED (" + std::to_string(failures) + ")") << "\n";
    return failures > 0 ? 1 : 0;
}