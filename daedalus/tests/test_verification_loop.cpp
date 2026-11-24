// SPDX-License-Identifier: BSL-1.0
// §VerificationLoop: worker → verifier supervision over one shared stream.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "api/LLMClient.h"
#include "core/BackgroundProcessManager.h"
#include "daedalus/AgentSink.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

json text_response(std::string text) {
    return {
        {"id", "chatcmpl-verification"},
        {"object", "chat.completion"},
        {"choices", json::array({{
            {"index", 0},
            {"message", {{"role", "assistant"}, {"content", std::move(text)}}},
            {"finish_reason", "stop"},
        }})},
        {"usage", {{"prompt_tokens", 10}, {"completion_tokens", 5},
                   {"total_tokens", 15}}},
    };
}

json status_response(std::string_view state) {
    return {
        {"id", "chatcmpl-verification"},
        {"object", "chat.completion"},
        {"choices", json::array({{
            {"index", 0},
            {"message", {
                {"role", "assistant"},
                {"content", nullptr},
                {"tool_calls", json::array({{
                    {"id", "call_status"},
                    {"type", "function"},
                    {"function", {
                        {"name", "set_status"},
                        {"arguments", json{{"state", state}}.dump()},
                    }},
                }})},
            }},
            {"finish_reason", "tool_calls"},
        }})},
        {"usage", {{"prompt_tokens", 10}, {"completion_tokens", 5},
                   {"total_tokens", 15}}},
    };
}

json bash_start_response(std::string command, std::string call_id,
                         bool detached = false) {
    return {
        {"id", "chatcmpl-verification"},
        {"object", "chat.completion"},
        {"choices", json::array({{
            {"index", 0},
            {"message", {
                {"role", "assistant"},
                {"content", nullptr},
                {"tool_calls", json::array({{
                    {"id", std::move(call_id)},
                    {"type", "function"},
                    {"function", {
                        {"name", "execute_bash"},
                        {"arguments", json{
                            {"action", "start"},
                            {"command", std::move(command)},
                            {"detached", detached},
                        }.dump()},
                    }},
                }})},
            }},
            {"finish_reason", "tool_calls"},
        }})},
        {"usage", {{"prompt_tokens", 10}, {"completion_tokens", 5},
                   {"total_tokens", 15}}},
    };
}

class VerificationServer {
public:
    explicit VerificationServer(std::vector<json> responses)
        : responses_(std::move(responses)) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) throw std::runtime_error("socket() failed");
        int yes = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr),
                   sizeof(addr)) < 0) {
            throw std::runtime_error("bind failed");
        }
        if (::listen(listen_fd_, 4) < 0)
            throw std::runtime_error("listen failed");
        socklen_t len = sizeof(addr);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { loop(); });
    }

    ~VerificationServer() {
        running_.store(false);
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        if (thread_.joinable()) thread_.join();
    }

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

    std::vector<json> requests() const {
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

        size_t seq = 0;
        {
            std::lock_guard lock(requests_mutex_);
            requests_.push_back(json::parse(in.substr(body_start + 4)));
            seq = requests_.size() - 1;
        }

        // An unexpected extra supervisor iteration receives an invalid verdict,
        // which cannot be mistaken for approval and fails the request-count check.
        const json body = seq < responses_.size()
            ? responses_[seq]
            : text_response("unexpected extra request");
        const std::string payload = body.dump();
        const std::string response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: " + std::to_string(payload.size()) + "\r\n"
            "Connection: close\r\n\r\n" + payload;
        ::send(cfd, response.data(), response.size(), 0);
    }

    std::vector<json> responses_;
    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{true};
    std::thread thread_;
    mutable std::mutex requests_mutex_;
    std::vector<json> requests_;
};

