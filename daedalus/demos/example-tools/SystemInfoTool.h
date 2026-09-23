#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "tools/BaseTool.h"

class FileIO;

using json = nlohmann::json;

/**
 * SystemInfoTool - Gathers system information about the current environment
 * 
 * Collects information about:
 * - Kernel (name, version, release)
 * - Operating System (name, version, architecture)
 * - Container (Docker/Podman detection, image info)
 * - Date/Time (current time, timezone)
 * - Display Server (X11/Wayland, desktop environment, window manager)
 * - Session (terminal, user, hostname)
 */
class SystemInfoTool : public BaseTool {
public:
    /**
     * Get the name of this tool
     */
    std::string get_name() const override { return "system_info"; }
    
    /**
     * Get the description of this tool
     */
    std::string get_description() const override { return "Gather system information: OS, kernel, container, display server, session, and current time."; }
    
    /**
     * Get the tool definition in JSON format for LLM
     */
    json get_definition() const override;
    
    /**
     * Execute the tool to gather system information
     * @param args JSON object (unused for this tool - no parameters)
     * @param file_io Reference to FileIO for file operations
     * @return JSON result containing all system information
     */
    json execute(const json& args, FileIO& file_io) override;

public:
    // Helper functions for gathering different types of system info
    json get_kernel_info() const;
    json get_os_info() const;
    json get_container_info() const;
    json get_date_time_info() const;
    json get_display_info() const;
    json get_session_info() const;
    
    // Low-level detection helpers
    std::string read_file_content(const std::string& path) const;
    std::string get_env_var(const std::string& name) const;
    bool file_exists(const std::string& path) const;
};
