#pragma once

#include <string>
#include <nlohmann/json.hpp>

class FileIO;  // Forward declaration

using json = nlohmann::json;

/**
 * BaseTool - Abstract base class for all tools
 * Each tool implements execute() to perform its operation
 */
class BaseTool {
public:
    virtual ~BaseTool() = default;
    
    /**
     * Get the name of this tool
     */
    [[nodiscard]] virtual std::string get_name() const = 0;
    
    /**
     * Get the description of this tool
     */
    [[nodiscard]] virtual std::string get_description() const = 0;
    
    /**
     * Get the tool definition in JSON format for LLM
     */
    [[nodiscard]] virtual json get_definition() const = 0;
    
    /**
     * Execute the tool with given arguments
     * @param args JSON object containing tool arguments
     * @param file_io Reference to FileIO for file operations
     * @return JSON result of the tool execution
     */
    virtual json execute(const json& args, FileIO& file_io) = 0;
    
protected:
    BaseTool() = default;
};
