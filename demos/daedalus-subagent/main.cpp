// SPDX-License-Identifier: BSL-1.0
// demos/daedalus-subagent/main.cpp
//
// Sub-agent composition pattern, implemented purely with the public
// <daedalus.h> API.
//
// Pattern:
//   - Construct a parent Agent. Its system prompt says: "use the
//     spawn_subagent tool to delegate focused investigations."
//   - Define a host tool called `spawn_subagent` that:
//        1. Constructs a child Agent with the same project_root
//        2. Calls child.submit_user_message(task)
//        3. Calls child.run_one_turn()
//        4. Returns the child's last_assistant_text() as the tool result
//   - Register the tool with the parent Agent.
//
// To the parent Agent, `spawn_subagent` is just a tool that returns a
// string. The parent never knows there's another Agent inside it.

#include <iostream>
#include <filesystem>
#include <memory>
#include <string>

#include <daedalus.h>
#include <daedalus/paths.h>
#include <daedalus/default_paths.h>

#include "ThinkingSpinner.h"
#include "sinks.h"

namespace fs = std::filesystem;

// =============================================================================
// SpawnSubAgentTool — host-registered tool that runs a child Agent
// =============================================================================

class SpawnSubAgentTool : public daedalus::BaseTool {
public:
    struct Config {
        std::shared_ptr<daedalus::AgentSink> child_sink;
        fs::path   project_root;
        std::string child_model_name;
        std::string child_system_prompt;
        int         max_iterations = 30;
        // §subagent-1 (2026-07-16): monotonic counter for unique
        // child session_ids. Parent Agent invokes this tool on a
        // single thread, so an atomic is unnecessary — a plain int is
        // enough and avoids the move-only constraint atomic imposes
        // on the containing Config struct.
        int spawn_count = 0;
    };

    explicit SpawnSubAgentTool(Config cfg) : cfg_(std::move(cfg)) {}

    std::string get_name() const override { return "spawn_subagent"; }

    std::string get_description() const override {
        return "Delegate a focused investigation to a child agent. "
               "Returns the child's final text answer. Use this when you "
               "want a single focused response without using your own tools. "
               "The child has read-only access and won't modify anything.";
    }

    nlohmann::json get_definition() const override {
        return {
            {"type", "function"},
            {"function", {
                {"name", "spawn_subagent"},
                {"description", get_description()},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"task", {
                            {"type", "string"},
                            {"description", "The focused task for the child agent."}
                        }}
                    }},
                    {"required", {"task"}}
                }}
            }}
        };
    }

    nlohmann::json execute(const nlohmann::json& args,
                           daedalus::HostToolContext& ctx) override {
        std::string task = args.value("task", std::string{});
        if (task.empty())
            return {{"status", "error"}, {"message", "missing 'task' argument"}};

        daedalus::AgentConfig child_cfg;
        child_cfg.project_root        = ctx.project_root();
        child_cfg.model_name          = cfg_.child_model_name;
        child_cfg.system_prompt       = cfg_.child_system_prompt;
        child_cfg.max_agent_iterations = cfg_.max_iterations;
        // §subagent-1 (2026-07-16): unique session_id per spawn so
        // subagents don't share conversation history in the SQLite
        // store. Use a monotonic counter (cfg_.spawn_count) so
        // re-using the parent in one session produces stable IDs.
        child_cfg.session_id          = "subagent:" + std::to_string(++cfg_.spawn_count);
        // §B2-fix (2026-07-16): use user_data_root() (XDG pure path),
        // not default_daedalus_home(). The latter wraps user_root() which
        // honors legacy `~/.daedalus/` as a fallback — but that dir can
        // exist (empty) on migrated users whose real models live in the
        // XDG data path. Same bug daedalus-cli just hit; same fix.
        // Users who need the single-root override (`$DAEDALUS_HOME=...`)
        // should pass `--home` explicitly to the parent daemon.
        child_cfg.daedalus_home       = daedalus::Paths::global().user_data_root();
        child_cfg.auto_approve       = true;   // child is read-only
        child_cfg.agent_name         = "child";
        // §subagent-2 (2026-07-16): was a hardcoded "/home/ubuntu/.daedalus/skills/"
        // path. Use Paths::global() so the demo works on any system.
        child_cfg.skills_dirs         = { daedalus::Paths::global().global_skills_dir() };
        child_cfg.stream              = true;

        daedalus::Agent child(std::move(child_cfg), cfg_.child_sink);
        child.initialize();
        child.submit_user_message(std::move(task));
        child.run_one_turn();

        std::string answer = child.last_assistant_text();
        auto st = child.state();
        child.shutdown();

        return {
            {"status",  daedalus::to_string(st)},
            {"answer",  answer},
            {"state",   daedalus::to_string(st)}
        };
    }

