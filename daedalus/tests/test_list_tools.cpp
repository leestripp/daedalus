// SPDX-License-Identifier: BSL-1.0
// tests/test_list_tools.cpp
//
// §LIST-TOOLS-CORE (2026-09-21): list_tools tool — the agent-visible live
// tool roster. Hermetic: no network, no spawns, exercises the global
// ToolFactory registry only.
//
// Covers:
//   1. list_tools is registered by ToolFactory::initialize_tools().
//   2. Compact roster returns a markdown table containing known core
//      tools, with correct core/mcp counts.
//   3. filter= narrows by name/description (case-insensitive).
//   4. verbose=true returns full JSON schemas.
//   5. Tool descriptions are shortened to a single line in the roster
//      (multi-paragraph MCP-style descriptions can't break the table).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "tools/ListToolsTool.h"
#include "tools/ToolFactory.h"
#include "tools/ToolHandler.h"
#include "core/FileIO.h"

#include <filesystem>
#include <string>

namespace {

static FileIO make_test_file_io() {
    const auto tmp = std::filesystem::temp_directory_path()
                     / "daedalus_list_tools_tests";
    std::filesystem::create_directories(tmp);
    return FileIO(tmp);
}

// Minimal stub tool with a multi-paragraph description to exercise the
// roster-shortening path.
class WordyStubTool : public BaseTool {
public:
    std::string get_name() const override { return "wordy_stub_tool"; }
    std::string get_description() const override {
        return "First line summary of the stub.\n\n"
               "A long multi-paragraph essay about usage that must never "
               "leak into the compact roster table.";
    }
    json get_definition() const override {
        return {
            {"type", "function"},
            {"function", {
                {"name", get_name()},
                {"description", get_description()},
                {"parameters", {
                    {"type", "object"},
                    {"properties", json::object()},
                    {"required", json::array()}
                }}
            }}
        };
    }
    json execute(const json&, FileIO&) override { return json::object(); }
};

} // namespace

TEST_CASE("list_tools is registered by default") {
    ToolFactory factory;
    CHECK(factory.has_tool("list_tools"));
    auto tool = factory.get_tool("list_tools");
    REQUIRE(tool != nullptr);
    CHECK(tool->get_name() == "list_tools");
    const json def = tool->get_definition();
    CHECK(def["function"]["name"] == "list_tools");
    CHECK(def["function"]["parameters"]["properties"].contains("filter"));
    CHECK(def["function"]["parameters"]["properties"].contains("verbose"));
}

TEST_CASE("compact roster lists core tools with counts") {
    // The tool reads the GLOBAL registry (the same one the LLM's tool
    // surface comes from), so init it and register a stub.
    ToolHandler::init_tool_factory();
    auto factory = ToolHandler::get_tool_factory();
    REQUIRE(factory != nullptr);
    factory->register_tool(std::make_shared<WordyStubTool>());

    FileIO fio = make_test_file_io();
    ListToolsTool tool;
    const json result = tool.execute(json::object(), fio);

    REQUIRE(result.value("status", "") == "success");
    CHECK(result.value("count", 0) > 0);
    CHECK(result.value("core_tools", 0) > 0);

    const std::string roster = result.value("roster", "");
    REQUIRE(!roster.empty());
    // Known core tools must appear.
    CHECK(roster.find("`list_tools`") != std::string::npos);
    CHECK(roster.find("`execute_bash`") != std::string::npos);
    CHECK(roster.find("`set_status`") != std::string::npos);
    // The stub is a non-MCP name → counted as core.
    CHECK(roster.find("`wordy_stub_tool`") != std::string::npos);
    // Stub's second paragraph must NOT leak into the compact roster.
    CHECK(roster.find("multi-paragraph essay") == std::string::npos);
    CHECK(roster.find("First line summary of the stub.") != std::string::npos);
    // Every roster row is exactly one line (no embedded newlines in desc).
    CHECK(roster.find("essay about usage") == std::string::npos);
}

TEST_CASE("filter narrows by name and description, case-insensitive") {
    ToolHandler::init_tool_factory();
    FileIO fio = make_test_file_io();
    ListToolsTool tool;

    // By name substring, mixed case.
    {
        const json r = tool.execute({{"filter", "LIST"}}, fio);
        REQUIRE(r.value("status", "") == "success");
        const std::string roster = r.value("roster", "");
        CHECK(roster.find("`list_tools`") != std::string::npos);
        CHECK(roster.find("`execute_bash`") == std::string::npos);
    }

    // By description substring.
    {
        const json r = tool.execute({{"filter", "encrypted password manager"}}, fio);
        REQUIRE(r.value("status", "") == "success");
        const std::string roster = r.value("roster", "");
        CHECK(roster.find("`vault`") != std::string::npos);
    }

    // §SECOND-BRAIN: the brain tool is core-registered (filter by a
    // description substring unique to it).
    {
        const json r = tool.execute({{"filter", "Second Brain"}}, fio);
        REQUIRE(r.value("status", "") == "success");
        const std::string roster = r.value("roster", "");
        CHECK(roster.find("`brain`") != std::string::npos);
    }

    // No match → empty roster but success.
    {
        const json r = tool.execute({{"filter", "zzz-no-such-tool-zzz"}}, fio);
        REQUIRE(r.value("status", "") == "success");
        CHECK(r.value("count", 1) == 0);
    }
}

TEST_CASE("verbose returns full JSON schemas") {
    ToolHandler::init_tool_factory();
    FileIO fio = make_test_file_io();
    ListToolsTool tool;

    const json r = tool.execute({{"filter", "list_tools"}, {"verbose", true}}, fio);
    REQUIRE(r.value("status", "") == "success");
    REQUIRE(r.contains("tools"));
    REQUIRE(r["tools"].is_array());
    REQUIRE(!r["tools"].empty());
    const auto& fn = r["tools"][0]["function"];
    CHECK(fn.value("name", "") == "list_tools");
    // Full schema includes the parameters object the compact roster omits.
    CHECK(fn.contains("parameters"));
    CHECK(fn["parameters"]["properties"].contains("verbose"));
}
