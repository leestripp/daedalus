#include "monkeybyte/ui/chat/ChatPanel.h"

#include "monkeybyte/AppServices.h"
#include "monkeybyte/backend/ModelBridge.h"
#include "monkeybyte/ui/chat/ChatMessage.h"
#include "monkeybyte/ui/chat/ToolCard.h"
#include "monkeybyte/ui/chat/UserCommands.h"
#include "monkeybyte/ui/UiGuard.h"

#include <algorithm>
#include <cctype>
#include <daedalus/ProviderPlugin.h>
#include <daedalus/ProviderRegistry.h>
#include <daedalus/settings.h>
#include <nlohmann/json.hpp>

namespace monkeybyte {

using nlohmann::json;

namespace {

// 950 → "950", 12800 → "12.8k", 250000 → "250k" (same style as StatusBar)
std::string kformat(int n) {
    char buf[32];
    if (n < 1000) std::snprintf(buf, sizeof(buf), "%d", n);
    else if (n < 10000) std::snprintf(buf, sizeof(buf), "%.1fk", n / 1000.0);
    else std::snprintf(buf, sizeof(buf), "%dk", (n + 500) / 1000);
    return buf;
}

} // namespace

ChatPanel::ChatPanel(AppServices& services)
    : Gtk::Box(Gtk::Orientation::VERTICAL), services_(services) {
    add_css_class("chat-panel");

    controller_ = std::make_unique<ChatController>(services_);

    // daedalus asks before running bash commands (auto_approve=false).
    // The request arrives on the agent thread; ask on the GTK main loop
    // and answer via respond(). Buttons: Deny (this run), Approve (run
    // once), Always (run and never prompt again for this exact command,
    // tracked in always_approved_commands_ for the panel's lifetime).
    controller_->set_approval_handler(
        [this](const std::string& command, std::function<void(bool)> respond) {
            Glib::signal_idle().connect_once([this, command, respond = std::move(respond)]() mutable {
                // Set is only touched on the GTK main loop → no locking.
                if (always_approved_commands_.contains(command)) {
                    respond(true);
                    return;
                }
                auto* dialog = gtk_alert_dialog_new(
                    "The agent wants to run a shell command");
                const std::string detail =
                    "The AI agent is asking for permission to execute the "
                    "following command in your workspace. It will run with "
                    "your user permissions and can read or modify files.\n\n" +
                    command;
                gtk_alert_dialog_set_detail(GTK_ALERT_DIALOG(dialog), detail.c_str());
                const char* buttons[] = {"Deny", "Approve", "Always", nullptr};
                gtk_alert_dialog_set_buttons(GTK_ALERT_DIALOG(dialog), buttons);
                gtk_alert_dialog_set_cancel_button(GTK_ALERT_DIALOG(dialog), 0);
                gtk_alert_dialog_set_default_button(GTK_ALERT_DIALOG(dialog), 1);
                // Heap context so the async callback can both answer the
                // gate and record an "Always" choice.
                struct ApprovalRequest {
                    ChatPanel* self;
                    std::shared_ptr<bool> alive;
                    std::string command;
                    std::function<void(bool)> respond;
                };
                auto* req = new ApprovalRequest{
                    this, alive_, command, std::move(respond)};
                // get_root() yields the gtkmm WRAPPER, not the GObject;
                // GTK_WINDOW(get_root()) type-puns C++ memory, fails the
                // runtime type check (GLib critical) and yields NULL —
                // the alert then maps detached and can hide behind the
                // main window while the agent gate waits forever.
                auto* root_win = dynamic_cast<Gtk::Window*>(get_root());
                GtkWindow* parent =
                    root_win ? GTK_WINDOW(root_win->gobj()) : nullptr;
                gtk_alert_dialog_choose(
                    GTK_ALERT_DIALOG(dialog), parent, nullptr,
                    [](GObject* source, GAsyncResult* result, gpointer data) {
                        std::unique_ptr<ApprovalRequest> req(
                            static_cast<ApprovalRequest*>(data));
                        const int choice = gtk_alert_dialog_choose_finish(
                            GTK_ALERT_DIALOG(source), result, nullptr);
                        // 0 = Deny, 1 = Approve, 2 = Always; dismissal (-1)
                        // counts as Deny.
                        if (choice == 2 && *req->alive)
                            req->self->always_approved_commands_.insert(
                                req->command);
                        req->respond(choice >= 1);
                    },
                    req);
                g_object_unref(dialog);
            });
        });

    build_header();
    build_messages();
    build_input();
    wire_controller();
    restore_history();
}

ChatPanel::~ChatPanel() { *alive_ = false; }

// (Re)fill the model dropdown from the settings store, preserving the
// current selection when possible (§7.1).
void ChatPanel::populate_models() {
    ui_guard("populate_models", [this] {
    std::string current;
    if (auto item = model_dropdown_.get_selected_item())
        current = std::dynamic_pointer_cast<Gtk::StringObject>(item)->get_string();
    if (current.empty())
        current = services_.chat->get_string("selected-model");

    const auto models = model_bridge::list_models(
        daedalus::SettingsStore::instance(), services_.store);
    auto names = Gtk::StringList::create({});
    unsigned selected = GTK_INVALID_LIST_POSITION;
    unsigned default_idx = 0;
    for (const auto& m : models) {
        const std::string display = model_bridge::display_name(m);
        names->append(display);
        const auto idx = names->get_n_items() - 1;
        if (display == current) selected = idx;
        if (m.is_default) default_idx = idx;
    }

    // Avoid spurious selected-model emissions while the model list is swapped.
    updating_models_ = true;
    model_dropdown_.set_model(names);
    if (selected != GTK_INVALID_LIST_POSITION)
        model_dropdown_.set_selected(selected);
    else if (names->get_n_items() > 0)
        model_dropdown_.set_selected(default_idx);
    updating_models_ = false;

    // Announce the effective selection so the status bar stays in sync after
    // initial population or when the list changes (e.g. from Settings).
    if (auto item = model_dropdown_.get_selected_item()) {
        const auto display =
            std::dynamic_pointer_cast<Gtk::StringObject>(item)->get_string();
        signal_model_changed_.emit(display);
    }
    populate_thinking_levels(); // levels are per-model (§ThinkingLevel)
    });
}

// §ThinkingLevel — (re)fill the thinking dropdown for the selected model.
// Always visible: a model without thinking support gets a single "None"
// entry (inert — daedalus silently ignores the setting for it).
void ChatPanel::populate_thinking_levels() {
    ui_guard("populate_thinking_levels", [this] {
    const std::string display = current_model_display();
    auto& registry = daedalus::SettingsStore::instance();
    const auto levels =
        model_bridge::supported_thinking_levels(registry, services_.store, display);

    // Saved per-model selection (monkeybyte.db kv "agent.thinking_level.<model>").
    std::string saved = "Default";
    const auto models = model_bridge::list_models(registry, services_.store);
    const auto m = std::find_if(models.begin(), models.end(), [&](const UiModel& u) {
        return model_bridge::display_name(u) == display;
    });
    if (m != models.end())
        saved = services_.store.get("agent.thinking_level." + m->name).value_or("Default");

    auto names = Gtk::StringList::create({});
    unsigned selected = GTK_INVALID_LIST_POSITION;
    if (levels.empty()) {
        names->append("None");
    } else {
        names->append("Default");
        for (const auto l : levels) names->append(daedalus::to_string(l));
    }
    for (unsigned i = 0; i < names->get_n_items(); ++i) {
        if (names->get_string(i) == saved) {
            selected = i;
            break;
        }
    }

    // Avoid spurious emissions while the list is swapped (§7.1 pattern).
    updating_thinking_ = true;
    thinking_dropdown_.set_model(names);
    thinking_dropdown_.set_selected(
        selected != GTK_INVALID_LIST_POSITION ? selected : 0);
    updating_thinking_ = false;

    // Sync the controller with the effective selection (a saved level that
    // is no longer offered snaps back to the first entry).
    on_thinking_level_changed();
    });
}

void ChatPanel::on_thinking_level_changed() {
    if (updating_thinking_) return;
    ui_guard("on_thinking_level_changed", [this] {
    auto item = thinking_dropdown_.get_selected_item();
    if (!item) return;
    const auto label = std::dynamic_pointer_cast<Gtk::StringObject>(item)->get_string();
    const auto level = daedalus::thinking_level_from_string(label.raw());
    controller_->set_thinking_level(level); // applied on next agent build

    const std::string display = current_model_display();
    auto& registry = daedalus::SettingsStore::instance();
    const auto models = model_bridge::list_models(registry, services_.store);
    const auto m = std::find_if(models.begin(), models.end(), [&](const UiModel& u) {
        return model_bridge::display_name(u) == display;
    });
    if (m != models.end())
        services_.store.set("agent.thinking_level." + m->name,
                            daedalus::to_string(level));
    });
}

// Marshal worker-thread controller signals onto the GTK main loop.
// The alive_ guard drops callbacks that land after the panel is gone
// (shutdown mid-turn): the widgets they would touch are already destroyed.
template <typename T>
void ChatPanel::marshal(sigc::signal<void(T)>& sig, std::function<void(T)> handler) {
    sig.connect([this, alive = alive_, handler = std::move(handler)](T value) {
        Glib::signal_idle().connect_once(
            [this, alive, handler, value = std::move(value)]() mutable {
                if (!*alive) return;
                handler(std::move(value));
            });
    });
}

// ── header (§7.1) ───────────────────────────────────────────────────
void ChatPanel::build_header() {
    auto* header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    header->add_css_class("chat-header");

    auto* title = Gtk::make_managed<Gtk::Label>("Agent");
    title->set_halign(Gtk::Align::START);

    model_dropdown_.set_hexpand(true);
    populate_models();
    model_dropdown_.property_selected().signal_changed().connect([this] {
        if (updating_models_) return;
        auto item = model_dropdown_.get_selected_item();
        if (!item) return;
        const auto display =
            std::dynamic_pointer_cast<Gtk::StringObject>(item)->get_string();
        signal_model_changed_.emit(display);
        // §ThinkingLevel — the level list is per-model; refill + restore.
        populate_thinking_levels();
    });

    // §ThinkingLevel — per-model thinking-level picker, right of the model
    // dropdown; always visible (unsupported models show a single "None").
    thinking_dropdown_.set_tooltip_text("Thinking level");
    thinking_dropdown_.property_selected().signal_changed().connect(
        [this] { on_thinking_level_changed(); });

    // Re-populate when the panel is shown again (e.g. after adding a model
    // in Settings) — the dropdown is stale otherwise.
    signal_map().connect([this] { populate_models(); });

    {
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        auto* icon = Gtk::make_managed<Gtk::Image>();
        icon->set_from_icon_name("mb-stop-symbolic");
        icon->set_pixel_size(12);
        box->append(*icon);
        box->append(*Gtk::make_managed<Gtk::Label>("Stop"));
        stop_btn_.set_child(*box);
    }
    stop_btn_.set_tooltip_text("Stop the current turn");
    stop_btn_.set_visible(false);
    stop_btn_.signal_clicked().connect([this] { cancel_session(); });

    {
        auto* icon = Gtk::make_managed<Gtk::Image>();
        icon->set_from_icon_name("mb-trash-symbolic");
        icon->set_pixel_size(16);
        clear_btn_.set_child(*icon);
    }
    clear_btn_.set_tooltip_text("Clear chat");
    clear_btn_.signal_clicked().connect([this] { clear_history(); });

    header->append(*title);
    header->append(model_dropdown_);
    header->append(thinking_dropdown_);
    header->append(stop_btn_);
    header->append(clear_btn_);
    append(*header);

    // error banner (§7.2)
    error_banner_.add_css_class("error-banner");
    error_banner_.set_visible(false);
    error_banner_.set_wrap(true);
    append(error_banner_);
}

// ── messages (§7.3) ─────────────────────────────────────────────────
void ChatPanel::build_messages() {
    messages_.set_orientation(Gtk::Orientation::VERTICAL);
    messages_.set_margin(8);

    welcome_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    welcome_->set_valign(Gtk::Align::CENTER);
    welcome_->set_vexpand(true);
    auto* hi = Gtk::make_managed<Gtk::Label>("👋 Welcome to MonkeyByte!");
    auto* sub = Gtk::make_managed<Gtk::Label>(
        "I'm monkey-agent, your AI coding assistant.");
    sub->add_css_class("muted");
    welcome_->append(*hi);
    welcome_->append(*sub);
    messages_.append(*welcome_);

    scroller_.set_vexpand(true);
    // Never scroll horizontally: bubbles wrap to the panel width.
    scroller_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroller_.set_child(messages_);

    // §7.3 — keep the view pinned to the bottom as content grows.
    // stick_to_bottom_ tracks USER intent only: value changes caused by our
    // own set_value() calls (scroll_to_bottom, the re-pin below) are wrapped
    // in programmatic_scroll_ and skipped here. Transient mid-layout states
    // (a wrapped bubble renegotiating height-for-width across several layout
    // passes) used to land the value >12 px above a stale bottom and flip
    // the latch off, after which the re-pin below stopped firing and bubbles
    // no longer scrolled the view.
    auto vadj = scroller_.get_vadjustment();
    vadj->signal_value_changed().connect([this] {
        if (programmatic_scroll_) return;
        auto adj = scroller_.get_vadjustment();
        // 12 px tolerance: fractional layouts drift a few px short of the
        // bottom (§7.3). Reaching the bottom always re-engages following;
        // leaving it only counts outside the stick-lock window (residual
        // inertial scroll after a forced pin must not unfollow — §7.3).
        if (adj->get_value() >= adj->get_upper() - adj->get_page_size() - 12)
            stick_to_bottom_ = true;
        else if (g_get_monotonic_time() > stick_lock_until_us_)
            stick_to_bottom_ = false;
    });
    vadj->signal_changed().connect([this] {
        if (!stick_to_bottom_) return;
        auto adj = scroller_.get_vadjustment();
        programmatic_scroll_ = true;
        adj->set_value(adj->get_upper() - adj->get_page_size());
        programmatic_scroll_ = false;
    });

    append(scroller_);
}

// ── input bar (§7.5) ────────────────────────────────────────────────
void ChatPanel::build_input() {
    // §7.6 — activity strip: fixed above the input bar (outside the
    // scroller, so it can never be clipped). Always visible: the label
    // shows the current agent state ("Ready", "Thinking…", "Done", …)
    // and the spinner spins while a turn is running.
    activity_row_.add_css_class("chat-activity");
    activity_label_.add_css_class("muted");
    activity_label_.set_text("Ready");
    // Reserve the spinner's space even while stopped so the label
    // doesn't shift when a turn starts (an idle GtkSpinner measures 0).
    activity_spinner_.set_size_request(16, 16);
    activity_row_.append(activity_spinner_);
    activity_row_.append(activity_label_);
    append(activity_row_);

    auto* bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    bar->add_css_class("chat-input");

    auto* input_scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    input_scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    input_scroll->set_hexpand(true);
    input_scroll->set_child(input_);
    input_.set_wrap_mode(Gtk::WrapMode::WORD_CHAR);
    input_.set_accepts_tab(false);
    input_scroll->set_min_content_height(32);
    input_scroll->set_max_content_height(120); // ~6 lines (§7.5)
    input_scroll->set_propagate_natural_height(true);

    // Enter sends, Shift+Enter newline
    auto keys = Gtk::EventControllerKey::create();
    keys->signal_key_pressed().connect(
        [this](guint keyval, guint, Gdk::ModifierType state) {
            if (keyval != GDK_KEY_Return && keyval != GDK_KEY_KP_Enter)
                return false;
            const bool shift = static_cast<bool>(state & Gdk::ModifierType::SHIFT_MASK);
            if (shift) return false; // let TextView insert the newline
            on_send();
            return true;
        },
        false);
    input_.add_controller(keys);

    send_btn_.set_label("Send");
    send_btn_.add_css_class("suggested-action");
    send_btn_.signal_clicked().connect([this] { on_send(); });

    // §READ-IMAGE — image attachment. Emits ![name](path) into the input,
    // which daedalus turns into an image_url part for vision models.
    attach_btn_.set_icon_name("mb-file-image-symbolic");
    attach_btn_.set_tooltip_text("Attach image");
    attach_btn_.signal_clicked().connect([this] { on_attach_image(); });

    // §7.8 — user command menu ("/init", "/compress", …)
    commands_btn_.set_label("/");
    commands_btn_.set_tooltip_text("Commands");
    auto* menu_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    menu_box->set_margin(6);
    for (const auto& cmd : user_commands()) {
        auto* row = Gtk::make_managed<Gtk::Button>();
        row->add_css_class("flat");
        auto* row_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        auto* name = Gtk::make_managed<Gtk::Label>("/" + cmd.name);
        auto* desc = Gtk::make_managed<Gtk::Label>(cmd.description);
        desc->add_css_class("muted");
        desc->set_wrap(true);
        row_box->append(*name);
        row_box->append(*desc);
        row->set_child(*row_box);
        row->signal_clicked().connect(
            [this, name = cmd.name] { run_command(name); });
        menu_box->append(*row);
    }
    auto* popover = Gtk::make_managed<Gtk::Popover>();
    popover->set_child(*menu_box);
    commands_btn_.set_popover(*popover);

    bar->append(commands_btn_);
    bar->append(attach_btn_);
    bar->append(*input_scroll);
    bar->append(send_btn_);
    append(*bar);
}

// ── controller wiring ───────────────────────────────────────────────
void ChatPanel::wire_controller() {
    marshal<ChatState>(controller_->state_changed, [this](ChatState s) {
        set_processing(s == ChatState::Processing || s == ChatState::Cancelling);
        // §7.6 — the strip mirrors the agent state at all times.
        if (s == ChatState::Processing) show_activity("Thinking…");
        else if (s == ChatState::Cancelling) show_activity("Cancelling…");
        else if (s == ChatState::Error) stop_activity("Error");
        else stop_activity("Ready");
    });

    marshal<std::string>(controller_->user_message_sent, [this](std::string text) {
        if (welcome_) {
            messages_.remove(*welcome_);
            welcome_ = nullptr;
        }
        auto* bubble = Gtk::make_managed<ChatMessage>(ChatMessage::Role::User);
        bubble->set_text(text);
        // §MSG-HEADERS — same origin the controller stamps into `_meta`,
        // so this live bubble and its restored copy read identically.
        const std::string origin = ChatController::interactive_source();
        bubble->set_origin(origin, "");
        messages_.append(*bubble);
        active_assistant_ = nullptr;
        history_.push_back({"user", text, origin, ""});
        refresh_counts();
        scroll_to_bottom_forced(); // your own message always pins (§7.3)
    });

    marshal<std::string>(controller_->text_delta, [this](const std::string& delta) {
        activity_text_ = "Streaming…";
        poke_activity();
        ensure_assistant_bubble().append_text(delta);
        scroll_to_bottom();
    });

    marshal<std::string>(controller_->agent_summary, [this](std::string summary) {
        // §7.3 — stop-tool summary: its own finalized assistant bubble,
        // persisted to history so it survives a restart (unlike streamed
        // assistant bodies, which are session-only).
        finalize_active_bubble();
        auto* bubble = Gtk::make_managed<ChatMessage>(ChatMessage::Role::Assistant);
        bubble->set_text(summary);
        // §MSG-HEADERS — the summary is a context-compression artifact
        // (daedalus stamps `system:context-status`), not the model's
        // voice; label it so it can't masquerade as a fresh reply.
        bubble->set_origin("system:context-status", "");
        bubble->finalize();
        messages_.append(*bubble);
        history_.push_back({"assistant", summary, "system:context-status", ""});
        refresh_counts();
        scroll_to_bottom();
    });

    marshal<std::string>(controller_->tell_user_message, [this](std::string msg) {
        // §7.3 — tell_user message: its own finalized assistant bubble,
        // persisted to history like the stop-tool summary (no tool card —
        // the output is user-facing prose, not raw-JSON tool noise).
        finalize_active_bubble();
        auto* bubble = Gtk::make_managed<ChatMessage>(ChatMessage::Role::Assistant);
        bubble->set_text(msg);
        bubble->finalize();
        messages_.append(*bubble);
        history_.push_back({"assistant", msg, "", ""});
        refresh_counts();
        scroll_to_bottom();
    });

    marshal<ToolInfo>(controller_->tool_started,
                      [this](const ToolInfo& t) {
        activity_text_ = "Running " + t.name + "…";
        poke_activity();
        // §7.3 — interleave bubbles with tool cards: close the current
        // assistant bubble so its text stays above the cards and any text
        // streamed after the tool call opens a fresh bubble below them.
        finalize_active_bubble();
        // §7.3 — tell_user renders as an assistant bubble (tell_user_message
        // signal), so no tool card is created for it.
        if (t.name != "tell_user") add_tool_card(t);
    });
    marshal<ToolInfo>(controller_->tool_updated,
                      [this](const ToolInfo& t) {
        if (t.done) activity_text_ = "Thinking…";
        poke_activity();
        update_tool_card(t);
        if (!t.live_output.empty()) scroll_to_bottom(); // follow live output
    });

    marshal<std::string>(controller_->error_message, [this](std::string msg) {
        show_error(msg);
        auto& bubble = ensure_assistant_bubble();
        // Keep the error on its own line — streamed text rarely ends with \n.
        bubble.append_text((bubble.has_text() ? "\n❌ " : "❌ ") + msg);
        bubble.finalize();
        // §7.2 — errors can now arrive mid-turn (on_error is live): let any
        // subsequent assistant text start a fresh bubble.
        active_assistant_ = nullptr;
        scroll_to_bottom();
    });

    // §8 — forward live agent stats to the status bar.
    marshal<daedalus::AgentStats>(controller_->stats_changed,
                                  [this](const daedalus::AgentStats& s) {
        signal_stats_changed_.emit(s);
    });

    marshal<std::string>(controller_->finished, [this](const std::string& reason) {
        // §7.6 — resting state (emitted after state_changed, so it wins).
        stop_activity(reason == "cancelled" ? "Cancelled"
                      : reason == "error"   ? "Error"
                                            : "Done");
        finalize_turn();
        if (reason == "cancelled") {
            auto* note = Gtk::make_managed<ChatMessage>(ChatMessage::Role::Assistant);
            note->set_text("⚠️ Operation cancelled by user.");
            messages_.append(*note);
        }
        refresh_counts();
        scroll_to_bottom();
    });

    // §7.6/§7.3/§7.8 — context compression feedback (daedalus 90% loop and
    // manual /compress both come through these signals): the strip explains
    // the silent summary stretch; the done event leaves a permanent note
    // with the before/after token counts (§8 status bar refreshes itself
    // via the stats signal). marshal<> only fits single-arg signals, so
    // these two are idle-dispatched by hand.
    controller_->compacting.connect([this, alive = alive_] {
        Glib::signal_idle().connect_once([this, alive] {
            if (*alive) show_activity("Compacting…");
        });
    });
    controller_->compacted.connect(
        [this, alive = alive_](std::size_t before, std::size_t after) {
            Glib::signal_idle().connect_once([this, alive, before, after] {
                if (!*alive) return;
                add_info_bubble("🗜 Context compressed: " +
                                kformat(static_cast<int>(before)) + " → " +
                                kformat(static_cast<int>(after)) + " tokens");
                show_activity("Thinking…"); // the turn continues
            });
        });
}

ChatMessage& ChatPanel::ensure_assistant_bubble() {
    if (!active_assistant_) {
        active_assistant_ = Gtk::make_managed<ChatMessage>(ChatMessage::Role::Assistant);
        messages_.append(*active_assistant_);
    }
    return *active_assistant_;
}

// §7.3 — close the streaming bubble. The empty history entry keeps the
// message counts consistent; the body itself stays session-only (skipped
// on restore, as before).
void ChatPanel::finalize_active_bubble() {
    if (!active_assistant_) return;
    active_assistant_->finalize();
    history_.push_back({"assistant", "", "", ""});
    active_assistant_ = nullptr;
}

void ChatPanel::add_tool_card(const ToolInfo& info) {
    // tool cards append below the just-finalized assistant bubble (§7.3)
    auto* card = Gtk::make_managed<ToolCard>(info);
    tool_cards_[info.id] = card;
    messages_.append(*card);
    scroll_to_bottom();
}

// §7.6 — activity strip (fixed above the input bar). Always visible: the
// label mirrors the agent state and the spinner spins while a turn runs.
void ChatPanel::show_activity(const std::string& text) {
    activity_text_ = text;
    poke_activity();
}

void ChatPanel::poke_activity() {
    activity_label_.set_text(activity_text_);
    activity_spinner_.start();
}

void ChatPanel::stop_activity(const std::string& idle_text) {
    activity_spinner_.stop();
    activity_text_ = idle_text;
    activity_label_.set_text(idle_text);
}

void ChatPanel::update_tool_card(const ToolInfo& info) {
    auto it = tool_cards_.find(info.id);
    if (it != tool_cards_.end()) it->second->update(info);
}

void ChatPanel::finalize_turn() {
    finalize_active_bubble();
    set_processing(false);
}

// §7.3 — follow new content only while the user is at the bottom; someone
// who scrolled up during streaming is not yanked back down.
void ChatPanel::scroll_to_bottom() {
    if (!stick_to_bottom_) return;
    scroll_to_bottom_forced();
}

void ChatPanel::scroll_to_bottom_forced() {
    stick_to_bottom_ = true;
    // §7.3 — protect the pin against residual inertial-scroll value changes
    // (touchpad fling on Wayland) that arrive right after Enter.
    stick_lock_until_us_ = g_get_monotonic_time() + G_USEC_PER_SEC;
    auto pin = [this] {
        auto adj = scroller_.get_vadjustment();
        programmatic_scroll_ = true;
        adj->set_value(adj->get_upper() - adj->get_page_size());
        programmatic_scroll_ = false;
    };
    Glib::signal_idle().connect_once(pin);
    // Second pass after late multi-pass layouts (a wrapping bubble's height
    // can settle one frame after the idle) and kinetic scroll drift.
    Glib::signal_timeout().connect_once(pin, 150);
}

void ChatPanel::set_processing(bool processing) {
    processing_ = processing;
    // §7.5 — daedalus has message-queue support (ChatController queues
    // mid-turn sends host-side and the turn thread drains them), so the
    // input area is NEVER disabled: the user can add tasks while the
    // agent works and they are picked up when its loop exits. Only the
    // Stop button still reflects the state. Commands stay idle-gated in
    // run_command() (with feedback) — /help and /compress work mid-turn.
    stop_btn_.set_visible(processing);
}

void ChatPanel::show_error(const std::string& message) {
    error_banner_.set_text(message);
    error_banner_.set_visible(true);
}

// ── actions ─────────────────────────────────────────────────────────
void ChatPanel::on_send() {
    const auto text = input_.get_buffer()->get_text();
    if (text.empty()) return; // §7.5 — mid-turn sends queue, never block
    if (text.find_first_not_of(" \t\n\r") == Glib::ustring::npos) return;

    // §7.8 — intercept user commands before they reach the agent.
    const std::string raw = text;
    if (auto cmd = parse_command(raw)) {
        input_.get_buffer()->set_text("");
        error_banner_.set_visible(false);
        run_command(*cmd);
        return;
    }
    if (looks_like_command(raw)) {
        input_.get_buffer()->set_text("");
        add_info_bubble("Unknown command " + raw + " — try /help");
        return;
    }

    error_banner_.set_visible(false);
    input_.get_buffer()->set_text("");
    controller_->send_message(text, current_model_display());
}

nlohmann::json ChatPanel::send_message_sync(const std::string& text,
                                            const std::string& model,
                                            int timeout_s) {
    if (processing_)
        return {{"error", "chat is busy"}};
    if (text.empty() ||
        text.find_first_not_of(" \t\n\r") == std::string::npos)
        return {{"error", "empty text"}};

    // Optional model switch through the header dropdown, so the selection
    // persists and the status bar follows exactly like a manual pick.
    if (!model.empty()) {
        auto list = std::dynamic_pointer_cast<Gtk::StringList>(
            model_dropdown_.get_model());
        std::vector<std::string> available;
        bool found = false;
        if (list)
            for (guint i = 0; i < list->get_n_items(); ++i) {
                const std::string name = list->get_string(i);
                available.push_back(name);
                if (name == model) {
                    model_dropdown_.set_selected(i);
                    found = true;
                }
            }
        if (!found)
            return {{"error", "unknown model '" + model + "'"},
                    {"available", available}};
    }
    const std::string display = current_model_display();
    if (display.empty())
        return {{"error",
                "no model selected - configure one in Settings > Models"}};

    error_banner_.set_visible(false);

    // finished fires on the turn thread; capture under a mutex.
    std::mutex m;
    std::string reason;
    bool done = false;
    auto conn = controller_->finished.connect([&](const std::string& r) {
        std::lock_guard lock(m);
        reason = r;
        done = true;
    });

    controller_->send_message(text, display);

    // Nested main-context wait. A bare blocking iteration can sleep
    // forever when no source is ready (a turn that never finishes), so a
    // 50 ms timeout source guarantees the loop wakes — same
    // pattern as eval_page_json's freeze fix.
    auto* ctx = g_main_context_default();
    const gint64 deadline =
        g_get_monotonic_time() + static_cast<gint64>(timeout_s) * G_USEC_PER_SEC;
    const guint waker = g_timeout_add(
        50, G_SOURCE_FUNC(+[] { return G_SOURCE_CONTINUE; }), nullptr);
    while (true) {
        {
            std::lock_guard lock(m);
            if (done) break;
        }
        if (g_get_monotonic_time() >= deadline) break;
        g_main_context_iteration(ctx, TRUE);
    }
    g_source_remove(waker);
    conn.disconnect();

    nlohmann::json out = {{"model", display}};
    {
        std::lock_guard lock(m);
        if (done) out["reason"] = reason;
    }
    const std::string terr = controller_->last_terminal_error();
    if (!terr.empty()) out["terminal_error"] = terr;
    if (!out.contains("reason"))
        out["error"] = "timeout after " + std::to_string(timeout_s) +
                         "s (turn still running)";
    return out;
}

// §READ-IMAGE — gated on the plugin's vision table so a text-only model
// gets a clear note here instead of daedalus silently dropping the image.
void ChatPanel::on_attach_image() {
    if (!current_model_supports_vision()) {
        add_info_bubble("The selected model (" + current_model_display() +
                        ") doesn't support image input.");
        return;
    }

    auto* toplevel = dynamic_cast<Gtk::Window*>(get_root());
    if (!toplevel) return;

    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Attach image");
    auto images = Gtk::FileFilter::create();
    images->set_name("Images");
    for (const char* mime : {"image/png", "image/jpeg", "image/gif",
                             "image/bmp", "image/webp"})
        images->add_mime_type(mime);
    auto any = Gtk::FileFilter::create();
    any->set_name("All files");
    any->add_pattern("*");
    auto filters = Gio::ListStore<Gtk::FileFilter>::create();
    filters->append(images);
    filters->append(any);
    dialog->set_filters(filters);
    dialog->set_default_filter(images);

    dialog->open(*toplevel, [this, dialog](Glib::RefPtr<Gio::AsyncResult>& result) {
        Glib::RefPtr<Gio::File> file;
        try {
            file = dialog->open_finish(result);
        } catch (const Gtk::DialogError&) {
            return; // cancelled
        } catch (const Glib::Error& e) {
            show_error(std::string("Attach image: ") + e.what());
            return;
        }
        if (!file) return;
        const std::string path = file->get_path();
        if (path.empty()) return; // non-local file — nothing daedalus can read
        const std::string name = Glib::path_get_basename(path);

        // Append on a fresh line when the input already has text; the
        // markdown stays visible and editable before sending.
        auto buffer = input_.get_buffer();
        std::string text = buffer->get_text();
        if (!text.empty() && text.back() != '\n') text += '\n';
        text += "![" + name + "](" + path + ")";
        buffer->set_text(text);
        input_.grab_focus();
    });
}

bool ChatPanel::current_model_supports_vision() {
    const std::string display = current_model_display();
    if (display.empty()) return true;

    auto& registry = daedalus::SettingsStore::instance();
    const auto models = model_bridge::list_models(registry, services_.store);
    const UiModel* model = nullptr;
    for (const auto& m : models)
        if (model_bridge::display_name(m) == display) { model = &m; break; }
    if (!model) return true; // unknown — let the API decide

    std::string host;
    for (const auto& prov : model_bridge::list_providers(registry))
        if (prov.name == model->provider) { host = prov.host; break; }

    const auto plugin =
        daedalus::ProviderRegistry::resolve(model->provider, host);
    if (!plugin) return true;
    for (const auto& info : plugin->known_models()) {
        const std::string& id = info.model_id;
        if (id.size() == model->model_id.size() &&
            std::equal(id.begin(), id.end(), model->model_id.begin(),
                       [](char x, char y) {
                           return std::tolower(static_cast<unsigned char>(x)) ==
                                  std::tolower(static_cast<unsigned char>(y));
                       }))
            return info.supports_vision;
    }
    return true; // not in the plugin table — assumed vision-capable
}

std::string ChatPanel::current_model_display() {
    if (auto item = model_dropdown_.get_selected_item())
        return std::dynamic_pointer_cast<Gtk::StringObject>(item)->get_string();
    return {};
}

// Transient assistant-style note; not persisted to history (§7.8).
void ChatPanel::add_info_bubble(const std::string& text) {
    if (welcome_) {
        messages_.remove(*welcome_);
        welcome_ = nullptr;
    }
    auto* bubble = Gtk::make_managed<ChatMessage>(ChatMessage::Role::Assistant);
    bubble->set_text(text);
    bubble->finalize();
    messages_.append(*bubble);
    active_assistant_ = nullptr;
    scroll_to_bottom();
}

void ChatPanel::run_command(const std::string& name) {
    error_banner_.set_visible(false);
    // §7.5 — input is no longer disabled mid-turn, so commands are
    // reachable while the agent works. Most are idle-only (they inspect
    // or rebuild the session); /help is pure UI and /compress is
    // explicitly supported mid-turn by daedalus (§7.8).
    if (processing_ && name != "help" && name != "compress") {
        add_info_bubble("**/" + name + "** is only available while the agent "
                        "is idle — typed messages are queued and picked up "
                        "when the current turn ends.");
        return;
    }

    if (name == "clear") {
        clear_history();
        return;
    }
    if (name == "init") {
        // Base prompt mirrors the icarus CLI's /init (docs/cli.md §3);
        // handed to the agent as a normal message so Stop cancels it.
        // Plan.md P2: the project type tunes what AGENTS.md should cover
        // (a film workspace has no build commands; a C++ one does).
        std::string prompt =
            "Please analyze this project and create or update the "
            "AGENTS.md file. The file should include: project overview, "
            "tech stack, build and test commands, project structure, "
            "code style and conventions, and any other relevant "
            "information for AI coding agents. First explore the "
            "codebase (look at CMakeLists.txt, README, source files, "
            "etc.), then write a comprehensive AGENTS.md using the "
            "execute_bash tool.";
        const auto& ws = services_.files.workspace();
        if (!ws.empty()) {
            if (const auto* type =
                    services_.project_types.detect(ws.string())) {
                if (type->id == "film")
                    prompt =
                        "Please analyze this filmmaking project and create "
                        "or update the AGENTS.md file. The file should "
                        "include: project overview (script title, format, "
                        "genre, logline), document inventory (screenplays "
                        "in .fountain, treatments/shot lists/notes in "
                        "Markdown), project structure, the current draft "
                        "status, and any conventions an AI assistant should "
                        "follow (Fountain syntax for scripts, Markdown for "
                        "everything else). First explore the workspace "
                        "files, then write the AGENTS.md using the "
                        "execute_bash tool. Keep the existing "
                        "<!-- monkeybyte:type: film --> comment at the top "
                        "if one is present.";
                else if (type->id == "web")
                    prompt +=
                        " This is a web project: document package.json "
                        "scripts, the framework, and the dev/build/test "
                        "commands.";
                else if (type->id == "cpp")
                    prompt +=
                        " This is a C++ project: document the CMake "
                        "targets, C++ standard, and warning/test setup.";
                if (type->id != "film")
                    prompt +=
                        " Keep the <!-- monkeybyte:type: " + type->id +
                        " --> comment at the top if one is present.";
            }
        }
        controller_->send_message(prompt, current_model_display());
        return;
    }
    if (name == "compress") {
        if (!controller_->has_agent()) {
            add_info_bubble("Nothing to compress yet — start a session first.");
            return;
        }
        // §7.8 — daedalus compresses the history internally: immediately
        // when idle, at the current cycle's end mid-turn. The outcome
        // arrives via the strip ("Compacting…") and the compaction bubble
        // on success, or an error bubble when the request was rejected
        // (too short) / the summary call failed — compress_context()
        // returns false only for the up-front rejections, which carry
        // their own message.
        if (controller_->compress_context())
            add_info_bubble("🗜 Compression requested — daedalus is compressing "
                            "the context.");
        return;
    }
    if (name == "help") {
        std::string text = "Available commands:\n";
        for (const auto& c : user_commands())
            text += "- **/" + c.name + "** — " + c.description + "\n";
        add_info_bubble(text);
        return;
    }
    if (name == "usage") {
        const auto s = controller_->stats();
        if (!s) {
            add_info_bubble("No session yet — send a message first.");
            return;
        }
        std::string text;
        {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "Iteration %d/%d · tokens %d (prompt %d, completion %d) · "
                          "context %d/%d · tool calls %d done, %d failed",
                          s->iteration, s->max_iterations, s->total_tokens,
                          s->prompt_tokens, s->completion_tokens, s->total_tokens,
                          s->max_context_size, s->completed_tool_calls,
                          s->failed_tool_calls);
            text = buf;
        }
        // §ContextCache — server-side prompt-cache hits (only shown once the
        // provider actually reports any; §ProviderPlugins plugin name too).
        if (s->cached_tokens > 0)
            text += " · cached " + std::to_string(s->cached_tokens);
        if (const auto plugin = controller_->active_plugin(); !plugin.empty())
            text += " · plugin " + plugin;
        add_info_bubble(text);
        return;
    }
    if (name == "tools") {
        const auto tools = controller_->tools();
        if (tools.empty()) {
            add_info_bubble("No session yet — send a message first.");
            return;
        }
        std::string text = "Registered tools:\n";
        for (const auto& t : tools) text += "- **" + t.name + "** — " + t.description + "\n";
        add_info_bubble(text);
        return;
    }
    if (name == "skills") {
        const auto skills = controller_->skills();
        if (skills.empty()) {
            add_info_bubble("No skills loaded.");
            return;
        }
        std::string text = "Loaded skills:\n";
        for (const auto& s : skills) text += "- **" + s.name + "** — " + s.description + "\n";
        add_info_bubble(text);
        return;
    }
    if (name == "mcp") {
        const auto servers = controller_->mcp_servers();
        if (servers.empty()) {
            add_info_bubble("No MCP servers configured. Add them in Settings → MCP Servers.");
            return;
        }
        std::string text = "Configured MCP servers:\n";
        for (const auto& s : servers) text += "- " + s + "\n";
        add_info_bubble(text);
        return;
    }
    add_info_bubble("Unknown command /" + name + " — try /help");
}

