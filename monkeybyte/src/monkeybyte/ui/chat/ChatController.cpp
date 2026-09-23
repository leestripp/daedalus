#include "monkeybyte/ui/chat/ChatController.h"

#include "monkeybyte/AppServices.h"
#include "monkeybyte/backend/Logger.h"
#include "monkeybyte/backend/ModelBridge.h"
#include "monkeybyte/ui/chat/AppUiTool.h"
#include "monkeybyte/ui/chat/WebFetchTool.h"
#include "monkeybyte/ui/chat/WebSearchTool.h"
#include "monkeybyte/ui/UiGuard.h"

#include <daedalus/default_paths.h>
#include <daedalus/paths.h>
#include <daedalus/settings.h>
#include <daedalus/types/ErrorKind.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <unordered_map>

namespace monkeybyte {

using nlohmann::json;

namespace {

// Blocks the agent thread until the UI answers an approval prompt.
struct ApprovalGate {
    std::mutex mutex;
    std::condition_variable cv;
    bool answered = false;
    bool approved = false;
    bool cancelled = false;

    void respond(bool ok) {
        {
            std::lock_guard lock(mutex);
            answered = true;
            approved = ok;
        }
        cv.notify_all();
    }
    void cancel() {
        {
            std::lock_guard lock(mutex);
            cancelled = true;
        }
        cv.notify_all();
    }
    bool wait() {
        std::unique_lock lock(mutex);
        cv.wait(lock, [this] { return answered || cancelled; });
        return answered && approved && !cancelled;
    }
};

} // namespace

// daedalus::AgentSink → ChatController signals (design §10.1 vocabulary).
class PanelSink : public daedalus::AgentSink {
public:
    explicit PanelSink(ChatController& controller) : controller_(controller) {}

    void on_assistant_delta(std::string_view text) noexcept override {
        controller_.text_delta.emit(std::string(text));
    }

    void on_tool_call(std::string_view name, std::string_view call_id,
                      const json& args) noexcept override {
        // noexcept sink: any escape is std::terminate — catch and log.
        ui_guard("on_tool_call", [&] {
        ToolInfo t;
        t.id = call_id.empty() ? std::string(name) + "#" + std::to_string(next_seq_++)
                               : std::string(call_id);
        t.name = name;
        t.args = args.dump(2);
        t.status = "running";
        started_[t.id] = std::chrono::steady_clock::now();
        controller_.tool_started.emit(std::move(t));
        });
    }

    void on_tool_progress(std::string_view /*name*/, std::string_view call_id,
                          const json& progress) noexcept override {
        ui_guard("on_tool_progress", [&] {
        ToolInfo t;
        t.id = call_id;
        std::string status = progress.value("stage", "");
        if (const auto msg = progress.value("message", ""); !msg.empty())
            status += (status.empty() ? "" : ": ") + msg;
        if (progress.contains("percent"))
            status += " (" + std::to_string(progress["percent"].get<int>()) + "%)";
        t.status = status.empty() ? "running" : status;

        // §7.3 — live output streaming: execute_bash forwards sanitised
        // stdout/stderr as {"stage":"output","output_chunk":"..."} events.
        // Keep a bounded tail per call so the card can show the command's
        // recent output while it runs.
        if (progress.value("stage", "") == "output") {
            if (const auto chunk = progress.value("output_chunk", ""); !chunk.empty()) {
                auto& acc = live_[t.id];
                acc += chunk;
                constexpr std::size_t kLiveCap = 8 * 1024;
                if (acc.size() > kLiveCap) {
                    acc.erase(0, acc.size() - kLiveCap);
                    // don't split a UTF-8 sequence at the cut point
                    while (!acc.empty() &&
                           (static_cast<unsigned char>(acc.front()) & 0xC0) == 0x80)
                        acc.erase(0, 1);
                }
                t.live_output = acc;
            }
        }
        controller_.tool_updated.emit(std::move(t));
        });
    }

