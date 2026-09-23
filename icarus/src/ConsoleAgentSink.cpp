/**
 * @file src/ConsoleAgentSink.cpp
 * @brief ConsoleAgentSink implementation with ANSI widget rendering.
 */

#include "ConsoleAgentSink.h"

#include <iostream>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <termios.h>
#include <unistd.h>
#include <algorithm>
#include <unordered_set>

#include "daedalus/types/AgentState.h"
#include <daedalus/Agent.h>

namespace daedalus {

// §Phase 9.10: set of AgentStates that warrant showing the spinner.
static const std::unordered_set<AgentState> THINKING_STATES = {
    AgentState::Understanding, AgentState::Planning,
    AgentState::Acting,        AgentState::Verifying,
    AgentState::Documenting,   AgentState::Compressing,
};

ConsoleAgentSink::ConsoleAgentSink(icarus::ui::RenderConfig cfg)
    : cfg_(std::move(cfg))
    , style_(cfg_)
    , hud_(cfg_)
    , last_output_time_(std::chrono::steady_clock::now()) {
    spinner_.set_message(style_.primary("Thinking..."));
    spinner_.output = &std::cout;
}

ConsoleAgentSink::~ConsoleAgentSink() {
    shutting_down_.store(true, std::memory_order_release);
    stop_agent_turn();
    spinner_watchdog_running_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(spinner_watchdog_mutex_);
        spinner_watchdog_cv_.notify_all();
    }
    if (spinner_watchdog_thread_.joinable()) {
        spinner_watchdog_thread_.join();
    }
}

void ConsoleAgentSink::set_log_path(std::string path) {
    log_path_ = std::move(path);
}

void ConsoleAgentSink::set_approval_reader(LineReader reader) {
    approval_reader_ = std::move(reader);
}

void ConsoleAgentSink::set_auto_approve_setter(std::function<void(bool)> setter) {
    auto_approve_setter_ = std::move(setter);
}

void ConsoleAgentSink::on_assistant_delta(std::string_view text) noexcept {
    // Streaming output has arrived: stop the spinner and reset the idle
    // timer. The watchdog will restart it if the stream pauses.
    on_agent_output();

    {
        std::lock_guard<std::mutex> lk(delta_mutex_);
        delta_buffer_ += text;

        if (delta_buffer_.size() >= DELTA_FLUSH_THRESHOLD ||
            delta_buffer_.find('\n') != std::string::npos) {
            // Flush while still holding the lock so the buffer cannot be
            // appended concurrently by another delta callback.
            if (!delta_buffer_.empty()) {
                std::cout << delta_buffer_ << std::flush;
                stream_mid_line_ = delta_buffer_.back() != '\n';
                delta_buffer_.clear();
            }
        }
    }
}

void ConsoleAgentSink::flush_delta_buffer() {
    std::lock_guard<std::mutex> lk(delta_mutex_);
    if (!delta_buffer_.empty()) {
        // Don't wrap streaming assistant text with before_output/after_output;
        // those clear/reposition the cursor and break natural line wrapping.
        // The scroll region already keeps the footer fixed.
        std::cout << delta_buffer_ << std::flush;
        stream_mid_line_ = delta_buffer_.back() != '\n';
        delta_buffer_.clear();
    }
}

void ConsoleAgentSink::commit_mid_line() {
    std::lock_guard<std::mutex> lk(delta_mutex_);
    if (stream_mid_line_) {
        std::cout << '\n' << std::flush;
        stream_mid_line_ = false;
    }
}

void ConsoleAgentSink::on_tool_call(std::string_view name,
                                    const nlohmann::json& args) noexcept {
    // §fix (2026-07-31, mirrors daedalus f89ea41): flush buffered assistant
    // text first so it appears above the tool card — deltas buffer until
    // newline/threshold (see on_assistant_delta).
    flush_delta_buffer();
    // set_status is the LLM's internal state-signalling tool — the spinner
    // label already reflects it via on_state_change, so a card would be
    // pure protocol chatter.
    if (name != "set_status") {
        render_tool_header(name, args);
    }
    if (name == "execute_bash") {
        save_terminal_for_subprocess();
    }
    // §ToolSpinner: animate "Running <tool>..." for the duration of the
    // call. set_status is internal protocol chatter (see above) — the HUD
    // state label already covers it.
    if (name != "set_status") {
        begin_tool_spinner(name);
    }
}

