// tests/test_network_diagnostics.cpp
//
// Unit tests for the three P3 network diagnostic tools:
//   - DnsLookupTool  (dns_lookup)
//   - TcpCheckTool   (tcp_check)
//   - TlsInspectTool (tls_inspect)
//
// We use a tiny in-process HTTP server fixture (same as test_http_request.cpp)
// for tcp_check success cases. The tls_inspect real-Internet test is gated
// on OMNINET_TLS_INSPECT_TEST_HOST being set, so CI / offline machines
// skip it cleanly.

#include "NetworkTool.h"
#include "core/FileIO.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

using json = nlohmann::json;

static int failures = 0;
#define EXPECT(cond, msg) do { \
    if (!(cond)) { ++failures; std::cerr << "  FAIL: " << msg << std::endl; } \
    else         { std::cerr << "  OK:   " << msg << std::endl; } \
} while (0)

// ---------- Tiny TCP server fixture ----------

class TinyServer {
public:
    TinyServer() : running_(false) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) throw std::runtime_error("socket() failed");
        int yes = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) throw std::runtime_error("bind failed");
        if (::listen(listen_fd_, 4) < 0) throw std::runtime_error("listen failed");

        socklen_t len = sizeof(addr);
        ::getsockname(listen_fd_, (sockaddr*)&addr, &len);
        port_ = ntohs(addr.sin_port);

        running_.store(true);
        thread_ = std::thread([this] { loop(); });
    }

    ~TinyServer() {
        running_.store(false);
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        if (thread_.joinable()) thread_.join();
    }

    int port() const { return port_; }
    int connections_accepted() const { return count_.load(); }

private:
    void loop() {
        while (running_.load()) {
            sockaddr_in caddr{};
            socklen_t clen = sizeof(caddr);
            int cfd = ::accept(listen_fd_, (sockaddr*)&caddr, &clen);
            if (cfd < 0) break;
            ++count_;
            // Drain whatever the client sent, then close.
            char buf[256];
            // Use non-blocking-ish: just recv what's there with a short timeout.
            timeval tv{1, 0};
            ::setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            while (true) {
                ssize_t n = ::recv(cfd, buf, sizeof(buf), 0);
                if (n <= 0) break;
            }
            ::close(cfd);
        }
    }

    std::atomic<bool> running_;
    std::atomic<int> count_{0};
    int listen_fd_ = -1;
    int port_ = 0;
    std::thread thread_;
};

// ---------- DnsLookupTool ----------

static void test_dns_lookup() {
    std::cerr << "test_dns_lookup\n";
    FileIO fileio(std::filesystem::current_path());
    DnsLookupTool tool;

    // missing hostname
    auto r0 = tool.execute(json::object(), fileio);
    EXPECT(r0.value("status", "") == "error", "missing hostname -> error");

    // invalid record_type
    auto r1 = tool.execute(json{{"hostname", "localhost"}, {"record_type", "PTR"}}, fileio);
    EXPECT(r1.value("status", "") == "error", "PTR record_type -> error");

    // localhost (should resolve to 127.0.0.1)
    auto r2 = tool.execute(json{{"hostname", "localhost"}, {"record_type", "A"}}, fileio);
    EXPECT(r2.value("status", "") == "success", "localhost A -> success");
    EXPECT(r2.value("count", 0) >= 1, "at least 1 record");
    if (r2.contains("records") && !r2["records"].empty()) {
        EXPECT(r2["records"][0]["family"] == "IPv4", "localhost first record is IPv4");
        EXPECT(r2["records"][0]["address"] == "127.0.0.1", "localhost address is 127.0.0.1");
    }

    // ANY -> may include IPv4 + IPv6
    auto r3 = tool.execute(json{{"hostname", "localhost"}, {"record_type", "ANY"}}, fileio);
    EXPECT(r3.value("status", "") == "success", "localhost ANY -> success");
    EXPECT(r3.value("v4_count", 0) >= 1, "ANY has at least 1 IPv4 record");

    // nonexistent hostname
    auto r4 = tool.execute(
        json{{"hostname", "this-host-definitely-does-not-exist-daedalus-test.invalid"}},
        fileio);
    EXPECT(r4.value("status", "") == "error", "nonexistent hostname -> error");
    EXPECT(r4.value("message", "").size() > 0, "nonexistent error message non-empty");
}

