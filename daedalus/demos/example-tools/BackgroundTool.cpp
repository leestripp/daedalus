/**
 * @file BackgroundTool.cpp
 * @brief Background process management tool implementation
 */

#include "BackgroundTool.h"

#include "core/BackgroundProcessManager.h"
#include "core/FileIO.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

json BackgroundTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", json::object({
            {"name", get_name()},
            {"description", "Manage background processes for server applications. Start processes in background, send input, read intermediate output, and stop processes. Natural exits automatically deliver status and final output as a later user message that wakes the agent, so after start you may safely call set_status(\"Done\") or stop instead of polling for completion."},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"action", json::object({
                        {"type", "string"},
                        {"description", "Action to perform: start, send, poll, stop, or list"},
                        {"enum", {"start", "send", "poll", "stop", "list"}}
                    })},
                    {"command", json::object({
                        {"type", "string"},
                        {"description", "Command to execute (use with action=start)"}
                    })},
                    {"cwd", json::object({
                        {"type", "string"},
                        {"description", "Working directory for the process (use with action=start)"}
                    })},
                    {"process_id", json::object({
                        {"type", "string"},
                        {"description", "Process ID returned from start action (use with action=send, poll, stop)"}
                    })},
                    {"input", json::object({
                        {"type", "string"},
                        {"description", "Input data to send to process stdin (use with action=send)"}
                    })},
                    {"clear_output", json::object({
                        {"type", "boolean"},
                        {"description", "Whether to clear output after reading (use with action=poll, default false)"}
                    })}
                })},
                {"required", json::array({"action"})}
            })}
        })}
    });
}

json BackgroundTool::execute(const json& args, FileIO& /*file_io*/) {
    // §FIX-json-type-error: the args come from the LLM, which may send
    // "true" as a string or a number where a bool/string is expected.
    // Without this catch, nlohmann's type_error escaped the tool.
    try {
        std::string action;
        if (args.contains("action")) {
            action = args["action"].get<std::string>();
        } else {
            return json::object({
                {"status", "error"},
                {"message", "Missing 'action' parameter"}
            });
        }

        if (action == "start") {
            return handle_start(args);
        } else if (action == "send") {
            return handle_send(args);
        } else if (action == "poll") {
            return handle_poll(args);
        } else if (action == "stop") {
            return handle_stop(args);
        } else if (action == "list") {
            return handle_list(args);
        } else {
            return json::object({
                {"status", "error"},
                {"message", "Unknown action: " + action + ". Valid actions: start, send, poll, stop, list"}
            });
        }
    } catch (const std::exception& e) {
        return json::object({
            {"status", "error"},
            {"message", std::string("Invalid arguments: ") + e.what()}
        });
    }
}

json BackgroundTool::handle_start(const json& args) {
    if (!args.contains("command")) {
        return json::object({
            {"status", "error"},
            {"message", "Missing 'command' parameter for start action"}
        });
    }
    
    std::string command = args["command"].get<std::string>();
    std::string cwd = "";
    
    if (args.contains("cwd") && args["cwd"].is_string()) {
        cwd = args["cwd"].get<std::string>();
    }
    
    auto& manager = BackgroundProcessManager::get_instance();
    std::string process_id = manager.start_background(command, cwd);
    
    if (process_id.empty()) {
        return json::object({
            {"status", "error"},
            {"message", "Failed to start background process: " + command}
        });
    }
    
    auto info = manager.get_process_info(process_id);
    if (!info) {
        return json::object({
            {"status", "error"},
            {"message", "Process started but info not found"}
        });
    }
    
    return json::object({
        {"status", "success"},
        {"process_id", process_id},
        {"pid", static_cast<int>(info->pid)},
        {"command", info->command},
        {"message", "Process started in background. Do not poll for completion: you may safely end this turn with set_status(\"Done\") or stop, and the exit status and final output will arrive later as a user message that wakes you. Use process_id to send input, read intermediate output, or stop the process."}
    });
}

json BackgroundTool::handle_send(const json& args) {
    if (!args.contains("process_id")) {
        return json::object({
            {"status", "error"},
            {"message", "Missing 'process_id' parameter for send action"}
        });
    }
    
    if (!args.contains("input")) {
        return json::object({
            {"status", "error"},
            {"message", "Missing 'input' parameter for send action"}
        });
    }
    
    std::string process_id = args["process_id"].get<std::string>();
    std::string input = args["input"].get<std::string>();
    
    auto& manager = BackgroundProcessManager::get_instance();
    
    // Check if process exists
    auto info = manager.get_process_info(process_id);
    if (!info) {
        return json::object({
            {"status", "error"},
            {"message", "Process not found: " + process_id}
        });
    }
    
    // Check if already exited
    if (manager.has_exited(process_id)) {
        return json::object({
            {"status", "error"},
            {"process_id", process_id},
            {"exited", true},
            {"message", "Process has already exited"}
        });
    }
    
    // Send input
    auto result = manager.send_to_process(process_id, input);
    
    if (!result.success) {
        return json::object({
            {"status", "error"},
            {"process_id", process_id},
            {"message", result.error}
        });
    }
    
    return json::object({
        {"status", "success"},
        {"process_id", process_id},
        {"bytes_sent", result.bytes_sent}
    });
}

