/**
 * @file BackgroundProcessManager.cpp
 * @brief Background process management implementation
 */

#include "BackgroundProcessManager.h"

#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <signal.h>
#include <errno.h>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <poll.h>
#include <spawn.h>
#include <fstream>

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
// Helper - not used currently but kept for future
// static void set_close_on_exec(int fd) { ... }

BackgroundProcessManager::BackgroundProcessManager()
    : temp_base_(std::filesystem::temp_directory_path()
                 / (std::string(TEMP_DIR_PREFIX) + std::to_string(::getpid()))) {
    // Each host process owns its capture namespace. Parallel test binaries
    // (and concurrent Daedalus hosts) all start their logical IDs at bg_1;
    // sharing one directory lets them unlink or truncate each other's files.
    ensure_temp_dir();
}

BackgroundProcessManager::~BackgroundProcessManager() {
    cleanup_all();
    if (monitor_thread_.joinable()) {
        monitor_thread_.request_stop();
        monitor_cv_.notify_all();
        monitor_thread_.join();
    }
}

BackgroundProcessManager& BackgroundProcessManager::get_instance() {
    static BackgroundProcessManager instance;
    return instance;
}

void BackgroundProcessManager::ensure_temp_dir() {
    if (!std::filesystem::exists(temp_base_)) {
        std::filesystem::create_directories(temp_base_);
    }
}

std::string BackgroundProcessManager::generate_process_id() {
    process_counter_++;
    return "bg_" + std::to_string(process_counter_);
}

bool BackgroundProcessManager::create_fifo(const std::string& path) {
    // Remove existing file if present
    unlink(path.c_str());
    
    // Create named pipe
    if (mkfifo(path.c_str(), 0600) < 0) {
        if (errno != EEXIST) {
            return false;
        }
    }
    return true;
}

bool BackgroundProcessManager::remove_fifo(const std::string& path) {
    if (unlink(path.c_str()) < 0) {
        return false;
    }
    return true;
}

