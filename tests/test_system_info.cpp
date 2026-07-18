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

TEST_CASE("SystemInfoTool - ISO 8601 timezone offset carries colon (§FIX-tz-colon-index)") {
    SystemInfoTool tool;
    json dt = tool.get_date_time_info();
    REQUIRE(dt.contains("iso"));
    std::string iso = dt["iso"].get<std::string>();
    // e.g. "2026-07-18T11:00:50+00:00" — when a numeric offset is
    // present it must be "+HH:MM", not "+HHMM".
    REQUIRE(iso.size() >= 6);
    char sign = iso[iso.size() - 6];
    if (sign == '+' || sign == '-') {
        CHECK(iso[iso.size() - 3] == ':');
    }
}

TEST_CASE("SystemInfoTool - os-release values match the file (§FIX-os-release-offsets)") {
    // Read the expected values straight from /etc/os-release (if this
    // host has one) and strip optional surrounding quotes.
    auto read_key = [](const std::string& key) -> std::string {
        std::ifstream f("/etc/os-release");
        if (!f.is_open()) return "";
        std::string line;
        while (std::getline(f, line)) {
            if (line.rfind(key, 0) == 0) {
                std::string v = line.substr(key.size());
                if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
                    v = v.substr(1, v.size() - 2);
                }
                return v;
            }
        }
        return "";
    };

    std::string expected_name = read_key("NAME=");
    if (expected_name.empty()) return;  // no /etc/os-release on this host

    SystemInfoTool tool;
    json os = tool.get_os_info();
    REQUIRE(os.contains("name"));
    CHECK(os["name"].get<std::string>() == expected_name);

    std::string expected_id = read_key("ID=");
    if (!expected_id.empty()) {
        REQUIRE(os.contains("id"));
        CHECK(os["id"].get<std::string>() == expected_id);
    }
}
