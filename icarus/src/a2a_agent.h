// SPDX-License-Identifier: BSL-1.0
// src/a2a_agent.h
//
// A2A Agent implementation — icarus as an A2A agent.
//
// Runs an HTTP server speaking the Agent2Agent Protocol (A2A) by
// implementing daedalus::a2a::IA2aTaskBackend. Each incoming task is
// handled by a dedicated daedalus::Agent instance.
//
// Usage:
//   icarus --a2a-agent [--a2a-port 8080]

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include <daedalus.h>
#include <daedalus/a2a/A2aServer.h>

struct CliArgs;

namespace a2a {

/**
 * @brief Run icarus as an A2A agent HTTP server.
 *
 * Blocks until SIGINT/SIGTERM is received (or the server otherwise stops).
 * The parsed CLI args are honoured for each per-task agent: --model,
 * --home, --skills, --system-prompt, --no-memory and --agent-name all apply;
 * --a2a-port selects the listen port.
 *
 * @param args  Parsed command-line arguments.
 * @return 0 on clean shutdown, non-zero on error.
 */
int run_a2a_agent(const CliArgs& args);

} // namespace a2a