    void on_tool_result(std::string_view name, std::string_view call_id,
                        const json& result) noexcept override {
        ui_guard("on_tool_result", [&] {
        ToolInfo t;
        t.id = call_id;
        t.name = name;
        t.done = true;
        t.success = !result.contains("error") && result.value("success", true);
        std::string body = result.dump(2);
        if (body.size() > 2000) { // §7.3
            body.resize(2000);
            body += " (truncated)";
        }
        t.result = body;

        // duration: done / done (450ms) / done (1.3s) (§7.3)
        long ms = -1;
        if (auto it = started_.find(t.id); it != started_.end()) {
            ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - it->second)
                     .count();
            started_.erase(it);
        }
        live_.erase(t.id); // drop the accumulated live-output tail
        // The daedalus stop tool carries a user-facing summary of the turn
        // (§7.3). Stash it — the turn epilogue emits it after the final
        // tool event so its bubble lands below every tool card.
        if (name == "stop") {
            if (const auto summary = result.value("summary", ""); !summary.empty())
                stop_summary_ = summary;
        }
        // §7.3 — tell_user carries a user-facing message: surface it as
        // its own assistant bubble instead of a raw-JSON tool card (the
        // panel skips the card for this tool; see ChatPanel).
        if (name == "tell_user") {
            if (const auto content = result.value("content", ""); !content.empty())
                controller_.tell_user_message.emit(content);
        }
        if (ms < 0) t.status = "done";
        else if (ms < 1000) t.status = "done (" + std::to_string(ms) + "ms)";
        else {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "done (%.1fs)", ms / 1000.0);
            t.status = buf;
        }
        // A denied approval ends the turn with no assistant reply (daedalus
        // interrupts the loop) — make the outcome visible on the card
        // instead of a misleading "done".
        if (result.value("status", "") == "denied") {
            t.status = "denied";
            t.success = false;
        }
        controller_.tool_updated.emit(std::move(t));
        controller_.emit_stats(); // §8 — tokens/context grew with this LLM round
        });
    }

    // §REQ-5 (daedalus 2b25f27) — typed overload; no int casting.
    void on_state_change(daedalus::AgentState /*from*/,
                         daedalus::AgentState to) noexcept override {
        // daedalus state machine is internal; ChatState is driven by the
        // turn thread lifecycle instead (§7.6). State transitions mark new
        // iterations — a good moment to refresh the status-bar stats (§8).
        // Compressing is surfaced though: the compression stretch goes
        // quiet while daedalus summarizes, and the activity strip should
        // say why (auto 90% loop or manual /compress).
        if (to == daedalus::AgentState::Compressing)
            controller_.compacting.emit();
        controller_.emit_stats();
    }

    // §7.3/§7.8 — daedalus rebuilt the history during context compression
    // (90% auto loop or host-triggered Agent::compress_context();
    // before/after token counts). Surface it in the chat and refresh the
    // status-bar context figure, which just dropped.
    void on_compaction(std::size_t before_tokens,
                       std::size_t after_tokens) noexcept override {
        controller_.compacted.emit(before_tokens, after_tokens);
        controller_.emit_stats();
    }

    void on_error(daedalus::ErrorKind kind, std::string_view message,
                  bool will_retry) noexcept override {
        // §7.2 — structured error channel. Terminal failures (will_retry
        // == false) are shown in the chat IMMEDIATELY: daedalus feeds a
        // fatal 4xx back into the conversation and loops, ending the turn
        // as Done only after its retry budget is spent — far too late for
        // feedback. Transient errors (will_retry) may still recover; they
        // only feed the detail capture. Everything is mirrored to the
        // error log for after-the-fact diagnosis, tagged with the
        // machine-readable ErrorKind.
        const std::string kind_name = daedalus::to_string(kind);
        Logger::instance().error("[" + kind_name + "] " +
                                 (will_retry ? "API error (transient): "
                                             : "API error (terminal): ") +
                                 std::string(message));
        std::string line = "[" + kind_name + "] " + std::string(message);
        if (line.size() > 300) {
            line.resize(300);
            line += "…";
        }
        {
            std::lock_guard lock(errors_mutex_);
            if (recent_errors_.empty() || recent_errors_.back() != line)
                recent_errors_.push_back(line);
            if (recent_errors_.size() > 3) recent_errors_.erase(recent_errors_.begin());
            if (will_retry || line == terminal_error_)
                return; // transient, or the retry loop repeating the same
                        // terminal error — already shown
            terminal_error_ = line;
        }
        // Actionable hints for the failure classes the user can fix (§7.2).
        if (kind == daedalus::ErrorKind::Auth)
            line += "\nCheck the API key in Settings → Providers.";
        else if (kind == daedalus::ErrorKind::RateLimited)
            line += "\nProvider rate limit — retry shortly or switch model.";
        // New terminal error: surface it live (ChatPanel marshals the
        // signal onto the GTK main loop).
        controller_.error_message.emit(std::move(line));
    }

    void on_log(daedalus::LogLevel level, std::string_view message) noexcept override {
        // §7.2 — capture Error/Fatal lines so the turn-end error banner can
        // show the real cause (e.g. HTTP 404 body) instead of a generic
        // "agent reported an error". Log lines stay out of the live UI.
        if (level != daedalus::LogLevel::Error && level != daedalus::LogLevel::Fatal)
            return;
        Logger::instance().error(message); // untruncated mirror (§7.2)
        std::string line(message);
        if (line.size() > 300) {
            line.resize(300);
            line += "…";
        }
        std::lock_guard lock(errors_mutex_);
        // A failing API call is retried in a loop and logs the same line
        // every time — keep consecutive duplicates out of the banner.
        if (!recent_errors_.empty() && recent_errors_.back() == line) return;
        recent_errors_.push_back(std::move(line));
        if (recent_errors_.size() > 3) recent_errors_.erase(recent_errors_.begin());
    }

    void clear_errors() {
        std::lock_guard lock(errors_mutex_);
        recent_errors_.clear();
        terminal_error_.clear();
    }

    // §7.3 — stop-tool summary captured for the turn epilogue ("" when the
    // turn ended without one). Emitted after the final tool event so its
    // chat bubble lands below every tool card.
    std::string take_stop_summary() {
        std::string s = std::move(stop_summary_);
        stop_summary_.clear();
        return s;
    }

    // Newline-joined recent Error/Fatal log lines ("" when none).
    std::string recent_errors_text() const {
        std::lock_guard lock(errors_mutex_);
        std::string out;
        for (const auto& e : recent_errors_) {
            if (!out.empty()) out += '\n';
            out += e;
        }
        return out;
    }

    // Last terminal on_error (will_retry=false) message this turn
    // ("" when none) — the authoritative "turn failed" signal (§7.2).
    std::string terminal_error_text() const {
        std::lock_guard lock(errors_mutex_);
        return terminal_error_;
    }

    bool request_command_approval(std::string_view command) override {
        auto gate = std::make_shared<ApprovalGate>();
        {
            std::lock_guard lock(gates_mutex_);
            gates_.push_back(gate);
        }
        // Ask the UI (main loop) to approve; respond() unblocks us.
        if (controller_.approval_handler_) {
            controller_.approval_handler_(std::string(command),
                                          [gate](bool ok) { gate->respond(ok); });
        } else {
            gate->respond(false); // no handler → deny
        }
        const bool ok = gate->wait();
        std::lock_guard lock(gates_mutex_);
        std::erase(gates_, gate);
        return ok;
    }

    void cancel_all_approvals() {
        std::lock_guard lock(gates_mutex_);
        for (auto& g : gates_) g->cancel();
    }

    // §BUG-008 — a command ran WITHOUT prompting (auto-approve mode or one
    // of daedalus's approval caches). Informational only; mirror it to the
    // error log so there is an audit trail of unprompted executions.
    void on_command_auto_approved(
        std::string_view command,
        daedalus::ApprovalSource source) noexcept override {
        const char* why = source == daedalus::ApprovalSource::AutoApprove
            ? "auto-approve"
            : source == daedalus::ApprovalSource::SessionCache
                ? "session-approved cache"
                : "persisted approvals cache";
        Logger::instance().error("Command ran without approval prompt (" +
                                 std::string(why) + "): " +
                                 std::string(command));
    }

