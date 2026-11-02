/**
 * @file src/ConsoleAgentSink.h
 * @brief Default AgentSink implementation for the CLI demo.
 *
 * Wires every sink event to the console:
 *   - on_assistant_delta → cout (with buffering and final newline)
 *   - on_log            → log file always; cerr only for Warn/Error/Debug
 *                         (Info chatter stays out of the terminal)
 *   - on_tool_call      → cerr (tool header card)
 *   - on_tool_result    → cerr (tool result summary)
 *   - on_state_change   → cerr (Phase 9.6)
 *   - on_compaction     → cerr (Phase 9.6)
 *   - on_bell           → play_system_bell()
 *   - request_command_approval → y/n prompt on cin
 *
 * Hosts that need different I/O (TUI, web, GTK4) implement their own
 * AgentSink subclass and pass it to LLMClient::set_sink().
 */
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <termios.h>
#include <thread>

#include <nlohmann/json.hpp>

#include "daedalus/AgentSink.h"

// §Phase 9.10: demos/daedalus-cli/CMakeLists.txt adds demos/common/ to
// the include path, so the header resolves as a bare filename.
#include "ThinkingSpinner.h"
#include "ui/AnsiRenderer.h"
#include "ui/InputReader.h"
#include "ui/StatusHud.h"

namespace daedalus {

class ConsoleAgentSink final : public AgentSink {
public:
    explicit ConsoleAgentSink(icarus::ui::RenderConfig cfg = icarus::ui::RenderConfig::detect());
    ~ConsoleAgentSink() override;

    // icarus: optional timestamped log file for post-hoc inspection.
    void set_log_path(std::string path);

    // §Task 6 (CodeMax.md §16.5.3): injected line reader for command
    // approval. The owning main() loop installs a std::function that
    // reads through icarus::ui::InputReader so the sink does not need
    // to depend on linenoise directly. The installed reader must not
    // run concurrently with the background InputThread — main() wraps
    // it so the input thread is paused for the duration of the read
    // (linenoise is single-threaded; stdin must have exactly one
    // reader, or the two race for the user's answer). If no reader is
    // installed, request_command_approval() falls back to cooked
    // std::getline.
    using LineReader = icarus::ui::InputReader::LineReader;
    void set_approval_reader(LineReader reader);

    // §Phase 9.10: expose the spinner so the main loop can override
    // the message ("Thinking...", "Planning...", etc.) before calling
    // start(). The sink itself manages the auto-stop on assistant_delta
    // and the start/stop on state changes.
    ThinkingSpinner& spinner() noexcept { return spinner_; }
    const ThinkingSpinner& spinner() const noexcept { return spinner_; }

    // Turn-level spinner lifecycle. main() brackets every agent turn with
    // these calls so the spinner runs from the moment the user submits —
    // covering the dead time before daedalus fires its first state-change
    // or LLM-wait event. The sink's callbacks also drive them, and both
    // are idempotent and safe to call concurrently with sink callbacks
    // (guarded by spinner_lifecycle_mutex_ + atomics).
    //   start_agent_turn() - start spinner + watchdog when agent work begins.
    //   stop_agent_turn()  - stop spinner and mark the turn as finished.
    void start_agent_turn();
    void stop_agent_turn();

    // Streaming output — buffer deltas, flush on final newline or buffer limit.
    void on_assistant_delta(std::string_view text) noexcept override;

    // Tool lifecycle — draw our own cards now that daedalus no longer calls
    // on_tool_display().
    void on_tool_call(std::string_view name,
                      const nlohmann::json& args) noexcept override;
    // §Task D1 (CodeMax.md §16): 3-arg overload with call_id. Delegates to
    // the 2-arg form (empty call_id) so the header is rendered once.
    void on_tool_call(std::string_view name,
                      std::string_view call_id,
                      const nlohmann::json& args) noexcept override;
    void on_tool_result(std::string_view name,
                        const nlohmann::json& result) noexcept override;
    // §Task D2: 3-arg on_tool_result with call_id.
    void on_tool_result(std::string_view name,
                        std::string_view call_id,
                        const nlohmann::json& result) noexcept override;
    // §Phase C1: streaming tool-call progress (compact stderr marker).
    void on_tool_progress(std::string_view name,
                          const nlohmann::json& progress) noexcept override;
    // §Task D3: 3-arg on_tool_progress with call_id + new shape
    // {stage, percent, message}.
    void on_tool_progress(std::string_view name,
                          std::string_view call_id,
                          const nlohmann::json& progress) noexcept override;

