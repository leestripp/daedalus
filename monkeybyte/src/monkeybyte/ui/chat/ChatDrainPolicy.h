#pragma once

#include <cstddef>

namespace monkeybyte::chat_drain {

// The next atomic decision made by ChatController's driving-thread drain.
// Keeping this policy separate makes the background-wait queue boundary
// deterministic to test without a GTK display or a live model server.
enum class Action {
    ResumeSuspended,
    SubmitQueuedSend,
    RunPending,
    Rest,
};

struct Snapshot {
    bool error = false;
    bool waiting = false;
    bool resume_first = false;
    bool turn_requested = false;
    std::size_t queued_sends = 0;
    std::size_t agent_sends = 0;
    std::size_t pending_steers = 0;
    std::size_t background_steers = 0;
};

[[nodiscard]] constexpr Action choose(const Snapshot& s) noexcept {
    if (s.error) return Action::Rest;

    // A suspended outer turn owns the pipeline until its required background
    // result arrives. Neither host-queued nor Agent-queued Sends may join or
    // repeatedly retry that turn. This also covers Send clicked while the
    // verifier was Processing, immediately before it returned WAITING.
    if (s.resume_first || (s.waiting && s.background_steers > 0))
        return Action::ResumeSuspended;
    if (s.waiting) return Action::Rest;

    if (s.queued_sends > 0) return Action::SubmitQueuedSend;
    if (s.turn_requested || s.agent_sends > 0 || s.pending_steers > 0)
        return Action::RunPending;
    return Action::Rest;
}

} // namespace monkeybyte::chat_drain
