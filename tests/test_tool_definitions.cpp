/**
 * @file test_tool_definitions.cpp
 * @brief Unit tests for ToolHandler tool definitions
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
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
        
        // Should have multiple tools (the actual count depends on implementation)
        CHECK(tool_names.size() >= 10);
        
        // Check for key tools we expect
        bool has_read_file = false;
        bool has_execute_bash = false;
        bool has_create_file = false;
        bool has_edit_file_patch = false;
        
        for (const auto& name : tool_names) {
            if (name == "read_file") has_read_file = true;
            if (name == "execute_bash") has_execute_bash = true;
            if (name == "create_file") has_create_file = true;
            if (name == "edit_file_patch") has_edit_file_patch = true;
        }
        
        CHECK(has_read_file);
        CHECK(has_execute_bash);
        CHECK(has_create_file);
        CHECK(has_edit_file_patch);
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
            "execute_bash", "create_file", "read_file"
        });
        json filtered = json::parse(filtered_defs);
        
        std::vector<std::string> names;
        for (const auto& item : filtered) {
            if (item.contains("function") && item["function"].contains("name")) {
                names.push_back(item["function"]["name"]);
            }
        }
        
        CHECK(std::find(names.begin(), names.end(), "execute_bash") == names.end());
        CHECK(std::find(names.begin(), names.end(), "create_file") == names.end());
        CHECK(std::find(names.begin(), names.end(), "read_file") == names.end());
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