private:
    ChatController& controller_;
    int next_seq_ = 0;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> started_;
    std::unordered_map<std::string, std::string> live_; // per-call live output tails
    std::mutex gates_mutex_;
    std::vector<std::shared_ptr<ApprovalGate>> gates_;
    mutable std::mutex errors_mutex_;
    std::vector<std::string> recent_errors_;
    std::string terminal_error_;
    std::string stop_summary_; // §7.3 — pending stop-tool summary
};

// ── ChatController ──────────────────────────────────────────────────

ChatController::ChatController(AppServices& services)
    : services_(services), sink_(std::make_shared<PanelSink>(*this)) {}

ChatController::~ChatController() {
    cancel();
    if (turn_thread_.joinable()) turn_thread_.join();
}

ChatState ChatController::state() const {
    std::lock_guard lock(state_mutex_);
    return state_;
}

void ChatController::set_state(ChatState s) {
    {
        std::lock_guard lock(state_mutex_);
        state_ = s;
    }
    state_changed.emit(s);
}

void ChatController::set_approval_handler(ApprovalHandler handler) {
    approval_handler_ = std::move(handler);
}

void ChatController::set_context_provider(std::function<std::string()> provider) {
    context_provider_ = std::move(provider);
}

void ChatController::set_thinking_level(daedalus::ThinkingLevel level) {
    thinking_level_ = level;
    // §ThinkingLevel (daedalus f90a738) — runtime switch on the live
    // session: the provider plugin maps the level into each outgoing
    // payload, so it applies from the next request with no rebuild
    // (set_auto_approve pattern).
    if (agent_ && level != active_thinking_level_) {
        ui_guard("set_thinking_level",
                 [&] { agent_->set_thinking_level(level); });
        active_thinking_level_ = level;
    }
}

