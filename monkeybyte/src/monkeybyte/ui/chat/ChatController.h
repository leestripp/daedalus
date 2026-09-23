#pragma once

#include <daedalus/Agent.h>
#include <daedalus/types/AgentStats.h>
#include <daedalus/types/HistoryEntry.h>
#include <daedalus/types/SkillInfo.h>
#include <daedalus/types/ThinkingLevel.h>
#include <daedalus/types/ToolInfo.h>

#include <atomic>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sigc++/sigc++.h>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace monkeybyte {

struct AppServices;
class PanelSink;

// §7.6 — agent lifecycle state machine.
enum class ChatState { Idle, Processing, Cancelling, Error };

// One tool call's lifecycle data (§7.3).
struct ToolInfo {
    std::string id;
    std::string name;
    std::string args;    // raw JSON
    std::string result;  // raw JSON (on done)
    std::string status;  // "running" / "done (450ms)" / progress text
    std::string live_output; // sanitised stdout/stderr tail while running (§7.3)
    bool done = false;
    bool success = false;
};

// Drives an in-process daedalus::Agent (design §10, single-process variant).
// The agent runs turns on a worker thread; sink callbacks arrive on the
// agent/HTTP threads, so UI receivers MUST marshal to the GTK main loop
// (ChatPanel does this via Glib::signal_idle().connect_once).
class ChatController {
public:
    explicit ChatController(AppServices& services);
    ~ChatController();

    ChatController(const ChatController&) = delete;
    ChatController& operator=(const ChatController&) = delete;

    void send_message(const std::string& text, const std::string& model_display);
    void cancel();
    // Agent teardown. wipe_session=true (user /clear, 🗑 button, workspace
    // change) also removes daedalus's persisted message chain so the next
    // agent build doesn't silently restore the cleared conversation;
    // wipe_session=false (settings rebuild, chat kept) preserves it.
    void clear(bool wipe_session);

    ChatState state() const;

    // §7.8 — user commands. Idle-only, main-thread accessors; return
    // empty/nullopt when no agent session exists yet.
    bool has_agent() const { return agent_ != nullptr; }
    std::optional<daedalus::AgentStats> stats() const;
    // §HISTORY-SNAPSHOT (daedalus 2b25f27, our REQ-2) — the conversation
    // the agent actually sees (post-restore, post-compression,
    // post-sanitization; tool calls elided), for rendering the transcript
    // at startup. Builds the agent for `model_display` when none exists —
    // daedalus restores its persisted session on build, which is where the
    // snapshot comes from. Main thread, idle only; returns {} when no
    // model is configured (ensure_agent throws) or the session is empty.
    std::vector<daedalus::HistoryEntry> history_snapshot(
        const std::string& model_display);
    // §7.2 — last terminal on_error text of the current/last turn (""
    // when none): the same string the error banner shows. Exposed for the
    // UiControl chat.send hook so a headless caller gets the real cause.
    // Backed by the sink's mutex-guarded capture, NOT agent_->last_error()
    // — the hook calls this from the GTK main thread, where daedalus's
    // driving-thread-only accessors are off-limits (and the turn may even
    // still be running, on the timeout path).
    std::string last_terminal_error() const;
    std::vector<daedalus::ToolInfo> tools() const;
    std::vector<daedalus::SkillInfo> skills() const;
    std::vector<std::string> mcp_servers() const;
    // §ProviderPlugins — name of the provider plugin the running session
    // resolved to ("" when no agent exists yet).
    std::string active_plugin() const;

    // §ThinkingLevel — host-selected thinking level (chat header dropdown).
    // Applied at runtime to the live agent (daedalus Agent::set_thinking_level,
    // next-request effect, no rebuild); also seeded into AgentConfig on
    // every build so a fresh session keeps the selection.
    void set_thinking_level(daedalus::ThinkingLevel level);

    // §7.8 /compress — ask daedalus to compress the live session's history
    // (Agent::compress_context, daedalus §AutoCompress; no user message is
    // sent). Mid-turn it's a flag consumed at the end of the current cycle
    // (honored even on cancel); idle it compresses IMMEDIATELY and
    // synchronously on the caller's thread, so the idle case runs on the
    // turn thread here. Feedback: the before/after bubble arrives via
    // compacted (on_compaction) on success; every other outcome is known
    // from the CompressResult return (§COMPRESS-RESULT, daedalus 2b25f27)
    // and reported via error_message. Returns false only when there is no
    // session, so the caller skips its own "requested" bubble.
    bool compress_context();