void ConsoleAgentSink::on_tool_call(std::string_view name,
                                    std::string_view call_id,
                                    const nlohmann::json& args) noexcept {
    (void)call_id;
    on_tool_call(name, args);
}

void ConsoleAgentSink::on_tool_result(std::string_view name,
                                     const nlohmann::json& result) noexcept {
    // §ToolSpinner: the call is over — the watchdog's next restart uses the
    // HUD state label again. The spinner itself is stopped below by
    // render_tool_result() -> on_agent_output().
    end_tool_spinner();
    // If the output already streamed live via output_chunk progress events,
    // elide it from the result card so it is not printed twice.
    nlohmann::json display = result;
    if (streamed_tool_output_ && display.contains("output")) {
        display["output"] = "<streamed live above>";
    }
    streamed_tool_output_ = false;
    // set_status: see on_tool_call — internal protocol tool, not rendered.
    if (name != "set_status") {
        render_tool_result(name, display);
    }
    // §tell_user (2026-08-02): the tool call header truncates args to 50
    // chars, so the user never sees the full text. Both tell_user's
    // "content" and stop's "summary" are user-directed messages carried
    // verbatim in the result — print them in full to stdout after the card.
    const char* user_field = name == "tell_user" ? "content"
                           : name == "stop"      ? "summary"
                                                 : nullptr;
    if (user_field) {
        if (auto it = result.find(user_field);
            it != result.end() && it->is_string()) {
            print_user_facing_text(it->get_ref<const std::string&>());
        }
    }
    if (name == "execute_bash") {
        restore_terminal_after_subprocess();
    }
}

void ConsoleAgentSink::print_user_facing_text(std::string_view text) {
    // delta_mutex_ so the message cannot interleave byte-wise with
    // an assistant-text flush, and to keep stream_mid_line_ correct.
    std::lock_guard<std::mutex> lk(delta_mutex_);
    std::cout << text;
    if (text.empty() || text.back() != '\n') {
        std::cout << '\n';
    }
    std::cout << std::flush;
    stream_mid_line_ = false;
}

void ConsoleAgentSink::on_tool_result(std::string_view name,
                                     std::string_view call_id,
                                     const nlohmann::json& result) noexcept {
    (void)call_id;
    on_tool_result(name, result);
}

void ConsoleAgentSink::on_tool_progress(std::string_view name,
                                       const nlohmann::json& progress) noexcept {
    on_agent_output();

    // Live output chunk from a long-running tool (e.g. execute_bash). The
    // chunk has already been sanitised by daedalus so it is safe to print
    // verbatim to the TTY; consecutive chunks reassemble exactly.
    if (auto it = progress.find("output_chunk");
        it != progress.end() && it->is_string()) {
        const auto& chunk = it->get_ref<const std::string&>();
        // delta_mutex_ so a chunk can never interleave byte-wise with an
        // assistant-text flush, and to keep stream_mid_line_ consistent.
        std::lock_guard<std::mutex> lk(delta_mutex_);
        std::cout << chunk << std::flush;
        if (!chunk.empty()) {
            stream_mid_line_ = chunk.back() != '\n';
        }
        streamed_tool_output_ = true;
        return;
    }

    const std::string stage = progress.value("stage", "");
    const int percent = progress.value("percent", -1);
    const char* arrow = cfg_.unicode_enabled ? "  ↳ " : "  > ";
    std::ostringstream oss;
    if (percent >= 0) {
        oss << arrow << style_.info(std::string(name)) << style_.dim(": ")
            << stage << " (" << percent << "%)";
    } else {
        oss << arrow << style_.info(std::string(name)) << style_.dim(": ")
            << stage;
    }
    std::cerr << oss.str() << "\n";
}

void ConsoleAgentSink::on_tool_progress(std::string_view name,
                                       std::string_view call_id,
                                       const nlohmann::json& progress) noexcept {
    (void)call_id;
    on_tool_progress(name, progress);
}