void ChatController::ensure_agent(const std::string& model_display) {
    const std::string workspace = services_.files.workspace().empty()
        ? "."
        : services_.files.workspace().string();
    const bool auto_approve =
        services_.store.get("agent.auto_approve").value_or("0") == "1";
    if (agent_ && model_display == active_model_display_ &&
        workspace == active_workspace_ &&
        thinking_level_ == active_thinking_level_) {
        if (auto_approve != active_auto_approve_) {
            // Settings → Interface toggle flipped: apply at runtime instead
            // of rebuilding (a rebuild would drop the live session).
            agent_->set_auto_approve(auto_approve);
            active_auto_approve_ = auto_approve;
        }
        return;
    }

    // Find the selected model in the daedalus provider/model registry
    // (display = "provider - model_id", §7.1).
    auto& registry = daedalus::SettingsStore::instance();
    const auto models = model_bridge::list_models(registry, services_.store);
    const auto it = std::find_if(models.begin(), models.end(), [&](const UiModel& m) {
        return model_bridge::display_name(m) == model_display;
    });
    if (it == models.end())
        throw std::runtime_error("Model not configured: " + model_display);
    const UiModel& selected = *it;
    // Effective settings: model merged with its provider's host/key/endpoint.
    const auto resolved = registry.resolve_model(selected.name);
    if (!resolved)
        throw std::runtime_error("Provider not configured for model: " + selected.name);

    if (agent_) {
        agent_->shutdown();
        agent_.reset();
    }
    sink_->clear_errors();

    daedalus::AgentConfig cfg;
    cfg.project_root = workspace;
    // Stable per-host session id — doubles as the Moonshot prompt_cache_key
    // fallback, keeping server-side prompt-cache affinity across restarts
    // (daedalus §ContextCache).
    cfg.session_id = "gtk-chat";
    cfg.model_name = selected.name; // resolved against the daedalus registry
    cfg.api_host = resolved->host;
    cfg.chat_endpoint = resolved->chat_endpoint;
    cfg.api_key = model_bridge::resolve_env_key(resolved->api_key);
    cfg.max_context_size = resolved->max_context_size;
    cfg.temperature = resolved->temperature;
    // std::optional — a set value is authoritative (daedalus §BUG-006): its
    // own SettingsStore auto_approve row is ignored, so OFF here always
    // means the approval dialog (Settings → Interface).
    cfg.auto_approve = auto_approve;
    // §ThinkingLevel — host-selected thinking level (chat header dropdown;
    // Default = send nothing, unsupported models ignore it).
    cfg.thinking_level = thinking_level_;
    // Provider-facing client identity (HTTP User-Agent on LLM requests).
    cfg.user_agent = "daedalus-monkeybyte/" MONKEYBYTE_VERSION;
    cfg.stream = true;
    // §REQ-1 (daedalus 2b25f27): public kill switch for the messages flush
    // worker. Pin ON explicitly — session persistence must not silently
    // disappear if the upstream default ever flips.
    cfg.session_auto_flush = true;

    // Host-policy enable/disable (Settings → Agent, §4.5). daedalus does
    // not persist these sets; monkeybyte seeds them on every agent build.
    {
        const auto off_skills = services_.store.load_disabled_skills();
        cfg.skills_disabled.insert(off_skills.begin(), off_skills.end());
        const auto off_tools = services_.store.load_disabled_tools();
        cfg.tools_disabled.insert(off_tools.begin(), off_tools.end());
        // MCP: the store's enabled flag is the UI state, but the manager
        // only skips names in the runtime disabled set — pass both.
        for (const auto& s : registry.list_mcp_servers())
            if (!s.enabled) cfg.mcp_servers_disabled.insert(s.name);
        cfg.mcp_enabled =
            services_.store.get("agent.mcp_enabled").value_or("1") == "1";
    }

    // Agent tuning limits (Settings → Agent → Limits) — daedalus kv.
    cfg.max_agent_iterations = registry.get_max_agent_iterations();
    cfg.max_tool_result_size = registry.get_max_tool_result_size();
    cfg.max_request_size = registry.get_max_request_size();
    cfg.max_response_size = registry.get_max_response_size();

    // Opt in to the user's global daedalus skills (daedalus loads none
    // unless the host passes skills_dirs). Each immediate subdirectory
    // is one skill; the agent discovers/activates them at runtime.
    cfg.skills_dirs = { daedalus::Paths::global().global_skills_dir() };

    // per-model system prompt binding (§4.5). An explicit binding always
    // wins; the workspace's project type (Plan.md) only applies when the
    // model has none — the type's prompt then becomes the host prompt.
    const long prompt_id = services_.store.get_model_system_prompt(selected.name);
    if (prompt_id > 0) {
        const auto prompts = services_.store.list_system_prompts();
        const auto p = std::find_if(prompts.begin(), prompts.end(), [&](const SystemPrompt& sp) {
            return sp.id == prompt_id;
        });
        if (p != prompts.end()) cfg.system_prompt = p->prompt;
    } else if (const auto* type = services_.project_types.detect(workspace)) {
        cfg.system_prompt = type->system_prompt;
    }

    agent_ = std::make_unique<daedalus::Agent>(std::move(cfg), sink_);
    agent_->initialize();
    // daedalus's default remember_approvals=true silently records every
    // host-approved command into its session cache, making plain "Approve"
    // sticky for exact repeats. Disable it so the dialog's Approve means
    // "this once" and only "Always" skips future prompts (tracked by
    // ChatPanel). Process-wide switch (daedalus Agent.h note).
    agent_->set_remember_approvals(false);
    // Let the agent drive the app UI (sizers, panels, geometry, open files).
    // daedalus's ToolFactory is PROCESS-GLOBAL; before daedalus 0.9.7
    // register_tool() appended without dedup, so registering on every
    // agent rebuild put two app_ui_control definitions in the payload and
    // the provider rejected it with HTTP 400 "function name app_ui_control
    // is duplicated". 0.9.7 made registration replace-by-name, but we
    // still register once per process as cheap insurance; the factory
    // entry outlives any single Agent (HostToolAdapter holds shared_ptrs
    // to tool + context).
    static bool host_tools_registered = false;
    if (!host_tools_registered) {
        agent_->register_tool(std::make_unique<AppUiTool>());
        // Tab-less research tools (design §6): web_fetch + web_search run
        // on the agent's tool thread, no GTK involved.
        agent_->register_tool(std::make_unique<WebFetchTool>());
        agent_->register_tool(std::make_unique<WebSearchTool>());
        host_tools_registered = true;
    }
    active_model_display_ = model_display;
    active_workspace_ = workspace;
    active_auto_approve_ = auto_approve;
    active_thinking_level_ = thinking_level_;
}

