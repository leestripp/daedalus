/**
 * @file test_background_process.cpp
 * @brief Unit tests for BackgroundProcessManager functionality
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <core/BackgroundProcessManager.h>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <csignal>

using namespace std::chrono_literals;

TEST_CASE("BackgroundProcessManager - start and stop simple command") {
    auto& manager = BackgroundProcessManager::get_instance();
    manager.cleanup_all();
    
    // Start a simple background process
    std::string pid = manager.start_background("sleep 5");
    CHECK(!pid.empty());
    
    // Verify process is not exited
    CHECK(!manager.has_exited(pid));
    
    // Get process info
    auto info = manager.get_process_info(pid);
    CHECK(info.has_value());
    CHECK(info->pid > 0);
    CHECK(info->command == "sleep 5");
    
    // Stop the process
    bool stopped = manager.stop_process(pid);
    CHECK(stopped);
    
    // Verify process is exited
    CHECK(manager.has_exited(pid));
}

TEST_CASE("BackgroundProcessManager - list active processes") {
    auto& manager = BackgroundProcessManager::get_instance();
    manager.cleanup_all();
    
    // Start a couple of processes
    std::string pid1 = manager.start_background("sleep 10");
    std::string pid2 = manager.start_background("sleep 10");
    
    CHECK(!pid1.empty());
    CHECK(!pid2.empty());
    CHECK(pid1 != pid2);
    
    // List active processes
    auto active = manager.list_active_processes();
    CHECK(active.size() >= 2);  // At least our 2 processes
    
    // Clean up
    manager.stop_process(pid1);
    manager.stop_process(pid2);
}

TEST_CASE("BackgroundProcessManager - poll output") {
    auto& manager = BackgroundProcessManager::get_instance();
    manager.cleanup_all();
    
    // Start a simple sleep process that produces no output
    std::string pid = manager.start_background("sleep 2");
    CHECK(!pid.empty());
    
    // Verify process is not exited immediately
    CHECK(!manager.has_exited(pid));
    
    // Poll output - should have empty stdout/stderr
    ProcessOutput output = manager.poll_output(pid);
    CHECK(output.stdout.empty());
    CHECK(output.stderr.empty());
    
    // Process should still be running (sleep 2 takes time)
    CHECK(!manager.has_exited(pid));
    
    // Stop the process
    bool stopped = manager.stop_process(pid);
    CHECK(stopped);
    
    // Verify process is exited after stop
    CHECK(manager.has_exited(pid));
}

TEST_CASE("BackgroundProcessManager - stop non-existent process") {
    auto& manager = BackgroundProcessManager::get_instance();
    manager.cleanup_all();
    
    // Try to stop a process that doesn't exist
    bool result = manager.stop_process("bg_99999");
    CHECK(result == false);  // Should return false for non-existent
    
    CHECK(manager.has_exited("bg_99999"));  // Non-existent = considered exited
}

TEST_CASE("BackgroundProcessManager - send to non-existent process") {
    auto& manager = BackgroundProcessManager::get_instance();
    manager.cleanup_all();
    
    SendResult result = manager.send_to_process("bg_99999", "test");
    CHECK(!result.success);
    CHECK(!result.error.empty());
}

TEST_CASE("BackgroundProcessManager - poll non-existent process") {
    auto& manager = BackgroundProcessManager::get_instance();
    manager.cleanup_all();
    
    ProcessOutput output = manager.poll_output("bg_99999");
    CHECK(output.stdout.empty());
    CHECK(output.stderr.empty());
    // Non-existent process returns empty output, exited flag depends on internal state
    // The key point is it doesn't crash
}

TEST_CASE("BackgroundProcessManager - process info") {
    auto& manager = BackgroundProcessManager::get_instance();
    manager.cleanup_all();
    
    // Start a process
    std::string pid = manager.start_background("echo test");
    CHECK(!pid.empty());
    
    // Get info while running
    auto info = manager.get_process_info(pid);
    CHECK(info.has_value());
    CHECK(info->process_id == pid);
    CHECK(info->pid > 0);
    CHECK(info->command == "echo test");
    CHECK(info->fifo_path.empty() == false);
    CHECK(info->stdout_path.empty() == false);
    CHECK(info->stderr_path.empty() == false);
    CHECK(info->exited == false);
    
    // Stop the process
    manager.stop_process(pid);
    
    // After stop, cleanup_process removes from map, so info may not be available
    auto info_after = manager.get_process_info(pid);
    // info_after may be nullopt since cleanup removes the entry
}

TEST_CASE("BackgroundProcessManager - cleanup all") {
    auto& manager = BackgroundProcessManager::get_instance();
    manager.cleanup_all();
    
    // Start multiple processes
    std::string pid1 = manager.start_background("sleep 100");
    std::string pid2 = manager.start_background("sleep 100");
    
    CHECK(!pid1.empty());
    CHECK(!pid2.empty());
    
    // Cleanup all
    manager.cleanup_all();
    
    // Verify all stopped
    auto active = manager.list_active_processes();
    CHECK(active.empty());
}
TEST_CASE("BackgroundProcessManager - natural exit callback is exactly once") {
    auto& manager = BackgroundProcessManager::get_instance();
    manager.cleanup_all();

    std::mutex mutex;
    std::condition_variable cv;
    int calls = 0;
    BackgroundProcessInfo completed;
    ProcessOutput final_output;

    const std::string id = manager.start_background(
        "printf 'callback-output'; exit 0", "",
        [&](const BackgroundProcessInfo& info, const ProcessOutput& output) {
            {
                std::lock_guard lock(mutex);
                ++calls;
                completed = info;
                final_output = output;
            }
            cv.notify_all();
        });
    REQUIRE(!id.empty());

    // Status polling must coexist with the monitor rather than racing it
    // for waitpid(). The callback still owns terminal-status delivery.
    for (int i = 0; i < 20; ++i) {
        (void)manager.poll_output(id);
        std::this_thread::sleep_for(5ms);
    }
    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, 2s, [&] { return calls == 1; }));
        CHECK(completed.process_id == id);
        CHECK(completed.exit_code == 0);
        CHECK(!completed.signaled);
        CHECK(final_output.exited);
        CHECK(final_output.stdout == "callback-output");
    }
    std::this_thread::sleep_for(30ms);
    CHECK(calls == 1);
    CHECK(manager.poll_output(id).exit_code == 0);
}

TEST_CASE("BackgroundProcessManager - callbacks preserve failure and signal status") {
    auto& manager = BackgroundProcessManager::get_instance();
    manager.cleanup_all();

    std::mutex mutex;
    std::condition_variable cv;
    std::vector<ProcessOutput> outputs;
    const auto callback = [&](const BackgroundProcessInfo&,
                              const ProcessOutput& output) {
        {
            std::lock_guard lock(mutex);
            outputs.push_back(output);
        }
        cv.notify_all();
    };

    const std::string failed = manager.start_background("exit 23", "", callback);
    const std::string signaled = manager.start_background("kill -TERM $$", "", callback);
    REQUIRE(!failed.empty());
    REQUIRE(!signaled.empty());

    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, 2s, [&] { return outputs.size() == 2; }));
    }
    CHECK(manager.get_process_info(failed)->exit_code == 23);
    CHECK(manager.get_process_info(signaled)->signaled);
    CHECK(manager.get_process_info(signaled)->signal_num == SIGTERM);
    std::this_thread::sleep_for(30ms);
    CHECK(outputs.size() == 2);
}

TEST_CASE("BackgroundProcessManager - explicit stop suppresses completion") {
    auto& manager = BackgroundProcessManager::get_instance();
    manager.cleanup_all();

    std::atomic<int> calls{0};
    const std::string id = manager.start_background(
        "sleep 30", "", [&](const BackgroundProcessInfo&, const ProcessOutput&) {
            ++calls;
        });
    REQUIRE(!id.empty());
    CHECK(manager.stop_process(id));
    std::this_thread::sleep_for(30ms);
    CHECK(calls == 0);
}
