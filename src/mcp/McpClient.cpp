#include "McpClient.h"
#include "core/ThreadPool.h"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <chrono>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>  // send(), MSG_NOSIGNAL
#include <sys/wait.h>
#include <sys/types.h>
#include <optional>
#include <vector>
#include <string>
#include <map>
#include <functional>
#include <nlohmann/json.hpp>

#include "core/Logger.h"

// §Phase 5.1: helper macro that converts LOG_CERR(INFO,  ...); style
// statements into LOG_INFO/LOG_WARNING/LOG_ERROR calls via the
// existing Logger. Same formatted text routed through the Logger
// instead of writing to stderr directly.
#define LOG_CERR(level, expr) do {                          \
        std::ostringstream _daedalus_msg;                   \
        _daedalus_msg << expr;                               \
        LOG_##level(_daedalus_msg.str());                    \
    } while (0)
namespace mcp {

McpClient::McpClient()
    : pipes_(nullptr)
    , connected_(false)
    , initialized_(false)
    , next_request_id_(1) {
}

McpClient::~McpClient() {
    disconnect();
}

bool McpClient::connect(const std::string& command,
                         const std::map<std::string, std::string>& env,
                         [[maybe_unused]] int timeout_ms) {
    if (connected_) {
        last_error_ = "Already connected";
        return false;
    }
    
    // Allocate pipes
    pipes_ = new McpPipes();
    
    int stdin_pipe[2];
    int stdout_pipe[2];
    
    if (pipe(stdin_pipe) == -1 || pipe(stdout_pipe) == -1) {
        last_error_ = "Failed to create pipes: " + std::string(strerror(errno));
        delete pipes_;
        pipes_ = nullptr;
        return false;
    }
    
    // Make stdout read end non-blocking
    int flags = fcntl(stdout_pipe[0], F_GETFL);
    fcntl(stdout_pipe[0], F_SETFL, flags | O_NONBLOCK);
    
    // Fork
    pid_t pid = fork();
    
    if (pid == -1) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        last_error_ = "Failed to fork: " + std::string(strerror(errno));
        delete pipes_;
        pipes_ = nullptr;
        return false;
    }
    
    if (pid == 0) {
        // Child process
        close(stdin_pipe[1]);  // Close write end of stdin
        close(stdout_pipe[0]); // Close read end of stdout

        // §9.7.A — join the agent's process group so a single
        // McpServerManager::shutdown() can SIGTERM all MCP children at
        // once via killpg(), and so the parent's setpgid() / killpg()
        // race is closed (also set in the parent right after fork
        // returns).
        if (setpgid(0, getpgid(getppid())) != 0) {
            // Not fatal: still continue; just the killpg path won't work
            // for this particular child. Some MCP servers spawn their own
            // session — those change pgroup themselves; we override below.
            // Non-fatal: continue.
            (void)0;
        }

        // Redirect stdin
        if (dup2(stdin_pipe[0], STDIN_FILENO) == -1) {
            _exit(127);
        }
        // Redirect stdout
        if (dup2(stdout_pipe[1], STDOUT_FILENO) == -1) {
            _exit(127);
        }

        close(stdin_pipe[0]);
        close(stdout_pipe[1]);
        
        // Set up environment - explicitly set PATH so commands can be found
        // This is critical when .bashrc/.profile are missing or broken
        for (const auto& [key, value] : env) {
            if (setenv(key.c_str(), value.c_str(), 1) != 0) {
                // Log but continue
            }
        }
        
        // CRITICAL: Ensure PATH is set so uvx/commands can be found
        // If PATH is not set, use a default that includes ~/.local/bin (where uvx lives)
        char* current_path = getenv("PATH");
        if (current_path == nullptr || strlen(current_path) == 0) {
            setenv("PATH", "/home/lee/.local/bin:/usr/local/bin:/usr/bin:/bin", 1);
        }
        
        // Parse command - inline parsing logic
        std::vector<std::string> args;
        std::string current;
        bool in_quote = false;
        char quote_char = '\0';
        
        for (size_t i = 0; i < command.size(); ++i) {
            char c = command[i];
            
            if (c == '"' || c == '\'') {
                if (in_quote) {
                    if (c == quote_char) {
                        in_quote = false;
                    } else {
                        current += c;
                    }
                } else {
                    in_quote = true;
                    quote_char = c;
                }
            } else if (c == ' ' || c == '\t') {
                if (!current.empty()) {
                    args.push_back(current);
                    current.clear();
                }
            } else {
                current += c;
            }
        }
        if (!current.empty()) {
            args.push_back(current);
        }
        
        // Build argv - wrap with bash -c to ensure proper shell environment
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>("/bin/bash"));
        argv.push_back(const_cast<char*>("-c"));
        
