/**
 * @file daedalus/AgentSink.h
 * @brief Callback interface that host applications implement to receive
 *        events from the agent and to answer blocking I/O requests.
 *
 * AgentSink is the *only* way the library talks to the outside world.
 * All logging, streaming text, tool lifecycle, state transitions, and
 * host I/O requests (user input, command approval) flow through here.
 *
 * Default implementations of every method are no-ops (or sane defaults),
 * so hosts only override what they care about. The methods marked
 * "MANDATORY" in the comments below are not — they all have defaults —
 * but a typical host will want to override on_log at minimum.
 *
 * Threading: every method is called on the agent's worker thread. Hosts
 * that need to touch UI frameworks with thread affinity (GTK, Qt) MUST
 * marshal the call to the UI thread before acting.
 */
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "daedalus/types/LogLevel.h"

namespace daedalus {

class AgentSink {
public:
    virtual ~AgentSink() = default;

    // ------------------------------------------------------------------
    // Streaming output
    // ------------------------------------------------------------------

    /// @brief A chunk of assistant text streamed from the LLM.
    /// Called repeatedly as tokens arrive. Empty chunks are possible.
    virtual void on_assistant_delta(std::string_view text) noexcept
    {
        (void)text;
    }

    // ------------------------------------------------------------------
    // Tool lifecycle
    // ------------------------------------------------------------------

    /// @brief A tool call is about to be executed.
    virtual void on_tool_call(std::string_view name,
                              const nlohmann::json& args) noexcept
    {
        (void)name; (void)args;
    }

    /// @brief A tool call has finished (success or failure indicated in result).
    virtual void on_tool_result(std::string_view name,
                                const nlohmann::json& result) noexcept
    {
        (void)name; (void)result;
    }

    // ------------------------------------------------------------------
    // State / lifecycle
    // ------------------------------------------------------------------

    /// @brief Agent transitioned from one state to another. `to` is the
    /// cast-to-int value of the new AgentState.
    virtual void on_state_change(int /*from*/, int /*to*/) noexcept {}

    /// @brief Free-form log message from the agent.
    /// Default: writes to stderr. Hosts typically override this.
    virtual void on_log(daedalus::LogLevel level,
                        std::string_view message) noexcept;

    /// @brief Context was just compacted.
    virtual void on_compaction(std::size_t /*before_tokens*/,
                               std::size_t /*after_tokens*/) noexcept {}

    /// @brief Agent wants the terminal to flash / system to beep.
    /// Default: no-op. CLI/TUI may ring the bell, web may play a sound.
    virtual void on_bell() noexcept {}

    // ------------------------------------------------------------------
    // Blocking host I/O requests
    //
    // These are called on the agent's worker thread. They may block.
    // Hosts that cannot block (UI thread, async event loop) MUST
    // implement these by posting to the appropriate event loop and
    // waiting for the response synchronously on the worker thread,
    // or by returning an immediate fallback.
    //
    // These methods are not noexcept precisely so hosts CAN throw
    // to abort the current turn if input/approval fails.
    // ------------------------------------------------------------------

    /// @brief The execute_bash tool wants permission to run a command.
    /// @return true to approve, false to deny.
    /// @throws Anything to abort the turn.
    virtual bool request_command_approval(std::string_view command)
    {
        (void)command;
        return false;
    }
};

}  // namespace daedalus