private:
    Config cfg_;
};

int main(int argc, char* argv[]) {
    // §F2 fix: --help short-circuit before argv parsing.
    // Without this, `daedalus-subagent --help` treats "--help" as project_root
    // and Agent::initialize() throws std::invalid_argument("Project path
    // does not exist: --help") → std::terminate.
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            std::cout << "Usage: daedalus-subagent <project_root> <model>\n"
                      << "\n"
                      << "Demonstrates the sub-agent composition pattern:\n"
                      << "  - parent Agent receives a user request,\n"
                      << "  - delegates focused investigations to a child Agent\n"
                      << "    via the host-registered `spawn_subagent` tool,\n"
                      << "  - returns the child's answer as the tool result.\n"
                      << "\n"
                      << "Arguments:\n"
                      << "  project_root   Directory to investigate\n"
                      << "  model          Model name to use (must be configured)\n"
                      << "\n"
                      << "Options:\n"
                      << "  --help, -h     Show this message and exit\n"
                      << "  --version, -v  Show version\n"
                      << "\n"
                      << "In-session commands:\n"
                      << "  /quit, /exit   Leave the demo\n";
            return 0;
        }
        if (a == "--version" || a == "-v") {
            std::cout << "daedalus-subagent (libdaedalus)\n";
            return 0;
        }
    }

    fs::path project_root = (argc > 1) ? argv[1] : ".";

    auto parent_sink = std::make_shared<StdoutSink>();
    auto child_sink  = std::make_shared<ChildOnlySink>();

    daedalus::AgentConfig parent_cfg;
    parent_cfg.project_root  = project_root;
    parent_cfg.model_name    = (argc > 2) ? argv[2] : "";
    parent_cfg.max_agent_iterations = 100;
    parent_cfg.auto_approve  = true;
    parent_cfg.session_id    = "subagent-demo-parent";
    parent_cfg.agent_name    = "parent";
    // §subagent-3 (2026-07-16): explicit daedalus_home using
    // user_data_root() so the parent finds the same model store as the
    // child. Without this the parent may pick up a different default
    // (legacy `~/.daedalus/` or an empty XDG path).
    parent_cfg.daedalus_home = daedalus::Paths::global().user_data_root();
    // §subagent-2 (2026-07-16): was hardcoded "/home/ubuntu/.daedalus/skills/".
    // Use Paths::global() so the demo works on any system.
    parent_cfg.skills_dirs   = { daedalus::Paths::global().global_skills_dir() };
    parent_cfg.stream        = true;
    parent_cfg.system_prompt =
        "You are a parent agent that orchestrates work by delegating "
        "focused investigations to a child agent. When the user asks a "
        "specific question (e.g. 'what does this file do?', 'summarize "
        "the function X', 'find the callers of Y'), call the "
        "spawn_subagent tool with a clear, specific task. Use the "
        "child's answer to form your response. Don't try to do the "
        "investigation yourself — delegate.";

    daedalus::Agent parent(std::move(parent_cfg), parent_sink);

    SpawnSubAgentTool::Config spawn_cfg;
    spawn_cfg.child_sink          = child_sink;
    spawn_cfg.project_root         = project_root;
    spawn_cfg.child_model_name     = (argc > 2) ? argv[2] : "";
    spawn_cfg.child_system_prompt =
        "You are a focused child agent. You answer a single "
        "specific question that the parent agent delegated. You may "
        "use the read-only tools (read_file, list_directory, "
        "grep_search) to investigate. When you have a clear answer, "
        "respond in plain text and stop. Do not start a new task.";
    spawn_cfg.max_iterations = 20;

    parent.register_tool(std::make_unique<SpawnSubAgentTool>(std::move(spawn_cfg)));
    parent.initialize();

    std::cout << "\nSub-agent demo ready. The parent will delegate focused "
                  "work to a child agent. /quit to exit.\n\n";

    std::string line;
    while (true) {
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, line)) break;   // EOF
        if (line == "/quit" || line == "/exit") break;
        if (line.empty()) continue;
        parent.submit_user_message(std::move(line));
        parent.run_one_turn();
        std::cout << "\n";
    }
    parent.shutdown();
    return 0;
}
