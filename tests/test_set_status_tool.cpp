/**
 * @file test_set_status_tool.cpp
 * @brief §State Machine — tests for SetStatusTool.
 *
 * These tests verify:
 *   1. Tool definition (JSON schema) is well-formed.
 *   2. Valid state names are accepted (case-insensitive) and normalised.
 *   3. Invalid state names return a structured error.
 *   4. Missing / wrong-type 'state' argument returns a structured error.
 *   5. The success status string is "set_state" so LLMClient can
 *      branch on it and fire the public state-change event.
 *   6. agent_state_from() (the inverse of to_string()) round-trips.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>
#include <filesystem>

#include "core/FileIO.h"
#include "tools/SetStatusTool.h"
#include "daedalus/types/AgentState.h"

using json = nlohmann::json;

// ============================================================================
// Suite 1: Tool definition is well-formed.
// ============================================================================

TEST_SUITE("SetStatusTool — JSON schema") {

    TEST_CASE("Tool name is 'set_status'") {
        SetStatusTool tool;
        CHECK(tool.get_name() == "set_status");
    }

    TEST_CASE("Definition declares 'state' as a required string") {
        SetStatusTool tool;
        json def = tool.get_definition();
        REQUIRE(def.contains("function"));
        const auto& fn = def["function"];
        REQUIRE(fn.contains("parameters"));
        const auto& params = fn["parameters"];
        REQUIRE(params.contains("properties"));
        REQUIRE(params["properties"].contains("state"));
        CHECK(params["properties"]["state"]["type"] == "string");
        REQUIRE(params.contains("required"));
        CHECK(params["required"].is_array());
        CHECK(params["required"].size() == 1);
        CHECK(params["required"][0] == "state");
    }
}

// ============================================================================
// Suite 2: Valid state names — accepted, normalised.
// ============================================================================

TEST_SUITE("SetStatusTool — valid input") {

    TEST_CASE("Every public AgentState name round-trips") {
        SetStatusTool tool;
        FileIO io(std::filesystem::temp_directory_path());  // unused by SetStatusTool
        const std::vector<std::string> valid = {
            "Idle", "Understanding", "Planning", "Acting", "Verifying",
            "Documenting", "AwaitingApproval", "Compressing",
            "Done", "Cancelled", "Error",
        };
        for (const auto& name : valid) {
            json args = {{"state", name}};
            json r = tool.execute(args, io);
            CHECK(r["status"] == "set_state");
            CHECK(r["state"]  == name);
        }
    }

    TEST_CASE("Lower-case input is normalised to title-case") {
        SetStatusTool tool;
        FileIO io(std::filesystem::temp_directory_path());
        json args = {{"state", "planning"}};
        json r = tool.execute(args, io);
        CHECK(r["status"] == "set_state");
        CHECK(r["state"]  == "Planning");
    }

    TEST_CASE("UPPER-CASE input is normalised") {
        SetStatusTool tool;
        FileIO io(std::filesystem::temp_directory_path());
        json args = {{"state", "ACTING"}};
        json r = tool.execute(args, io);
        CHECK(r["status"] == "set_state");
        CHECK(r["state"]  == "Acting");
    }

    TEST_CASE("MiXeD case is normalised") {
        SetStatusTool tool;
        FileIO io(std::filesystem::temp_directory_path());
        json args = {{"state", "VeRiFyInG"}};
        json r = tool.execute(args, io);
        CHECK(r["status"] == "set_state");
        CHECK(r["state"]  == "Verifying");
    }

    TEST_CASE("Leading/trailing whitespace is trimmed") {
        SetStatusTool tool;
        FileIO io(std::filesystem::temp_directory_path());
        json args = {{"state", "  Done \t"}};
        json r = tool.execute(args, io);
        CHECK(r["status"] == "set_state");
        CHECK(r["state"]  == "Done");
    }
}

// ============================================================================
// Suite 3: Invalid input — structured error.
// ============================================================================

TEST_SUITE("SetStatusTool — invalid input") {

    TEST_CASE("Unknown state returns error listing valid names") {
        SetStatusTool tool;
        FileIO io(std::filesystem::temp_directory_path());
        json args = {{"state", "Foo"}};
        json r = tool.execute(args, io);
        CHECK(r["status"]  == "error");
        CHECK(r.contains("message"));
        std::string msg = r["message"].get<std::string>();
        CHECK(msg.find("Foo") != std::string::npos);
        // Message should list every valid state so the model can self-correct.
        CHECK(msg.find("Planning")    != std::string::npos);
        CHECK(msg.find("Acting")      != std::string::npos);
        CHECK(msg.find("Verifying")   != std::string::npos);
        CHECK(msg.find("Documenting") != std::string::npos);
        CHECK(msg.find("Done")        != std::string::npos);
    }

    TEST_CASE("Empty string returns error") {
        SetStatusTool tool;
        FileIO io(std::filesystem::temp_directory_path());
        json args = {{"state", ""}};
        json r = tool.execute(args, io);
        CHECK(r["status"] == "error");
    }

    TEST_CASE("Whitespace-only string returns error") {
        SetStatusTool tool;
        FileIO io(std::filesystem::temp_directory_path());
        json args = {{"state", "   \t  "}};
        json r = tool.execute(args, io);
        CHECK(r["status"] == "error");
    }

    TEST_CASE("Missing 'state' key returns error") {
        SetStatusTool tool;
        FileIO io(std::filesystem::temp_directory_path());
        json args = json::object();
        json r = tool.execute(args, io);
        CHECK(r["status"] == "error");
    }

    TEST_CASE("Non-string 'state' returns error") {
        SetStatusTool tool;
        FileIO io(std::filesystem::temp_directory_path());
        json args = {{"state", 42}};
        json r = tool.execute(args, io);
        CHECK(r["status"] == "error");
    }

    TEST_CASE("JSON-string form (legacy clients) is also accepted") {
        SetStatusTool tool;
        FileIO io(std::filesystem::temp_directory_path());
        // Some legacy LLM clients send the args as a JSON-encoded string
        // rather than an object.  SetStatusTool accepts both.
        json args = std::string(R"({"state":"Done"})");
        json r = tool.execute(args, io);
        CHECK(r["status"] == "set_state");
        CHECK(r["state"]  == "Done");
    }

    TEST_CASE("JSON-string form with unknown state still errors") {
        SetStatusTool tool;
        FileIO io(std::filesystem::temp_directory_path());
        json args = std::string(R"({"state":"Bogus"})");
        json r = tool.execute(args, io);
        CHECK(r["status"] == "error");
    }
}

// ============================================================================
// Suite 4: agent_state_from() — the public inverse of to_string().
// ============================================================================

TEST_SUITE("daedalus::agent_state_from") {

    TEST_CASE("Round-trips every public state") {
        bool ok = false;
        CHECK(daedalus::agent_state_from("Idle", &ok)             == daedalus::AgentState::Idle);             CHECK(ok);
        CHECK(daedalus::agent_state_from("Understanding", &ok)    == daedalus::AgentState::Understanding);    CHECK(ok);
        CHECK(daedalus::agent_state_from("Planning", &ok)         == daedalus::AgentState::Planning);         CHECK(ok);
        CHECK(daedalus::agent_state_from("Acting", &ok)           == daedalus::AgentState::Acting);           CHECK(ok);
        CHECK(daedalus::agent_state_from("Verifying", &ok)        == daedalus::AgentState::Verifying);        CHECK(ok);
        CHECK(daedalus::agent_state_from("Documenting", &ok)      == daedalus::AgentState::Documenting);      CHECK(ok);
        CHECK(daedalus::agent_state_from("AwaitingApproval", &ok) == daedalus::AgentState::AwaitingApproval); CHECK(ok);
        CHECK(daedalus::agent_state_from("Compressing", &ok)      == daedalus::AgentState::Compressing);      CHECK(ok);
        CHECK(daedalus::agent_state_from("Done", &ok)             == daedalus::AgentState::Done);             CHECK(ok);
        CHECK(daedalus::agent_state_from("Cancelled", &ok)        == daedalus::AgentState::Cancelled);        CHECK(ok);
        CHECK(daedalus::agent_state_from("Error", &ok)            == daedalus::AgentState::Error);            CHECK(ok);
    }

    TEST_CASE("Case-insensitive matching") {
        bool ok = false;
        CHECK(daedalus::agent_state_from("planning",   &ok) == daedalus::AgentState::Planning); CHECK(ok);
        CHECK(daedalus::agent_state_from("ACTING",     &ok) == daedalus::AgentState::Acting);   CHECK(ok);
        CHECK(daedalus::agent_state_from("VeRiFyInG",  &ok) == daedalus::AgentState::Verifying);CHECK(ok);
    }

    TEST_CASE("Unknown name returns Idle and ok=false") {
        bool ok = true;
        auto s = daedalus::agent_state_from("Bogus", &ok);
        CHECK(s == daedalus::AgentState::Idle);
        CHECK_FALSE(ok);
    }

    TEST_CASE("Empty string returns Idle and ok=false") {
        bool ok = true;
        auto s = daedalus::agent_state_from("", &ok);
        CHECK(s == daedalus::AgentState::Idle);
        CHECK_FALSE(ok);
    }

    TEST_CASE("ok pointer may be nullptr (callers can ignore the flag)") {
        // Must not crash.
        CHECK(daedalus::agent_state_from("Done")             == daedalus::AgentState::Done);
        CHECK(daedalus::agent_state_from("Bogus")            == daedalus::AgentState::Idle);
    }
}

// ============================================================================
// Suite 5: Round-trip — to_string(agent_state_from(name)) == Normalised(name).
// ============================================================================

TEST_SUITE("to_string ↔ agent_state_from round-trip") {

    TEST_CASE("Every public name survives a round-trip") {
        const std::vector<std::string> names = {
            "Idle", "Understanding", "Planning", "Acting", "Verifying",
            "Documenting", "AwaitingApproval", "Compressing",
            "Done", "Cancelled", "Error",
        };
        for (const auto& name : names) {
            bool ok = false;
            auto s = daedalus::agent_state_from(name, &ok);
            REQUIRE(ok);
            CHECK(std::string(daedalus::to_string(s)) == name);
        }
    }
}