        // Reconstruct the command string for bash -c
        std::string full_command;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) full_command += " ";
            // Quote args that contain spaces or special chars
            if (args[i].find(' ') != std::string::npos || 
                args[i].find('*') != std::string::npos ||
                args[i].find('?') != std::string::npos) {
                full_command += "\"" + args[i] + "\"";
            } else {
                full_command += args[i];
            }
        }
        argv.push_back(const_cast<char*>(full_command.c_str()));
        argv.push_back(nullptr);
        
        // Execute via bash to ensure proper shell environment
        execvp(argv[0], argv.data());
        _exit(127);
    }
    
    // Parent process
    close(stdin_pipe[0]);  // Close read end of stdin
    close(stdout_pipe[1]); // Close write end of stdout

    pipes_->stdin_write = stdin_pipe[1];
    pipes_->stdout_read = stdout_pipe[0];
    pipes_->child_pid = pid;

    // §9.7.A — also setpgid on the parent side to close the race with
    // the child's setpgid() (POSIX says either side can do it; some
    // kernels ignore the child's if not yet exec'd). Best-effort; the
    // shutdown path falls back to per-PID kill() if killpg() ESRCHs.
    if (setpgid(pid, getpgid(getpid())) != 0) {
        if (errno != EACCES && errno != EPERM) {
            // Best-effort. Common reasons: another thread already set it,
            // the child exec'd something that reset its pgroup. Either
            // way, the destructor + shutdown paths still work via per-PID
            // kill, so we just log and continue.
            LOG_CERR(INFO, "[MCP] WARNING: setpgid(" << pid << ") failed: "
                      << strerror(errno));
        }
    }

    connected_ = true;

    return true;
}

