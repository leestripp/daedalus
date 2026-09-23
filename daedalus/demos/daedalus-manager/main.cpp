// SPDX-License-Identifier: BSL-1.0
// demos/daedalus-manager/main.cpp
//
// Manager-pattern demo: 3-Agent orchestration, all on the public
// <daedalus.h> API.
//
// Pattern (per the §Phase 7 "modes are demos" principle):
//
//   ┌─ host loop (capped at N iterations) ──────────────────────┐
//   │  1. submit goal to planner agent  → run_one_turn           │
//   │  2. submit plan to actor agent    → run_one_turn           │
//   │  3. submit work to validator      → run_one_turn           │
//   │  4. parse verdict from validator's last assistant text     │
//   │     "VERDICT: APPROVED"                          → done   │
//   │     "VERDICT: REJECTED: <reasons>"                → loop  │
//   └────────────────────────────────────────────────────────────┘
//
// All 3 agents share per-role message histories (planner/actor/validator
// namespaces). The verdict flows through normal assistant text.

#include <iostream>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <daedalus.h>
#include <daedalus/default_paths.h>
#include <daedalus/paths.h>

#include "ThinkingSpinner.h"
#include "tagged_sink.h"
#include "verdict.h"

namespace fs = std::filesystem;

// =============================================================================
// build one Agent (planner, actor, or validator)
// =============================================================================

struct AgentSpec {
    std::string name;
    std::string system_prompt;
    int         max_iterations;
};

static std::unique_ptr<daedalus::Agent>
make_agent(const AgentSpec& spec,
           const daedalus::AgentConfig& base_cfg,
           std::shared_ptr<daedalus::AgentSink> sink)
{
    daedalus::AgentConfig cfg = base_cfg;
    cfg.system_prompt       = spec.system_prompt;
    cfg.max_agent_iterations = spec.max_iterations;
    cfg.session_id          = "manager-demo:" + spec.name;
    cfg.agent_name          = spec.name;   // §Phase 8.8: per-role namespace
    return std::make_unique<daedalus::Agent>(std::move(cfg), sink);
}