struct ScratchDir {
    fs::path path;
    ScratchDir() {
        path = fs::temp_directory_path()
            / ("daedalus_verification_test_"
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
    cfg.auto_mode = true;
    cfg.stream = false;
    cfg.verification_enabled = true;
    cfg.max_verification_rounds = 3;
    return cfg;
}

struct ErrorSink : daedalus::AgentSink {
    void on_error(daedalus::ErrorKind, std::string_view message,
                  bool will_retry) noexcept override {
        if (!will_retry) terminal_messages.emplace_back(message);
    }
    std::vector<std::string> terminal_messages;
};

size_t user_message_count(const json& request) {
    size_t count = 0;
    for (const auto& message : request.at("messages")) {
        if (message.value("role", "") == "user") ++count;
    }
    return count;
}

void check_stable_static_prefix(const std::vector<json>& requests) {
    REQUIRE(!requests.empty());
    for (const auto& request : requests) {
        CHECK(request.at("tools") == requests.front().at("tools"));
        REQUIRE(!request.at("messages").empty());
        CHECK(request.at("messages")[0] == requests.front().at("messages")[0]);
        // Only the real initial instruction uses the human/user role. Agent
        // handoffs ride assistant output plus the trailing system role prompt.
        CHECK(user_message_count(request) == 1);
    }
}

void install_completion_route(LLMClient& client) {
    ToolHandler::set_auto_mode(true);
    ToolHandler::set_background_completion_handler(
        [&client](const BackgroundProcessInfo& info, const ProcessOutput& output) {
            client.submit_steer_message(
                "Background completion " + info.process_id + "\n" + output.stdout,
                "trigger:background-process", info.process_id);
        });
}

void clear_completion_route() {
    ToolHandler::set_background_completion_handler({});
    BackgroundProcessManager::get_instance().cleanup_all();
}

bool request_contains(const json& request, std::string_view needle) {
    return request.dump().find(needle) != std::string::npos;
}

struct CompletionRouteGuard {
    CompletionRouteGuard() { clear_completion_route(); }
    ~CompletionRouteGuard() { clear_completion_route(); }
};

} // namespace

TEST_CASE("verification verdict parser accepts only the exact protocol") {
    CHECK(parse_verification_verdict("VERDICT: APPROVED\nLooks good")
          == VerificationVerdict::Approved);
    CHECK(parse_verification_verdict(" \nVERDICT: CHANGES_REQUIRED\r\nFix tests")
          == VerificationVerdict::ChangesRequired);
    CHECK(parse_verification_verdict("approved")
          == VerificationVerdict::Invalid);
    CHECK(parse_verification_verdict("VERDICT: APPROVED eventually")
          == VerificationVerdict::Invalid);
    CHECK(parse_verification_verdict("")
          == VerificationVerdict::Invalid);
}

TEST_CASE("worker completion is followed by verifier approval") {
    VerificationServer server({
        text_response("worker completed the implementation"),
        text_response("VERDICT: APPROVED\nVerified the implementation and tests."),
    });
    ScratchDir scratch;
    LLMClient client(make_config(server.base_url(), scratch.path));
    client.initialize();

    CHECK(client.run_one_turn("implement the feature", "user:test")
          == AgentState::DONE);
    const auto requests = server.requests();
    REQUIRE(requests.size() == 2);
    check_stable_static_prefix(requests);
    const auto& verification_messages = requests[1].at("messages");
    CHECK(verification_messages.back().at("role") == "system");
    CHECK(verification_messages.back().at("content").get<std::string>().find(
              "Verification Agent") != std::string::npos);
    CHECK(verification_messages.back().at("content").get<std::string>().find(
              "VERDICT: APPROVED") != std::string::npos);
    CHECK(client.get_last_assistant_text().find("Verified the implementation")
          != std::string::npos);
}

TEST_CASE("verifier corrections return to the worker and then re-verify") {
    VerificationServer server({
        text_response("initial worker attempt"),
        text_response("VERDICT: CHANGES_REQUIRED\nAdd the missing edge-case test."),
        text_response("worker added the edge-case test"),
        text_response("VERDICT: APPROVED\nAll checks now pass."),
    });
    ScratchDir scratch;
    LLMClient client(make_config(server.base_url(), scratch.path));
    client.initialize();

    CHECK(client.run_one_turn("implement the feature", "user:test")
          == AgentState::DONE);
    const auto requests = server.requests();
    REQUIRE(requests.size() == 4);
    check_stable_static_prefix(requests);

    const auto& correction_messages = requests[2].at("messages");
    CHECK(correction_messages.back().at("role") == "system");
    CHECK(correction_messages.back().at("content").get<std::string>().find(
              "Worker Agent Resumption") != std::string::npos);
    bool found_verifier_feedback = false;
    for (const auto& message : correction_messages) {
        if (message.value("role", "") == "assistant"
            && message.value("content", "").find("Add the missing edge-case test")
                != std::string::npos) {
            found_verifier_feedback = true;
        }
    }
    CHECK(found_verifier_feedback);
    CHECK(requests[3].at("messages").back().at("content")
              .get<std::string>().find("Verification Agent") != std::string::npos);
}

TEST_CASE("verification round exhaustion stops without approval") {
    VerificationServer server({
        text_response("worker one"),
        text_response("VERDICT: CHANGES_REQUIRED\nFirst correction."),
        text_response("worker two"),
        text_response("VERDICT: CHANGES_REQUIRED\nStill not satisfied."),
    });
    ScratchDir scratch;
    auto config = make_config(server.base_url(), scratch.path);
    config.max_verification_rounds = 2;
    LLMClient client(config);
    auto sink = std::make_shared<ErrorSink>();
    client.set_sink(sink);
    client.initialize();

    CHECK(client.run_one_turn("implement the feature", "user:test")
          == AgentState::CANCELLED);
    CHECK(server.requests().size() == 4);
    REQUIRE(sink->terminal_messages.size() == 1);
    CHECK(sink->terminal_messages.front().find("without verification approval")
          != std::string::npos);
}

TEST_CASE("inner Done status is held until the verifier approves") {
    VerificationServer server({
        status_response("Done"),
        text_response("VERDICT: APPROVED\nVerified."),
    });
    ScratchDir scratch;
    LLMClient client(make_config(server.base_url(), scratch.path));
    std::vector<daedalus::AgentState> states;
    client.set_state_change_callback(
        [&](daedalus::AgentState state) { states.push_back(state); });
    client.initialize();

    CHECK(client.run_one_turn("implement the feature", "user:test")
          == AgentState::DONE);
    CHECK(server.requests().size() == 2);
    REQUIRE(states.size() == 1);
    CHECK(states.front() == daedalus::AgentState::Verifying);
}


TEST_CASE("worker suspends for multiple required jobs and consumes both completions") {
    using namespace std::chrono_literals;
    CompletionRouteGuard cleanup_guard;
    VerificationServer server({
        bash_start_response("sleep 0.15; printf 'first-complete'", "call_first"),
        bash_start_response("sleep 0.25; printf 'second-complete'", "call_second"),
        text_response("worker is waiting for both jobs"),
        text_response("worker consumed both background results"),
        text_response("VERDICT: APPROVED\nBoth results were consumed."),
    });
    ScratchDir scratch;
    LLMClient client(make_config(server.base_url(), scratch.path));
    client.initialize();
    install_completion_route(client);

    CHECK(client.run_one_turn("run both jobs", "user:test") == AgentState::WAITING);
    REQUIRE(server.requests().size() == 3);
    std::this_thread::sleep_for(400ms);
    CHECK(client.pending_steer_count() == 2);

    CHECK(client.run_one_turn("", "trigger:background-process") == AgentState::DONE);
    const auto requests = server.requests();
    REQUIRE(requests.size() == 5);
    CHECK(request_contains(requests[3], "first-complete"));
    CHECK(request_contains(requests[3], "second-complete"));
    CHECK(requests[4].at("messages").back().at("content")
              .get<std::string>().find("Verification Agent") != std::string::npos);
    CHECK(client.pending_steer_count() == 0);
}

TEST_CASE("verifier suspends and resumes as verifier after its own background job") {
    using namespace std::chrono_literals;
    CompletionRouteGuard cleanup_guard;
    VerificationServer server({
        text_response("worker completed"),
        bash_start_response("sleep 0.15; printf 'verifier-complete'", "call_verify"),
        text_response("verifier is waiting for its check"),
        text_response("VERDICT: APPROVED\nThe asynchronous check passed."),
    });
    ScratchDir scratch;
    LLMClient client(make_config(server.base_url(), scratch.path));
    client.initialize();
    install_completion_route(client);

    CHECK(client.run_one_turn("verify asynchronously", "user:test")
          == AgentState::WAITING);
    REQUIRE(server.requests().size() == 3);
    std::this_thread::sleep_for(250ms);
    REQUIRE(client.pending_steer_count() == 1);

    CHECK(client.run_one_turn("", "trigger:background-process") == AgentState::DONE);
    const auto requests = server.requests();
    REQUIRE(requests.size() == 4);
    CHECK(request_contains(requests[3], "verifier-complete"));
    CHECK(requests[3].at("messages").back().at("role") == "system");
    CHECK(requests[3].at("messages").back().at("content")
              .get<std::string>().find("Verification Agent") != std::string::npos);
}

TEST_CASE("detached long-lived services do not suspend verified completion") {
    CompletionRouteGuard cleanup_guard;
    VerificationServer server({
        bash_start_response("sleep 30", "call_detached", true),
        text_response("worker left the service running detached"),
        text_response("VERDICT: APPROVED\nDetached service is intentional."),
    });
    ScratchDir scratch;
    LLMClient client(make_config(server.base_url(), scratch.path));
    client.initialize();
    install_completion_route(client);

    CHECK(client.run_one_turn("start the detached service", "user:test")
          == AgentState::DONE);
    CHECK(server.requests().size() == 3);
    CHECK(client.pending_steer_count() == 0);
}
