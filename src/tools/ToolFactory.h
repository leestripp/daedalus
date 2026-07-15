#pragma once

#include <memory>
#include <vector>
#include <string>
#include <nlohmann/json.hpp>
#include "BaseTool.h"

class FileIO;
namespace mcp { class McpServerManager; }  // §Phase 11: forward declaration

using json = nlohmann::json;

/**
 * ToolFactory - Factory class for creating and managing tools
 */
class ToolFactory {
public:
    ToolFactory();
    ~ToolFactory() = default;
    
    /**
     * Get all tool definitions as a JSON array
     */
    [[nodiscard]] json get_all_definitions() const;
    
    /**
     * Execute a tool by name (no progress callback).
     * @param tool_name Name of the tool to execute
     * @param args Arguments for the tool
     * @param file_io Reference to FileIO
     * @return JSON result of execution
     */
    json execute_tool(const std::string& tool_name, const json& args, FileIO& file_io);

    /**
     * Execute a tool by name, with an optional progress callback.
     * Tools that override the 3-arg `BaseTool::execute` overload
     * receive `progress` and can call it freely; tools that don't
     * override it get the same behaviour as the no-callback version.
     */
    json execute_tool(const std::string& tool_name,
                      const json& args,
                      FileIO& file_io,
                      ToolProgressFn progress);

    /**
     * Rebuild the registered set of MCP-proxied tools from the currently
     * connected MCP servers. Each server's `tools/list` becomes one
     * `McpProxiedTool` per tool, named `mcp__<server>__<tool>`.
     *
     * Call this whenever MCP server state has changed (after a successful
     * new connection, after a disconnect, after `servers.json` is re-read).
     * The factory treats the result as the authoritative MCP-tool surface
     * until the next call. See Agent.md §18.
     */
    void refresh_mcp_tools();
    
    /**
     * Check if a tool exists
     */
    [[nodiscard]] bool has_tool(const std::string& tool_name) const;
    
    /**
     * Get a tool by name
     */
    [[nodiscard]] std::shared_ptr<BaseTool> get_tool(const std::string& tool_name) const;
    
    /**
     * Get list of all tool names
     */
    [[nodiscard]] std::vector<std::string> get_tool_names() const;

    /**
     * §Phase 6: register an external tool (e.g. a HostTool adapter).
     * Public so the Agent facade can hand host-registered tools down
     * here after the factory is constructed.
     */
    void register_tool(std::shared_ptr<BaseTool> tool);

    // §Phase 11: inject McpServerManager*. Must be called before
    // refresh_mcp_tools(). nullptr = MCP disabled.
    void set_mcp_manager(mcp::McpServerManager* mcp_manager);

private:
    std::vector<std::shared_ptr<BaseTool>> m_tools;

    // §Phase 11: injected McpServerManager* (nullptr = MCP disabled).
    mcp::McpServerManager* mcp_manager_ = nullptr;

    void initialize_tools();
};
