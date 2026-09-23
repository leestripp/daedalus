/**
 * @file test_subprocess_timeout.cpp
 * @brief §SUBPROC-EOF-HANG / §SUBPROC-DRAIN-HANG regression tests
 *        (daedalus_bugs.md BUG 2, MonkeyByte 2026-09-01).
 *
 * Two infinite hangs in Subprocess::execute_combined:
 *
 *  Hang A — EOF branch: if the direct child closes its inherited pipe fds
 *  but keeps running (`exec sleep 300 >&- 2>&-`), both pipes hit EOF while
 *  the child is alive; the branch then did a blocking waitpid() that never
 *  consulted the configured timeout.
 *
 *  Hang B — timeout drain: after kill_group() (killpg), a grandchild that
 *  escaped the process group (setsid) keeps the pipe write ends open, so
 *  EOF never arrives; the drain loop had no iteration cap (and used &&).
 *
 * Pre-fix, cases 3 and 4 hang forever (run pre-fix under an external
 * `timeout` wrapper to observe). Post-fix both return promptly with
 * timed_out == true.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/Subprocess.h"

#include <chrono>
#include <cstdlib>
#include <stop_token>
#include <thread>

namespace {

double run_timed(const std::string& cmd, int timeout_ms, SubprocessResult& out) {
    const auto t0 = std::chrono::steady_clock::now();
    out = Subprocess::execute_combined(cmd, "", timeout_ms);
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(t1 - t0).count();
}

} // namespace

TEST_CASE("subprocess: normal command returns output and exit code") {
    SubprocessResult r;
    const double secs = run_timed("echo hello-subprocess-test", 5000, r);
    CHECK(secs < 10.0);
    CHECK_FALSE(r.timed_out);
    CHECK(r.exit_code == 0);
    CHECK(r.stdout.find("hello-subprocess-test") != std::string::npos);
}

TEST_CASE("subprocess: plain timeout kills a sleeping child promptly") {
    SubprocessResult r;
    const double secs = run_timed("sleep 30", 1000, r);
    // kill_group gives 100ms grace + reap slack; 10s is generous.
    CHECK(secs < 10.0);
    CHECK(r.timed_out);
}

TEST_CASE("subprocess: §SUBPROC-EOF-HANG — child that closed its fds still honours the timeout") {
    // Direct child closes stdout/stderr but keeps running: both pipes hit
    // EOF while the child is alive. Pre-fix this blocked in waitpid()
    // forever, ignoring the 2000ms timeout.
    SubprocessResult r;
    const double secs = run_timed("exec sleep 300 >&- 2>&-", 2000, r);
    CHECK(secs < 15.0);
    CHECK(r.timed_out);
}

TEST_CASE("subprocess: §BASH-CANCEL — stop_token kills a sleeping child instantly") {
    // User-facing stop must interrupt a synchronous execute_bash without
    // waiting for the tool timeout (which can be an hour). Request stop
    // from another thread 200ms into a 600s run; the poll loop should
    // kill the process group and return within a couple of seconds.
    std::stop_source source;
    SubprocessResult r;
    const auto t0 = std::chrono::steady_clock::now();
    std::thread stopper([src = source]() mutable {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        src.request_stop();
    });
    r = Subprocess::execute_combined("sleep 600", "", 600000, source.get_token());
    stopper.join();
    const double secs = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    CHECK(secs < 5.0);
    CHECK(r.cancelled);
    CHECK_FALSE(r.timed_out);
    // The child was actually killed, not merely abandoned.
    CHECK(std::system("pgrep  'sleep 600' >/dev/null 2>&1") != 0);
}

TEST_CASE("subprocess: §BASH-CANCEL — stop_token honoured on the EOF branch too") {
    // Same cancel guarantee when the child has closed its inherited pipe
    // fds (the §SUBPROC-EOF-HANG branch): both pipes EOF while the child
    // sleeps on; cancel must still kill it instantly.
    std::stop_source source;
    const auto t0 = std::chrono::steady_clock::now();
    std::thread stopper([src = source]() mutable {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        src.request_stop();
    });
    SubprocessResult r =
        Subprocess::execute_combined("exec sleep 601 >&- 2>&-", "", 600000,
                                     source.get_token());
    stopper.join();
    const double secs = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    CHECK(secs < 5.0);
    CHECK(r.cancelled);
    CHECK_FALSE(r.timed_out);
    std::system("pkill  'sleep 601' 2>/dev/null");  // belt-and-braces cleanup
}

TEST_CASE("subprocess: §SUBPROC-DRAIN-HANG — setsid grandchild holding pipes cannot hang the timeout path") {
    // Grandchild escapes the process group (setsid) and inherits the pipe
    // write ends; killpg() cannot reach it, so EOF never arrives. Pre-fix
    // the timeout branch's drain loop spun forever.
    SubprocessResult r;
    const double secs = run_timed("setsid sleep 613 & sleep 100", 2000, r);
    CHECK(secs < 15.0);
    CHECK(r.timed_out);
    // Clean up the orphaned grandchild so it does not linger on CI hosts.
    std::system("pkill  'sleep 613' 2>/dev/null");
}