json BackgroundTool::handle_poll(const json& args) {
    if (!args.contains("process_id")) {
        return json::object({
            {"status", "error"},
            {"message", "Missing 'process_id' parameter for poll action"}
        });
    }
    
    std::string process_id = args["process_id"].get<std::string>();
    bool clear_output = false;
    
    if (args.contains("clear_output")) {
        clear_output = args["clear_output"].get<bool>();
    }
    
    auto& manager = BackgroundProcessManager::get_instance();
    
    // Check if process exists
    auto info = manager.get_process_info(process_id);
    if (!info) {
        return json::object({
            {"status", "error"},
            {"message", "Process not found: " + process_id}
        });
    }
    
    // Poll output
    auto output = manager.poll_output(process_id, clear_output);
    
    json result = json::object({
        {"status", "success"},
        {"process_id", process_id},
        {"exited", output.exited}
    });
    
    // Add exit info if applicable
    if (output.exited) {
        result["exit_code"] = output.exit_code;
        if (output.signaled) {
            result["signaled"] = true;
            result["signal"] = output.signal_num;
        }
    }
    
    // Sanitize and add output (don't send binary to LLM)
    result["stdout"] = sanitize_output(output.stdout);
    result["stderr"] = sanitize_output(output.stderr);
    
    // Include message if there's output
    if (!output.stdout.empty() || !output.stderr.empty()) {
        result["message"] = "Output from process (stdout: " + 
            std::to_string(output.stdout.size()) + " bytes, stderr: " +
            std::to_string(output.stderr.size()) + " bytes)";
    } else if (output.exited) {
        result["message"] = "Process has exited with code " + std::to_string(output.exit_code);
    } else {
        result["message"] = "No output available yet";
    }
    
    return result;
}

json BackgroundTool::handle_stop(const json& args) {
    if (!args.contains("process_id")) {
        return json::object({
            {"status", "error"},
            {"message", "Missing 'process_id' parameter for stop action"}
        });
    }
    
    std::string process_id = args["process_id"].get<std::string>();
    
    auto& manager = BackgroundProcessManager::get_instance();
    
    // Check if process exists
    auto info = manager.get_process_info(process_id);
    if (!info) {
        return json::object({
            {"status", "error"},
            {"message", "Process not found: " + process_id}
        });
    }
    
    // Stop the process
    bool success = manager.stop_process(process_id);
    
    if (!success) {
        return json::object({
            {"status", "error"},
            {"process_id", process_id},
            {"message", "Failed to stop process"}
        });
    }
    
    return json::object({
        {"status", "success"},
        {"process_id", process_id},
        {"message", "Process stopped and resources cleaned up"}
    });
}

json BackgroundTool::handle_list(const json& /*args*/) {
    auto& manager = BackgroundProcessManager::get_instance();
    auto active = manager.list_active_processes();
    
    json processes = json::array();
    for (const auto& info : active) {
        processes.push_back(json::object({
            {"process_id", info.process_id},
            {"pid", static_cast<int>(info.pid)},
            {"command", info.command},
            {"working_dir", info.working_dir},
            {"started_at", std::chrono::system_clock::to_time_t(info.started_at)},
            {"exited", info.exited}
        }));
    }
    
    return json::object({
        {"status", "success"},
        {"count", active.size()},
        {"processes", processes},
        {"message", std::to_string(active.size()) + " active background process(es)"}
    });
}

std::string BackgroundTool::sanitize_output(const std::string& output) {
    // Check first 512 bytes for binary content
    const size_t check_size = std::min(output.size(), static_cast<size_t>(512));
    size_t non_printable = 0;
    for (size_t i = 0; i < check_size; i++) {
        unsigned char c = static_cast<unsigned char>(output[i]);
        // Count non-printable characters (excluding common whitespace)
        if (c < 32 && c != '\t' && c != '\n' && c != '\r') {
            non_printable++;
        }
    }
    
    // If more than 10% non-printable, it's likely binary
    if (check_size > 0 && non_printable > check_size / 10) {
        return "[Binary output - " + std::to_string(output.size()) + " bytes]";
    }
    
    // Truncate from middle if too large
    const size_t max_size = 20000;   // 20KB max
    const size_t head_size = 8000;   // Keep first 8KB
    const size_t tail_size = 8000;   // Keep last 8KB
    
    if (output.size() > max_size) {
        // Very large output - take head and tail
        std::string head = output.substr(0, head_size);
        std::string tail = output.substr(output.size() - tail_size);
        
        size_t lines_skipped = 0;
        size_t pos = head_size;
        while (pos < output.size() - tail_size) {
            if (output[pos] == '\n') {
                lines_skipped++;
            }
            pos++;
        }
        
        return head + 
            "\n\n[...] " + std::to_string(lines_skipped) + " lines skipped [...] \n\n" + 
            tail +
            "\n[Output truncated - showing first " + std::to_string(head_size) + 
            " and last " + std::to_string(tail_size) + " bytes of " + 
            std::to_string(output.size()) + " total]";
    }
    
    return output;
}