    // LLM request lifecycle — §daedalus 0.7.3-4.
    void on_llm_wait_started() noexcept override;
    void on_llm_response_started() noexcept override;
    void on_llm_request_finished() noexcept override;

    // State / lifecycle — §Phase 9.6 observability.
    void on_state_change(int from, int to) noexcept override;

    // Structured error channel (§on_error). Terminal LLM failures still end
    // the turn with AgentState::Done, so a turn's failure is visible only
    // through this callback — rendered distinctly (warning while retrying,
    // error when terminal) and always written to the log file.
    void on_error(daedalus::ErrorKind kind,
                  std::string_view message,
                  bool will_retry) noexcept override;

    // Context compaction — §Phase 9.6 observability.
    void on_compaction(std::size_t before_tokens,
                       std::size_t after_tokens) noexcept override;

    // Log messages.
    void on_log(daedalus::LogLevel level,
                std::string_view message) noexcept override;

    // Bell.
    void on_bell() noexcept override;

    // Blocking I/O.
    bool request_command_approval(std::string_view command) override;

    // §BUG-008 (daedalus 0.8.7): a command was approved WITHOUT prompting
    // (auto-approve mode, session cache, or persisted cache). Informational.
    void on_command_auto_approved(
        std::string_view command,
        daedalus::ApprovalSource source) noexcept override;

    // §BUG-008: injected setter so the sink's 'a' answer can drive
    // Agent::set_auto_approve() — daedalus's authoritative runtime
    // auto-approve. When unset, the sink falls back to its local
    // approval_always_ flag (tests / sinks without an Agent).
    void set_auto_approve_setter(std::function<void(bool)> setter);

    // Wake the REPL when a background process completion is queued. The
    // handler only wakes input_queue; main remains the sole driving thread.
    void set_turn_request_handler(std::function<void()> handler);
    void on_turn_requested(std::string_view source) noexcept override;

private:
    /// Start the ThinkingSpinner with the current HUD status message.
    void start_status_spinner();

    // §ToolSpinner (2026-08-07): tool-execution spinner. Long-running tools
    // (execute_bash especially) previously killed the spinner for their
    // whole duration — save_terminal_for_subprocess() called
    // stop_agent_turn(), which disarmed the watchdog — so the UI looked
    // frozen. Now the spinner shows "Running <tool>... (Ns)" while a tool
    // executes. The TTY-takeover premise behind the old pause is false:
    // daedalus's Subprocess pipes child stdout/stderr
    // (src/core/Subprocess.cpp), so the child never paints the terminal —
    // its output returns as sanitised output_chunk events printed by this
    // sink, and printing already stops the spinner via on_agent_output().
    void begin_tool_spinner(std::string_view name);
    void end_tool_spinner() noexcept;
    /// "Running <name>... (Ns)" label. Caller holds spinner_lifecycle_mutex_.
    std::string tool_spinner_label() const;

    /// Lifecycle helpers for the turn-level spinner watchdog.
    ///   on_agent_output()      - stop spinner and reset the idle timer on any output.
    ///   spinner_watchdog_loop() - restart the spinner after 3 s of quiet.
    /// (start_agent_turn()/stop_agent_turn() are public — main() brackets
    /// each turn with them.)
    void on_agent_output();
    void spinner_watchdog_loop();

    /// Save/restore terminal state around execute_bash subprocesses that may
    /// take over the TTY (editors, pagers, etc.).
    void save_terminal_for_subprocess();
    void restore_terminal_after_subprocess();

    /// Flush any buffered assistant text to stdout.
    void flush_delta_buffer();

    /// If the last stdout text ended mid-line, print the missing newline.
    /// Used at turn end so follow-up output starts on a fresh line.
    void commit_mid_line();

    /// Append one timestamped line to the log file (no-op when log_path_
    /// is empty). Shared by on_log() and on_error().
    void write_log_line(std::string_view level, std::string_view message);

    /// Render a tool-call header card to stderr.
    void render_tool_header(std::string_view name,
                            const nlohmann::json& args);

    /// Render a tool-result summary to stderr.
    void render_tool_result(std::string_view name,
                            const nlohmann::json& result);