void McpClient::disconnect() {
    if (pipes_) {
        if (pipes_->child_pid > 0) {
            // §9.7.A — killpg first (covers all descendants), then
            // SIGKILL after a 2 s cap. The waitpid is non-blocking so
            // we don't hang on a zombie we can't reap.
            pid_t pgrp = getpgid(pipes_->child_pid);
            if (pgrp > 0) {
                killpg(pgrp, SIGTERM);
            } else {
                kill(pipes_->child_pid, SIGTERM);
            }
            int status = 0;
            // Wait up to 2 s for graceful shutdown.
            for (int i = 0; i < 20; ++i) {
                pid_t r = waitpid(pipes_->child_pid, &status, WNOHANG);
                if (r == pipes_->child_pid || r == -1) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            // If still alive, SIGKILL.
            if (kill(pipes_->child_pid, 0) == 0) {
                if (pgrp > 0) killpg(pgrp, SIGKILL);
                else kill(pipes_->child_pid, SIGKILL);
                waitpid(pipes_->child_pid, &status, 0);
            }
        }
        delete pipes_;
        pipes_ = nullptr;
    }
    connected_ = false;
    initialized_ = false;
    tools_.clear();
}

bool McpClient::is_connected() const {
    return connected_;
}

bool McpClient::initialize() {
    if (!connected_) {
        last_error_ = "Not connected";
        return false;
    }

    if (initialized_) {
        return true;
    }

    // §SIGPIPE note (2026-07-14): the underlying transport's
    // send_request() / send_notification() write to the MCP subprocess's
    // stdin pipe via send(fd, ..., MSG_NOSIGNAL), which is the surgical
    // fix for "subprocess exits before parent writes its notification".
    // A closed pipe then returns EPIPE instead of raising SIGPIPE that
    // would kill the daemon. No signal-handler dance required.

    // Send initialize request
    json params = {
        {"protocolVersion", "2025-06-18"},
        {"capabilities", {{"roots", json::object()}, {"sampling", json::object()}}},
        {"clientInfo", {{"name", "daedalus"}, {"version", "1.0.0"}}}
    };
    
    json response = send_request("initialize", params, 10000);
    
    if (response.is_null()) {
        return false;
    }
    
    if (response.contains("error")) {
        last_error_ = "Initialize error: " + response["error"].dump();
        return false;
    }
    
    // Store server info
    if (response.contains("result") && response["result"].contains("serverInfo")) {
        server_version_ = response["result"]["serverInfo"].value("version", "unknown");
    }
    
    initialized_ = true;
    
    // Send initialized notification (no response expected)
    send_notification("notifications/initialized", json::object());
    
    return true;
}

std::vector<ToolDefinition> McpClient::list_tools() {
    if (!initialized_) {
        last_error_ = "Not initialized";
        return {};
    }
    
    json response = send_request("tools/list", json::object());
    
    if (response.is_null() || response.contains("error")) {
        last_error_ = response.is_null() ? "No response" : response["error"].dump();
        return {};
    }
    
    std::vector<ToolDefinition> tools;
    
    if (response.contains("result") && response["result"].contains("tools")) {
        for (const auto& tool : response["result"]["tools"]) {
            ToolDefinition def;
            def.name = tool.value("name", "");
            def.description = tool.value("description", "");
            def.input_schema = tool.value("inputSchema", json::object());
            tools.push_back(def);
        }
    }
    
    tools_ = tools;
    return tools;
}

ToolResult McpClient::call_tool(const std::string& tool_name, const json& arguments) {
    if (!initialized_) {
        return {false, "", "Not initialized"};
    }
    
    json params = {
        {"name", tool_name},
        {"arguments", arguments}
    };
    
    json response = send_request("tools/call", params);
    
    if (response.is_null()) {
        return {false, "", last_error_};
    }
    
    if (response.contains("error")) {
        std::string error = response["error"].value("message", response["error"].dump());
        return {false, "", error};
    }
    
    // Parse result content
    std::string content;
    if (response.contains("result") && response["result"].contains("content")) {
        for (const auto& item : response["result"]["content"]) {
            if (item.contains("text")) {
                content += item["text"].get<std::string>();
            }
        }
    }
    
    return {true, content, std::nullopt};
}

std::string McpClient::understand_image(const std::string& image_url,
                                         const std::string& prompt) {
    json args = {
        {"prompt", prompt},
        {"image_source", image_url}  // Note: MiniMax MCP expects "image_source", not "image_url"
    };
    
    ToolResult result = call_tool("understand_image", args);
    
    if (result.success) {
        return result.content;
    }
    
    return "Error: " + (result.error.value_or("Unknown error"));
}

json McpClient::web_search(const std::string& query, [[maybe_unused]] int limit) {
    json args = {
        {"query", query}
    };

    ToolResult result = call_tool("web_search", args);

    if (result.success) {
        // Try to parse as JSON, otherwise return as text
        try {
            return json::parse(result.content);
        } catch (...) {
            return json{{"text", result.content}};
        }
    }

    return json{{"error", result.error.value_or("Unknown error")}};
}

// ============================================================================
// Async / batch support
// ============================================================================

std::future<ToolResult> McpClient::call_tool_async(
    const std::string& tool_name,
    const json& arguments,
    std::stop_token stop_token
) {
    // Submit the (synchronous) call_tool to the ThreadPool so it doesn't
    // block the caller. Cancellation is cooperative: if stop_token is
    // requested while the underlying read_response is polling, the
    // future will resolve once the in-flight request returns or times out.
    using daedalus::concurrent::ThreadPool;
    using daedalus::concurrent::TaskPriority;
    return ThreadPool::get_instance().submit(
        TaskPriority::NORMAL,
        [this, tool_name, arguments, stop_token]() {
            if (stop_token.stop_requested()) {
                return ToolResult{false, "", std::string("Cancelled")};
            }
            return call_tool(tool_name, arguments);
        }
    );
}

std::vector<std::future<ToolResult>> McpClient::call_tools_batch(
    const std::vector<ToolCallSpec>& calls
) {
    std::vector<std::future<ToolResult>> results;
    results.reserve(calls.size());

    // Each call is independent - submit them all to the pool in parallel.
    // For stdio transport the calls are serialized at the OS level (one pipe),
    // but at least they don't block the calling thread.
    using daedalus::concurrent::ThreadPool;
    using daedalus::concurrent::TaskPriority;
    for (const auto& call : calls) {
        results.push_back(
            ThreadPool::get_instance().submit(
                TaskPriority::NORMAL,
                [this, call]() {
                    return this->call_tool(call.tool_name, call.arguments);
                }
            )
        );
    }
    return results;
}

std::string McpClient::get_last_error() const {
    return last_error_;
}

// =============================================================================
// §9.7.C — list_changed notifications are now detected inline inside
// read_response() (search for "FIRING_LIST_CHANGED" in this file). The
// previous design — a separate listener thread that read from the same
// stdout pipe as the main reader — was broken: two threads racing for
// `read()` on a shared fd meant a real JSON-RPC response could be eaten
// by the listener and the caller would hang until timeout. The callbacks
// are now wired via set_list_changed_callback() / clear_list_changed_callback().
// =============================================================================

json McpClient::send_request(const std::string& method, const json& params, int timeout_ms) {
    if (!pipes_ || pipes_->stdin_write < 0 || pipes_->stdout_read < 0) {
        last_error_ = "Not connected";
        return json::object();
    }
    
    // Build JSON-RPC request
    json request = {
        {"jsonrpc", "2.0"},
        {"id", next_request_id_++},
        {"method", method}
    };
    
    if (!params.is_null()) {
        request["params"] = params;
    }
    
    // Send request
    std::string request_str = request.dump() + "\n";
    // MSG_NOSIGNAL suppresses SIGPIPE if the subprocess has closed
    // stdin (e.g. it crashed/exited before reading our request). On
    // Linux this works for pipe FDs as well as sockets (since 2.6.32).
    ssize_t written = send(pipes_->stdin_write, request_str.c_str(),
                           request_str.size(), MSG_NOSIGNAL);

    if (written < 0) {
        last_error_ = "Failed to write to stdin: " + std::string(strerror(errno));
        return json::object();
    }
    
    // Read response
    return read_response(timeout_ms);
}

void McpClient::send_notification(const std::string& method, const json& params) {
    if (!pipes_ || pipes_->stdin_write < 0) {
        return;
    }
    
    json notification = {
        {"jsonrpc", "2.0"},
        {"method", method}
    };
    
    if (!params.is_null()) {
        notification["params"] = params;
    }
    
    std::string notif_str = notification.dump() + "\n";
    // MSG_NOSIGNAL: see send_request() comment. Subprocess may have
    // closed stdin between initialize() and this notification.
    [[maybe_unused]] ssize_t rc = send(pipes_->stdin_write, notif_str.c_str(),
                                       notif_str.size(), MSG_NOSIGNAL);
}

json McpClient::read_response(int timeout_ms) {
    if (!pipes_ || pipes_->stdout_read < 0) {
        return json::object();
    }
    
    std::string buffer;
    struct pollfd pfd = {pipes_->stdout_read, POLLIN, 0};
    int remaining = timeout_ms;
    
    while (true) {
        int poll_timeout = std::min(remaining, 100);
        int ret = poll(&pfd, 1, poll_timeout);
        
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            last_error_ = "Poll error: " + std::string(strerror(errno));
            return json::object();
        }
        
        if (ret == 0) {
            // Timeout on this iteration
            if (timeout_ms >= 0) {
                remaining -= 100;
                if (remaining <= 0) {
                    last_error_ = "Timeout waiting for response";
                    return json::object();
                }
            }
            continue;
        }
        
        if (pfd.revents & POLLIN) {
            char buf[4096];
            ssize_t n = read(pipes_->stdout_read, buf, sizeof(buf) - 1);

            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                last_error_ = "Read error: " + std::string(strerror(errno));
                return json::object();
            }

            if (n == 0) {
                // EOF
                last_error_ = "Server closed connection";
                return json::object();
            }

            buf[n] = '\0';
            buffer += buf;

            // Only attempt to parse when we have a COMPLETE line (terminated by \n).
            // B4 FIX: previously, partial lines were dropped on parse failure, which
            // could cause us to lose data if the server writes a large response in
            // multiple chunks. Now we only parse complete (\n-terminated) lines, and
            // on parse failure we discard the malformed line (a real error).
            size_t newline_pos = buffer.find('\n');
            if (newline_pos != std::string::npos) {
                std::string line = buffer.substr(0, newline_pos);

                // Skip SSE headers if present
                if (line.size() >= 5 && line.substr(0, 5) == "data:") {
                    line = line.substr(5);
                    size_t start = line.find_first_not_of(" \t");
                    if (start != std::string::npos) {
                        line = line.substr(start);
                    }
                }

                // Remove the consumed line from buffer (keep any trailing data)
                buffer = buffer.substr(newline_pos + 1);

                if (!line.empty()) {
                    try {
                        json parsed = json::parse(line);

                        // §9.7.C — INLINE notification dispatch. A JSON-RPC
                        // notification has a "method" field but no "id"
                        // (server-initiated, not a response to our request).
                        // The only notification we currently care about is
                        // `notifications/tools/list_changed`; fire the
                        // registered callback and keep reading. This
                        // replaces the old separate-thread listener which
                        // raced with the main reader for `read()` on the
                        // shared stdout fd and could lose response bytes.
                        if (!parsed.contains("id") && parsed.contains("method")) {
                            const std::string& m = parsed["method"].get_ref<const std::string&>();
                            if (m == "notifications/tools/list_changed" ||
                                m == "notifications/prompts/list_changed" ||
                                m == "notifications/resources/list_changed") {
                                ListChangedCallback cb_copy;
                                {
                                    std::scoped_lock lk(cb_mu_);
                                    cb_copy = list_changed_cb_;
                                }
                                if (cb_copy) {
                                    try { cb_copy(); }
                                    catch (const std::exception& e) {
                                        last_error_ = std::string(
                                            "list_changed callback threw: ") + e.what();
                                    } catch (...) {
                                        last_error_ = "list_changed callback threw (unknown)";
                                    }
                                }
                            }
                            // Discard the notification and continue reading
                            // the byte stream for our response.
                            continue;
                        }

                        return parsed;
                    } catch (const std::exception& e) {
                        // Malformed line - log and continue (B4: don't lose data)
                        last_error_ = std::string("Malformed JSON-RPC line: ") + e.what();
                        // Keep reading - maybe the next line is the real response
                    }
                }
            }
            // No newline yet - keep accumulating
        }
        
        if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
            last_error_ = "Server disconnected";
            return json::object();
        }
    }
}

std::string McpClient::parse_sse_data(const std::string& line) {
    if (line.substr(0, 5) == "data:") {
        return line.substr(5);
    }
    return "";
}

std::string McpClient::read_all_output(int timeout_ms) {
    if (!pipes_ || pipes_->stdout_read < 0) {
        return "";
    }
    
    std::string result;
    struct pollfd pfd = {pipes_->stdout_read, POLLIN, 0};
    int remaining = timeout_ms;
    
    while (true) {
        int poll_timeout = std::min(remaining, 100);
        int ret = poll(&pfd, 1, poll_timeout);
        
        if (ret <= 0) {
            break;
        }
        
        if (pfd.revents & POLLIN) {
            char buf[4096];
            ssize_t n = read(pipes_->stdout_read, buf, sizeof(buf) - 1);
            if (n <= 0) {
                break;
            }
            buf[n] = '\0';
            result += buf;
        } else {
            break;
        }
        
        if (timeout_ms >= 0) {
            remaining -= 100;
            if (remaining <= 0) break;
        }
    }
    
    return result;
}

} // namespace mcp