void ChatPanel::cancel_session() {
    if (controller_) controller_->cancel();
}

void ChatPanel::reset_agent() {
    // Agent teardown only; chat AND the persisted daedalus session are
    // kept so the rebuilt agent resumes the visible conversation.
    if (controller_) controller_->clear(false);
}

void ChatPanel::clear_history() {
    // Also wipes daedalus's persisted session (§7.8) so the next agent
    // build doesn't silently restore the cleared conversation.
    if (controller_) controller_->clear(true);
    stop_activity();
    tool_cards_.clear();
    active_assistant_ = nullptr;
    while (auto* child = messages_.get_first_child())
        messages_.remove(*child);
    history_.clear();
    refresh_counts();
    build_messages_welcome_only();
}

void ChatPanel::build_messages_welcome_only() {
    welcome_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    welcome_->set_valign(Gtk::Align::CENTER);
    welcome_->set_vexpand(true);
    auto* hi = Gtk::make_managed<Gtk::Label>("👋 Welcome to MonkeyByte!");
    auto* sub = Gtk::make_managed<Gtk::Label>(
        "I'm monkey-agent, your AI coding assistant.");
    sub->add_css_class("muted");
    welcome_->append(*hi);
    welcome_->append(*sub);
    messages_.append(*welcome_);
}

