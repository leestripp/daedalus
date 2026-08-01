/**
 * @file test_tool_definitions.cpp
 * @brief Unit tests for ToolHandler tool definitions
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <algorithm>
#include <string>
#include <vector>
// Include ToolHandler after doctest
#include "tools/ToolHandler.h"
#include "tools/ToolFactory.h"

using json = nlohmann::json;

TEST_SUITE("ToolDefinitions") {
    
    TEST_CASE("get_tool_definitions returns non-empty string") {
        std::string defs = ToolHandler::get_tool_definitions();
        CHECK(!defs.empty());
    }
    
    TEST_CASE("get_tool_definitions returns valid JSON") {
        std::string defs = ToolHandler::get_tool_definitions();
        REQUIRE_NOTHROW(
            json parsed = json::parse(defs);
            CHECK(parsed.is_array());
        );
    }
    
    TEST_CASE("get_tool_definitions contains expected tools") {
        std::string defs = ToolHandler::get_tool_definitions();
        json parsed = json::parse(defs);

        // Track tool names found
        std::vector<std::string> tool_names;
        for (const auto& item : parsed) {
            if (item.contains("function") && item["function"].contains("name")) {
                tool_names.push_back(item["function"]["name"]);
            }
        }

        // §TOOL-TRIM (prompt.txt): the default library surface is minimal.
        // Only core agent tools are registered by ToolFactory. File / git /
        // search / pdf / system-info / background / network tools live in
        // demos/example-tools/ and are registered by hosts that need them.
        CHECK(tool_names.size() >= 8);

        // Check for core tools we expect
        bool has_execute_bash = false;
        bool has_tell_user = false;
        bool has_stop = false;
        bool has_set_status = false;

        for (const auto& name : tool_names) {
            if (name == "execute_bash") has_execute_bash = true;
            if (name == "tell_user") has_tell_user = true;
            if (name == "stop") has_stop = true;
            if (name == "set_status") has_set_status = true;
        }

        CHECK(has_execute_bash);
        CHECK(has_tell_user);
        CHECK(has_stop);
        CHECK(has_set_status);

        // compress_context was removed: compression is host/system
        // driven (90% threshold or Agent::request_context_compression()).
        CHECK(std::find(tool_names.begin(), tool_names.end(), "compress_context") == tool_names.end());

        // Non-core tools are NOT in the default set.
        CHECK(std::find(tool_names.begin(), tool_names.end(), "read_file") == tool_names.end());
        CHECK(std::find(tool_names.begin(), tool_names.end(), "create_file") == tool_names.end());
        CHECK(std::find(tool_names.begin(), tool_names.end(), "edit_file_patch") == tool_names.end());
    }
    
    TEST_CASE("get_tool_definitions has required fields for each tool") {
        std::string defs = ToolHandler::get_tool_definitions();
        json parsed = json::parse(defs);
        
        for (const auto& item : parsed) {
            // Each tool should have function with name and description
            REQUIRE(item.contains("type"));
            CHECK(item["type"] == "function");
            
            REQUIRE(item.contains("function"));
            const json& func = item["function"];
            
            REQUIRE(func.contains("name"));
            REQUIRE(func.contains("description"));
            REQUIRE(!func["name"].get<std::string>().empty());
            REQUIRE(!func["description"].get<std::string>().empty());
            
            // If parameters exist, they should be an object
            if (func.contains("parameters")) {
                CHECK(func["parameters"].is_object());
            }
        }
    }
    
    TEST_CASE("get_tool_definitions_filtered excludes specified tools") {
        std::string all_defs = ToolHandler::get_tool_definitions();
        json all_parsed = json::parse(all_defs);
        
        // Filter out execute_bash
        std::string filtered_defs = ToolHandler::get_tool_definitions_filtered({"execute_bash"});
        json filtered_parsed = json::parse(filtered_defs);
        
        // Filtered should have fewer tools
        CHECK(filtered_parsed.size() < all_parsed.size());
        
        // Verify execute_bash is NOT in filtered
        for (const auto& item : filtered_parsed) {
            if (item.contains("function") && item["function"].contains("name")) {
                std::string name = item["function"]["name"];
                CHECK(name != "execute_bash");
            }
        }
    }
    
    TEST_CASE("get_tool_definitions_filtered with multiple exclusions") {
        std::string filtered_defs = ToolHandler::get_tool_definitions_filtered({
            "execute_bash", "tell_user", "compress_context"
        });
        json filtered = json::parse(filtered_defs);

        std::vector<std::string> names;
        for (const auto& item : filtered) {
            if (item.contains("function") && item["function"].contains("name")) {
                names.push_back(item["function"]["name"]);
            }
        }

        CHECK(std::find(names.begin(), names.end(), "execute_bash") == names.end());
        CHECK(std::find(names.begin(), names.end(), "tell_user") == names.end());
        CHECK(std::find(names.begin(), names.end(), "compress_context") == names.end());
    }

    TEST_CASE("matches_pattern wildcard semantics (§FIX-leading-wildcard)") {
        // Exact and prefix (no wildcard)
        CHECK(ToolHandler::matches_pattern("git status", "git status") == true);
        CHECK(ToolHandler::matches_pattern("git status -s", "git status") == true);

        // Pure wildcard matches everything
        CHECK(ToolHandler::matches_pattern("anything at all", "*") == true);
        CHECK(ToolHandler::matches_pattern("ls -la", "***") == true);

        // Trailing wildcard = prefix match
        CHECK(ToolHandler::matches_pattern("rm -rf build", "rm -rf *") == true);
        CHECK(ToolHandler::matches_pattern("ls build", "rm -rf *") == false);

        // Leading wildcard = suffix match
        CHECK(ToolHandler::matches_pattern("cat notes.txt", "*.txt") == true);
        CHECK(ToolHandler::matches_pattern("cat notes.md", "*.txt") == false);

        // Leading + trailing wildcard = contains match
        CHECK(ToolHandler::matches_pattern("git status -s", "*status*") == true);
        CHECK(ToolHandler::matches_pattern("git log", "*status*") == false);

        // Anchored on both ends with middle parts in order
        CHECK(ToolHandler::matches_pattern("make all install", "make*install") == true);
        CHECK(ToolHandler::matches_pattern("make install all", "make*all") == true);
        CHECK(ToolHandler::matches_pattern("xmake all", "make*all") == false);
    }
}
