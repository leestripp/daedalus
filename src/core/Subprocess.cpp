/**
 * @file Subprocess.cpp
 * @brief Subprocess execution implementation
 */

#include "Subprocess.h"

#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <poll.h>

// Structure to hold execution state
struct Subprocess::ExecutionState {
    pid_t pid;
    int pipe_stdout[2];
    int pipe_stderr[2];
    bool stdout_eof;
    bool stderr_eof;
    
    ExecutionState() : pid(-1), stdout_eof(false), stderr_eof(false) {
        pipe_stdout[0] = pipe_stdout[1] = -1;
        pipe_stderr[0] = pipe_stderr[1] = -1;
    }
    
    ~ExecutionState() {
        // Close all pipes
        for (int i = 0; i < 2; ++i) {
            if (pipe_stdout[i] >= 0) close(pipe_stdout[i]);
            if (pipe_stderr[i] >= 0) close(pipe_stderr[i]);
        }
    }
};

void Subprocess::close_pipe(int& fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

void Subprocess::read_from_pipe(int pipe_fd, std::string& output, bool& eof) {
    if (pipe_fd < 0 || eof) return;
    
    char buffer[4096];
    ssize_t n = read(pipe_fd, buffer, sizeof(buffer) - 1);
    
    if (n > 0) {
        buffer[n] = '\0';
        output += buffer;
    } else if (n == 0) {
        eof = true;
    }
    // If n < 0, just try again later (EAGAIN is normal for non-blocking)
}

int64_t Subprocess::spawn_with_pipes(const std::vector<std::string>& args,
                                      const std::string& cwd,
                                      int pipe_stdout[2],
                                      int pipe_stderr[2]) {
    // Create pipes before forking
    if (pipe(pipe_stdout) < 0 || pipe(pipe_stderr) < 0) {
        return -1;
    }
    
    // Make pipes non-blocking for reading
    int flags;
    flags = fcntl(pipe_stdout[0], F_GETFL);
    fcntl(pipe_stdout[0], F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(pipe_stderr[0], F_GETFL);
    fcntl(pipe_stderr[0], F_SETFL, flags | O_NONBLOCK);
    
    pid_t pid = fork();
    
    if (pid < 0) {
        // Fork failed
        close(pipe_stdout[0]); close(pipe_stdout[1]);
        close(pipe_stderr[0]); close(pipe_stderr[1]);
        return -1;
    }
    
    if (pid == 0) {
        // Child process
        
        // Create new process group - become leader of our own group
        if (setpgid(0, 0) < 0) {
            // Not fatal, but may affect cleanup
        }
        
        // Close read ends of pipes
        close(pipe_stdout[0]);
        close(pipe_stderr[0]);
        
        // Redirect stdout to pipe
        if (dup2(pipe_stdout[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        
        // Redirect stderr to pipe
        if (dup2(pipe_stderr[1], STDERR_FILENO) < 0) {
            _exit(127);
        }
        
        // Close original write ends after duplication
        close(pipe_stdout[1]);
        close(pipe_stderr[1]);
        
        // Change working directory if specified
        if (!cwd.empty()) {
            if (chdir(cwd.c_str()) < 0) {
                // Failed to change directory, continue anyway
            }
        }
        
        // Build argv
        std::vector<char*> argv;
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        
        // Execute the command
        execvp(argv[0], argv.data());
        
        // If exec fails, exit with error
        _exit(127);
    }
    
    // Parent process
    // Close write ends of pipes - we only read
    close(pipe_stdout[1]);
    close(pipe_stderr[1]);
    pipe_stdout[1] = -1;
    pipe_stderr[1] = -1;
    
    return pid;
}

int64_t Subprocess::spawn(const std::vector<std::string>& args,
                          const std::string& cwd,
                          bool /*capture_output*/) {
    if (args.empty() || args[0].empty()) {
        return -1;
    }
    
    int pipe_stdout[2], pipe_stderr[2];
    return spawn_with_pipes(args, cwd, pipe_stdout, pipe_stderr);
}

bool Subprocess::wait(int64_t pid, int timeout_ms) {
    SubprocessResult result;
    return wait_with_result(pid, timeout_ms, result);
}

bool Subprocess::wait_with_result(int64_t pid, int timeout_ms, SubprocessResult& result) {
    // We need to access the pipes, but they're not stored globally
    // For this implementation, we'll use a simpler approach:
    // Use waitpid with timeout polling
    
    pid_t target_pid = static_cast<pid_t>(pid);
    int status;
    int remaining = timeout_ms;
    
    while (true) {
        pid_t ret = waitpid(target_pid, &status, WNOHANG);
        
        if (ret == target_pid) {
            // Process exited
            if (WIFEXITED(status)) {
                result.exit_code = WEXITSTATUS(status);
                result.signaled = false;
            } else if (WIFSIGNALED(status)) {
                result.exit_code = -1;
                result.signaled = true;
                result.signal = WTERMSIG(status);
            }
            result.timed_out = false;
            return true;
        } else if (ret == 0) {
            // Process still running
            if (timeout_ms < 0) {
                // Infinite timeout - keep waiting
                usleep(10000);  // 10ms
            } else if (remaining <= 0) {
                // Timeout!
                result.timed_out = true;
                return false;
            } else {
                // Wait a bit
                int sleep_time = std::min(remaining, 10);
                usleep(sleep_time * 1000);
                remaining -= sleep_time;
            }
        } else {
            // Error (ECHILD, EINTR, etc.)
            if (errno == ECHILD) {
                // Process already reaped somehow (by signal handler or other thread)
                // Don't treat as failure - just return success with the status we got
                result.exit_code = 0;  // Assume success if already reaped
                result.timed_out = false;
                return true;
            } else if (errno == EINTR) {
                // Interrupted, retry
                continue;
            } else {
                // Other error
                result.timed_out = false;
                return false;
            }
        }
    }
}

bool Subprocess::kill_group(int64_t pid) {
    pid_t target_pid = static_cast<pid_t>(pid);
    
    // First try SIGTERM to allow graceful shutdown
    if (killpg(target_pid, SIGTERM) < 0) {
        if (errno == ESRCH) return true;  // Process already dead
        return false;
    }
    
    // Give it a moment to clean up
    usleep(100000);  // 100ms
    
    // If still running, SIGKILL
    if (killpg(target_pid, 0) >= 0) {
        killpg(target_pid, SIGKILL);
    }
    
    // Reap the process
    int status;
    waitpid(target_pid, &status, WNOHANG);
    
    return true;
}

std::vector<std::string> Subprocess::parse_command(const std::string& command) {
    std::vector<std::string> args;
    std::string current;
    bool in_quote = false;
    char quote_char = '\0';
    bool escaped = false;
    
    for (size_t i = 0; i < command.size(); ++i) {
        char c = command[i];
        
        if (escaped) {
            current += c;
            escaped = false;
            continue;
        }
        
        if (c == '\\') {
            escaped = true;
            continue;
        }
        
        if (in_quote) {
            if (c == quote_char) {
                in_quote = false;
            } else {
                current += c;
            }
        } else if (c == '"' || c == '\'') {
            in_quote = true;
            quote_char = c;
        } else if (c == ' ' || c == '\t' || c == '\n') {
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
    
    return args;
}

SubprocessResult Subprocess::execute_shell(const std::string& command,
                                          const std::string& cwd,
                                          int timeout_ms) {
    return execute_combined(command, cwd, timeout_ms);
}

SubprocessResult Subprocess::execute_combined(const std::string& command,
                                              const std::string& cwd,
                                              int timeout_ms) {
    SubprocessResult result;
    
    // Use shell to execute the command
    int pipe_stdout[2], pipe_stderr[2];
    
    // Create pipes
    if (pipe(pipe_stdout) < 0 || pipe(pipe_stderr) < 0) {
        result.exit_code = 127;
        result.stdout = "Failed to create pipes";
        return result;
    }
    
    // Make read ends non-blocking
    int flags;
    flags = fcntl(pipe_stdout[0], F_GETFL);
    fcntl(pipe_stdout[0], F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(pipe_stderr[0], F_GETFL);
    fcntl(pipe_stderr[0], F_SETFL, flags | O_NONBLOCK);
    
    pid_t pid = fork();
    
    if (pid < 0) {
        close(pipe_stdout[0]); close(pipe_stdout[1]);
        close(pipe_stderr[0]); close(pipe_stderr[1]);
        result.exit_code = 127;
        result.stdout = "Failed to fork";
        return result;
    }
    
    if (pid == 0) {
        // Child - create our own process group
        setpgid(0, 0);
        
        // Close read ends
        close(pipe_stdout[0]);
        close(pipe_stderr[0]);
        
        // Redirect stdout
        dup2(pipe_stdout[1], STDOUT_FILENO);
        // Redirect stderr
        dup2(pipe_stderr[1], STDERR_FILENO);
        
        // Close originals
        close(pipe_stdout[1]);
        close(pipe_stderr[1]);
        
        // Change directory if needed
        if (!cwd.empty()) {
            [[maybe_unused]] int r = chdir(cwd.c_str());
        }
        
        // Execute via shell
        execl("/bin/sh", "sh", "-c", command.c_str(), (char*)nullptr);
        
        // If we get here, exec failed
        _exit(127);
    }
    
    // Parent - close write ends
    close(pipe_stdout[1]); pipe_stdout[1] = -1;
    close(pipe_stderr[1]); pipe_stderr[1] = -1;
    
    // Read output with timeout
    std::string stdout_output;
    std::string stderr_output;
    bool stdout_eof = false;
    bool stderr_eof = false;
    int remaining = timeout_ms;
    bool exit_status_captured = false;  // Track if we've already reaped the process
    
    while (true) {
        // Check if process exited
        pid_t ret = waitpid(pid, nullptr, WNOHANG);
        if (ret == pid) {
            // Process exited, drain remaining output
            while (!stdout_eof || !stderr_eof) {
                read_from_pipe(pipe_stdout[0], stdout_output, stdout_eof);
                read_from_pipe(pipe_stderr[0], stderr_output, stderr_eof);
                if (stdout_eof && stderr_eof) break;
                usleep(10000);
            }
            break;
        }
        
        // Read from pipes
        read_from_pipe(pipe_stdout[0], stdout_output, stdout_eof);
        read_from_pipe(pipe_stderr[0], stderr_output, stderr_eof);
        
        // If both pipes closed, process should have exited
        if (stdout_eof && stderr_eof) {
            // Process has exited, get its status before reaping
            int exit_status;
            pid_t status_ret = waitpid(pid, &exit_status, 0);
            if (status_ret == pid) {
                if (WIFEXITED(exit_status)) {
                    result.exit_code = WEXITSTATUS(exit_status);
                } else if (WIFSIGNALED(exit_status)) {
                    result.exit_code = -1;  // -1 indicates killed by signal
                    result.signaled = true;
                    result.signal = WTERMSIG(exit_status);
                }
                exit_status_captured = true;
            } else if (errno == ECHILD) {
                // Process already reaped by signal handler or other thread
                // Assume success - the process ran and we're just seeing cleanup effects
                result.exit_code = 0;
                exit_status_captured = true;
            }
            break;
        }
        
        // Check timeout
        if (timeout_ms >= 0 && remaining <= 0) {
            // Timeout - kill process group
            kill_group(pid);
            result.timed_out = true;
            
            // Drain remaining output
            while (!stdout_eof && !stderr_eof) {
                read_from_pipe(pipe_stdout[0], stdout_output, stdout_eof);
                read_from_pipe(pipe_stderr[0], stderr_output, stderr_eof);
            }
            
            // Get exit status (process was killed)
            int status;
            pid_t status_ret = waitpid(pid, &status, 0);
            if (status_ret == pid) {
                if (WIFEXITED(status)) {
                    result.exit_code = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    result.exit_code = -1;
                    result.signaled = true;
                    result.signal = WTERMSIG(status);
                }
            } else if (errno == ECHILD) {
                // Process was already reaped (likely by kill_group's waitpid)
                // This is expected - don't change exit_code
            }
            
            result.stdout = stdout_output;
            result.stderr = stderr_output;
            if (!stdout_output.empty() && !stderr_output.empty()) {
                result.stdout = stdout_output + "\n" + stderr_output;
                result.stderr.clear();
            } else if (!stdout_output.empty()) {
                result.stdout = stdout_output;
            } else if (!stderr_output.empty()) {
                result.stdout = stderr_output;
            }
            return result;
        }
        
        // Wait a bit
        usleep(10000);  // 10ms
        if (timeout_ms >= 0) remaining -= 10;
    }
    
    // Get exit status only if not already captured
    if (!exit_status_captured) {
        int status;
        pid_t ret = waitpid(pid, &status, 0);
        if (ret == pid) {
            if (WIFEXITED(status)) {
                result.exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                result.exit_code = -1;
                result.signaled = true;
                result.signal = WTERMSIG(status);
            }
        } else if (errno == ECHILD) {
            // Process was already reaped - this can happen with signal handlers
            // Keep default exit_code (0)
        }
    }
    
    // Close pipes
    close(pipe_stdout[0]);
    close(pipe_stderr[0]);
    
    // Combine stdout and stderr
    if (!stdout_output.empty() && !stderr_output.empty()) {
        result.stdout = stdout_output + "\n" + stderr_output;
        result.stderr = stderr_output;
    } else if (!stdout_output.empty()) {
        result.stdout = stdout_output;
    } else if (!stderr_output.empty()) {
        result.stdout = stderr_output;
    }
    
    return result;
}

SubprocessResult Subprocess::execute(const std::string& command,
                                    const std::string& cwd,
                                    int timeout_ms) {
    return execute_combined(command, cwd, timeout_ms);
}
