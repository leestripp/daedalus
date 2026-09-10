/**
 * @file daedalus/types/HistoryEntry.h
 * @brief One entry of the read-only conversation history snapshot
 *        returned by Agent::history_snapshot() (§HISTORY-SNAPSHOT,
 *        2026-09-10, MonkeyByte upstream report REQ-2).
 *
 * The snapshot reflects what the agent actually sees — post-restore,
 * post-compression, post-sanitization — so GUI hosts can render the
 * transcript from the library instead of keeping a parallel chat store
 * that silently diverges.
 */
#pragma once

#include <string>

namespace daedalus {

struct HistoryEntry {
    std::string role;     ///< "system" | "user" | "assistant"
    std::string content;  ///< text content; tool calls/results are elided
};

}  // namespace daedalus