    /// Print user-facing tool text (tell_user content, stop summary)
    /// verbatim to stdout, mutex-guarded against assistant-text flushes.
    void print_user_facing_text(std::string_view text);

    std::string delta_buffer_;
    std::mutex delta_mutex_;  // protects delta_buffer_ + stream_mid_line_
    static constexpr std::size_t DELTA_FLUSH_THRESHOLD = 2048;

    // True when the last text printed to std::cout ended mid-line (no
    // trailing '\n'). The spinner draws with '\r' + erase-to-EOL on the
    // current line, so restarting it while the stream is mid-line would
    // wipe the partial text from the screen (it is already out of
    // delta_buffer_, so it never reprints). start_status_spinner()
    // commits the line with a '\n' before starting the animation.
    // Guarded by delta_mutex_.
    bool stream_mid_line_ = false;

    // Visual rendering configuration and helpers (declared early because
    // spinner_/hud_ depend on them).
    icarus::ui::RenderConfig cfg_;
    icarus::ui::Style style_;
    icarus::ui::StatusHud hud_;

    // Last AgentState value delivered via on_state_change. This is the
    // authoritative source for the spinner label: Agent::state() only
    // tracks Understanding/terminal states, while the intermediate
    // states (Planning, Acting, ...) arrive only through the callback.
    std::atomic<int> current_state_{0};  // 0 = AgentState::Idle

    // §Phase 9.10: spinner shown on stderr between user submit and the
    // first streamed assistant_delta. See demos/common/ThinkingSpinner.h
    // for the lifecycle / auto-stop contract.
    ThinkingSpinner spinner_;

    // Watchdog that restarts the spinner when the agent produces no output
    // for a few seconds. This keeps the cursor alive during long tool runs
    // or gaps between streamed chunks.
    std::thread spinner_watchdog_thread_;
    std::atomic<bool> spinner_watchdog_running_{false};
    std::atomic<bool> turn_active_{false};
    std::atomic<std::chrono::steady_clock::time_point> last_output_time_;
    std::mutex spinner_watchdog_mutex_;
    std::condition_variable spinner_watchdog_cv_;
    static constexpr std::chrono::seconds kSpinnerIdleRestart{3};

    // Serializes start()/stop() of the spinner because the agent worker thread
    // (on_agent_output) and the watchdog thread can both touch it.
    std::mutex spinner_lifecycle_mutex_;

    // §ToolSpinner: set while a tool call is executing so the spinner label
    // shows the tool + elapsed time instead of the HUD state. tool_active_
    // is the atomic fast-path; the name/start/last-update fields are guarded
    // by spinner_lifecycle_mutex_ (written on the agent worker thread in
    // on_tool_call/on_tool_result, read/refreshed by the watchdog).
    std::atomic<bool> tool_active_{false};
    std::string active_tool_name_;
    std::chrono::steady_clock::time_point tool_start_time_{};
    std::chrono::steady_clock::time_point last_tool_label_update_{};

    // Set true at the start of the destructor so any late callback or watchdog
    // tick avoids touching the sticky panel after it has been destroyed.
    std::atomic<bool> shutting_down_{false};

    // Saved terminal state around execute_bash subprocesses that may take over
    // the terminal (e.g. pagers, editors, TUIs).
    struct termios saved_tio_for_subprocess_{};
    bool subprocess_termios_saved_ = false;

    // icarus: optional timestamped log file path.
    std::string log_path_;

    // §Task 6: injected line reader for command approval. Default
    // empty → request_command_approval() falls back to std::getline.
    LineReader approval_reader_;

    // §BUG-008: injected Agent::set_auto_approve() handle for the 'a'
    // answer. Empty → fall back to approval_always_ below.
    std::function<void(bool)> auto_approve_setter_;
    std::function<void()> turn_request_handler_;

    // Session-scoped "always approve" flag, set when the user answers 'a'
    // at the approval prompt. Atomic: approvals are requested from the
    // agent worker thread.
    std::atomic<bool> approval_always_{false};

    // Set when a tool streamed live output via output_chunk progress
    // events, so on_tool_result can elide the duplicated "output" field
    // instead of printing it a second time. Only touched from the agent
    // worker thread (progress → result is a strict sequence per call).
    bool streamed_tool_output_ = false;

};

}  // namespace daedalus
