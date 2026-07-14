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
#include <daedalus/default_paths.h>

#include "ThinkingSpinner.h"
#include "sinks.h"

namespace fs = std::filesystem;

// =============================================================================
// SpawnSubAgentTool — host-registered tool that runs a child Agent
// =============================================================================

class SpawnSubAgentTool : public daedalus::HostTool {
public:
    struct Config {
        std::shared_ptr<daedalus::AgentSink> child_sink;
        fs::path   project_root;
        std::string child_model_name;
        std::string child_system_prompt;
        int         max_iterations = 30;
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
        child_cfg.session_id          = "subagent:";
        child_cfg.settings_path       = daedalus::default_settings_path();
        child_cfg.auto_approve       = true;   // child is read-only
        child_cfg.agent_name         = "child";
        child_cfg.skills_dirs         = { "/home/ubuntu/.daedalus/skills/" };
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
    fs::path project_root = (argc > 1) ? argv[1] : ".";

    auto parent_sink = std::make_shared<StdoutSink>();
    auto child_sink  = std::make_shared<ChildOnlySink>();

    daedalus::AgentConfig parent_cfg;
    parent_cfg.project_root  = project_root;
    parent_cfg.model_name    = (argc > 2) ? argv[2] : "";
    parent_cfg.settings_path = daedalus::default_settings_path();
    parent_cfg.max_agent_iterations = 100;
    parent_cfg.auto_approve  = true;
    parent_cfg.session_id    = "subagent-demo-parent";
    parent_cfg.agent_name    = "parent";
    parent_cfg.skills_dirs   = { "/home/ubuntu/.daedalus/skills/" };
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
    while (std::getline(std::cin, line)) {
        if (line == "/quit" || line == "/exit") break;
        if (line.empty()) continue;
        parent.submit_user_message(std::move(line));
        parent.run_one_turn();
        std::cout << "\n";
    }
    parent.shutdown();
    return 0;
}