void ConsoleAgentSink::render_tool_header(std::string_view name,
                                          const nlohmann::json& args) {
    on_agent_output();

    std::vector<std::string> content;
    content.reserve(args.size() + 1);
    const char* tool_prefix = cfg_.unicode_enabled ? "\xF0\x9F\x94\xA7 " : "";
    content.emplace_back(std::string(tool_prefix) + "TOOL CALL: " + std::string(name));

    if (args.is_object() && !args.empty()) {
        for (const auto& [key, val] : args.items()) {
            std::string val_str;
            if (val.is_string()) {
                val_str = val.get<std::string>();
            } else if (val.is_number_integer()) {
                val_str = std::to_string(val.get<long long>());
            } else if (val.is_number_unsigned()) {
                val_str = std::to_string(val.get<unsigned long long>());
            } else if (val.is_number_float()) {
                // Trim the trailing-zero noise std::to_string(double) produces.
                val_str = std::to_string(val.get<double>());
                while (val_str.size() > 1 && val_str.back() == '0') val_str.pop_back();
                if (!val_str.empty() && val_str.back() == '.') val_str.pop_back();
            } else if (val.is_boolean()) {
                val_str = val.get<bool>() ? "true" : "false";
            } else if (val.is_array() || val.is_object()) {
                val_str = "[" + std::to_string(val.size()) + " items]";
            } else {
                val_str = "[unknown type]";
            }

            // Keep the card layout clean by flattening embedded newlines.
            std::replace_if(val_str.begin(), val_str.end(),
                            [](char c) { return c == '\n' || c == '\r'; }, ' ');

            if (val_str.length() > 50) {
                val_str.replace(47, std::string::npos, "...");
            }

            content.emplace_back(key + ": " + val_str);
        }
    }

    std::ostringstream joined;
    for (std::size_t i = 0; i < content.size(); ++i) {
        if (i) joined << '\n';
        joined << content[i];
    }
    const std::string text = joined.str();

    std::cerr << "\n";
    const int live_width = icarus::ui::effective_terminal_width(cfg_);
    if (live_width >= 40) {
        auto lines = icarus::ui::wrap_lines(text, live_width - 4);
        auto box = icarus::ui::make_box(lines, std::nullopt, cfg_, icarus::ui::Alignment::Auto);
        for (const auto& line : box.lines) {
            std::cerr << style_.dim(line) << "\n";
        }
    } else {
        std::cerr << text;
    }
    std::cerr << std::flush;
}