void ChatController::send_message(const std::string& text,
                                  const std::string& model_display) {
    if (text.empty()) return; // §12.3
    if (model_display.empty()) {
        error_message.emit("No model selected. Add one in Settings → Models.");
        set_state(ChatState::Idle);
        return;
    }

    // Attach contextual info (e.g. editor selection) for the agent only;
    // the chat bubble still shows just what the user typed. Evaluated on
    // the GTK main thread, before anything is queued.
    std::string full = text;
    if (context_provider_) {
        std::string ctx;
        ui_guard("context_provider", [&] { ctx = context_provider_(); });
        if (!ctx.empty()) full = ctx + "\n\n" + text;
    }

    // Every message lands in the host queue first. When a turn is already
    // running the message stays queued and the turn thread's drain loop
    // picks it up when the agent exits its current loop — daedalus's own
    // submit_user_message queue is not safe to touch cross-thread, so the
    // turn thread is the ONLY thread that ever talks to it. The push and
    // the state check/transition share state_mutex_ with the drain loop's
    // final empty-check, so a message can never strand between "queue
    // empty" and "gone Idle".
    bool mid_turn = false;
    {
        std::lock_guard lock(state_mutex_);
        mid_turn = state_ == ChatState::Processing ||
                   state_ == ChatState::Cancelling;
        queued_messages_.push_back(std::move(full));
        if (!mid_turn) state_ = ChatState::Processing;
    }
    user_message_sent.emit(text); // echo the bubble immediately
    if (mid_turn) return;
    state_changed.emit(ChatState::Processing);

    if (turn_thread_.joinable()) turn_thread_.join();
    try {
    turn_thread_ = std::jthread([this, model_display](std::stop_token) {
        std::string reason = "done";
        bool transitioned = false; // loop sets the resting state atomically
        try {
            // Per-turn capture — errors from a previous turn must not
            // leak into this turn's banner (ensure_agent only clears
            // when it rebuilds the agent).
            sink_->clear_errors();
            ensure_agent(model_display);
            // Drain loop: each queued message gets exactly one daedalus
            // submit + run_one_turn (daedalus warns any other ratio
            // leaves its queue off-by-one). Stop only interrupts the
            // CURRENT turn — queued tasks still run. An error ends the
            // drain instead of hammering a failing backend; the remaining
            // messages stay queued and go FIRST on the next send,
            // preserving FIFO.
            for (;;) {
                std::string next;
                {
                    std::lock_guard lock(state_mutex_);
                    if (reason != "error" && !queued_messages_.empty()) {
                        next = std::move(queued_messages_.front());
                        queued_messages_.pop_front();
                    } else {
                        // Final queue check and the resting-state
                        // transition are one atomic step: a main-thread
                        // send either landed in the queue before this
                        // (drained above) or sees Idle/Error after it
                        // (and spawns a fresh thread). Nothing strands.
                        state_ = reason == "error" ? ChatState::Error
                                                   : ChatState::Idle;
                        transitioned = true;
                    }
                }
                if (transitioned) break;
                agent_->submit_user_message(std::move(next));
                agent_->run_one_turn();
                const auto s = agent_->state();
                if (s == daedalus::AgentState::Cancelled) {
                    reason = "cancelled";
                    // A cancel landing between submit_user_message() and
                    // run_one_turn()'s consume strands the message in
                    // daedalus's own queue — the next turn would then run
                    // the STALE message (the off-by-one daedalus warns
                    // about). Drop any leftovers with the queue-management
                    // API; legal here because this thread IS the driving
                    // thread (the only one that ever touches the agent).
                    if (agent_->pending_count() > 0) {
                        Logger::instance().error(
                            "Cancelled turn left " +
                            std::to_string(agent_->pending_count()) +
                            " message(s) in daedalus's queue — clearing");
                        agent_->clear_pending_messages();
                    }
                }
                else if (s == daedalus::AgentState::Error) {
                    reason = "error";
                    // §7.2 — show the real cause (captured from the agent's
                    // Error-level logs) rather than a bare generic message.
                    const auto details = sink_->recent_errors_text();
                    error_message.emit(details.empty()
                        ? "The agent reported an error."
                        : details);
                } else if (agent_->turn_had_terminal_error()) {
                    // §7.2 / §TURN-ERROR (daedalus 2b25f27, our BUG-4):
                    // terminal LLM failure (e.g. a fatal HTTP 4xx like
                    // "invalid temperature") still ends the turn as Done —
                    // ask the agent instead of reconstructing the outcome
                    // from sink callbacks. Driving-thread call: legal here.
                    // The error was already shown live from on_error; just
                    // flag the turn for state/finished.
                    reason = "error";
                } else reason = "done";
                if (reason != "error")
                    // Fresh per-turn error capture for the next queued
                    // message (first turn's was cleared above ensure_agent).
                    sink_->clear_errors();
            }
        } catch (const std::exception& e) {
            reason = "error";
            Logger::instance().error(std::string("Turn failed with exception: ") + e.what());
            error_message.emit(e.what());
        }
        // §7.3 — the stop-tool summary closes the turn: emit it after every
        // tool/text event so its bubble appends below all tool cards.
        if (auto summary = sink_->take_stop_summary(); !summary.empty())
            agent_summary.emit(std::move(summary));
        emit_stats(); // §8 — final numbers before the turn closes
        const ChatState rest =
            reason == "error" ? ChatState::Error : ChatState::Idle;
        if (!transitioned) {
            // Exception path: the loop's atomic transition never ran.
            std::lock_guard lock(state_mutex_);
            state_ = rest;
        }
        state_changed.emit(rest);
        finished.emit(reason);
    });
    } catch (const std::system_error& e) { // jthread spawn failed
        Logger::instance().error(std::string("send_message: ") + e.what());
        error_message.emit(e.what());
        set_state(ChatState::Error);
        finished.emit("error");
    }
}

