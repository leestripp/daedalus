// SPDX-License-Identifier: BSL-1.0
// tests/test_multi_instance.cpp
//
// §Phase 8 acceptance: daedalus::Agent instances are independent. Two
// Agents must be able to coexist in one process: both initialize, both
// run, both shutdown cleanly. This locks in the Phase 2 "no singleton"
// contract — if anyone reintroduces process-wide state, this test fails.

#include <atomic>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include <daedalus.h>

namespace fs = std::filesystem;

namespace {

// Minimal no-op sink.
struct NullSink : public daedalus::AgentSink {
    void on_log(daedalus::LogLevel, std::string_view) noexcept override {}
};

// Per-test scratch directory under the system temp dir. Auto-created
// and cleaned up at process exit. Replaces the previous hardcoded
// `/tmp/daedalus_multi_{a,b}` paths which silently passed only when
// leftover dirs happened to exist from a prior run.
struct ScratchDir {
    fs::path path;
    explicit ScratchDir(const std::string& tag) {
        path = fs::temp_directory_path() /
               ("daedalus_test_" + tag + "_" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

}  // namespace

int main() {
    NullSink sink_a, sink_b;
    auto shared_a = std::shared_ptr<daedalus::AgentSink>(&sink_a, [](void*) {});
    auto shared_b = std::shared_ptr<daedalus::AgentSink>(&sink_b, [](void*) {});
    (void)shared_a;
    (void)shared_b;

    ScratchDir dir_a("multi_a");
    ScratchDir dir_b("multi_b");

    // ---- Two Agents, both without LLM credentials (no network) ----
    // §MCP-RACE: keep this test hermetic — do not load MCP servers from
    // the user's real daedalus.db. That avoids forking uvx/uv subprocesses
    // and a known setpgid/killpg race on rapid shutdown.
    daedalus::AgentConfig cfg_a;
    cfg_a.project_root = dir_a.path;
    cfg_a.session_id   = "agent-A";
    cfg_a.mcp_enabled  = false;

    daedalus::AgentConfig cfg_b;
    cfg_b.project_root = dir_b.path;
    cfg_b.session_id   = "agent-B";
    cfg_b.mcp_enabled  = false;

    daedalus::Agent agent_a(std::move(cfg_a), nullptr);
    daedalus::Agent agent_b(std::move(cfg_b), nullptr);

    // Both initialize cleanly without interfering.
    agent_a.initialize();
    agent_b.initialize();

    assert(agent_a.state() == daedalus::AgentState::Idle);
    assert(agent_b.state() == daedalus::AgentState::Idle);

    // No queued messages → run_one_turn is a no-op for both.
    agent_a.run_one_turn();
    agent_b.run_one_turn();
    assert(agent_a.state() == daedalus::AgentState::Idle);
    assert(agent_b.state() == daedalus::AgentState::Idle);

    // Cancel only affects one (we don't have a way to cancel from the
    // public API surface, but we can verify shutdown order independence).
    agent_a.shutdown();
    // After A is down, B should still be functional.
    assert(agent_b.state() == daedalus::AgentState::Idle);
    agent_b.shutdown();

    // Re-initialise A to verify it can be reused (Phase 2 contract:
    // not strictly required, but a useful sanity check).
    agent_a.initialize();
    agent_a.shutdown();

    std::cerr << "[test] multi_instance PASSED (two Agents coexisted; "
              << "order-independent shutdown; re-init OK)\n";
    return 0;
}