// SPDX-License-Identifier: BSL-1.0
#include "LoopbackServer.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace monkeybyte {
namespace {

using nlohmann::json;
constexpr std::size_t max_request_size = 4 * 1024 * 1024;

void close_fd(int& fd) noexcept {
    if (fd >= 0) ::close(fd);
    fd = -1;
}

bool send_all(int fd, std::string_view bytes) noexcept {
    while (!bytes.empty()) {
        const auto sent = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return false;
        bytes.remove_prefix(static_cast<std::size_t>(sent));
    }
    return true;
}

void set_timeouts(int fd) {
    timeval timeout{.tv_sec = 15, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

std::string reason_phrase(int status) {
    if (status == 200) return "OK";
    if (status == 400) return "Bad Request";
    if (status == 401) return "Unauthorized";
    if (status == 404) return "Not Found";
    return "Internal Server Error";
}

void send_response(int fd, int status, const json& body) {
    const auto payload = body.dump();
    const auto headers = "HTTP/1.1 " + std::to_string(status) + " " + reason_phrase(status) +
        "\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: " +
        std::to_string(payload.size()) + "\r\nConnection: close\r\n\r\n";
    if (send_all(fd, headers)) send_all(fd, payload);
}

struct Request {
    std::string method;
    std::string path;
    std::string token;
    json body = json::object();
};

Request read_request(int fd) {
    std::string bytes;
    char buffer[8192];
    std::size_t header_end = std::string::npos;
    while ((header_end = bytes.find("\r\n\r\n")) == std::string::npos) {
        const auto count = ::recv(fd, buffer, sizeof(buffer), 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("Incomplete HTTP request.");
        bytes.append(buffer, static_cast<std::size_t>(count));
        if (bytes.size() > max_request_size) throw std::runtime_error("HTTP request is too large.");
    }
    const auto first_end = bytes.find("\r\n");
    const auto first = bytes.substr(0, first_end);
    const auto method_end = first.find(' ');
    const auto path_end = first.find(' ', method_end + 1);
    if (method_end == std::string::npos || path_end == std::string::npos) {
        throw std::runtime_error("Malformed HTTP request line.");
    }
    Request request;
    request.method = first.substr(0, method_end);
    request.path = first.substr(method_end + 1, path_end - method_end - 1);
    std::size_t content_length = 0;
    std::size_t cursor = first_end + 2;
    while (cursor < header_end) {
        const auto end = bytes.find("\r\n", cursor);
        const auto line = bytes.substr(cursor, end - cursor);
        const auto colon = line.find(':');
        if (colon != std::string::npos) {
            auto name = line.substr(0, colon);
            std::ranges::transform(name, name.begin(), [](unsigned char value) { return std::tolower(value); });
            auto value = line.substr(colon + 1);
            const auto begin = value.find_first_not_of(" \t");
            value = begin == std::string::npos ? "" : value.substr(begin);
            if (name == "content-length") content_length = std::stoull(value);
            if (name == "x-monkeybyte-token") request.token = value;
        }
        cursor = end + 2;
    }
    if (content_length > max_request_size) throw std::runtime_error("HTTP body is too large.");
    while (bytes.size() < header_end + 4 + content_length) {
        const auto count = ::recv(fd, buffer, sizeof(buffer), 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("Incomplete HTTP body.");
        bytes.append(buffer, static_cast<std::size_t>(count));
    }
    if (content_length > 0) request.body = json::parse(bytes.substr(header_end + 4, content_length));
    return request;
}

json read_response(int fd) {
    std::string bytes;
    char buffer[8192];
    while (true) {
        const auto count = ::recv(fd, buffer, sizeof(buffer), 0);
        if (count < 0 && errno == EINTR) continue;
        if (count == 0) break;
        if (count < 0) throw std::runtime_error("Failed to read UI command response.");
        bytes.append(buffer, static_cast<std::size_t>(count));
        if (bytes.size() > max_request_size) throw std::runtime_error("UI command response is too large.");
    }
    const auto split = bytes.find("\r\n\r\n");
    if (split == std::string::npos) throw std::runtime_error("Malformed UI command response.");
    return json::parse(bytes.substr(split + 4));
}

int connect_loopback(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("Cannot create loopback socket.");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        auto copy = fd;
        close_fd(copy);
        throw std::runtime_error("Cannot connect to the Electron UI service.");
    }
    return fd;
}

}  // namespace

LoopbackServer::LoopbackServer(std::string token, HttpHandler handler)
    : token_(std::move(token)), handler_(std::move(handler)) {
    if (token_.size() < 32) throw std::invalid_argument("A strong loopback token is required.");
}

LoopbackServer::~LoopbackServer() { stop(); }

void LoopbackServer::start() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) throw std::runtime_error("Cannot create backend listener.");
    int reuse = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
        ::listen(listen_fd_, 16) < 0) {
        close_fd(listen_fd_);
        throw std::runtime_error("Cannot bind the backend to loopback.");
    }
    socklen_t length = sizeof(address);
    if (getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length) < 0) {
        close_fd(listen_fd_);
        throw std::runtime_error("Cannot determine backend port.");
    }
    port_ = ntohs(address.sin_port);
    for (int index = 0; index < 4; ++index) {
        workers_.emplace_back([this](std::stop_token token) { worker(token); });
    }
    accept_thread_ = std::jthread([this](std::stop_token token) { accept_loop(token); });
}

