// SPDX-License-Identifier: BSL-1.0
#include "AgentService.h"
#include "LoopbackServer.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
std::atomic_bool terminate_requested = false;

std::string environment(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}
}  // namespace

int main(int argc, char** argv) {
    try {
        std::filesystem::path data_dir;
        std::uint16_t ui_port = 0;
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--data-dir" && index + 1 < argc) data_dir = argv[++index];
            else if (argument == "--ui-port" && index + 1 < argc) ui_port = static_cast<std::uint16_t>(std::stoi(argv[++index]));
            else throw std::invalid_argument("Unknown argument: " + argument);
        }
        const auto token = environment("MONKEYBYTE_BRIDGE_TOKEN");
        const auto ui_token = environment("MONKEYBYTE_UI_TOKEN");
        if (data_dir.empty() || ui_port == 0 || token.empty() || ui_token.empty()) {
            throw std::invalid_argument("Backend requires --data-dir, --ui-port, and two loopback tokens.");
        }
        std::filesystem::create_directories(data_dir);
        const bool test_mode = environment("MONKEYBYTE_BACKEND_TEST_MODE") == "1";
        std::unique_ptr<monkeybyte::AgentService> service;
        monkeybyte::LoopbackServer server(token, [&](std::string method, std::string path,
                                                     nlohmann::json body) -> monkeybyte::HttpResponse {
            using nlohmann::json;
            if (test_mode && method == "GET" && path == "/v1/test/tools") return {200, service->tools()};
            if (test_mode && method == "POST" && path == "/v1/test/ui") {
                return {200, service->execute_ui(body.at("command").get<std::string>(), body.value("args", json::object()))};
            }
            if (method == "GET" && path == "/v1/chimpflow/config") return {200, service->chimpflow_config()};
            if (method == "GET" && path == "/v1/health") return {200, service->status()};
            if (method == "GET" && path == "/v1/sessions") return {200, service->sessions()};
            if (method == "POST" && path == "/v1/sessions/ensure") {
                return {200, service->ensure_session(body.at("workspace").get<std::string>(), body.value("sessionId", ""))};
            }
            if (method == "POST" && path == "/v1/sessions/create") {
                return {200, service->create_session(body.at("workspace").get<std::string>(), body.value("name", "New Chat"))};
            }
            if (method == "POST" && path == "/v1/sessions/select") return {200, service->select_session(body.at("id").get<std::string>())};
            if (method == "POST" && path == "/v1/sessions/rename") {
                return {200, service->rename_session(body.at("id").get<std::string>(), body.at("name").get<std::string>())};
            }
            if (method == "POST" && path == "/v1/sessions/delete") return {200, service->delete_session(body.at("id").get<std::string>())};
            if (method == "GET" && path == "/v1/history") return {200, service->history()};
            if (method == "POST" && path == "/v1/chat/clear") return {200, service->clear_history()};
            if (method == "POST" && path == "/v1/chat/send") {
                return {200, service->send(body.value("text", ""), body.value("attachments", json::array()))};
            }
            if (method == "POST" && path == "/v1/chat/steer") {
                return {200, service->steer(body.value("text", ""), body.value("attachments", json::array()))};
            }
            if (method == "POST" && path == "/v1/chat/cancel") return {200, service->cancel()};
            if (method == "POST" && path == "/v1/debug/log") {
                return {200, service->debug_log(body.value("entry", ""))};
            }
            if (method == "POST" && path == "/v1/chat/approval") {
                return {200, service->respond(body.at("id").get<std::string>(), body.at("approved").get<bool>())};
            }
            if (method == "GET" && path == "/v1/settings") return {200, service->settings()};
            if (method == "POST" && path == "/v1/settings") return {200, service->update_settings(body)};
            if (method == "POST" && path == "/v1/models/select") return {200, service->select_model(body.at("model").get<std::string>())};
            if (method == "POST" && path == "/v1/models/thinking") return {200, service->select_thinking(body.at("level").get<std::string>())};
            return {404, {{"error", "Unknown route: " + method + " " + path}}};
        });
        service = std::make_unique<monkeybyte::AgentService>(data_dir / "sessions.json",
            [&](std::string event, nlohmann::json payload) { server.publish(event, payload); },
            [&](std::string command, nlohmann::json args) {
                return monkeybyte::loopback_post(ui_port, ui_token, {{"command", command}, {"args", args}});
            });
        server.start();
        std::signal(SIGTERM, [](int) { terminate_requested.store(true); });
        std::signal(SIGINT, [](int) { terminate_requested.store(true); });
        std::cout << nlohmann::json{{"ready", true}, {"port", server.port()}}.dump() << std::endl;
        while (!terminate_requested.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        service.reset();
        server.stop();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MonkeyByte backend: " << error.what() << '\n';
        return 1;
    }
}