// §7.8 /compress — ask daedalus to compress the live session's history
// (Agent::compress_context, daedalus §AutoCompress): no user message, no
// summary prompt, no reseed. daedalus runs the same 70/30 LLM-summary
// path as the 90% auto trigger. When a turn is running the request is a
// flag consumed at the end of the current cycle (honored even on
// cancel); when idle daedalus compresses IMMEDIATELY and synchronously
// on the caller's thread — so the idle case runs on our turn thread to
// keep the UI responsive. Outcome feedback is the CompressResult return
// (§COMPRESS-RESULT, daedalus 2b25f27, our REQ-3) — no more duplicated
// message-count thresholds or observed-flag guessing. Success feedback
// comes via compacting/compacted (on_compaction). Returns false only
// when there is no session at all, so the caller skips its own
// "requested" bubble.
bool ChatController::compress_context() {
    if (!agent_) {
        error_message.emit("Nothing to compress yet — start a session first.");
        return false;
    }
    if (state() == ChatState::Processing || state() == ChatState::Cancelling) {
        // Turn in flight: daedalus defers the flag to the cycle checkpoint.
        // On success the compacted bubble appears when the cycle ends; a
        // checkpoint failure surfaces via on_error.
        const auto result = agent_->compress_context();
        if (result != daedalus::CompressResult::Deferred)
            Logger::instance().error(
                std::string("mid-turn compress_context returned ") +
                daedalus::to_string(result) + " (expected Deferred)");
        return true;
    }
    // Idle: daedalus compresses synchronously on the caller's thread —
    // offload the blocking summary call.
    set_state(ChatState::Processing);
    compacting.emit(); // strip: "Compacting…" (§7.6)
    if (turn_thread_.joinable()) turn_thread_.join();
    try {
    turn_thread_ = std::jthread([this](std::stop_token) {
        // noexcept; on_compaction fires from here on Compressed.
        switch (agent_->compress_context()) {
        case daedalus::CompressResult::Compressed:
            break; // before/after bubble already emitted via on_compaction
        case daedalus::CompressResult::NothingToCompress:
            // daedalus carries the request over to the next turn's first
            // cycle — say so, or the user thinks the click was ignored.
            error_message.emit(
                "Nothing to compress yet — the conversation is too short. "
                "daedalus will compress at the start of the next turn.");
            break;
        case daedalus::CompressResult::Failed:
            error_message.emit("⚠️ Compression did not complete — the summary "
                               "request failed. daedalus will retry at the "
                               "start of the next turn.");
            break;
        case daedalus::CompressResult::NoModel:
            error_message.emit("Nothing to compress yet — no model configured.");
            break;
        case daedalus::CompressResult::Deferred:
            // No turn is running here, so this should be unreachable.
            Logger::instance().error("idle compress_context returned Deferred");
            break;
        }
        emit_stats(); // §8 — context figure dropped
        set_state(ChatState::Idle);
        finished.emit("done");
    });
    } catch (const std::system_error& e) { // jthread spawn failed
        Logger::instance().error(std::string("compress_context: ") + e.what());
        set_state(ChatState::Error);
        finished.emit("error");
    }
    return true;
}

