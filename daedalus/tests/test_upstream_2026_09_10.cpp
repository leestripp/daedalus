// SPDX-License-Identifier: BSL-1.0
// tests/test_upstream_2026_09_10.cpp
//
// MonkeyByte upstream report (2026-09-10, vs d73d1ba) coverage:
//
//   BUG-3 / REQ-4 (§QUEUE-MUTEX): the pending-message queue and
//     Agent::cancel() are internally synchronized — callable from any
//     thread. Concurrent submit/clear/snapshot/cancel must not crash and
//     must keep the queue consistent.
//   BUG-4 (§TURN-ERROR): a turn that fails terminally (here: no model
//     configured) is visible via Agent::turn_had_terminal_error() /
//     Agent::last_error() after run_one_turn() returns; the flag clears
//     when the next turn starts.
//   REQ-3 (§COMPRESS-RESULT): compress_context() with no model
//     configured returns CompressResult::NoModel.
//   REQ-5 (§STATECHANGE-TYPED): the typed on_state_change(AgentState,
//     AgentState) overload fires, and a legacy int-only sink still
//     receives events through the default forwarding.

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <daedalus.h>

namespace {

struct NullSink : public daedalus::AgentSink {
    void on_log(daedalus::LogLevel, std::string_view) noexcept override {}
};

// §STATECHANGE-TYPED: records transitions through the typed overload only.
struct TypedSink : public NullSink {
    std::vector<std::pair<daedalus::AgentState, daedalus::AgentState>> transitions;
    void on_state_change(daedalus::AgentState from,
                         daedalus::AgentState to) noexcept override {
        transitions.emplace_back(from, to);
    }
};

// Legacy protocol: overrides only the int overload. The typed default in
// AgentSink must forward to it.
struct LegacyIntSink : public NullSink {
    std::vector<std::pair<int, int>> transitions;
    void on_state_change(int from, int to) noexcept override {
        transitions.emplace_back(from, to);
    }
};

daedalus::AgentConfig test_cfg(const std::string& dir) {
    daedalus::AgentConfig cfg;
    cfg.mcp_enabled = false;  // unrelated to this test; never start real MCP servers
    const auto base = std::filesystem::temp_directory_path() / dir;
    cfg.project_root  = base / "project";
    // Isolate from any real host settings store ($XDG_CONFIG_HOME/
    // daedalus/daedalus.db) — an empty daedalus_home means no model
    // resolves, so no LLMClient is constructed (the §TURN-ERROR
    // no-model path under test).
    cfg.daedalus_home = base / "home";
    std::filesystem::create_directories(cfg.project_root);
    std::filesystem::create_directories(cfg.daedalus_home);
    return cfg;
}

#define CHECK_MSG(cond)                                                     \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::cerr << "FAILED (line " << __LINE__ << "): " #cond "\n";   \
            return 1;                                                       \
        }                                                                   \
    } while (0)

int run() {
    // ---- 1. §QUEUE-MUTEX: cross-thread queue storm -----------------------
    // The driving thread submits/edits/snapshots while a second thread
    // hammers cancel() + pending_count() + clear — the exact cross-thread
    // pattern of a UI Stop button (MonkeyByte BUG-3). Pre-fix this was a
    // data race (UB) on pending_user_messages_.
    {
        auto agent = daedalus::Agent(test_cfg("daedalus_qmutex"),
                                     std::make_shared<NullSink>());
        std::atomic<bool> stop{false};
        std::atomic<bool> error{false};

        std::thread ui([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                agent.cancel();                 // was: unguarded empty() read
                (void)agent.pending_count();
                (void)agent.pending_messages();
            }
        });

        for (int i = 0; i < 2000 && !error.load(); ++i) {
            agent.submit_user_message("msg " + std::to_string(i));
            if (i % 7 == 0) agent.clear_pending_messages();
            if (i % 11 == 0) (void)agent.edit_pending_message(0, "edited");
            if (i % 13 == 0) (void)agent.remove_pending_message(0);
        }
        stop.store(true);
        ui.join();

        // Queue is in a valid state and still usable single-threaded.
        agent.clear_pending_messages();
        CHECK_MSG(agent.pending_count() == 0);
        agent.submit_user_message("final");
        CHECK_MSG(agent.pending_count() == 1);
        const auto msgs = agent.pending_messages();
        CHECK_MSG(msgs.size() == 1 && msgs[0].text == "final");
    }
    std::cout << "[1] queue mutex storm OK\n";

    // ---- 2. §TURN-ERROR: no-model turn is visible as a terminal error ----
    {
        auto agent = daedalus::Agent(test_cfg("daedalus_turnerr"),
                                     std::make_shared<NullSink>());
        CHECK_MSG(!agent.turn_had_terminal_error());
        CHECK_MSG(!agent.last_error().has_value());

        agent.submit_user_message("hello");
        agent.run_one_turn();   // no model configured -> terminal Internal error

        CHECK_MSG(agent.turn_had_terminal_error());
        const auto err = agent.last_error();
        CHECK_MSG(err.has_value());
        CHECK_MSG(err->kind == daedalus::ErrorKind::Internal);
        CHECK_MSG(err->message.find("no model configured") != std::string::npos);

        // Next turn start clears the record (empty queue -> no-op turn).
        agent.run_one_turn();
        CHECK_MSG(!agent.turn_had_terminal_error());
        CHECK_MSG(!agent.last_error().has_value());
    }
    std::cout << "[2] turn-error query OK\n";

    // ---- 3. §COMPRESS-RESULT: no model -> NoModel -------------------------
    {
        auto agent = daedalus::Agent(test_cfg("daedalus_compres"),
                                     std::make_shared<NullSink>());
        CHECK_MSG(agent.compress_context() == daedalus::CompressResult::NoModel);
    }
    std::cout << "[3] compress NoModel OK\n";

    // ---- 4. §STATECHANGE-TYPED: typed + legacy sinks both fire ------------
    {
        auto typed = std::make_shared<TypedSink>();
        auto agent = daedalus::Agent(test_cfg("daedalus_states_t"), typed);
        agent.submit_user_message("hello");
        agent.run_one_turn();   // no model -> ... -> Error transition
        CHECK_MSG(!typed->transitions.empty());
        bool saw_error = false;
        for (const auto& [f, t] : typed->transitions) {
            (void)f;
            if (t == daedalus::AgentState::Error) saw_error = true;
        }
        CHECK_MSG(saw_error);

        auto legacy = std::make_shared<LegacyIntSink>();
        auto agent2 = daedalus::Agent(test_cfg("daedalus_states_l"), legacy);
        agent2.submit_user_message("hello");
        agent2.run_one_turn();
        CHECK_MSG(!legacy->transitions.empty());
        const int err_int = static_cast<int>(daedalus::AgentState::Error);
        bool saw_err_int = false;
        for (const auto& [f, t] : legacy->transitions) {
            (void)f;
            if (t == err_int) saw_err_int = true;
        }
        CHECK_MSG(saw_err_int);
    }
    std::cout << "[4] typed state change OK\n";

    std::cout << "ALL OK\n";
    return 0;
}

}  // namespace

int main() {
    try {
        return run();
    } catch (const std::exception& e) {
        std::cerr << "EXCEPTION: " << e.what() << "\n";
        return 1;
    }
}