static void test_dns_tool_definition() {
    std::cerr << "test_dns_tool_definition\n";
    DnsLookupTool tool;
    auto def = tool.get_definition();
    EXPECT(def["function"]["name"] == "dns_lookup", "name == dns_lookup");
    EXPECT(def["function"]["parameters"]["required"][0] == "hostname",
           "required = [hostname]");
    EXPECT(def["function"]["parameters"]["properties"]["record_type"].contains("enum"),
           "record_type is enum");
}

// ---------- TcpCheckTool ----------

static void test_tcp_check() {
    std::cerr << "test_tcp_check\n";
    FileIO fileio(std::filesystem::current_path());
    TcpCheckTool tool;

    // missing host
    auto r0 = tool.execute(json{{"port", 80}}, fileio);
    EXPECT(r0.value("status", "") == "error", "missing host -> error");

    // invalid port
    auto r1 = tool.execute(json{{"host", "127.0.0.1"}, {"port", 0}}, fileio);
    EXPECT(r1.value("status", "") == "error", "port 0 -> error");

    auto r1b = tool.execute(json{{"host", "127.0.0.1"}, {"port", 70000}}, fileio);
    EXPECT(r1b.value("status", "") == "error", "port 70000 -> error");

    // unreachable port
    auto r2 = tool.execute(
        json{{"host", "127.0.0.1"}, {"port", 1}, {"timeout_seconds", 2}},
        fileio);
    EXPECT(r2.value("status", "") == "error", "port 1 -> error");
    EXPECT(r2.value("reachable", true) == false, "reachable=false on failure");
    EXPECT(r2.contains("elapsed_ms"), "elapsed_ms present on failure");

    // reachable: connect to our tiny in-process server
    {
        TinyServer srv;
        auto r3 = tool.execute(
            json{{"host", "127.0.0.1"}, {"port", srv.port()}, {"timeout_seconds", 2}},
            fileio);
        EXPECT(r3.value("status", "") == "success", "tiny server reachable");
        EXPECT(r3.value("reachable", false) == true, "reachable=true");
        EXPECT(r3.value("port", 0) == srv.port(), "port matches");
        EXPECT(r3.contains("elapsed_ms"), "elapsed_ms present");
    }
}

static void test_tcp_tool_definition() {
    std::cerr << "test_tcp_tool_definition\n";
    TcpCheckTool tool;
    auto def = tool.get_definition();
    EXPECT(def["function"]["name"] == "tcp_check", "name == tcp_check");
    auto req = def["function"]["parameters"]["required"];
    bool has_host = false, has_port = false;
    for (const auto& r : req) {
        if (r == "host") has_host = true;
        if (r == "port") has_port = true;
    }
    EXPECT(has_host && has_port, "required = [host, port]");
}

// ---------- TlsInspectTool ----------

static void test_tls_inspect_local_validation() {
    std::cerr << "test_tls_inspect_local_validation\n";
    FileIO fileio(std::filesystem::current_path());
    TlsInspectTool tool;

    // missing host
    auto r0 = tool.execute(json::object(), fileio);
    EXPECT(r0.value("status", "") == "error", "missing host -> error");

    // invalid port
    auto r1 = tool.execute(json{{"host", "127.0.0.1"}, {"port", 0}}, fileio);
    EXPECT(r1.value("status", "") == "error", "port 0 -> error");

    // unreachable TLS port
    auto r2 = tool.execute(
        json{{"host", "127.0.0.1"}, {"port", 1}, {"timeout_seconds", 2}},
        fileio);
    EXPECT(r2.value("status", "") == "error", "TLS port 1 -> error");
    EXPECT(r2.value("message", "").size() > 0, "error message non-empty");
    EXPECT(r2.contains("elapsed_ms"), "elapsed_ms present");
}

