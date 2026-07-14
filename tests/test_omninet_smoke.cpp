// tests/test_omninet_smoke.cpp
//
// Smoke test: verify OmniNet is reachable from daedalus, init/cleanup works,
// and we can use the public API. This is the P1 acceptance test for the
// OmniNet FetchContent integration.
//
// Build target is set up in tests/CMakeLists.txt; see there.

#include <iostream>
#include "OmniNet.h"   // main umbrella header; pulls in Logger, Config, etc.

static int failures = 0;
#define EXPECT(cond, msg) do { \
    if (!(cond)) { ++failures; std::cerr << "  FAIL: " << msg << std::endl; } \
    else         { std::cerr << "  OK:   " << msg << std::endl; } \
} while (0)

int main() {
    std::cerr << "=== OmniNet smoke test ===\n";

    // 1. init / cleanup round-trip
    EXPECT(true, "OmniNet.h included successfully");
    OmniNet::init();
    std::cerr << "  info: OmniNet::init() returned\n";
    OmniNet::cleanup();
    EXPECT(true, "OmniNet::cleanup() returned");

    // 2. re-init to confirm idempotency-ish behavior
    OmniNet::init();
    EXPECT(true, "OmniNet::init() re-callable");
    OmniNet::cleanup();

    // 3. Logger is reachable
    auto& logger = OmniNet::Logger::instance();
    logger.info(std::string("smoke test: OmniNet logger reachable from daedalus"));
    EXPECT(true, "Logger::instance() reachable");

    // 4. Config is reachable
    auto& config = OmniNet::Config::instance();
    auto data_dir = config.dataDir();
    std::cerr << "  info: dataDir() = " << data_dir << "\n";
    EXPECT(!data_dir.empty(), "Config::instance().dataDir() returns non-empty path");

    std::cerr << "\nResults: " << (failures == 0 ? "all passed" : "FAILED") << "\n";
    return failures > 0 ? 1 : 0;
}
