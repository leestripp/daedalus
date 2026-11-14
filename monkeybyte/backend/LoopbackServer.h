// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace monkeybyte {

struct HttpResponse {
    int status = 200;
    nlohmann::json body;
};

using HttpHandler = std::function<HttpResponse(std::string, std::string, nlohmann::json)>;

class LoopbackServer {
public:
    LoopbackServer(std::string token, HttpHandler handler);
    ~LoopbackServer();

    void start();
    void stop() noexcept;
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    void publish(std::string_view event, const nlohmann::json& payload);

private:
    std::string token_;
    HttpHandler handler_;
    int listen_fd_ = -1;
    std::uint16_t port_ = 0;
    std::atomic_bool stopping_ = false;
    std::jthread accept_thread_;
    std::vector<std::jthread> workers_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::queue<int> connections_;
    std::mutex clients_mutex_;
    std::set<int> event_clients_;

    void accept_loop(std::stop_token stop_token);
    void worker(std::stop_token stop_token);
    void serve(int fd, std::stop_token stop_token);
    void serve_events(int fd, std::stop_token stop_token);
};

/// Blocking authenticated call to the Electron main process's loopback UI service.
nlohmann::json loopback_post(std::uint16_t port, std::string_view token,
                             const nlohmann::json& payload);

}  // namespace monkeybyte