static void test_tls_tool_definition() {
    std::cerr << "test_tls_tool_definition\n";
    TlsInspectTool tool;
    auto def = tool.get_definition();
    EXPECT(def["function"]["name"] == "tls_inspect", "name == tls_inspect");
    auto req = def["function"]["parameters"]["required"];
    bool has_host = false;
    for (const auto& r : req) if (r == "host") has_host = true;
    EXPECT(has_host, "required = [host]");
    EXPECT(def["function"]["parameters"]["properties"].contains("port"),
           "properties has port");
    EXPECT(def["function"]["parameters"]["properties"]["port"]["default"] == 443,
           "default port == 443");
}

// Real-Internet test gated by env var so CI / offline machines skip it.
static void test_tls_inspect_real_internet() {
    const char* host_env = std::getenv("OMNINET_TLS_INSPECT_TEST_HOST");
    if (!host_env || !*host_env) {
        std::cerr << "test_tls_inspect_real_internet SKIPPED"
                     " (OMNINET_TLS_INSPECT_TEST_HOST not set)\n";
        return;
    }
    std::cerr << "test_tls_inspect_real_internet " << host_env << "\n";

    FileIO fileio(std::filesystem::current_path());
    TlsInspectTool tool;
    auto r = tool.execute(json{
        {"host", host_env},
        {"port", 443},
        {"timeout_seconds", 10}
    }, fileio);

    EXPECT(r.value("status", "") == "success", "TLS inspect succeeds");
    EXPECT(r.contains("tls_version"), "tls_version field present");
    EXPECT(r.value("tls_version", "") != "", "tls_version non-empty");
    EXPECT(r.contains("cipher"), "cipher field present");
    EXPECT(r.value("cipher", "") != "", "cipher non-empty");
    EXPECT(r.contains("certificate"), "certificate field present");
    EXPECT(r["certificate"].value("present", false) == true, "cert present");
    EXPECT(r["certificate"].value("subject", "").size() > 0, "cert subject non-empty");
    EXPECT(r["certificate"].value("issuer", "").size() > 0, "cert issuer non-empty");
    EXPECT(r["certificate"].value("not_before", "").size() > 0, "cert not_before non-empty");
    EXPECT(r["certificate"].value("not_after", "").size() > 0, "cert not_after non-empty");
    EXPECT(r["certificate"].contains("days_until_expiry"), "days_until_expiry present");
    EXPECT(r["certificate"].contains("signature_algorithm"), "signature_algorithm present");
    EXPECT(r["certificate"]["public_key"].contains("algorithm"), "public_key algorithm present");
    EXPECT(r["certificate"]["public_key"].contains("size_bits"), "public_key size_bits present");
    EXPECT(r["certificate"].contains("subject_alt_names"), "SAN list present");
    EXPECT(r["certificate"].value("fingerprint_sha256", "").size() == 64,
           "fingerprint 64 hex chars (SHA-256)");
    EXPECT(r.value("peer_cert_verified", false) == true,
           "peer cert verified by system CA");
}

// ---------- main ----------

int main() {
    std::cerr << "=== Network diagnostic tool tests ===\n";
    test_dns_lookup();
    test_dns_tool_definition();
    test_tcp_check();
    test_tcp_tool_definition();
    test_tls_inspect_local_validation();
    test_tls_tool_definition();
    test_tls_inspect_real_internet();
    std::cerr << "\nResults: " << (failures == 0 ? "all passed" : "FAILED (" + std::to_string(failures) + ")") << "\n";
    return failures > 0 ? 1 : 0;
}