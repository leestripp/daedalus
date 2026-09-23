/**
 * @file daedalus/types/AgentError.h
 * @brief The last terminal (non-retried) error of the current/most recent
 *        turn, queryable after Agent::run_one_turn() returns.
 *
 * Terminal LLM failures (fatal HTTP 4xx, retries exhausted, no model
 * configured) still end the turn with public AgentState::Done — the
 * internal state machine has no error state for those paths. Hosts that
 * need to know a turn failed query Agent::turn_had_terminal_error() /
 * Agent::last_error() instead of reconstructing the outcome from
 * AgentSink::on_error callbacks (§TURN-ERROR, 2026-09-10, MonkeyByte
 * upstream report BUG-4).
 */
#pragma once

#include <string>

#include "daedalus/types/ErrorKind.h"

namespace daedalus {

struct AgentError {
    ErrorKind   kind = ErrorKind::Internal;
    std::string message;
};

}  // namespace daedalus