std::optional<daedalus::AgentStats> ChatController::stats() const {
    if (!agent_) return std::nullopt;
    return agent_->stats();
}

// §HISTORY-SNAPSHOT (daedalus 2b25f27, our REQ-2). Called once from
// ChatPanel at startup, main thread, before any turn exists. Builds the
// agent when needed — daedalus restores its persisted session on build,
// so the snapshot is the same conversation the agent resumes, not a
// host-side parallel copy.
std::vector<daedalus::HistoryEntry> ChatController::history_snapshot(
    const std::string& model_display) {
    try {
        ensure_agent(model_display);
    } catch (const std::exception& e) {
        // No model configured / provider incomplete — same as an empty
        // history: the panel shows the welcome screen.
        Logger::instance().error(
            std::string("history_snapshot: agent build skipped: ") + e.what());
        return {};
    }
    return agent_->history_snapshot();
}

// §8 — status-bar stats. Called from sink callbacks (agent thread) and the
// turn epilogue; a missing agent means there is nothing to publish.
void ChatController::emit_stats() {
    if (agent_) stats_changed.emit(agent_->stats());
}

std::string ChatController::active_plugin() const {
    if (!agent_) return "";
    std::string plugin;
    ui_guard("active_plugin", [&] { plugin = agent_->active_provider_plugin(); });
    return plugin;
}

