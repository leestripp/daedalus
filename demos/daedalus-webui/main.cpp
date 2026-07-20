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

#include <daedalus.h>
#include <daedalus/paths.h>

#include "session_manager.h"
#include "routes.h"

static void print_help(const char* argv0) {
    std::cout << "Usage: " << argv0 << " [options] [port] [project_dir]\n"
              << "\n"
              << "Phase 7 webui demo: HTTP + SSE host for libdaedalus.a.\n"
              << "One Agent per browser session. Streams tool calls, assistant\n"
              << "deltas, state changes, and log messages over Server-Sent Events.\n"
              << "\n"
              << "Positional arguments:\n"
              << "  port          HTTP port (default: 8080)\n"
              << "  project_dir   Project root directory (default: current dir)\n"
              << "\n"
              << "Options:\n"
              << "  -h, --help      Show this message and exit\n"
              << "  -v, --version   Show version\n"
              << "      --model NAME Use model NAME from settings\n"
              << "      --home PATH  Use PATH as the daedalus home (model store).\n"
              << "                   Overrides the XDG default. Useful when\n"
              << "                   testing with an isolated config.\n"
              << "      --auto          Auto-approve all bash commands (no browser\n"
              << "                   approval dialog). Use with caution.\n"
              << "      --cors-origins CSV  Override CORS allowed origins (default:\n"
              << "                   http://localhost,http://127.0.0.1).\n"
              << "                   Pass '*' for wildcard (NOT recommended;\n"
              << "                   only for trusted environments).\n"
              << "\n"
              << "Environment:\n"
              << "  DAEDALUS_WEBUI_PORT  Override default port\n"
              << "  DAEDALUS_HOME       Override model store location\n"
              << "\n"
              << "Endpoints (once running):\n"
              << "  GET  /                       Single-page browser UI\n"
              << "  POST /sessions               Create a new session\n"
              << "  POST /sessions/<id>/messages Submit a user message\n"
              << "  POST /sessions/<id>/cancel   Cancel current turn\n"
              << "  GET  /sessions/<id>/state    Get current AgentState\n"
              << "  GET  /sessions/<id>/stats    Get AgentStats (tokens, iterations)\n"
              << "  POST /sessions/<id>/approve  Approve/deny a pending bash command\n"
              << "  GET  /sessions/<id>/events   SSE event stream (text/event-stream)\n"
              << "  GET  /healthz                Liveness probe\n"
              << "  GET  /info                   Server-side metadata (cwd, etc.)\n"
              << "  Settings:\n"
              << "    GET  /settings/providers        List configured providers\n"
              << "    POST /settings/providers        Add/update a provider\n"
              << "    DEL  /settings/providers/<n>    Delete a provider\n"
              << "    GET  /settings/providers/discover?name=<n>  Discover provider models\n"
              << "    GET  /settings/models           List configured models\n"
              << "    POST /settings/models           Add/update a model\n"
              << "    DEL  /settings/models/<n>       Delete a model\n"
              << "    GET  /settings/models/discover?name=<n>  Discover models via model's provider\n"
              << "    POST /settings/models/default   Set default model\n"
              << "    GET  /settings/general          General/agent settings\n"
              << "    POST /settings/general          Update general settings\n"
              << "    GET  /settings/mcp              List MCP servers\n"
              << "    POST /settings/mcp              Add/update MCP server\n"
              << "    DEL  /settings/mcp/<n>          Delete MCP server\n";
}