void ConsoleAgentSink::render_tool_result(std::string_view name,
                                          const nlohmann::json& result) {
    on_agent_output();

    std::vector<std::string> content;
    const std::string status = result.value("status", "unknown");

    auto flatten = [](std::string s) {
        std::replace_if(s.begin(), s.end(),
                        [](char c) { return c == '\n' || c == '\r'; }, ' ');
        return s;
    };

    if (status == "success") {
        const char* mark = cfg_.unicode_enabled ? "\xE2\x9C\x85 " : "";
        content.emplace_back(mark + std::string(name) + " done");
    } else if (status == "error" || status == "denied") {
        // §denied (2026-08-02): execute_bash reports host-refused commands
        // with status "denied" — show its message like an error instead of
        // the bare "execute_bash: denied" line.
        const char* mark = cfg_.unicode_enabled ? "\xE2\x9D\x8C " : "";
        content.emplace_back(mark + std::string(name) + " failed: " +
                             flatten(result.value("message", result.value("error", "unknown error"))));
    } else {
        content.emplace_back(std::string(name) + ": " + status);
    }

    // §Todo card (2026-08-01): todo_add/edit/delete return the affected
    // item as a "todo" object — surface it so the card shows what changed,
    // not just "done".
    if (result.contains("todo") && result["todo"].is_object()) {
        const auto& todo = result["todo"];
        std::string line = "#" + std::to_string(todo.value("id", 0)) + ": "
                         + flatten(todo.value("text", ""));
        if (const std::string ts = todo.value("status", ""); !ts.empty())
            line += " (" + ts + ")";
        content.emplace_back(std::move(line));
    }

    // Bash-specific summary.
    if (result.contains("command")) {
        content.emplace_back(std::string("Command: ") + flatten(result.value("command", "")));
    }
    if (result.contains("exit_code")) {
        content.emplace_back("Exit: " + std::to_string(result.value("exit_code", -1)));
    }

    // Multi-line text block with head/tail elision (30-line cap, middle
    // skipped). Used for "output" and friends so large payloads stay
    // readable without flooding the terminal.
    auto append_text_block = [&content](const std::string& text,
                                        const char* label) {
        if (text.empty()) return;
        if (label) content.emplace_back(label);
        std::istringstream stream(text);
        std::string line;
        constexpr int max_lines = 30;
        std::vector<std::string> lines;
        while (std::getline(stream, line)) {
            // Strip a trailing carriage left over from CRLF output.
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back("  " + line);
        }

        if (lines.size() <= max_lines) {
            content.insert(content.end(), lines.begin(), lines.end());
        } else {
            for (std::size_t i = 0; i < 15; ++i) {
                content.push_back(lines[i]);
            }
            const int skipped = static_cast<int>(lines.size()) - 30;
            content.emplace_back("  ... (" + std::to_string(skipped) + " lines skipped, " +
                                 std::to_string(text.size()) + " bytes) ...");
            for (std::size_t i = lines.size() - 15; i < lines.size(); ++i) {
                content.push_back(lines[i]);
            }
        }
    };

    if (result.contains("output")) {
        append_text_block(result.value("output", ""), nullptr);
    }

    // §Hidden fields (2026-08-02): several tools return user-relevant
    // payload the card never showed. Surface it here — see AGENTS.md
    // (src/ConsoleAgentSink.cpp) for the full list.

    // execute_bash poll/background: process info + stdout/stderr (poll
    // returns those names instead of "output", so they never rendered).
    if (result.contains("process_id")) {
        content.emplace_back("Process: " + result.value("process_id", ""));
    }
    if (result.contains("pid")) {
        content.emplace_back("PID: " + std::to_string(result.value("pid", 0)));
    }
    if (result.contains("exited")) {
        content.emplace_back(result.value("exited", false)
                                 ? "Process exited" : "Process running");
    }
    if (result.value("signaled", false)) {
        content.emplace_back("Signal: " + std::to_string(result.value("signal", 0)));
    }
    {
        const bool has_stdout = result.contains("stdout");
        const bool has_stderr = result.contains("stderr");
        // Label blocks only when both streams are present.
        const char* out_label = has_stdout && has_stderr ? "Stdout:" : nullptr;
        const char* err_label = has_stdout && has_stderr ? "Stderr:" : nullptr;
        if (has_stdout) append_text_block(result.value("stdout", ""), out_label);
        if (has_stderr) append_text_block(result.value("stderr", ""), err_label);
    }

    // serper_search: the organic result list was completely invisible.
    if (result.contains("results") && result["results"].is_array()) {
        const auto& results = result["results"];
        constexpr std::size_t max_shown = 5;
        const int width = icarus::ui::effective_terminal_width(cfg_);
        const std::size_t cap = width > 20 ? static_cast<std::size_t>(width - 20) : 60;
        for (std::size_t i = 0; i < results.size() && i < max_shown; ++i) {
            const auto& r = results[i];
            // Truncate the title on its own so a long title cannot push
            // the URL off the line entirely.
            std::string line = icarus::ui::truncate(flatten(r.value("title", "")), 45, "…")
                             + " — " + flatten(r.value("url", ""));
            content.emplace_back(icarus::ui::truncate(line, cap, "…"));
        }
        if (results.size() > max_shown) {
            content.emplace_back("… +" + std::to_string(results.size() - max_shown) + " more");
        }
    }
    if (result.contains("extra") && result["extra"].is_object() &&
        result["extra"].contains("answer_box")) {
        const auto& box = result["extra"]["answer_box"];
        std::string answer = box.is_object()
                                 ? box.value("answer", box.value("snippet", ""))
                                 : "";
        if (!answer.empty()) {
            append_text_block(answer, "Answer:");
        }
    }

    // MCP proxied tools (mcp__<server>__<tool>): "result" is the MCP
    // payload as a plain string — previously only "mcp__… done" showed.
    if (name.starts_with("mcp__") && result.contains("result") &&
        result["result"].is_string()) {
        append_text_block(result["result"].get<std::string>(), nullptr);
    }

    // remember: show what was actually saved.
    if (result.contains("memory") && result["memory"].is_object()) {
        const auto& mem = result["memory"];
        if (const std::string cat = mem.value("category", ""); !cat.empty()) {
            content.emplace_back("Category: " + cat);
        }
        if (const std::string text = flatten(mem.value("content", ""));
            !text.empty()) {
            content.emplace_back(icarus::ui::truncate(text, 120, "…"));
        }
    }

    std::ostringstream joined;
    for (std::size_t i = 0; i < content.size(); ++i) {
        if (i) joined << '\n';
        joined << content[i];
    }
    const std::string text = joined.str();

    std::cerr << "\n";
    const int live_width = icarus::ui::effective_terminal_width(cfg_);
    if (live_width >= 40) {
        auto lines = icarus::ui::wrap_lines(text, live_width - 4);
        auto box = icarus::ui::make_box(lines, std::nullopt, cfg_, icarus::ui::Alignment::Auto);
        for (const auto& line : box.lines) {
            std::cerr << style_.dim(line) << "\n";
        }
    } else {
        std::cerr << text;
    }
    std::cerr << std::flush;
}

