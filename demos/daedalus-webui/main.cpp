// SPDX-License-Identifier: BSL-1.0
// demos/daedalus-webui/main.cpp
//
// Phase 7 webui demo: HTTP + SSE host for libdaedalus.a.
//
// One `daedalus::Agent` instance per browser session. Agent events
// (on_assistant_delta, on_tool_call, on_log, on_state_change) are
// streamed to the browser via Server-Sent Events. The user posts a
// message with POST /sessions/<id>/messages and a worker thread runs
// agent.run_one_turn().
//
// HTTP layer: OmniNet (https://gitlab.com/leestripp/omninet) — already a
// daedalus dependency. See Agent.md §Phase 7.
//
// Heavy lifting lives in separate headers:
//   - index_html.h      — embedded single-page browser UI (vanilla DOM)
//   - web_sse_sink.h    — AgentSink that broadcasts each event over SSE
//   - session_manager.h — one Agent + one SSE handler per HTTP session
//   - routes.h          — HTTP route registration
//
// This main.cpp is just server bootstrap + arg parsing.

#include <cstdlib>
#include <cstdint>
#include <iostream>

#include "OmniNet.h"
#include "http/HttpServer.h"

#include "session_manager.h"
#include "routes.h"

int main(int argc, char* argv[]) {
    uint16_t port = 8080;
    if (argc > 1) {
        try { port = static_cast<uint16_t>(std::stoi(argv[1])); } catch (...) {}
    }
    if (const char* env = std::getenv("DAEDALUS_WEBUI_PORT"); env && *env) {
        try { port = static_cast<uint16_t>(std::stoi(env)); } catch (...) {}
    }

    OmniNet::init();

    OmniNet::HTTP::Server::Config cfg;
    cfg.address           = "127.0.0.1";
    cfg.port              = port;
    cfg.workerCount       = 4;
    cfg.corsEnabled       = true;
    cfg.corsAllowedOrigins = "*";

    OmniNet::HTTP::Server server(cfg);
    websse::SessionManager mgr(server);
    websse::register_routes(server, mgr);

    std::cerr << "[daedalus-webui] listening on http://"
              << cfg.address << ":" << cfg.port << "/\n"
              << "[daedalus-webui] open in a browser; one Agent per session\n";

    server.start();      // blocks

    OmniNet::cleanup();
    return 0;
}