    // The UI registers a handler that asks the user to approve a bash
    // command (daedalus auto_approve=false). Called on the GTK main loop;
    // must invoke `respond(approved)` exactly once.
    using ApprovalHandler =
        std::function<void(const std::string& command, std::function<void(bool)> respond)>;
    void set_approval_handler(ApprovalHandler handler);

    // Optional hook returning extra context (e.g. the current editor
    // selection) that is prepended to the submitted message. Evaluated on
    // the GTK main thread inside send_message, before the turn thread runs.
    void set_context_provider(std::function<std::string()> provider);

    // signals (worker thread!)
    sigc::signal<void(ChatState)> state_changed;
    sigc::signal<void(std::string)> user_message_sent; // echo for the UI
    sigc::signal<void(std::string)> text_delta;        // assistant text chunk
    sigc::signal<void(std::string)> agent_summary;     // stop-tool summary (§7.3)
    sigc::signal<void(std::string)> tell_user_message; // tell_user content → bubble (§7.3)
    sigc::signal<void(ToolInfo)> tool_started;
    sigc::signal<void(ToolInfo)> tool_updated;         // progress / done
    sigc::signal<void(std::string)> error_message;
    sigc::signal<void(std::string)> finished;          // done reason
    // §7.6/§7.3/§7.8 — context compression (daedalus does it — the 90%
    // auto-compress loop or a host-triggered Agent::compress(), §7.8):
    // compacting fires when it starts (AgentState::Compressing), compacted
    // when history was rebuilt (AgentSink::on_compaction, before/after
    // token counts).
    sigc::signal<void()> compacting;
    sigc::signal<void(std::size_t, std::size_t)> compacted;
    // §8 — live agent stats for the status bar: emitted from sink callbacks
    // (tool results, state changes) and at turn end; zeroed on clear().
    sigc::signal<void(daedalus::AgentStats)> stats_changed;

private:
    friend class PanelSink;

    void set_state(ChatState s);
    void ensure_agent(const std::string& model_display); // create/recreate
    void emit_stats(); // agent thread: publish current AgentStats
    // Remove daedalus's persisted session for `root` (messages.json +
    // .tmp/.crash mirrors + SQLite mirror row) via daedalus's public
    // resolve_agent_dir / SettingsStore::delete_messages_json.
    static void wipe_agent_session(const std::filesystem::path& root);

    AppServices& services_;
    std::shared_ptr<PanelSink> sink_;
    std::unique_ptr<daedalus::Agent> agent_;
    std::jthread turn_thread_;

    std::string active_model_display_;
    std::string active_workspace_;
    bool active_auto_approve_ = false; // rebuild the agent when the toggle flips
    // §ThinkingLevel — pending (UI-set) vs. applied-to-live-agent levels.
    daedalus::ThinkingLevel thinking_level_ = daedalus::ThinkingLevel::Default;
    daedalus::ThinkingLevel active_thinking_level_ = daedalus::ThinkingLevel::Default;

    ApprovalHandler approval_handler_;
    std::function<std::string()> context_provider_;

    mutable std::mutex state_mutex_;
    ChatState state_ = ChatState::Idle;
    // User messages typed while a turn runs (design §7.5). daedalus's
    // message queue is internally synchronized since 2b25f27
    // (§QUEUE-MUTEX, our BUG-3) and Agent::cancel() is callable from any
    // thread, so this relay is no longer needed for thread safety — it is
    // kept for its semantics: strict FIFO, an error stops the drain with
    // the remainder going FIRST on the next send, and sharing
    // state_mutex_ makes "busy -> enqueue" vs. "queue empty -> go Idle"
    // atomic so a message can never strand behind the drain loop's final
    // empty check. The turn thread alone feeds daedalus, one
    // submit + run_one_turn per message.
    std::deque<std::string> queued_messages_;
};

} // namespace monkeybyte
