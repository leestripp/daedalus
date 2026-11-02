/**
 * @file BackgroundProcessManager.h
 * @brief Background process management for server applications
 *
 * Provides ability to:
 * - Start processes in background
 * - Send input to processes
 * - Poll output from processes
 * - Stop background processes
 * - List active processes
 *
 * Each background process is tracked by a unique process_id (e.g., "bg_001")
 */

#ifndef BACKGROUND_PROCESS_MANAGER_H
#define BACKGROUND_PROCESS_MANAGER_H

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <chrono>
#include <filesystem>
#include <ctime>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

/**
 * Information about a running background process
 */
struct BackgroundProcessInfo {
    std::string process_id;           // Unique ID assigned by manager (e.g., "bg_001")
    pid_t pid;                        // System PID
    std::string command;              // Original command
    std::string working_dir;          // Working directory at start
    std::chrono::system_clock::time_point started_at;
    bool exited = false;              // True if process has exited
    int exit_code = 0;               // Exit code if exited
    bool signaled = false;            // True if killed by signal
    int signal_num = 0;               // Signal number if signaled
    std::string fifo_path;            // Path to FIFO for stdin
    std::string stdout_path;          // Path to stdout capture file
    std::string stderr_path;          // Path to stderr capture file
};

/**
 * Result of a send operation
 */
struct SendResult {
    bool success = false;
    std::string error;
    size_t bytes_sent = 0;
};

/**
 * Output from a background process
 */
struct ProcessOutput {
    std::string stdout;
    std::string stderr;
    bool exited = false;
    int exit_code = 0;
    bool signaled = false;
    int signal_num = 0;
};

/**
 * BackgroundProcessManager - Manages background processes for the agent
 *
 * This singleton class allows the agent to:
 * 1. Start server applications in background
 * 2. Continue its reasoning loop while server runs
 * 3. Send commands to the server
 * 4. Read output from the server
 * 5. Stop the server when done
 *
 * Thread safety: public methods are synchronized. A single internal monitor
 * synchronizes waitpid() and dispatches natural process exits exactly once; explicit
 * stop_process()/cleanup_all() cancellations do not generate completions.
 */
class BackgroundProcessManager {
public:
    /// Called after a naturally exited process has been reaped and its final
    /// output tail is available. The callback runs on the manager monitor
    /// thread and must not destroy the manager.
    using CompletionCallback = std::function<void(
        const BackgroundProcessInfo&, const ProcessOutput&)>;

    /**
     * Get singleton instance
     */
    static BackgroundProcessManager& get_instance();

    /**
     * Start a process in background
     *
     * @param command Command to execute (will be parsed and run)
     * @param cwd Working directory for the process
     * @param on_complete Optional exactly-once natural-exit notification
     * @return Process ID on success, empty string on failure
     */
    std::string start_background(const std::string& command,
                                 const std::string& cwd = "",
                                 CompletionCallback on_complete = {});

    /**
     * Send input to a background process's stdin
     *
     * @param process_id ID returned from start_background()
     * @param input Data to send
     * @return SendResult with success/error info
     */
    SendResult send_to_process(const std::string& process_id, const std::string& input);

    /**
     * Poll output from a background process
     *
     * @param process_id ID returned from start_background()
     * @param clear_after_read If true, clear the output files after reading
     * @return ProcessOutput with stdout/stderr content
     */
    ProcessOutput poll_output(const std::string& process_id, bool clear_after_read = false);

    /**
     * Stop a background process
     *
     * @param process_id ID returned from start_background()
     * @return true if process was stopped (or already dead)
     */
    bool stop_process(const std::string& process_id);

    /**
     * List all active (not exited) background processes
     *
     * @return Vector of process info for active processes
     */
    std::vector<BackgroundProcessInfo> list_active_processes() const;

    /**
     * Get info about a specific process
     *
     * @param process_id ID returned from start_background()
     * @return Process info if found, nullopt if not found
     */
    std::optional<BackgroundProcessInfo> get_process_info(const std::string& process_id);

    /**
     * Clean up all resources (called on exit)
     *
     * Kills all running processes and removes FIFO/buffer files.
     */
    void cleanup_all();

    /**
     * Check if a process has exited
     *
     * @param process_id ID returned from start_background()
     * @return true if process has exited
     */
    bool has_exited(const std::string& process_id);

    /**
     * Get the base temp directory for process files
     */
    const std::filesystem::path& get_temp_base() const { return temp_base_; }

private:
    // Singleton - private constructors
    BackgroundProcessManager();
    ~BackgroundProcessManager();
    BackgroundProcessManager(const BackgroundProcessManager&) = delete;
    BackgroundProcessManager& operator=(const BackgroundProcessManager&) = delete;

    struct ProcessEntry {
        BackgroundProcessInfo info;
        CompletionCallback on_complete;
        bool stop_requested = false;
        bool completion_dispatched = false;
    };

    // Internal methods
    std::string generate_process_id();
    bool create_fifo(const std::string& path);
    bool remove_fifo(const std::string& path);
    void ensure_temp_dir();
    void cleanup_process(const std::string& process_id);
    pid_t spawn_process(const std::string& command, const std::string& cwd,
                        const std::string& fifo_path,
                        const std::string& stdout_path,
                        const std::string& stderr_path);
    bool write_to_fifo(const std::string& fifo_path, const std::string& data);
    std::string read_file(const std::string& path);
    void clear_file(const std::string& path);
    bool is_process_running(pid_t pid) const;
    bool kill_process_group(pid_t pid);
    void ensure_monitor_locked();
    void monitor_loop(std::stop_token stop_token);

    // Configuration
    static constexpr size_t MAX_OUTPUT_SIZE = 100000;  // 100KB max output buffer
    static constexpr const char* TEMP_DIR_PREFIX = "daedalus_bg_";

    // State. waitpid() runs only under mutex_ (natural exits on the monitor,
    // explicit kills on the stopping thread), so polling and status queries
    // can never race it for the exit status.
    std::filesystem::path temp_base_;
    mutable std::mutex mutex_;
    std::condition_variable monitor_cv_;
    std::jthread monitor_thread_;
    std::unordered_map<std::string, ProcessEntry> processes_;
    int process_counter_ = 0;
};

#endif // BACKGROUND_PROCESS_MANAGER_H