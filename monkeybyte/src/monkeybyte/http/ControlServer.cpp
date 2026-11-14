#include "monkeybyte/http/ControlServer.h"

#include "monkeybyte/ui/UiControl.h"
#include "monkeybyte/ui/UiGuard.h"

#include "http/HttpServer.h"

#include <cstdlib>
#include <exception>
#include <glib.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace monkeybyte {

namespace {
constexpr int kDefaultPort = 17878;

// OmniNet binds on a background thread inside startAsync(); a bind failure
// there throws across the thread boundary and terminates the process. Probe
// the port synchronously first so a conflict (e.g. a second window after
// re-activation, where the first window already owns the port) just disables
// the control server instead of killing the app.
bool port_available(int port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    const bool ok = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    ::close(fd);
    return ok;
}
} // namespace

ControlServer::ControlServer() {
    const char* env = std::getenv("MONKEYBYTE_CONTROL_PORT");
    port_ = env ? std::atoi(env) : kDefaultPort;
}

ControlServer::~ControlServer() { stop(); }

void ControlServer::start() {
    if (port_ <= 0) return; // disabled via MONKEYBYTE_CONTROL_PORT=0

    if (!port_available(port_)) {
        g_warning("ControlServer: 127.0.0.1:%d already in use, disabled", port_);
        return;
    }

    try {
        OmniNet::HTTP::Server::Config cfg;
        cfg.address = "127.0.0.1";
        cfg.port = static_cast<uint16_t>(port_);
        cfg.logRequests = false;
        cfg.logErrors = false;
        server_ = std::make_unique<OmniNet::HTTP::Server>(cfg);

        server_->router().get("/api/ui/geometry", [](const OmniNet::HTTP::Request&, OmniNet::HTTP::Response& resp) {
            const auto result = UiControl::instance().execute("app.geometry", {});
            resp.setBody(result.dump());
        });
        server_->router().get("/api/ui/commands", [](const OmniNet::HTTP::Request&, OmniNet::HTTP::Response& resp) {
            const auto result = UiControl::instance().execute("app.commands", {});
            resp.setBody(result.dump());
        });
        server_->router().post("/api/ui/control", [](const nlohmann::json& body, OmniNet::HTTP::Response& resp) {
            // Runs on an OmniNet worker thread: a json type_error from a
            // malformed request must not terminate the process.
            nlohmann::json result;
            ui_guard("/api/ui/control", [&] {
                const std::string command = body.value("command", "");
                const nlohmann::json args = body.contains("args") ? body["args"]
                                                                  : nlohmann::json::object();
                result = UiControl::instance().execute(command, args);
            });
            resp.setBody(result.empty() ? R"({"error":"handler failed"})"
                                        : result.dump());
        });

        server_->startAsync();
    } catch (const std::exception& e) {
        // A second window (re-activation) or a port conflict must not kill
        // the app — the control server is a testing aid, so just disable it.
        g_warning("ControlServer: disabled (%s)", e.what());
        server_.reset();
    }
}

void ControlServer::stop() {
    if (server_ && server_->isRunning()) server_->stop();
    server_.reset();
}

bool ControlServer::running() const {
    return server_ && server_->isRunning();
}

} // namespace monkeybyte