// §HISTORY-SNAPSHOT (daedalus 2b25f27, our REQ-2): render the transcript
// from the agent's own history snapshot — the conversation daedalus
// actually restored (post-compression, post-sanitization, tool calls
// elided) — instead of a host-side parallel JSON store that silently
// diverged after compression. Builds the agent eagerly for the selected
// model; with no configured model the snapshot is empty and the welcome
// screen stays.
void ChatPanel::restore_history() {
    const auto snapshot =
        controller_->history_snapshot(current_model_display());
    if (snapshot.empty()) return;

    if (welcome_) {
        messages_.remove(*welcome_);
        welcome_ = nullptr;
    }
    for (const auto& e : snapshot) {
        if (e.role == "system" || e.content.empty()) continue;
        auto* bubble = Gtk::make_managed<ChatMessage>(
            e.role == "user" ? ChatMessage::Role::User
                             : ChatMessage::Role::Assistant);
        bubble->set_text(e.content);
        // §MSG-HEADERS — `_meta` origin restored from the snapshot;
        // empty for messages persisted before headers existed.
        bubble->set_origin(e.source, e.received_at);
        bubble->finalize();
        messages_.append(*bubble);
        history_.push_back({e.role, e.content, e.source, e.received_at});
    }
    refresh_counts();
    scroll_to_bottom_forced(); // startup: show the latest messages (§7.3)
}

void ChatPanel::refresh_counts() {
    const int users = static_cast<int>(std::count_if(
        history_.begin(), history_.end(),
        [](const auto& h) { return h.role == "user"; }));
    const int msgs = static_cast<int>(history_.size());
    if (users != user_turns_ || msgs != total_messages_) {
        user_turns_ = users;
        total_messages_ = msgs;
        signal_counts_changed_.emit(user_turns_, total_messages_);
    }
}

} // namespace monkeybyte