std::string ChatController::last_terminal_error() const {
    return sink_->terminal_error_text();
}

std::vector<daedalus::ToolInfo> ChatController::tools() const {
    if (!agent_) return {};
    std::vector<daedalus::ToolInfo> result;
    ui_guard("tools", [&] { result = agent_->list_tools(); });
    return result;
}

std::vector<daedalus::SkillInfo> ChatController::skills() const {
    if (!agent_) return {};
    std::vector<daedalus::SkillInfo> result;
    ui_guard("skills", [&] { result = agent_->list_skills(); });
    return result;
}

std::vector<std::string> ChatController::mcp_servers() const {
    if (!agent_) return {};
    std::vector<std::string> result;
    ui_guard("mcp_servers", [&] { result = agent_->mcp_configured_servers(); });
    return result;
}

void ChatController::cancel() {
    if (state() != ChatState::Processing) return;
    set_state(ChatState::Cancelling);
    sink_->cancel_all_approvals();
    if (agent_) agent_->cancel();
    // the turn thread emits finished("cancelled") and resets to Idle
}

void ChatController::clear(bool wipe_session) {
    sink_->cancel_all_approvals();
    if (agent_) agent_->cancel();
    if (turn_thread_.joinable()) turn_thread_.join();
    // daedalus restores the persisted message chain on every agent build
    // (LLMClient init -> Messages::load, SQLite mirror first then
    // <workspace>/.daedalus/daedalus/messages.json), so a UI-side clear
    // must also wipe that store or the next session silently resumes the
    // cleared conversation (design §7.8 /clear). daedalus 0.9.7 (§CRASH-6)
    // made Messages::clear() persist properly, but no public
    // Agent::clear_messages() exists yet, so wipe via the public
    // resolve_agent_dir + SettingsStore::delete_messages_json instead.
    // Resolve the target root before active_workspace_ is reset below.
    const std::filesystem::path wipe_root =
        !active_workspace_.empty() ? std::filesystem::path(active_workspace_)
        : services_.files.workspace().empty() ? std::filesystem::path(".")
                                              : services_.files.workspace();
    if (agent_) {
        ui_guard("clear", [&] { agent_->shutdown(); });
        agent_.reset();
    }
    // After teardown, so no daedalus flush worker can re-save the chain.
    if (wipe_session) wipe_agent_session(wipe_root);
    active_model_display_.clear();
    active_workspace_.clear();
    {
        // Drop queued-but-unsent user messages as well (turn thread
        // already joined above, so no concurrent pop).
        std::lock_guard lock(state_mutex_);
        queued_messages_.clear();
    }
    set_state(ChatState::Idle);
    stats_changed.emit(daedalus::AgentStats{}); // §8 — drop the stat segments
}

void ChatController::wipe_agent_session(const std::filesystem::path& root) {
    ui_guard("wipe_agent_session", [&] {
        std::error_code ec;
        // Current layout plus the legacy read-fallback layout daedalus
        // still honors (default_paths.h).
        for (const auto& dir : {daedalus::resolve_agent_dir(root, ""),
                                root / ".daedalus"}) {
            const std::string base = (dir / "messages.json").string();
            for (const char* suffix : {"", ".tmp", ".crash"}) {
                std::filesystem::remove(base + suffix, ec);
                ec.clear();
            }
            // The SQLite mirror (<dir>/daedalus.db, key "default") is
            // consulted FIRST on load; only open the store when the db
            // file already exists so a clear doesn't create one.
            if (std::filesystem::exists(dir / "daedalus.db", ec)) {
                daedalus::SettingsStore::for_home(dir).delete_messages_json("default");
            }
            ec.clear();
        }
    });
}

} // namespace monkeybyte