void ConsoleAgentSink::start_status_spinner() {
    if (!cfg_.hud_enabled || shutting_down_.load(std::memory_order_acquire)) return;
    // If the last output to std::cout ended mid-line, commit it with a
    // newline before the spinner starts drawing. The spinner's frames
    // ('\r' + glyph + erase-to-EOL) draw on the current line and would
    // otherwise overwrite the partial streamed text — and since that text
    // is already out of delta_buffer_, it would never reprint. Lock order
    // note: callers hold spinner_lifecycle_mutex_, so delta_mutex_ is
    // always taken after it here (same order as the flush paths).
    {
        std::lock_guard<std::mutex> lk(delta_mutex_);
        if (stream_mid_line_) {
            std::cout << '\n' << std::flush;
            stream_mid_line_ = false;
        }
    }
    // §ToolSpinner: while a tool is executing the label shows the tool and
    // its elapsed time (refreshed at ~1 Hz by the watchdog) instead of the
    // agent state.
    if (tool_active_.load(std::memory_order_acquire)) {
        spinner_.set_message(tool_spinner_label());
    } else {
        // Label comes from the last on_state_change `to` value — NOT from
        // Agent::state(), which only ever reports Understanding/terminal
        // states (the LLM drives intermediate states via the set_status
        // tool, and those reach us only through the callback).
        hud_.set_state(std::string(daedalus::to_string(
            static_cast<AgentState>(current_state_.load(std::memory_order_acquire)))));
        hud_.apply_to_spinner(spinner_);
    }
    spinner_.start();
}

std::string ConsoleAgentSink::tool_spinner_label() const {
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - tool_start_time_).count();
    const std::string plain = "Running " + active_tool_name_ + "... (" +
                              std::to_string(secs) + "s)";
    return cfg_.color_enabled ? style_.primary(plain) : plain;
}

