// Repro: daedalus Subprocess::execute hang scenarios (upstream bug probe)
#include "core/Subprocess.h"
#include <cstdio>
#include <chrono>

static void run(const char* label, const char* cmd, int timeout_ms) {
    printf("[%s] start: %s (timeout %d ms)\n", label, cmd, timeout_ms);
    fflush(stdout);
    auto t0 = std::chrono::steady_clock::now();
    auto r = Subprocess::execute(cmd, "", timeout_ms);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    printf("[%s] RETURNED after %lld ms, exit=%d timed_out=%d out=%.60s\n",
           label, (long long)ms, r.exit_code, (int)r.timed_out, r.stdout.c_str());
    fflush(stdout);
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: repro A|B|C\n"); return 1; }
    switch (argv[1][0]) {
        case 'C': // control: normal fast command
            run("control", "echo hello", 5000); break;
        case 'A': // Hang A: child closes fds, keeps running -> EOF branch, blocking waitpid
            run("hangA", "exec sleep 300 >&- 2>&-", 5000); break;
        case 'B': // Hang B: setsid grandchild holds pipes, foreground times out
            run("hangB", "setsid sleep 600 & sleep 100", 5000); break;
    }
    return 0;
}
