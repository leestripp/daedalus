/**
 * @file test_status_only_breaker.cpp
 * @brief set_status terminal and same-kind runaway contract
 *        (§STATUS-KIND-BREAKER, 2026-10-25).
 *
 * Done is an intentional terminal signal for models that use set_status to
 * stop. Other states report UI progress only; the third consecutive
 * status-only iteration of the SAME kind suppresses a runaway. Mixed kinds
 * (Planning -> Acting -> Verifying) must never trip that breaker. Unknown
 * states return corrective feedback to the model and also continue the loop.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "api/LLMClient.h"

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// In-process HTTP server that answers every POST /v1/chat/completions with a
// valid chat completion. Two modes:
//   ALL_STATUS       — every response is ONE set_status("Done") tool call
//   STATUS_THEN_TEXT — alternates set_status and a normal text completion
//   STATUS_THEN_STOP — three set_status calls, then stop; repeats per turn
// Tool call ids are unique per request (call_0, call_1, ...).
// ---------------------------------------------------------------------------
class StatusOnlyServer {
public:
    enum Mode {
        ALL_STATUS, STATUS_THEN_TEXT, STATUS_THEN_STOP,
        SAME_STATE, RESET_THEN_STOP, CYCLING_STATUS, INVALID_THEN_STOP,
    };

    explicit StatusOnlyServer(Mode mode) : mode_(mode) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) throw std::runtime_error("socket() failed");
        int yes = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) < 0)
            throw std::runtime_error("bind failed");
        if (::listen(listen_fd_, 4) < 0)
            throw std::runtime_error("listen failed");

        socklen_t len = sizeof(addr);
        ::getsockname(listen_fd_, (sockaddr*)&addr, &len);
        port_ = ntohs(addr.sin_port);

        running_.store(true);
        thread_ = std::thread([this] { loop(); });
    }

    ~StatusOnlyServer() {
        running_.store(false);
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        if (thread_.joinable()) thread_.join();
    }

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }
    int request_count() const { return request_count_.load(); }
    std::vector<std::string> request_bodies() const {
        std::lock_guard lock(requests_mutex_);
        return request_bodies_;
    }

private:
    void loop() {
        while (running_.load()) {
            sockaddr_in caddr{};
            socklen_t clen = sizeof(caddr);
            int cfd = ::accept(listen_fd_, (sockaddr*)&caddr, &clen);
            if (cfd < 0) break;
            handle_one(cfd);
            ::close(cfd);
        }
    }

    void handle_one(int cfd) {
        // Read headers, then drain the body per Content-Length.
        std::string in;
        char buf[8192];
        size_t content_length = 0;
        size_t body_start = std::string::npos;
        while (true) {
            ssize_t n = ::recv(cfd, buf, sizeof(buf), 0);
            if (n <= 0) return;
            in.append(buf, static_cast<size_t>(n));
            body_start = in.find("\r\n\r\n");
            if (body_start != std::string::npos) {
                auto cl = in.find("Content-Length:");
                if (cl != std::string::npos) {
                    content_length = static_cast<size_t>(
                        std::strtoul(in.c_str() + cl + 15, nullptr, 10));
                }
                break;
            }
            if (in.size() > 1024 * 1024) return;
        }
        while (in.size() - (body_start + 4) < content_length) {
            ssize_t n = ::recv(cfd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            in.append(buf, static_cast<size_t>(n));
        }

        {
            std::lock_guard lock(requests_mutex_);
            request_bodies_.push_back(in.substr(body_start + 4));
        }

        const int seq = request_count_.fetch_add(1);
        std::string body;
        static constexpr std::string_view phases[] = {
            "Planning", "Acting", "Verifying", "Documenting"};
        switch (mode_) {
        case ALL_STATUS:
            body = status_tool_call_response(seq, "Done");
            break;
        case STATUS_THEN_TEXT:
            body = seq % 2 == 0 ? status_tool_call_response(seq, "Planning")
                                : text_response();
            break;
        case STATUS_THEN_STOP:
            body = seq < 3 ? status_tool_call_response(seq, phases[seq])
                           : stop_tool_call_response(seq);
            break;
        case SAME_STATE:
            body = status_tool_call_response(seq, "Planning");
            break;
        case RESET_THEN_STOP:
            if (seq < 2) body = status_tool_call_response(seq, "Planning");
            else if (seq < 4) body = status_tool_call_response(seq, "Acting");
            else body = stop_tool_call_response(seq);
            break;
        case CYCLING_STATUS:
            body = status_tool_call_response(seq, phases[seq % 4]);
            break;
        case INVALID_THEN_STOP:
            body = seq == 0 ? status_tool_call_response(seq, "NotAState")
                            : stop_tool_call_response(seq);
            break;
        }

        const std::string resp =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "Connection: close\r\n\r\n" + body;
        ::send(cfd, resp.data(), resp.size(), 0);
    }

    static std::string status_tool_call_response(int seq, std::string_view state) {
        return std::string(
            R"({"id":"chatcmpl-test","object":"chat.completion","choices":[)"
            R"({"index":0,"message":{"role":"assistant","content":null,)"
            R"("tool_calls":[{"id":"call_)" + std::to_string(seq) +
            R"(","type":"function","function":{"name":"set_status",)"
            R"("arguments":"{\"state\": \")" + std::string(state) +
            R"(\"}"}}]},)"
            R"("finish_reason":"tool_calls"}],)"
            R"("usage":{"prompt_tokens":10,"completion_tokens":5,"total_tokens":15}})");
    }

    static std::string stop_tool_call_response(int seq) {
        return std::string(
            R"({"id":"chatcmpl-test","object":"chat.completion","choices":[)"
            R"({"index":0,"message":{"role":"assistant","content":null,)"
            R"("tool_calls":[{"id":"call_)" + std::to_string(seq) +
            R"(","type":"function","function":{"name":"stop",)"
            R"("arguments":"{\"summary\": \"complete\"}"}}]},)"
            R"("finish_reason":"tool_calls"}],)"
            R"("usage":{"prompt_tokens":10,"completion_tokens":5,"total_tokens":15}})");
    }

    static std::string text_response() {
        return std::string(
            R"({"id":"chatcmpl-test","object":"chat.completion","choices":[)"
            R"({"index":0,"message":{"role":"assistant",)"
            R"("content":"All done."},"finish_reason":"stop"}],)"
            R"("usage":{"prompt_tokens":10,"completion_tokens":5,"total_tokens":15}})");
    }

    Mode mode_;
    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<int> request_count_{0};
    mutable std::mutex requests_mutex_;
    std::vector<std::string> request_bodies_;
    std::thread thread_;
};

struct ScratchDir {
    fs::path path;
    explicit ScratchDir(const std::string& tag) {
        path = fs::temp_directory_path() /
               ("daedalus_test_" + tag + "_" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

AppConfig make_config(const std::string& base_url, const fs::path& target) {
    AppConfig cfg;
    cfg.base_url = base_url;
    cfg.chat_endpoint = "/v1/chat/completions";
    cfg.api_key = "test-key";
    cfg.model = "test-model";
    cfg.plugin = "openai-compatible";  // loopback hosts auto-detect lmstudio
    cfg.target_directory = target;
    cfg.memory_disabled = true;
    cfg.stream = false;  // buffered JSON path — no SSE needed from the fake
    return cfg;
}

} // namespace

TEST_SUITE("set_status terminal + same-kind breaker (§STATUS-KIND-BREAKER)") {

    TEST_CASE("Done is an intentional immediate loop exit") {
        StatusOnlyServer server(StatusOnlyServer::ALL_STATUS);
        ScratchDir scratch("status-done-terminal");
        LLMClient client(make_config(server.base_url(), scratch.path));
        client.initialize();
        CHECK(client.run_one_turn("hello") == AgentState::DONE);
        CHECK(server.request_count() == 1);
    }

    TEST_CASE("different status kinds continue until explicit stop") {
        StatusOnlyServer server(StatusOnlyServer::STATUS_THEN_STOP);
        ScratchDir scratch("status-kind-transitions");
        LLMClient client(make_config(server.base_url(), scratch.path));
        client.initialize();
        CHECK(client.run_one_turn("hello") == AgentState::DONE);
        CHECK(server.request_count() == 4);
    }

    TEST_CASE("third consecutive same non-terminal kind ends a runaway") {
        StatusOnlyServer server(StatusOnlyServer::SAME_STATE);
        ScratchDir scratch("status-same-kind-three");
        LLMClient client(make_config(server.base_url(), scratch.path));
        client.initialize();
        CHECK(client.run_one_turn("hello") == AgentState::DONE);
        CHECK(server.request_count() == 3);
    }

    TEST_CASE("changing kind resets the repetition streak") {
        StatusOnlyServer server(StatusOnlyServer::RESET_THEN_STOP);
        ScratchDir scratch("status-kind-reset");
        LLMClient client(make_config(server.base_url(), scratch.path));
        client.initialize();
        CHECK(client.run_one_turn("hello") == AgentState::DONE);
        CHECK(server.request_count() == 5);
    }

    TEST_CASE("text completion and a new turn reset the streak") {
        StatusOnlyServer server(StatusOnlyServer::STATUS_THEN_TEXT);
        ScratchDir scratch("status-text-reset");
        LLMClient client(make_config(server.base_url(), scratch.path));
        client.initialize();
        CHECK(client.run_one_turn("first") == AgentState::DONE);
        CHECK(server.request_count() == 2);
        CHECK(client.run_one_turn("second") == AgentState::DONE);
        CHECK(server.request_count() == 4);
    }

    TEST_CASE("unknown state reports the error and the loop continues") {
        StatusOnlyServer server(StatusOnlyServer::INVALID_THEN_STOP);
        ScratchDir scratch("status-invalid-continues");
        LLMClient client(make_config(server.base_url(), scratch.path));
        client.initialize();
        CHECK(client.run_one_turn("hello") == AgentState::DONE);
        // The invalid value did not terminate. A second iteration reached
        // stop, and its request history contains the corrective tool result.
        REQUIRE(server.request_count() == 2);
        const auto requests = server.request_bodies();
        REQUIRE(requests.size() == 2);
        CHECK(requests[1].find("Unknown state") != std::string::npos);
        CHECK(requests[1].find("NotAState") != std::string::npos);
        CHECK(requests[1].find("Valid:") != std::string::npos);
    }

    TEST_CASE("max-iteration guard covers cycling non-terminal kinds") {
        StatusOnlyServer server(StatusOnlyServer::CYCLING_STATUS);
        ScratchDir scratch("status-cycling-max-iterations");
        auto store = daedalus::SettingsStore::for_home(scratch.path);
        store.set_max_agent_iterations(4);
        LLMClient client(make_config(server.base_url(), scratch.path), &store);
        client.initialize();
        CHECK(client.run_one_turn("hello") == AgentState::DONE);
        CHECK(server.request_count() == 4);
    }
}