void ConsoleAgentSink::begin_tool_spinner(std::string_view name) {
    if (!cfg_.hud_enabled || shutting_down_.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(spinner_lifecycle_mutex_);
    active_tool_name_ = std::string(name);
    tool_start_time_ = std::chrono::steady_clock::now();
    last_tool_label_update_ = {};
    tool_active_.store(true, std::memory_order_release);
    start_status_spinner();
}

void ConsoleAgentSink::end_tool_spinner() noexcept {
    tool_active_.store(false, std::memory_order_release);
}

void ConsoleAgentSink::start_agent_turn() {
    if (!cfg_.hud_enabled) return;
    turn_active_.store(true, std::memory_order_release);
    last_output_time_.store(std::chrono::steady_clock::now(), std::memory_order_release);

    {
        std::lock_guard<std::mutex> lk(spinner_lifecycle_mutex_);
        start_status_spinner();
    }

    if (!spinner_watchdog_running_.exchange(true, std::memory_order_acq_rel)) {
        spinner_watchdog_thread_ = std::thread([this] { spinner_watchdog_loop(); });
    } else {
        std::lock_guard<std::mutex> lk(spinner_watchdog_mutex_);
        spinner_watchdog_cv_.notify_all();
    }
}

void ConsoleAgentSink::on_agent_output() {
    {
        std::lock_guard<std::mutex> lk(spinner_lifecycle_mutex_);
        spinner_.stop();
    }
    last_output_time_.store(std::chrono::steady_clock::now(), std::memory_order_release);
}

void ConsoleAgentSink::stop_agent_turn() {
    turn_active_.store(false, std::memory_order_release);
    tool_active_.store(false, std::memory_order_release);  // §ToolSpinner
    {
        std::lock_guard<std::mutex> lk(spinner_watchdog_mutex_);
        spinner_watchdog_cv_.notify_all();
    }
    {
        std::lock_guard<std::mutex> lk(spinner_lifecycle_mutex_);
        spinner_.stop();
    }
    // Commit any buffered assistant text. Some models emit set_status(Done)
    // *before* streaming their reply; in that case the state-change flush
    // fires on an empty buffer and no further state change follows, so
    // without this the reply text would be silently dropped.
    // Called after spinner_lifecycle_mutex_ is released to preserve the
    // spinner_lifecycle → delta lock ordering.
    flush_delta_buffer();
    // The reply rarely ends with '\n'; commit the line so the next output
    // (input header, farewell, next card) starts on a fresh line.
    commit_mid_line();
}

void ConsoleAgentSink::spinner_watchdog_loop() {
    std::unique_lock<std::mutex> lk(spinner_watchdog_mutex_);
    while (spinner_watchdog_running_.load(std::memory_order_acquire)) {
        spinner_watchdog_cv_.wait_for(lk, std::chrono::milliseconds(100), [this] {
            return !spinner_watchdog_running_.load(std::memory_order_acquire)
                || !turn_active_.load(std::memory_order_acquire);
        });

        if (!spinner_watchdog_running_.load(std::memory_order_acquire)) break;
        if (!turn_active_.load(std::memory_order_acquire)) {
            // Turn finished while we were asleep: make sure the spinner is off
            // but do not hold the watchdog mutex while touching the spinner.
            lk.unlock();
            {
                std::lock_guard<std::mutex> spin_lk(spinner_lifecycle_mutex_);
                spinner_.stop();
            }
            lk.lock();
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto last = last_output_time_.load(std::memory_order_acquire);
        // turn_active_ is already gated by the thinking-state callbacks, so we
        // do not need to re-query agent_->state() here. Avoiding that query
        // removes a race with agent destruction during shutdown.
        if (now - last >= kSpinnerIdleRestart) {
            lk.unlock();
            {
                std::lock_guard<std::mutex> spin_lk(spinner_lifecycle_mutex_);
                start_status_spinner();
            }
            lk.lock();
        }

        // §ToolSpinner: keep the elapsed-seconds label fresh (~1 Hz) while
        // a tool runs so the user can see the command is alive, not hung.
        if (tool_active_.load(std::memory_order_acquire)) {
            lk.unlock();
            {
                std::lock_guard<std::mutex> spin_lk(spinner_lifecycle_mutex_);
                if (spinner_.is_running()
                    && now - last_tool_label_update_ >= std::chrono::seconds(1)) {
                    last_tool_label_update_ = now;
                    spinner_.set_message(tool_spinner_label());
                }
            }
            lk.lock();
        }
    }
}

void ConsoleAgentSink::save_terminal_for_subprocess() {
    // §ToolSpinner (2026-08-07): this used to call stop_agent_turn() to
    // "pause the spinner watchdog while the subprocess owns the terminal" —
    // which killed the spinner for the ENTIRE duration of every
    // execute_bash run and made long commands look hung. That premise is
    // false: daedalus's Subprocess pipes the child's stdout/stderr, so the
    // child never paints the TTY directly; its output returns as sanitised
    // output_chunk events printed by this sink (and printing already stops
    // the spinner via on_agent_output()). The termios save/restore below
    // stays: the child inherits stdin, so a misbehaving child could still
    // tcsetattr() our terminal behind our back.
    if (::tcgetattr(STDIN_FILENO, &saved_tio_for_subprocess_) == 0) {
        subprocess_termios_saved_ = true;
    }

}

void ConsoleAgentSink::restore_terminal_after_subprocess() {
    // Restore the termios we had before the subprocess took over the TTY.
    if (subprocess_termios_saved_) {
        ::tcsetattr(STDIN_FILENO, TCSANOW, &saved_tio_for_subprocess_);
        subprocess_termios_saved_ = false;
    }

    // Resume the spinner watchdog if the agent is still working after the
    // subprocess finishes (the next state change / LLM wait will also start
    // it, but this avoids a gap if those events are slow to arrive).
    if (THINKING_STATES.contains(
            static_cast<AgentState>(current_state_.load(std::memory_order_acquire)))) {
        start_agent_turn();
    }
}

void ConsoleAgentSink::on_llm_wait_started() noexcept {
    start_agent_turn();
}

void ConsoleAgentSink::on_llm_response_started() noexcept {
    // The LLM is responding. Treat this as output for spinner purposes:
    // stop animating and reset the idle timer. If actual bytes are slow to
    // arrive, the watchdog will restart the spinner after the idle threshold.
    on_agent_output();
}

void ConsoleAgentSink::on_llm_request_finished() noexcept {
    // The request/response cycle ended. If the agent is still in a thinking
    // state the turn stays active and the watchdog will keep restarting the
    // spinner during quiet gaps; otherwise finish the turn.
    if (!THINKING_STATES.contains(
            static_cast<AgentState>(current_state_.load(std::memory_order_acquire)))) {
        stop_agent_turn();
    }
    // §fix (2026-07-31, mirrors daedalus f89ea41): flush the delta buffer at
    // end of the LLM response. Short newline-less replies never hit the
    // flush threshold in on_assistant_delta() and stayed invisible whenever
    // the turn stayed in a thinking state (tool calls follow). No-op when
    // stop_agent_turn() already flushed. Mid-line text is safe here: a
    // spinner restart commits the partial line first (stream_mid_line_
    // guard in start_status_spinner()).
    flush_delta_buffer();
}

void ConsoleAgentSink::on_state_change(int from, int to) noexcept {
    (void)from;
    current_state_.store(to, std::memory_order_release);
    const AgentState s = static_cast<AgentState>(to);

    if (THINKING_STATES.contains(s)) {
        start_agent_turn();
    } else {
        // stop_agent_turn() also flushes buffered assistant text and
        // commits the trailing newline.
        stop_agent_turn();
        // Turn over: reset so the next turn opens with the "Thinking..."
        // placeholder instead of this turn's terminal label.
        current_state_.store(static_cast<int>(AgentState::Idle),
                             std::memory_order_release);
        // §prompt.txt: the per-turn separator is no longer drawn
        // here — the input header rendered by InputReader::print_header()
        // (hint line + horizontal rule) provides the same "end of
        // turn → ready for input" visual frame, so drawing another
        // rule on stderr here would just produce two stacked rules.
    }
}

void ConsoleAgentSink::on_compaction(std::size_t before_tokens,
                                     std::size_t after_tokens) noexcept {
    on_agent_output();

    std::ostringstream oss;
    oss << "Context compacted: " << before_tokens << " → " << after_tokens << " tokens";
    std::vector<std::string> content{oss.str()};
    auto box = icarus::ui::make_box(content, "Compact", cfg_, icarus::ui::Alignment::Auto);
    for (const auto& line : box.lines) {
        std::cerr << style_.muted(line) << "\n";
    }
    std::cerr << std::flush;
}

void ConsoleAgentSink::on_log(daedalus::LogLevel level,
                              std::string_view message) noexcept {
    // Suppress the daedalus library's own startup banner; icarus prints
    // its own detailed banner via src/banner.h.
    if (message.find("Autonomous Coding Agent") != std::string_view::npos) {
        return;
    }

    std::string level_str(daedalus::to_string(level));
    std::string styled_level;
    switch (level) {
        case daedalus::LogLevel::Error:
            styled_level = style_.error(level_str);
            break;
        case daedalus::LogLevel::Warn:
            styled_level = style_.warning(level_str);
            break;
        case daedalus::LogLevel::Info:
            styled_level = style_.info(level_str);
            break;
        default:
            styled_level = style_.muted(level_str);
            break;
    }

    write_log_line(level_str, message);

    // Info-level chatter goes to the log file only; the terminal shows
    // warnings and errors (anything the user may need to act on).
    if (level == daedalus::LogLevel::Info) {
        return;
    }

    on_agent_output();

    std::cerr << "[" << styled_level << "] " << style_.muted(std::string(message)) << "\n";
    std::cerr << std::flush;
}

void ConsoleAgentSink::write_log_line(std::string_view level,
                                      std::string_view message) {
    if (log_path_.empty()) {
        return;
    }

    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &time_t);
#else
    localtime_r(&time_t, &tm_buf);
#endif

    char time_buf[32];
    std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_buf);

    std::ofstream out(log_path_, std::ios::app);
    if (out) {
        out << '[' << time_buf << '.' << std::setfill('0') << std::setw(3)
            << ms.count() << "] "
            << '[' << level << "] "
            << message << "\n";
    }
}

