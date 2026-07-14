/**
 * @file test_system_info.cpp
 * @brief Unit tests for SystemInfoTool
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "tools/SystemInfoTool.h"
#include "core/FileIO.h"
#include <string>
#include <fstream>

using json = nlohmann::json;

TEST_CASE("SystemInfoTool - Basic functionality") {
    SystemInfoTool tool;
    
    // Check tool name
    CHECK(tool.get_name() == "system_info");
    
    // Check tool definition exists and has correct structure
    json definition = tool.get_definition();
    CHECK(definition["type"] == "function");
    CHECK(definition["function"]["name"] == "system_info");
    CHECK(definition["function"].contains("description"));
    CHECK(definition["function"].contains("parameters"));
    
    // Check parameters structure
    json& params = definition["function"]["parameters"];
    CHECK(params["type"] == "object");
    CHECK(params["properties"].empty());
    CHECK(params["required"] == json::array());
    
    // Description should be informative
    std::string description = definition["function"]["description"].get<std::string>();
    CHECK(description.find("kernel") != std::string::npos);
    CHECK(description.find("container") != std::string::npos);
    CHECK(description.find("X11") != std::string::npos);
    CHECK(description.find("Wayland") != std::string::npos);
}

TEST_CASE("SystemInfoTool - Tool definition schema") {
    SystemInfoTool tool;
    json definition = tool.get_definition();
    
    // Verify complete schema structure
    REQUIRE(definition.contains("type"));
    REQUIRE(definition.contains("function"));
    
    json& func = definition["function"];
    REQUIRE(func["name"] == "system_info");
    REQUIRE(func["parameters"].is_object());
    
    json& params = func["parameters"];
    REQUIRE(params["type"] == "object");
    REQUIRE(params.contains("properties"));
    REQUIRE(params.contains("required"));
}

TEST_CASE("SystemInfoTool - Tool name consistency") {
    SystemInfoTool tool;
    
    // The name in get_name() should match the definition
    CHECK(tool.get_name() == "system_info");
    
    json definition = tool.get_definition();
    CHECK(definition["function"]["name"] == tool.get_name());
}

TEST_CASE("SystemInfoTool - Execute with empty args") {
    SystemInfoTool tool;
    FileIO file_io(".");
    
    // Test with empty JSON object (what LLM might send)
    json args = json::object();
    
    json result = tool.execute(args, file_io);
    
    // Should have status field
    CHECK(result.contains("status"));
    CHECK(result["status"] == "success");
    
    // Should have system info
    CHECK(result.contains("system"));
}

TEST_CASE("SystemInfoTool - Execute with null") {
    SystemInfoTool tool;
    FileIO file_io(".");
    
    // Test with null (edge case)
    json null_args = nullptr;
    
    json result = tool.execute(null_args, file_io);
    
    // Should have status field
    CHECK(result.contains("status"));
    CHECK(result["status"] == "success");
    
    // Should have system info
    CHECK(result.contains("system"));
}