pid_t BackgroundProcessManager::spawn_process(const std::string& command,
                                               const std::string& cwd,
                                               const std::string& fifo_path,
                                               const std::string& stdout_path,
                                               const std::string& stderr_path) {
    pid_t pid = fork();
    
    if (pid < 0) {
        // Fork failed
        return -1;
    }
    
    if (pid == 0) {
        // Child process
        
        // Create new session - detach from controlling terminal
        if (setsid() < 0) {
            // Not fatal but may affect cleanup
        }
        
        // Redirect stdout and stderr FIRST so any diagnostic messages from
        // the stdin FIFO setup (or from the shell) go to the capture files
        // instead of the host's terminal/TUI.
        int stdout_fd = open(stdout_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (stdout_fd >= 0) {
            dup2(stdout_fd, STDOUT_FILENO);
            close(stdout_fd);
        } else {
            // Fallback to /dev/null so we never inherit the parent's stdout
            int null_fd = open("/dev/null", O_WRONLY);
            if (null_fd >= 0) {
                dup2(null_fd, STDOUT_FILENO);
                close(null_fd);
            }
        }
        
        int stderr_fd = open(stderr_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (stderr_fd >= 0) {
            dup2(stderr_fd, STDERR_FILENO);
            close(stderr_fd);
        } else {
            int null_fd = open("/dev/null", O_WRONLY);
            if (null_fd >= 0) {
                dup2(null_fd, STDERR_FILENO);
                close(null_fd);
            }
        }
        
        // Open FIFO for stdin - use blocking mode to ensure data is read.
        // O_RDWR allows reading even without another writer.
        int stdin_fd = open(fifo_path.c_str(), O_RDWR);
        if (stdin_fd >= 0) {
            dup2(stdin_fd, STDIN_FILENO);
            close(stdin_fd);
        } else {
            // §fix (2026-07-18): between fork() and exec() only
            // async-signal-safe calls are allowed in a MT program —
            // LOG_CERR allocates and takes the Logger mutex, which can
            // deadlock the child. The diagnostic now goes to the redirected
            // stderr capture file, not the host's terminal/TUI.
            static const char msg[] = "[BackgroundProcess] Failed to open stdin FIFO\n";
            (void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
        }
        
        // Change working directory
        if (!cwd.empty()) {
            if (chdir(cwd.c_str()) < 0) {
                // Continue anyway
            }
        }
        
        // Execute via shell
        execl("/bin/sh", "sh", "-c", command.c_str(), (char*)nullptr);
        
        // If exec fails
        _exit(127);
    }
    
    // Parent process - returns PID
    return pid;
}

bool BackgroundProcessManager::write_to_fifo(const std::string& fifo_path, const std::string& data) {
    // Open FIFO with O_RDWR to avoid blocking when no reader
    // O_RDWR allows writing even without a reader on the other end
    // (unlike O_WRONLY which blocks or fails with ENXIO when no reader)
    int fd = open(fifo_path.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        LOG_CERR(ERROR, "[BackgroundProcess] Failed to open FIFO: " << strerror(errno));
        return false;
    }
    
    // Set to blocking mode for the actual write (we want to ensure data is written)
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    
    // Write the data with timeout handling
    ssize_t written = 0;
    ssize_t total = data.size();
    const char* ptr = data.c_str();
    
    while (written < total) {
        ssize_t n = write(fd, ptr + written, total - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Would block - wait a bit and retry
                usleep(10000);
                continue;
            }
            close(fd);
            return false;
        }
        written += n;
    }
    
    // Sync to ensure data is flushed to kernel buffer
    fsync(fd);
    
    // Close the fd - this sends EOF to the reader
    // But the data should be in the kernel buffer for the reader to read
    close(fd);
    
    return written == total;
}

std::string BackgroundProcessManager::read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return "";
    }
    
    std::string content;
    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    if (size > MAX_OUTPUT_SIZE) {
        // Truncate from beginning - keep recent content
        file.seekg(size - MAX_OUTPUT_SIZE, std::ios::beg);
        content = "[...truncated...]\n";
    } else {
        file.seekg(0, std::ios::beg);
    }
    
    content += std::string((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    return content;
}

void BackgroundProcessManager::clear_file(const std::string& path) {
    std::ofstream file(path, std::ios::trunc);
    // Just truncate
}

bool BackgroundProcessManager::is_process_running(pid_t pid) const {
    if (pid <= 0) return false;
    
    // Send 0 signal - checks if process exists
    int result = kill(pid, 0);
    return result == 0 || errno == EPERM;
}

bool BackgroundProcessManager::kill_process_group(pid_t pid) {
    if (pid <= 0) return false;

    if (killpg(pid, SIGTERM) < 0) {
        if (errno == ESRCH) return true;
        return false;
    }

    usleep(100000);
    if (is_process_running(pid)) {
        killpg(pid, SIGKILL);
    }
    // stop_process()/cleanup_all() hold mutex_, so this cannot race the
    // monitor's waitpid(). Reap the killed child before its record is erased.
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return true;
}

std::string BackgroundProcessManager::start_background(
    const std::string& command,
    const std::string& cwd,
    CompletionCallback on_complete) {
    if (command.empty()) {
        return "";
    }

    std::lock_guard lock(mutex_);
    const std::string process_id = generate_process_id();
    const std::string fifo_path = (temp_base_ / (process_id + ".fifo")).string();
    const std::string stdout_path = (temp_base_ / (process_id + ".stdout")).string();
    const std::string stderr_path = (temp_base_ / (process_id + ".stderr")).string();

    if (!create_fifo(fifo_path)) {
        return "";
    }

    const pid_t pid = spawn_process(command, cwd, fifo_path, stdout_path, stderr_path);
    if (pid < 0) {
        remove_fifo(fifo_path);
        return "";
    }

    ProcessEntry entry;
    entry.info.process_id = process_id;
    entry.info.pid = pid;
    entry.info.command = command;
    entry.info.working_dir = cwd.empty() ? "." : cwd;
    entry.info.started_at = std::chrono::system_clock::now();
    entry.info.fifo_path = fifo_path;
    entry.info.stdout_path = stdout_path;
    entry.info.stderr_path = stderr_path;
    entry.on_complete = std::move(on_complete);

    processes_.emplace(process_id, std::move(entry));
    ensure_monitor_locked();
    monitor_cv_.notify_all();
    return process_id;
}

void BackgroundProcessManager::ensure_monitor_locked() {
    if (monitor_thread_.joinable()) return;
    monitor_thread_ = std::jthread(
        [this](std::stop_token stop_token) { monitor_loop(stop_token); });
}

void BackgroundProcessManager::monitor_loop(std::stop_token stop_token) {
    struct Completion {
        BackgroundProcessInfo info;
        ProcessOutput output;
        CompletionCallback callback;
    };

    while (!stop_token.stop_requested()) {
        std::vector<Completion> completions;
        {
            std::unique_lock lock(mutex_);
            for (auto& [id, entry] : processes_) {
                auto& info = entry.info;
                if (info.exited) continue;

                int status = 0;
                const pid_t ret = waitpid(info.pid, &status, WNOHANG);
                if (ret == 0) continue;
                if (ret < 0 && errno == EINTR) continue;

                info.exited = true;
                if (ret == info.pid) {
                    if (WIFEXITED(status)) {
                        info.exit_code = WEXITSTATUS(status);
                    } else if (WIFSIGNALED(status)) {
                        info.signaled = true;
                        info.signal_num = WTERMSIG(status);
                    }
                } else if (ret < 0 && errno != ECHILD) {
                    LOG_ERROR("[BackgroundProcess] waitpid failed for " + id
                              + ": " + std::strerror(errno));
                }

                if (!entry.stop_requested && !entry.completion_dispatched
                    && entry.on_complete) {
                    entry.completion_dispatched = true;
                    ProcessOutput output;
                    output.stdout = read_file(info.stdout_path);
                    output.stderr = read_file(info.stderr_path);
                    output.exited = true;
                    output.exit_code = info.exit_code;
                    output.signaled = info.signaled;
                    output.signal_num = info.signal_num;
                    completions.push_back(
                        {info, std::move(output), entry.on_complete});
                }
            }

            // Idle processes need a short wait to observe exit; an empty
            // manager sleeps until start_background() notifies it.
            monitor_cv_.wait_for(
                lock, processes_.empty() ? std::chrono::milliseconds(250)
                                         : std::chrono::milliseconds(10));
        }

        // Agent submission and host wake-up can call back into tools. Never
        // hold the manager mutex across user-provided completion callbacks.
        for (auto& completion : completions) {
            try {
                completion.callback(completion.info, completion.output);
            } catch (const std::exception& e) {
                LOG_ERROR("[BackgroundProcess] completion callback failed for "
                          + completion.info.process_id + ": " + e.what());
            } catch (...) {
                LOG_ERROR("[BackgroundProcess] completion callback failed for "
                          + completion.info.process_id + ": unknown exception");
            }
        }
    }
}

SendResult BackgroundProcessManager::send_to_process(
    const std::string& process_id,
    const std::string& input) {
    SendResult result;
    std::lock_guard lock(mutex_);

    auto it = processes_.find(process_id);
    if (it == processes_.end()) {
        result.error = "Process not found: " + process_id;
        return result;
    }

    const auto& info = it->second.info;
    if (info.exited) {
        result.error = "Process has already exited";
        return result;
    }
    if (!is_process_running(info.pid)) {
        result.error = "Process is no longer running";
        return result;
    }
    if (!write_to_fifo(info.fifo_path, input)) {
        result.error = "Failed to write to process stdin";
        return result;
    }

    result.success = true;
    result.bytes_sent = input.size();
    return result;
}

ProcessOutput BackgroundProcessManager::poll_output(
    const std::string& process_id,
    bool clear_after_read) {
    ProcessOutput output;
    std::lock_guard lock(mutex_);

    auto it = processes_.find(process_id);
    if (it == processes_.end()) return output;

    auto& info = it->second.info;
    output.stdout = read_file(info.stdout_path);
    output.stderr = read_file(info.stderr_path);
    output.exited = info.exited;
    output.exit_code = info.exit_code;
    output.signaled = info.signaled;
    output.signal_num = info.signal_num;

    if (clear_after_read) {
        clear_file(info.stdout_path);
        clear_file(info.stderr_path);
    }
    return output;
}

bool BackgroundProcessManager::stop_process(const std::string& process_id) {
    std::lock_guard lock(mutex_);
    auto it = processes_.find(process_id);
    if (it == processes_.end()) return false;

    auto& entry = it->second;
    entry.stop_requested = true;
    if (!entry.info.exited) {
        const bool result = kill_process_group(entry.info.pid);
        entry.info.exited = true;
        entry.info.signaled = true;
        entry.info.signal_num = SIGKILL;
        cleanup_process(process_id);
        return result;
    }

    cleanup_process(process_id);
    return true;
}

std::vector<BackgroundProcessInfo> BackgroundProcessManager::list_active_processes() const {
    std::lock_guard lock(mutex_);
    std::vector<BackgroundProcessInfo> active;
    for (const auto& [id, entry] : processes_) {
        if (!entry.info.exited) active.push_back(entry.info);
    }
    return active;
}

std::optional<BackgroundProcessInfo> BackgroundProcessManager::get_process_info(
    const std::string& process_id) {
    std::lock_guard lock(mutex_);
    auto it = processes_.find(process_id);
    if (it == processes_.end()) return std::nullopt;
    return it->second.info;
}

bool BackgroundProcessManager::has_exited(const std::string& process_id) {
    std::lock_guard lock(mutex_);
    auto it = processes_.find(process_id);
    return it == processes_.end() || it->second.info.exited;
}

void BackgroundProcessManager::cleanup_process(const std::string& process_id) {
    auto it = processes_.find(process_id);
    if (it == processes_.end()) return;

    const auto& info = it->second.info;
    if (!info.fifo_path.empty()) remove_fifo(info.fifo_path);
    if (!info.stdout_path.empty()) {
        std::error_code ec;
        std::filesystem::remove(info.stdout_path, ec);
    }
    if (!info.stderr_path.empty()) {
        std::error_code ec;
        std::filesystem::remove(info.stderr_path, ec);
    }
    processes_.erase(it);
}

void BackgroundProcessManager::cleanup_all() {
    std::lock_guard lock(mutex_);
    for (auto& [id, entry] : processes_) {
        entry.stop_requested = true;
        if (!entry.info.exited) kill_process_group(entry.info.pid);
    }

    std::vector<std::string> ids;
    ids.reserve(processes_.size());
    for (const auto& [id, entry] : processes_) ids.push_back(id);
    for (const auto& id : ids) cleanup_process(id);

    monitor_cv_.notify_all();
    if (!std::filesystem::exists(temp_base_)) return;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(temp_base_)) {
            std::error_code ec;
            std::filesystem::remove(entry.path(), ec);
        }
    } catch (const std::exception&) {
        // Best-effort temp cleanup only.
    }
}