int main(int argc, char* argv[]) {
    // §F2 fix: --help short-circuit before argv parsing.
    // Without this, `daedalus-manager --help` treats "--help" as project_root
    // and Agent::initialize() throws std::invalid_argument("Project path
    // does not exist: --help") → std::terminate.
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            std::cout << "Usage: daedalus-manager <project_root> <goal> <max_iters> <model>\n"
                      << "\n"
                      << "Demonstrates the 3-agent manager pattern:\n"
                      << "  - planner   : produces a numbered plan from the goal,\n"
                      << "  - actor     : executes the plan with tools,\n"
                      << "  - validator : inspects the project and emits VERDICT.\n"
                      << "Loop: planner -> actor -> validator -> (re-plan on REJECTED).\n"
                      << "\n"
                      << "Arguments:\n"
                      << "  project_root   Directory to operate on\n"
                      << "  goal           Free-form goal string\n"
                      << "  max_iters      Max planner/actor/validator iterations\n"
                      << "  model          Model name to use (must be configured)\n"
                      << "\n"
                      << "Options:\n"
                      << "  --help, -h           Show this message and exit\n"
                      << "  --version, -v        Show version\n"
                      << "  --discover-models <name>  Query the provider's /v1/models endpoint\n"
                      << "                            for the named configured model and exit\n"
                      << "\n"
                      << "Exit codes:\n"
                      << "  0   Validator approved within max_iters\n"
                      << "  2   Pipeline did not converge\n";
            return 0;
        }
        if (a == "--version" || a == "-v") {
            std::cout << "daedalus-manager (libdaedalus)\n";
            return 0;
        }
        if (a == "--discover-models") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --discover-models requires a model name\n";
                return 1;
            }
            const std::string name = argv[++i];
            auto result = daedalus::Agent::discover_models(
                name, daedalus::Paths::global().user_data_root());
            if (!result.success) {
                std::cerr << "Error: " << result.error_message << "\n";
                return 1;
            }
            for (const auto& m : result.models) {
                std::cout << m.id << "\n";
            }
            return 0;
        }
    }

    fs::path project_root = (argc > 1) ? argv[1] : ".";
    std::string goal = (argc > 2)
        ? argv[2]
        : "Add a copyright header (a single line: '// Copyright (c) "
          "2026 example.com. All rights reserved.') to every .cpp file "
          "in the project root, but only files that don't already have one.";
    int max_iterations = (argc > 3) ? std::stoi(argv[3]) : 3;
    std::string model_name = (argc > 4) ? argv[4] : "";

    std::cout << "Manager demo\n"
              << "  project_root = " << fs::absolute(project_root).string() << "\n"
              << "  goal         = " << goal << "\n"
              << "  max_iters    = " << max_iterations << "\n"
              << "  model        = " << (model_name.empty() ? "<settings.json default>" : model_name) << "\n\n";

    // ----- shared base config -----
    daedalus::AgentConfig base;
    base.project_root  = project_root;
    base.model_name    = model_name;
    base.auto_approve = true;          // demo: no bash-approval prompts
    // §manager-1 (2026-07-16): was hardcoded "/home/ubuntu/.daedalus/skills/".
    // Use Paths::global() so the demo works on any system.
    base.skills_dirs   = { daedalus::Paths::global().global_skills_dir() };
    base.stream        = true;
    // §manager-2 (2026-07-16): explicit daedalus_home using
    // user_data_root() so all 3 agents (planner/actor/validator) find
    // the same model store. Without this they may pick up a
    // different default than the child in daedalus-subagent.
    base.daedalus_home = daedalus::Paths::global().user_data_root();

    // ----- 3 agents -----
    AgentSpec planner_spec{
        "planner",
        "You are the PLANNER. Given the user's goal, produce a clear, "
        "numbered plan of concrete steps the next agent should take. "
        "Do NOT execute any tools. Output ONLY the plan as a numbered "
        "list (one step per line).",
        10
    };
    AgentSpec actor_spec{
        "actor",
        "You are the EXECUTOR. The PLANNER has produced a numbered plan "
        "earlier in this conversation. Read that plan and execute it. "
        "Use your tools (read_file, edit_file_patch, execute_bash, etc.) "
        "to make the changes. End with 'SUMMARY: <one-sentence>'. "
        "Do not start a new task.",
        50
    };
    AgentSpec validator_spec{
        "validator",
        "You are the VALIDATOR. The PLANNER designed a plan and the "
        "ACTOR executed it (see the conversation history). Examine the "
        "current state of the project and verify that the goal was met. "
        "You may use read_file, list_directory, grep_search. Do NOT "
        "edit files. End your turn with EXACTLY ONE line that begins "
        "with 'VERDICT:' followed by 'APPROVED' or 'REJECTED: "
        "<comma-separated issues>'.",
        30
    };

    auto planner   = make_agent(planner_spec,   base, std::make_shared<TaggedSink>("planner"));
    auto actor     = make_agent(actor_spec,     base, std::make_shared<TaggedSink>("actor"));
    auto validator = make_agent(validator_spec, base, std::make_shared<TaggedSink>("validator"));

    planner->initialize();
    actor->initialize();
    validator->initialize();

    // ----- host loop -----
    std::string feedback;
    bool        done     = false;
    int         iter     = 0;

    while (!done && iter < max_iterations) {
        ++iter;
        std::cout << "\n========= iteration " << iter << "/" << max_iterations << " =========\n";

        // 1. Planner
        std::string planner_msg = "Goal:\n" + goal;
        if (!feedback.empty()) {
            planner_msg += "\n\nPrevious iteration was REJECTED with these issues:\n"
                         + feedback
                         + "\nRevise the plan to address them.";
        }
        planner->submit_user_message(std::move(planner_msg));
        planner->run_one_turn();
        std::string plan = planner->last_assistant_text();
        std::cout << "\n[host] plan:\n" << plan << "\n";

        // 2. Actor
        std::string actor_msg = "Plan from the planner:\n" + plan
                              + "\n\nExecute this plan. End with 'SUMMARY: <one line>'.";
        actor->submit_user_message(std::move(actor_msg));
        actor->run_one_turn();
        std::string action = actor->last_assistant_text();
        std::cout << "\n[host] actor's summary:\n" << action << "\n";

        // 3. Validator
        std::string validator_msg =
            "Goal: " + goal + "\n\n"
            "Plan's plan:\n" + plan + "\n\n"
            "Actor's work:\n" + action + "\n\n"
            "Examine the project files. Output VERDICT: APPROVED "
            "or VERDICT: REJECTED: <issues>.";
        validator->submit_user_message(std::move(validator_msg));
        validator->run_one_turn();
        std::string verdict_text = validator->last_assistant_text();
        std::cout << "\n[host] validator said:\n" << verdict_text << "\n";

        // 4. Parse verdict
        Verdict v = parse_verdict(verdict_text);
        if (v.approved) {
            std::cout << "\n[host] ✓ Validator approved. Pipeline complete after "
                      << iter << " iteration(s).\n";
            done = true;
        } else {
            std::cout << "\n[host] ✗ Validator rejected. Issues: " << v.issues << "\n";
            feedback = v.issues.empty() ? "(no details)" : v.issues;
        }
    }

    if (!done) {
        std::cout << "\n[host] Pipeline did not converge in " << max_iterations
                  << " iterations. Last feedback: " << feedback << "\n";
    }

    // §ContextCache: surface provider prompt-cache hits (MiniMax/Kimi).
    // Quiet for cache-less providers so the demo output is unchanged.
    auto print_cache_line = [](const char* tag, auto& agent) {
        const auto st = agent->stats();
        if (st.cached_tokens <= 0) return;
        std::cout << "[host] " << tag << " tokens=" << st.total_tokens
                  << " (prompt=" << st.prompt_tokens
                  << ", completion=" << st.completion_tokens
                  << ", cached=" << st.cached_tokens << ")\n";
    };
    print_cache_line("planner",   planner);
    print_cache_line("actor",     actor);
    print_cache_line("validator", validator);

    planner->shutdown();
    actor->shutdown();
    validator->shutdown();
    return done ? 0 : 2;
}
