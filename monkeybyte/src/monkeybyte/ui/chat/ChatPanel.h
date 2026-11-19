#pragma once

#include "monkeybyte/ui/chat/ChatController.h"
#include "monkeybyte/backend/SpeechStream.h"

#include <gtkmm.h>
#include <map>
#include <nlohmann/json.hpp>
#include <memory>
#include <sigc++/sigc++.h>
#include <string>
#include <unordered_set>

namespace monkeybyte {

struct AppServices;
enum class DictationState;
class ChatMessage;
class ToolCard;

// §7 — right-docked agent chat.
class ChatPanel : public Gtk::Box {
public:
    explicit ChatPanel(AppServices& services);
    ~ChatPanel() override;

    void cancel_session(); // §12.1 — safe when idle
    void clear_history();
    // §RESTORE-RETRY — re-run the session restore after teardown; used
    // when the workspace switches and the new workspace's history should
    // replace the current view. Safe when nothing was restored yet.
    void reload_history();
    // Tear down the cached agent so the next message rebuilds it with
    // current settings (e.g. after MCP servers change). Chat is kept.
    void reset_agent();

    // Extra context (e.g. editor selection) attached to outgoing messages.
    void set_context_provider(std::function<std::string()> provider) {
        controller_->set_context_provider(std::move(provider));
    }

    // Tools of the live agent session (empty when no session exists) —
    // used by Settings → Agent → Tools.
    std::vector<daedalus::ToolInfo> agent_tools() const {
        return controller_->tools();
    }

    // UiControl chat.send hook (§14 test surface): send a prompt as if
    // typed, optionally switching the header model dropdown to `model`
    // (display name, "" = keep current) first. Blocks on a nested main
    // loop (with a guaranteed-wake timeout source — see eval_page_json)
    // until the turn finishes or `timeout_s` elapses; the result carries
    // the model used, the done reason, and any terminal error text.
    nlohmann::json send_message_sync(const std::string& text,
                                     const std::string& model,
                                     int timeout_s);

    sigc::signal<void(const std::string&)> signal_model_changed() {
        return signal_model_changed_;
    }
    sigc::signal<void(int, int)> signal_counts_changed() {
        return signal_counts_changed_;
    }
    // §8 — live agent stats for the status bar (zeroed when no session).
    sigc::signal<void(const daedalus::AgentStats&)> signal_stats_changed() {
        return signal_stats_changed_;
    }

private:
    void build_header();
    void build_volume_controls();
    void update_volume_controls();
    void build_messages();
    void build_input();
    void wire_controller();
    void wire_voice();
    void populate_models();
    // §ThinkingLevel — (re)fill the thinking-level dropdown for the
    // currently selected model and restore its saved level.
    void populate_thinking_levels();
    void on_thinking_level_changed();

    ChatMessage& ensure_assistant_bubble();
    // §7.3 — close the streaming bubble (tool call / turn end / summary).
    // Assistant text that arrives afterwards opens a fresh bubble below.
    void finalize_active_bubble();
    void add_tool_card(const ToolInfo& info);
    void update_tool_card(const ToolInfo& info);
    void finalize_turn();
    void scroll_to_bottom(); // pins only while stick_to_bottom_ (§7.3)
    // Pin regardless of the latch (own message, history restore).
    void scroll_to_bottom_forced();
    void set_processing(bool processing);
    void show_error(const std::string& message);
    void on_send();
    void on_steer();
    // §READ-IMAGE — attach-button flow: vision gate, file dialog, then
    // the chosen path is appended to the input as ![name](path), which
    // daedalus's LLMClient turns into an image_url content part.
    void on_attach_image();
    // §CHIMPFLOW — click once to record, again to transcribe for review.
    void on_microphone();
    void update_microphone(DictationState state);
    void refresh_microphone();
    // Mirrors LLMClient::model_supports_vision(): consult the provider
    // plugin's static model table; unknown models are assumed capable.
    bool current_model_supports_vision();

    // §7.6 — activity strip: fixed above the input bar (outside the
    // scroller, so it can't be clipped). Always visible; the label shows
    // the current agent state and the spinner spins while a turn runs.
    void show_activity(const std::string& text);
    void poke_activity(); // activity advanced: refresh label + keep spinning
    void stop_activity(const std::string& idle_text = "Ready"); // turn over