void LoopbackServer::stop() noexcept {
    if (stopping_.exchange(true)) return;
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        close_fd(listen_fd_);
    }
    {
        std::lock_guard lock(clients_mutex_);
        for (const int fd : event_clients_) ::shutdown(fd, SHUT_RDWR);
    }
    queue_cv_.notify_all();
    if (accept_thread_.joinable()) {
        accept_thread_.request_stop();
        accept_thread_.join();
    }
    for (auto& worker_thread : workers_) {
        worker_thread.request_stop();
        if (worker_thread.joinable()) worker_thread.join();
    }
    workers_.clear();
}

void LoopbackServer::accept_loop(std::stop_token stop_token) {
    while (!stop_token.stop_requested() && !stopping_.load()) {
        const int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (stopping_.load()) break;
            continue;
        }
        set_timeouts(fd);
        {
            std::lock_guard lock(queue_mutex_);
            connections_.push(fd);
        }
        queue_cv_.notify_one();
    }
}

void LoopbackServer::worker(std::stop_token stop_token) {
    while (!stop_token.stop_requested() && !stopping_.load()) {
        int fd = -1;
        {
            std::unique_lock lock(queue_mutex_);
            queue_cv_.wait(lock, [&] { return stopping_.load() || !connections_.empty(); });
            if (stopping_.load()) return;
            fd = connections_.front();
            connections_.pop();
        }
        serve(fd, stop_token);
        close_fd(fd);
    }
}

void LoopbackServer::serve(int fd, std::stop_token stop_token) {
    try {
        const auto request = read_request(fd);
        if (request.token != token_) {
            send_response(fd, 401, {{"error", "Unauthorized"}});
            return;
        }
        if (request.method == "GET" && request.path == "/v1/events") {
            serve_events(fd, stop_token);
            return;
        }
        const auto response = handler_(request.method, request.path, request.body);
        send_response(fd, response.status, response.body);
    } catch (const json::parse_error& error) {
        send_response(fd, 400, {{"error", error.what()}});
    } catch (const std::invalid_argument& error) {
        send_response(fd, 400, {{"error", error.what()}});
    } catch (const std::exception& error) {
        send_response(fd, 500, {{"error", error.what()}});
    } catch (...) {
        send_response(fd, 500, {{"error", "Unknown backend failure."}});
    }
}

void LoopbackServer::serve_events(int fd, std::stop_token stop_token) {
    {
        std::lock_guard lock(clients_mutex_);
        event_clients_.insert(fd);
    }
    send_all(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n");
    publish("ready", {{"ok", true}});
    while (!stop_token.stop_requested() && !stopping_.load()) {
        if (!send_all(fd, ": keepalive\n\n")) break;
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    std::lock_guard lock(clients_mutex_);
    event_clients_.erase(fd);
}

void LoopbackServer::publish(std::string_view event, const json& payload) {
    const auto bytes = "event: " + std::string(event) + "\ndata: " + payload.dump() + "\n\n";
    std::lock_guard lock(clients_mutex_);
    for (auto iterator = event_clients_.begin(); iterator != event_clients_.end();) {
        if (!send_all(*iterator, bytes)) {
            ::shutdown(*iterator, SHUT_RDWR);
            iterator = event_clients_.erase(iterator);
        } else ++iterator;
    }
}

nlohmann::json loopback_post(std::uint16_t port, std::string_view token, const json& payload) {
    int fd = connect_loopback(port);
    set_timeouts(fd);
    try {
        const auto body = payload.dump();
        const auto request = "POST / HTTP/1.1\r\nHost: 127.0.0.1\r\nX-MonkeyByte-Token: " +
            std::string(token) + "\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        if (!send_all(fd, request)) throw std::runtime_error("Failed to send UI command.");
        auto result = read_response(fd);
        close_fd(fd);
        return result;
    } catch (...) {
        close_fd(fd);
        throw;
    }
}

}  // namespace monkeybyte
