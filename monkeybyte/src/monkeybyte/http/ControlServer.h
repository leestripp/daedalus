#pragma once

#include <memory>
#include <string>

namespace OmniNet::HTTP { class Server; }

namespace monkeybyte {

// Local HTTP control endpoint backed by OmniNet (bundled via daedalus).
// Binds 127.0.0.1 only. Env override: MONKEYBYTE_CONTROL_PORT (0 = off).
//
//   GET  /api/ui/geometry           → geometry JSON
//   GET  /api/ui/commands           → {name: description}
//   POST /api/ui/control            → {"command": "sizer.delta", "args": {...}}
class ControlServer {
public:
    ControlServer();
    ~ControlServer();

    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    void start();
    void stop();
    bool running() const;
    int port() const { return port_; }

private:
    std::unique_ptr<OmniNet::HTTP::Server> server_;
    int port_ = 0;
};

} // namespace monkeybyte
