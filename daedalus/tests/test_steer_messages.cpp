// SPDX-License-Identifier: BSL-1.0
// §STEER-MESSAGE: mid-turn guidance is appended only after a COMPLETE
// assistant tool-call batch and all of its tool results. The fake provider
// deliberately returns two calls in one response; a user message between
// those results would violate the OpenAI-compatible conversation contract.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "api/LLMClient.h"

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

class SteerServer {
public:
    SteerServer() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) throw std::runtime_error("socket() failed");
        int yes = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr),
                   sizeof(addr)) < 0)
            throw std::runtime_error("bind failed");
        if (::listen(listen_fd_, 4) < 0)
            throw std::runtime_error("listen failed");
        socklen_t len = sizeof(addr);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { loop(); });
    }

    ~SteerServer() {
        release_first_response();
        running_.store(false);
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        if (thread_.joinable()) thread_.join();
    }

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

    bool wait_for_first_request() {
        std::unique_lock lock(gate_mutex_);
        return gate_cv_.wait_for(lock, 5s, [this] { return first_seen_; });
    }

    void release_first_response() {
        {
            std::lock_guard lock(gate_mutex_);
            release_first_ = true;
        }
        gate_cv_.notify_all();
    }

    std::vector<nlohmann::json> requests() const {
        std::lock_guard lock(requests_mutex_);
        return requests_;
    }

private:
    void loop() {
        while (running_.load()) {
            sockaddr_in caddr{};
            socklen_t clen = sizeof(caddr);
            const int cfd = ::accept(
                listen_fd_, reinterpret_cast<sockaddr*>(&caddr), &clen);
            if (cfd < 0) break;
            handle_one(cfd);
            ::close(cfd);
        }
    }

    void handle_one(int cfd) {
        std::string in;
        char buf[8192];
        size_t content_length = 0;
        size_t body_start = std::string::npos;
        while (true) {
            const ssize_t n = ::recv(cfd, buf, sizeof(buf), 0);
            if (n <= 0) return;
            in.append(buf, static_cast<size_t>(n));
            body_start = in.find("\r\n\r\n");
            if (body_start != std::string::npos) {
                const auto cl = in.find("Content-Length:");
                if (cl != std::string::npos) {
                    content_length = static_cast<size_t>(
                        std::strtoul(in.c_str() + cl + 15, nullptr, 10));
                }
                break;
            }
        }
        while (in.size() - (body_start + 4) < content_length) {
            const ssize_t n = ::recv(cfd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            in.append(buf, static_cast<size_t>(n));
        }

        int seq = 0;
        {
            std::lock_guard lock(requests_mutex_);
            requests_.push_back(
                nlohmann::json::parse(in.substr(body_start + 4)));
            seq = static_cast<int>(requests_.size() - 1);
        }

        if (seq == 0) {
            {
                std::unique_lock lock(gate_mutex_);
                first_seen_ = true;
                gate_cv_.notify_all();
                gate_cv_.wait(lock, [this] { return release_first_; });
            }
        }

        const std::string body = seq == 0 ? multi_tool_response()
                                          : stop_response();
        const std::string response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "Connection: close\r\n\r\n" + body;
        ::send(cfd, response.data(), response.size(), 0);
    }

    static std::string multi_tool_response() {
        return std::string(
            R"({"id":"chatcmpl-steer","object":"chat.completion","choices":[)"
            R"({"index":0,"message":{"role":"assistant","content":null,)"
            R"("tool_calls":[)"
            R"({"id":"call_a","type":"function","function":{"name":"set_status",)"
            R"("arguments":"{\"state\":\"Planning\"}"}},)"
            R"({"id":"call_b","type":"function","function":{"name":"set_status",)"
            R"("arguments":"{\"state\":\"Acting\"}"}}]},)"
            R"("finish_reason":"tool_calls"}],)"
            R"("usage":{"prompt_tokens":10,"completion_tokens":5,"total_tokens":15}})");
    }

    static std::string stop_response() {
        return std::string(
            R"({"id":"chatcmpl-steer","object":"chat.completion","choices":[)"
            R"({"index":0,"message":{"role":"assistant","content":null,)"
            R"("tool_calls":[{"id":"call_stop","type":"function",)"
            R"("function":{"name":"stop",)"
            R"("arguments":"{\"summary\":\"complete\"}"}}]},)"
            R"("finish_reason":"tool_calls"}],)"
            R"("usage":{"prompt_tokens":10,"completion_tokens":5,"total_tokens":15}})");
    }

    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{true};
    std::thread thread_;
    std::mutex gate_mutex_;
    std::condition_variable gate_cv_;
    bool first_seen_ = false;
    bool release_first_ = false;
    mutable std::mutex requests_mutex_;
    std::vector<nlohmann::json> requests_;
};

struct ScratchDir {
    fs::path path;
    ScratchDir() {
        path = fs::temp_directory_path()
               / ("daedalus_steer_test_"
                  + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
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
    cfg.plugin = "openai-compatible";
    cfg.target_directory = target;
    cfg.memory_disabled = true;
    cfg.stream = false;
    return cfg;
}

} // namespace

TEST_CASE("steer follows every result in a multi-tool batch") {
    SteerServer server;
    ScratchDir scratch;
    LLMClient client(make_config(server.base_url(), scratch.path));
    client.initialize();

    AgentState result = AgentState::CANCELLED;
    std::jthread turn([&] { result = client.run_one_turn("start", "user:test"); });

    REQUIRE(server.wait_for_first_request());
    client.submit_steer_message("first correction", "user:lee");
    client.submit_steer_message("second correction", "user:lee");
    CHECK(client.pending_steer_count() == 2);
    server.release_first_response();
    turn.join();

    CHECK(result == AgentState::DONE);
    CHECK(client.pending_steer_count() == 0);
    const auto requests = server.requests();
    REQUIRE(requests.size() == 2);
    const auto& messages = requests[1]["messages"];
    REQUIRE(messages.size() >= 5);

    // Locate the multi-call assistant explicitly. A transient context-status
    // system message normally follows the steers on the next request.
    size_t base = messages.size();
    for (size_t i = 0; i < messages.size(); ++i) {
        if (messages[i]["role"] == "assistant"
            && messages[i].contains("tool_calls")
            && messages[i]["tool_calls"].size() == 2
            && messages[i]["tool_calls"][0]["id"] == "call_a") {
            base = i;
            break;
        }
    }
    REQUIRE(base + 4 < messages.size());
    REQUIRE(messages[base]["tool_calls"].size() == 2);
    CHECK(messages[base + 1]["role"] == "tool");
    CHECK(messages[base + 1]["tool_call_id"] == "call_a");
    CHECK(messages[base + 2]["role"] == "tool");
    CHECK(messages[base + 2]["tool_call_id"] == "call_b");
    CHECK(messages[base + 3]["role"] == "user");
    CHECK(messages[base + 3]["content"].get<std::string>().find(
              "first correction") != std::string::npos);
    CHECK(messages[base + 4]["role"] == "user");
    CHECK(messages[base + 4]["content"].get<std::string>().find(
              "second correction") != std::string::npos);
}

TEST_CASE("uninjected steers can be cleared without touching normal history") {
    ScratchDir scratch;
    LLMClient client(make_config("http://127.0.0.1:1", scratch.path));
    client.submit_steer_message("discard me");
    CHECK(client.pending_steer_count() == 1);
    client.clear_steer_messages();
    CHECK(client.pending_steer_count() == 0);
    CHECK(client.history_snapshot().empty());
}