void ConsoleAgentSink::on_error(daedalus::ErrorKind kind,
                                std::string_view message,
                                bool will_retry) noexcept {
    const std::string kind_str = daedalus::to_string(kind);

    // Log-file trace mirrors what the library's default on_error → on_log
    // forwarding produced, with the kind and retry flag added.
    write_log_line("Error", "[" + kind_str + "] " + std::string(message)
                   + (will_retry ? " (will_retry=true)" : " (will_retry=false)"));

    on_agent_output();

    if (will_retry) {
        // Transient failure — the library is backing off and retrying.
        std::cerr << "[" << style_.warning(kind_str) << "] "
                  << style_.muted(std::string(message)) << "\n";
    } else {
        // Terminal failure — the turn will still end with AgentState::Done,
        // so make the failure unmissable.
        std::vector<std::string> content{std::string(message)};
        auto box = icarus::ui::make_box(content, style_.error("Error: " + kind_str), cfg_);
        for (const auto& line : box.lines) {
            std::cerr << line << "\n";
        }
    }
    std::cerr << std::flush;
}

void ConsoleAgentSink::on_bell() noexcept {
    std::cout << '\a' << std::flush;
}

bool ConsoleAgentSink::request_command_approval(std::string_view command) {
    // Session-scoped answers from a previous prompt: 'a' auto-approves,
    // everything else still prompts per command.
    if (approval_always_.load(std::memory_order_acquire)) {
        return true;
    }

    on_agent_output();

    std::vector<std::string> content{
        "Execute command:",
        std::string(command),
        "",
        "Approve? (y)es / (n)o / (a)lways this session"
    };
    auto box = icarus::ui::make_box(content, style_.warning("Command Approval"), cfg_);
    for (const auto& line : box.lines) {
        std::cout << line << "\n";
    }
    std::cout << "  " << std::flush;

    std::string line;
    if (approval_reader_) {
        // Bell: the agent is blocked waiting on a decision — an away
        // user needs to know their attention is required. TTY only.
        if (::isatty(STDOUT_FILENO)) {
            std::cout << '\a' << std::flush;
        }
        // §Task 6 (CodeMax.md §16.5.3): read through the injected
        // linenoise-backed reader. The "  " indent is passed as the
        // prompt so the user sees the same visual cue as the cooked
        // fallback below.
        auto maybe = approval_reader_("  ");
        if (!maybe) {
            // EOF / Ctrl+D on empty prompt → treat as "no".
            return false;
        }
        line = std::move(*maybe);
    } else {
        // Fallback for sinks created without a reader (tests, A2A mode).
        std::getline(std::cin, line);
    }
    if (line.empty()) return false;
    const char c = line[0];
    if (c == 'a' || c == 'A') {
        // §BUG-008 (daedalus 0.8.7): drive the library's authoritative
        // runtime auto-approve when the host wired it up — subsequent
        // commands then bypass this prompt entirely and surface via
        // on_command_auto_approved(). Fallback: the sink-local flag.
        if (auto_approve_setter_) {
            auto_approve_setter_(true);
        } else {
            approval_always_.store(true, std::memory_order_release);
        }
        return true;
    }
    return c == 'y' || c == 'Y';
}

void ConsoleAgentSink::on_command_auto_approved(
    std::string_view command, daedalus::ApprovalSource source) noexcept {
    try {
        on_agent_output();  // stop spinner / commit any mid-line stream
        const char* why = "auto-mode";
        switch (source) {
            case daedalus::ApprovalSource::AutoApprove:    why = "auto-mode";       break;
            case daedalus::ApprovalSource::SessionCache:   why = "session cache";   break;
            case daedalus::ApprovalSource::PersistedCache: why = "persisted cache"; break;
        }
        std::cout << style_.muted("Auto-approved (") << why
                  << style_.muted("): ") << command << "\n" << std::flush;
    } catch (...) {
        // noexcept contract — never let rendering kill the turn.
    }
}

} // namespace daedalus