    // §7.8 — user commands ("/init", "/compress", …)
    void run_command(const std::string& name);
    void add_info_bubble(const std::string& text); // transient, not persisted
    std::string current_model_display();

    void restore_history();
    // Tear the messages box back to empty (bubbles, tool cards, welcome,
    // history_) WITHOUT touching the persisted session — clear_history
    // wipes it; this only rebuilds the view (§RESTORE-RETRY).
    void teardown_messages();
    void build_messages_welcome_only();
    void refresh_counts();

    template <typename T>
    void marshal(sigc::signal<void(T)>& sig, std::function<void(T)> handler);

    AppServices& services_;
    std::unique_ptr<ChatController> controller_;

    // Flipped false in the destructor: queued main-loop callbacks from
    // worker-thread controller signals check it before touching widgets
    // (the turn thread can outlive the panel on shutdown).
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

    // header
    Gtk::DropDown model_dropdown_;
    Gtk::DropDown thinking_dropdown_; // §ThinkingLevel — per-model levels
    Gtk::Button stop_btn_, clear_btn_;
    Gtk::MenuButton volume_btn_;
    Gtk::Scale volume_scale_{Gtk::Orientation::HORIZONTAL};
    Gtk::Label volume_label_;
    Gtk::Button mute_btn_;
    bool updating_volume_ = false;

    // banner + messages
    Gtk::Label error_banner_;
    Gtk::ScrolledWindow scroller_;
    Gtk::Box messages_;
    Gtk::Box* welcome_ = nullptr;

    // input
    Gtk::TextView input_;
    Gtk::Button send_btn_;
    Gtk::Button steer_btn_;
    Gtk::MenuButton commands_btn_; // §7.8 — "/" user commands
    Gtk::Button attach_btn_;       // §READ-IMAGE — image attachment
    Gtk::Button mic_btn_;          // §CHIMPFLOW — dictation toggle

    ChatMessage* active_assistant_ = nullptr;
    SpeechStreamSegmenter speech_stream_; // §CHIMPFLOW-STREAM
    std::map<std::string, ToolCard*> tool_cards_;
    bool processing_ = false;
    bool updating_models_ = false;
    bool updating_thinking_ = false; // suppress emissions during refill
    // §7.3 — pinned-to-bottom tracking. The latch expresses USER intent
    // only: programmatic set_value() calls (scroll_to_bottom's idle, the
    // changed re-pin) are wrapped in programmatic_scroll_ so transient
    // mid-layout states can't move it (§7.3 "Scroll pinning").
    bool stick_to_bottom_ = true;
    bool programmatic_scroll_ = false;
    // Monotonic-time deadline until which value_changed may not CLEAR the
    // latch. scroll_to_bottom_forced sets it: inertial (touchpad fling)
    // scroll events can keep moving the value for a few hundred ms after
    // the user hits Enter, and those residual non-programmatic changes
    // otherwise flipped the latch back off before the new bubble's layout
    // pass, leaving the view stuck at the old bottom (bubble top sliver).
    gint64 stick_lock_until_us_ = 0;

    // §7.6 — activity strip (fixed above the input bar)
    Gtk::Box activity_row_{Gtk::Orientation::HORIZONTAL, 8};
    Gtk::Spinner activity_spinner_;
    Gtk::Label activity_label_;
    std::string activity_text_ = "Ready";

    // §MSG-HEADERS — host-side mirror entry; source/received_at drive the
    // origin header when a bubble is (re)built from history_.
    struct HistoryEntry {
        std::string role;
        std::string content;
        std::string source;
        std::string received_at;
    };
    std::vector<HistoryEntry> history_;
    int user_turns_ = 0;
    int total_messages_ = 0;

    // Commands the user approved with "Always" — exact-match, scoped to
    // this panel's lifetime (session). Checked before prompting so these
    // never show the approval dialog again.
    std::unordered_set<std::string> always_approved_commands_;

    sigc::signal<void(const std::string&)> signal_model_changed_;
    sigc::signal<void(int, int)> signal_counts_changed_;
    sigc::signal<void(const daedalus::AgentStats&)> signal_stats_changed_;
};

} // namespace monkeybyte
