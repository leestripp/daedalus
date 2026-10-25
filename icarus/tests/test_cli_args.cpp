// SPDX-License-Identifier: BSL-1.0
// tests/test_cli_args.cpp
//
// Unit tests for parse_cli_args() in src/util.h — the CLI flag parser.
// Pure function tests: no filesystem, no network, no daedalus runtime.
//
// NOTE: flags that call std::exit(1) on missing values (--home, --model,
// --threads with no argument, etc.) are deliberately NOT tested here —
// they terminate the test runner. Only well-formed invocations.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "util.h"

#include <string>
#include <vector>

namespace {

// Build an argv-style array from a list of strings (argv[0] = program name).
struct Argv {
    std::vector<std::string> storage{"icarus"};
    std::vector<char*>       ptrs;

    Argv(std::initializer_list<const char*> args) {
        for (auto* a : args) storage.emplace_back(a);
        for (auto& s : storage) ptrs.push_back(s.data());
    }
    int    argc() { return static_cast<int>(ptrs.size()); }
    char** argv() { return ptrs.data(); }
};

} // namespace

TEST_CASE("cli_args: defaults when no flags given") {
    Argv a{};
    CliArgs args = parse_cli_args(a.argc(), a.argv());

    CHECK(args.auto_mode == false);
    CHECK(args.no_memory == false);
    CHECK(args.clear_memory_flag == false);
    CHECK(args.model_list_only == false);
    CHECK(args.discover_models_only == false);
    CHECK(args.stream_enabled == true);   // streaming is the default
    CHECK(args.no_color == false);
    CHECK(args.no_hud == false);
    CHECK(args.no_unicode == false);
    CHECK(args.thread_count_override == 0);
    CHECK(args.model_override.empty());
    CHECK(args.target_path.empty());
    CHECK(args.daedalus_home.empty());
    CHECK(args.skills_dirs.empty());
    CHECK(args.agent_name.empty());
    CHECK(args.system_prompt_file.empty());
    CHECK(args.a2a_url.empty());
    CHECK(args.a2a_port == 0);
    CHECK(args.a2a_agent_mode == false);
    CHECK(args.acp_url_deprecated == false);
    CHECK(args.acp_agent_deprecated == false);
}

TEST_CASE("cli_args: simple boolean flags") {
    Argv a{"--auto", "--no-memory", "--clear-memory", "--model-list",
           "--no-color", "--no-hud", "--no-unicode"};
    CliArgs args = parse_cli_args(a.argc(), a.argv());

    CHECK(args.auto_mode == true);
    CHECK(args.no_memory == true);
    CHECK(args.clear_memory_flag == true);
    CHECK(args.model_list_only == true);
    CHECK(args.no_color == true);
    CHECK(args.no_hud == true);
    CHECK(args.no_unicode == true);
}

TEST_CASE("cli_args: --plain expands to no-color + no-hud + no-unicode") {
    Argv a{"--plain"};
    CliArgs args = parse_cli_args(a.argc(), a.argv());

    CHECK(args.no_color == true);
    CHECK(args.no_hud == true);
    CHECK(args.no_unicode == true);
}

TEST_CASE("cli_args: --no-stream / --stream, last one wins") {
    {
        Argv a{"--no-stream"};
        CliArgs args = parse_cli_args(a.argc(), a.argv());
        CHECK(args.stream_enabled == false);
    }
    {
        Argv a{"--no-stream", "--stream"};
        CliArgs args = parse_cli_args(a.argc(), a.argv());
        CHECK(args.stream_enabled == true);
    }
    {
        Argv a{"--stream", "--no-stream"};
        CliArgs args = parse_cli_args(a.argc(), a.argv());
        CHECK(args.stream_enabled == false);
    }
}

TEST_CASE("cli_args: value flags store their argument") {
    Argv a{"--home", "/tmp/dhome", "--model", "kimi-k3.1-pro",
           "--agent-name", "tester", "--threads", "8",
           "--system-prompt", "/tmp/prompt.md",
           "--a2a-url", "http://localhost:9000", "--a2a-port", "9000"};
    CliArgs args = parse_cli_args(a.argc(), a.argv());

    CHECK(args.daedalus_home == fs::path("/tmp/dhome"));
    CHECK(args.model_override == "kimi-k3.1-pro");
    CHECK(args.agent_name == "tester");
    CHECK(args.thread_count_override == 8);
    CHECK(args.system_prompt_file == fs::path("/tmp/prompt.md"));
    CHECK(args.a2a_url == "http://localhost:9000");
    CHECK(args.a2a_port == 9000);
}

TEST_CASE("cli_args: --skills accumulates multiple directories") {
    Argv a{"--skills", "/opt/skills-a", "--skills", "/opt/skills-b"};
    CliArgs args = parse_cli_args(a.argc(), a.argv());

    REQUIRE(args.skills_dirs.size() == 2);
    CHECK(args.skills_dirs[0] == fs::path("/opt/skills-a"));
    CHECK(args.skills_dirs[1] == fs::path("/opt/skills-b"));
}

TEST_CASE("cli_args: --discover-models captures the model name") {
    Argv a{"--discover-models", "gpt-test"};
    CliArgs args = parse_cli_args(a.argc(), a.argv());

    CHECK(args.discover_models_only == true);
    CHECK(args.discover_model_name == "gpt-test");
}

TEST_CASE("cli_args: --a2a-agent enables agent mode") {
    Argv a{"--a2a-agent"};
    CliArgs args = parse_cli_args(a.argc(), a.argv());

    CHECK(args.a2a_agent_mode == true);
    CHECK(args.acp_agent_deprecated == false);
}

TEST_CASE("cli_args: deprecated ACP aliases map onto A2A options") {
    {
        Argv a{"--acp-url", "http://legacy:8000"};
        CliArgs args = parse_cli_args(a.argc(), a.argv());
        CHECK(args.acp_url_deprecated == true);
        CHECK(args.a2a_url == "http://legacy:8000");
    }
    {
        Argv a{"--acp-agent"};
        CliArgs args = parse_cli_args(a.argc(), a.argv());
        CHECK(args.acp_agent_deprecated == true);
        CHECK(args.a2a_agent_mode == true);
    }
}

TEST_CASE("cli_args: positional argument becomes target_path") {
    Argv a{"--auto", "/some/project/dir"};
    CliArgs args = parse_cli_args(a.argc(), a.argv());

    CHECK(args.target_path == fs::path("/some/project/dir"));
    CHECK(args.auto_mode == true);
}

TEST_CASE("cli_args: unknown --flags are ignored, non-dash args are positional") {
    Argv a{"--definitely-not-a-real-flag", "projdir"};
    CliArgs args = parse_cli_args(a.argc(), a.argv());

    CHECK(args.target_path == fs::path("projdir"));
    CHECK(args.auto_mode == false);
}