int main(int argc, char* argv[]) {
    uint16_t port = 8080;
    std::string model_name;
    std::filesystem::path project_root = std::filesystem::current_path();
    std::filesystem::path daedalus_home;   // empty = use default
    bool auto_approve = false;
    // §webui-6: default to loopback + localhost origins. Hosts that
    // genuinely need wildcard (or a LAN origin) can override via
    // --cors-origins on the CLI.
    std::string cors_origins =
        "http://localhost,http://localhost:8080,"
        "http://127.0.0.1,http://127.0.0.1:8080";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_help(argv[0]);
            return 0;
        }
        if (arg == "-v" || arg == "--version") {
            std::cout << "daedalus-webui (libdaedalus)\n";
            return 0;
        }
        else if (arg == "--model") {
            if (i + 1 >= argc) {
                std::cerr << "--model requires a NAME argument\n";
                return 2;
            }
            model_name = argv[++i];
        }
        else if (arg == "--auto") {
            auto_approve = true;
        }
        else if (arg == "--cors-origins") {
            // §webui-6: explicit override. Use "*" for wildcard
            // (NOT recommended; only for trusted environments).
            if (i + 1 >= argc) {
                std::cerr << "--cors-origins requires a CSV argument\n";
                return 2;
            }
            cors_origins = argv[++i];
        }
        else if (arg == "--home") {
            // §webui-3 (2026-07-16): --home was previously parsed and
            // discarded. Now honoured by setting $DAEDALUS_HOME in the
            // process environment BEFORE Paths::global() is initialized.
            // Paths::user_root() honours $DAEDALUS_HOME, so this routes
            // every per-session AgentConfig (which sets daedalus_home
            // from Paths::global().user_root()) to the user's chosen
            // model store.
            if (i + 1 >= argc) {
                std::cerr << "--home requires a PATH argument\n";
                return 2;
            }
            daedalus_home = argv[++i];
        }
        else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "Unknown option: " << arg << "\n"
                      << "Run `" << argv[0] << " --help` for usage.\n";
            return 2;
        }
        else {
            // Positional: try port first, then project_dir.
            try {
                port = static_cast<uint16_t>(std::stoi(arg));
            } catch (...) {
                project_root = arg;
            }
        }
    }
    if (const char* env = std::getenv("DAEDALUS_WEBUI_PORT"); env && *env) {
        try { port = static_cast<uint16_t>(std::stoi(env)); } catch (...) {}
    }
    // §webui-3 (cont.): honour DAEDALUS_HOME in addition to --home.
    // --home on the CLI wins over the env var.
    if (daedalus_home.empty()) {
        if (const char* env = std::getenv("DAEDALUS_HOME"); env && *env) {
            daedalus_home = env;
        }
    }
    if (!daedalus_home.empty()) {
        // Set $DAEDALUS_HOME in the process environment so
        // Paths::user_root() picks it up. (user_data_root() honours
        // XDG only — user_root() honours both $DAEDALUS_HOME and
        // XDG. The session_manager sets daedalus_home = user_root(),
        // so this routes every session to the override.)
        setenv("DAEDALUS_HOME", daedalus_home.string().c_str(), 1);
        std::cerr << "[daedalus-webui] using daedalus_home=" << daedalus_home << "\n";
    }

    OmniNet::init();

    OmniNet::HTTP::Server::Config cfg;
    cfg.address           = "127.0.0.1";
    cfg.port              = port;
    cfg.workerCount       = 4;
    cfg.corsEnabled       = true;
    // §webui-6 (2026-07-16): default to loopback-only origins.
    // The default address is 127.0.0.1, but a host could pass `--bind`
    // (future) to expose this on a LAN address; in that case the
    // browser will reject responses whose Access-Control-Allow-Origin
    // doesn't match. Hosts that genuinely need wildcard CORS can
    // override via --cors-origins "*" on the CLI.
    cfg.corsAllowedOrigins = cors_origins;

    OmniNet::HTTP::Server server(cfg);
    websse::SessionManager mgr(server, model_name, project_root, auto_approve);
    websse::register_routes(server, mgr);

    std::cerr << "[daedalus-webui] listening on http://"
              << cfg.address << ":" << cfg.port << "/\n"
              << "[daedalus-webui] open in a browser; one Agent per session\n"
              << "[daedalus-webui] project_root=" << project_root << "\n"
              << "[daedalus-webui] auto_approve=" << (auto_approve ? "yes" : "no") << "\n";

    server.start();      // blocks

    OmniNet::cleanup();
    return 0;
}