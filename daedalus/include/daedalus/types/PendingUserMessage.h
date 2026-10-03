/**
 * @file daedalus/types/PendingUserMessage.h
 * @brief A user message sitting in the Agent's pending queue, waiting to
 *        be consumed by run_one_turn().
 *
 * Hosts receive snapshots of these from Agent::pending_messages() and can
 * rewrite the queue with Agent::edit_pending_message(),
 * Agent::remove_pending_message() and Agent::clear_pending_messages().
 */
#pragma once

#include <string>
#include <vector>

#include "daedalus/types/ImageAttachment.h"

namespace daedalus {

struct PendingUserMessage {
    std::string text;
    std::vector<ImageAttachment> images;   ///< optional attachments (may be empty)

    /// §MSG-HEADERS (2026-10-04): free-form `kind:id` origin of this
    /// message — "user:<name>" for the interactive host, "a2a:<agent>",
    /// "trigger:<name>", "email:<addr>", ... Stamped into the message's
    /// `_meta` (with a library-generated timestamp) when the turn
    /// consumes it, so the model can tell who/what sent each message
    /// and when. Empty = legacy interactive user message.
    std::string source;
};

}  // namespace daedalus
