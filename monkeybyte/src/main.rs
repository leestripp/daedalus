//! MonkeyByte-Rust — Phase 1/2: daemon bridge + chat UI shell.
//! Full plan: PLAN.md. Reference: ../monkeybyte-electron (retired).

mod chat;
mod daemon;
mod log;
mod narration;
mod tts;
mod workspace;

use chat::{Bubble, Chat, Role};
use eframe::egui;
use std::sync::atomic::AtomicBool;
use std::sync::{mpsc, Arc};

fn main() -> eframe::Result<()> {
    log::init();
    let options = eframe::NativeOptions::default();
    eframe::run_native(
        "MonkeyByte (Rust)",
        options,
        Box::new(|_cc| Ok(Box::new(MonkeyByteApp::new()))),
    )
}

struct MonkeyByteApp {
    chat: Chat,
    input: String,
    events: mpsc::Receiver<daemon::Event>,
    daemon: Option<daemon::Daemon>,
    events_cancel: Arc<AtomicBool>,
    status_line: String,
    narrator: Option<tts::Narrator>,
    speech_buffer: String,
    sessions: Vec<(String, String)>,
    active_session: Option<String>,
    workspace_root: Option<std::path::PathBuf>,
    workspace_expanded: std::collections::HashSet<std::path::PathBuf>,
    left_panel: LeftPanel,
    renaming: bool,
    rename_buffer: String,
    inspect: bool,
    /// Live rectangles of every major region, captured each frame from the
    /// panel show() responses — the UI-position ground truth.
    layout_rects: Vec<(String, egui::Rect)>,
}

#[derive(Clone, Copy, PartialEq)]
enum LeftPanel {
    Workspace,
    Search,
    Git,
    Settings,
}

impl MonkeyByteApp {
    fn new() -> Self {
        let (tx, rx) = mpsc::channel();
        let cancel = Arc::new(AtomicBool::new(false));
        let mut app = Self {
            chat: Chat::default(),
            input: String::new(),
            events: rx,
            daemon: None,
            events_cancel: cancel,
            status_line: "starting daemon...".to_string(),
            narrator: None,
            speech_buffer: String::new(),
            sessions: Vec::new(),
            active_session: None,
            workspace_root: None,
            workspace_expanded: std::collections::HashSet::new(),
            left_panel: LeftPanel::Workspace,
            renaming: false,
            rename_buffer: String::new(),
            inspect: false,
            layout_rects: Vec::new(),
        };
        let home = std::env::var("HOME").unwrap_or_default();
        let executable = format!("{home}/.local/lib/monkeybyte-electron/resources/backend/monkeybyte-electron-backend");
        let data_dir = format!("{home}/.config/monkeybyte-electron/backend");
        // One backend at a time — kill any orphaned Electron-era daemon
        // (its parent window is gone; it only risks data-dir races now).
        let _ = std::process::Command::new("pkill")
            .args(["-x", "monkeybyte-electron-backend"])
            .status();
        std::thread::sleep(std::time::Duration::from_millis(400));
        let ui_port = match daemon::start_ui_bridge() {
            Ok(port) => port,
            Err(err) => {
                app.status_line = format!("ui bridge failed: {err}");
                log::log(&format!("UI BRIDGE FAILED: {err}"));
                return app;
            }
        };
        log::log(&format!("ui bridge on port {ui_port}"));
        log::log(&format!("spawning daemon: {executable}"));
        match daemon::spawn(&executable, &data_dir, ui_port) {
            Ok((daemon, _ui_token)) => {
                log::log(&format!("daemon up: port={}", daemon.port));
                daemon.events(app.events_cancel.clone(), tx);
                app.status_line = format!("daemon ready on port {}", daemon.port);
                app.narrator = narrator_from_settings(&daemon);
                log::log(&format!("narrator: {}", if app.narrator.is_some() { "enabled" } else { "disabled" }));
                let (sessions, active) = load_sessions(&daemon);
                app.sessions = sessions;
                app.active_session = active;
                if let Ok(settings) = daemon.request("GET", "/v1/settings", &serde_json::Value::Null) {
                    if let Some(root) = settings["workspace"].as_str() {
                        app.workspace_root = Some(std::path::PathBuf::from(root));
                        log::log(&format!("workspace root: {root}"));
                    }
                }
                restore_history(&mut app.chat, &daemon);
                log::log(&format!("startup ok: {} bubbles, {} sessions", app.chat.bubbles.len(), app.sessions.len()));
                app.daemon = Some(daemon);
            }
            Err(error) => {
                log::log(&format!("daemon SPAWN FAILED: {error}"));
                app.status_line = format!("daemon failed: {error}");
            }
        }
        app
    }

    fn send(&mut self) {
        let text = std::mem::take(&mut self.input);
        if text.trim().is_empty() {
            return;
        }
        if let Some(narrator) = &self.narrator {
            narrator.cancel();
        }
        let was_busy = self.chat.busy;  // before the optimistic update below
        self.chat.busy = true;
        self.chat.bubbles.push(Bubble { role: Role::User, text: text.clone(), live: false });
        self.chat.bubbles.push(Bubble { role: Role::Assistant, text: String::new(), live: true });
        let body = serde_json::json!({ "text": text, "attachments": [] });
        let path = if was_busy { "/v1/chat/steer" } else { "/v1/chat/send" };
        log::log(&format!("POST {path}"));
        if let Some(daemon) = &self.daemon {
            if let Err(error) = daemon.request("POST", path, &body) {
                self.status_line = format!("send failed: {error}");
                self.chat.busy = false;
                self.chat.seal();
            }
        }
    }

    fn stop(&mut self) {
        if let Some(daemon) = &self.daemon {
            log::log("POST /v1/chat/cancel");
            let _ = daemon.request("POST", "/v1/chat/cancel", &serde_json::Value::Null);
        }
        if let Some(narrator) = &self.narrator {
            narrator.cancel();
        }
        self.chat.busy = false;
        self.chat.seal();
    }

    fn delete_session(&mut self) {
        let Some(id) = self.active_session.clone() else { return };
        if let Some(daemon) = self.daemon.as_ref() {
            log::log(&format!("POST /v1/sessions/delete {id}"));
            let _ = daemon.request("POST", "/v1/sessions/delete", &serde_json::json!({ "id": id }));
            let (sessions, active) = load_sessions(daemon);
            self.sessions = sessions;
            self.active_session = active;
            restore_history(&mut self.chat, daemon);
        }
    }

    fn rename_session(&mut self, name: &str) {
        let Some(id) = self.active_session.clone() else { return };
        if let Some(daemon) = self.daemon.as_ref() {
            log::log(&format!("POST /v1/sessions/rename {id}"));
            let _ = daemon.request("POST", "/v1/sessions/rename", &serde_json::json!({ "id": id, "name": name }));
            let (sessions, active) = load_sessions(daemon);
            self.sessions = sessions;
            self.active_session = active;
        }
    }

    /// Switch to another chat session: server select, then reload transcript.
    fn select_session(&mut self, id: &str) {
        if let Some(narrator) = &self.narrator {
            narrator.cancel();
        }
        if let Some(daemon) = self.daemon.as_ref() {
            let _ = daemon.request("POST", "/v1/sessions/select", &serde_json::json!({ "id": id }));
            self.active_session = Some(id.to_string());
            restore_history(&mut self.chat, daemon);
        }
    }

    fn create_session(&mut self) {
        if let Some(daemon) = self.daemon.as_ref() {
            let _ = daemon.request(
                "POST",
                "/v1/sessions/create",
                &serde_json::json!({ "workspace": "default", "name": "New Chat" }),
            );
            let (sessions, active) = load_sessions(daemon);
            self.sessions = sessions;
            self.active_session = active;
            restore_history(&mut self.chat, daemon);
        }
    }

    /// Feed assistant text to the narrator: complete sentences speak as they
    /// arrive; the remainder stays buffered until the turn ends (Release).
    fn narrate(&mut self, delta: &str) {
        if self.narrator.is_none() {
            return;
        }
        self.speech_buffer.push_str(delta);
        let (segments, remainder) = narration::drain_segments(&self.speech_buffer);
        self.speech_buffer = remainder;
        if let Some(narrator) = &self.narrator {
            for segment in segments {
                let plain = narration::to_speech_text(&segment);
                if !plain.is_empty() {
                    narrator.speak(&plain);
                }
            }
        }
    }

    fn drain_events(&mut self) {
        while let Ok(event) = self.events.try_recv() {
            match event {
                daemon::Event::AssistantDelta(payload) => {
                    if let Some(text) = payload["text"].as_str() {
                        self.narrate(text);
                    }
                    self.chat.apply_delta(&payload);
                }
                daemon::Event::UserMessage(payload) => self.chat.user_message(&payload),
                daemon::Event::Status(payload) => {
                    let was_busy = self.chat.busy;
                    self.chat.apply_status(&payload);
                    if was_busy && !self.chat.busy {
                        // Turn ended: speak the buffered remainder, then
                        // release any held prebuffer segment immediately.
                        if let Some(narrator) = &self.narrator {
                            if !self.speech_buffer.trim().is_empty() {
                                let plain = narration::to_speech_text(&self.speech_buffer);
                                if !plain.is_empty() {
                                    narrator.speak(&plain);
                                }
                            }
                            narrator.release();
                            self.speech_buffer.clear();
                        }
                    }
                }
                daemon::Event::Other(kind, _payload) => {
                    self.status_line = format!("event: {kind}");
                }
                daemon::Event::BackendError(message) => {
                    self.status_line = message;
                }
            }
        }
    }
}

/// Prefill the transcript from the daemon's history (array of {role, content}).
fn restore_history(chat: &mut Chat, daemon: &daemon::Daemon) {
    chat.bubbles.clear();
    let Ok(history) = daemon.request("GET", "/v1/history", &serde_json::Value::Null) else {
        return;
    };
    let entries = history
        .as_array()
        .cloned()
        .or_else(|| history["history"].as_array().cloned())
        .unwrap_or_default();
    for entry in entries {
        let role = match entry["role"].as_str() {
            Some("user") => Role::User,
            _ => Role::Assistant,
        };
        let content = entry["content"].as_str().unwrap_or("").to_string();
        if !content.trim().is_empty() {
            chat.bubbles.push(Bubble { role, text: content, live: false });
        }
    }
    chat.seal();
}

/// Build a narrator from the daemon's chimpflow settings, if enabled.
/// Field names mirror ../monkeybyte-electron/src/main/chimpflow.js.
fn narrator_from_settings(daemon: &daemon::Daemon) -> Option<tts::Narrator> {
    let settings = daemon.request("GET", "/v1/settings", &serde_json::Value::Null).ok()?;
    log::log("GET /v1/settings ok");
    let cf = settings["chimpflow"].as_object()?;
    if !cf["enabled"].as_bool().unwrap_or(false) {
        return None;
    }
    let host = cf["serverUrl"].as_str().unwrap_or("http://127.0.0.1");
    let port = cf["port"].as_u64().unwrap_or(8090);
    let cf = tts::ChimpFlow {
        base: format!("{host}:{port}"),
        api_key: cf["apiKey"].as_str().unwrap_or("").to_string(),
        tts_model: cf["ttsModel"].as_str().unwrap_or("kokoro-82m").to_string(),
        voice: cf["voice"].as_str().unwrap_or("af_heart").to_string(),
    };
    tts::Narrator::spawn(cf).ok()
}

/// GET /v1/sessions → ([(id, name)], active id). Values, not refs to `self`:
/// callers assign the fields while still holding a `&self.daemon` borrow.
fn load_sessions(daemon: &daemon::Daemon) -> (Vec<(String, String)>, Option<String>) {
    let Ok(resp) = daemon.request("GET", "/v1/sessions", &serde_json::Value::Null) else {
        return (Vec::new(), None);
    };
    let entries = resp["sessions"].as_array().cloned()
        .or_else(|| resp.as_array().cloned())
        .unwrap_or_default();
    let sessions = entries
        .iter()
        .filter_map(|e| Some((e["id"].as_str()?.to_string(), e["name"].as_str()?.to_string())))
        .collect();
    (sessions, resp["activeId"].as_str().map(String::from))
}

impl eframe::App for MonkeyByteApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        self.drain_events();

        // Status bar — Electron footer: git · agent state (right: context %).
        let mut rects: Vec<(String, egui::Rect)> = Vec::new();
        egui::TopBottomPanel::bottom("status").show(ctx, |ui| {
            ui.horizontal(|ui| {
                ui.small("git: —");
                ui.separator();
                if self.chat.busy {
                    ui.small("Agent Busy");
                } else {
                    ui.small("Agent Idle");
                }
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    ui.small(format!("context {:.0}%", self.chat.context_percent));
                    if ui
                        .selectable_label(self.inspect, "Inspect")
                        .on_hover_text("UI inspector: positions, widgets, hover ids")
                        .clicked()
                    {
                        self.inspect = !self.inspect;
                    }
                });
            });
        });

        // Activity bar — Electron `nav.activity-bar` icon rail.
        let r = egui::SidePanel::left("activity")
            .width_range(30.0..=48.0)
            .default_width(40.0)
            .show(ctx, |ui| {
                ui.vertical_centered(|ui| {
                    ui.add_space(4.0);
                    if ui
                        .selectable_label(matches!(self.left_panel, LeftPanel::Workspace), "\u{1f4c1}")
                        .on_hover_text("Workspace (Ctrl+1)")
                        .clicked()
                    {
                        self.left_panel = LeftPanel::Workspace;
                    }
                    if ui
                        .selectable_label(matches!(self.left_panel, LeftPanel::Search), "\u{2315}")
                        .on_hover_text("Search (Ctrl+2)")
                        .clicked()
                    {
                        self.left_panel = LeftPanel::Search;
                    }
                    if ui
                        .selectable_label(matches!(self.left_panel, LeftPanel::Git), "\u{2442}")
                        .on_hover_text("Source Control (Ctrl+3)")
                        .clicked()
                    {
                        self.left_panel = LeftPanel::Git;
                    }
                    ui.add_space((ui.available_height() - 44.0).max(0.0));
                    if ui
                        .selectable_label(matches!(self.left_panel, LeftPanel::Settings), "\u{2699}")
                        .on_hover_text("Settings (Ctrl+4)")
                        .clicked()
                    {
                        self.left_panel = LeftPanel::Settings;
                    }
                });
            });
        rects.push(("activity".into(), r.response.rect));

        // Sidebar — one page per activity selection (Electron `aside#sidebar`).
        let r = egui::SidePanel::left("sidebar")
            .resizable(true)
            .width_range(140.0..=460.0)
            .default_width(240.0)
            .show(ctx, |ui| {
                match self.left_panel {
                    LeftPanel::Workspace => {
                        ui.horizontal(|ui| {
                            ui.heading("Workspace");
                            if ui.button("\u{1f4c2}").on_hover_text("Open workspace").clicked() {}
                            if ui.button("\u{21bb}").on_hover_text("Refresh workspace").clicked() {
                                self.workspace_expanded.clear();
                            }
                        });
                        if let Some(root) = self.workspace_root.clone() {
                            ui.small(root.display().to_string());
                            ui.separator();
                            egui::ScrollArea::vertical().show(ui, |ui| {
                                workspace::tree_ui(ui, &root, &mut self.workspace_expanded);
                            });
                        } else {
                            ui.weak("Choose a workspace to begin.");
                        }
                    }
                    LeftPanel::Search => {
                        ui.heading("Search");
                        ui.separator();
                        ui.weak("Search lands in a later phase.");
                    }
                    LeftPanel::Git => {
                        ui.horizontal(|ui| {
                            ui.heading("Source Control");
                            if ui.button("\u{21bb}").on_hover_text("Refresh Git status").clicked() {}
                        });
                        ui.separator();
                        ui.weak("Open a Git workspace to see changes.");
                    }
                    LeftPanel::Settings => {
                        ui.heading("Settings");
                        ui.separator();
                        ui.weak("Settings land in a later phase.");
                    }
                }
            });
        rects.push(("sidebar".into(), r.response.rect));

        // Chat panel — Electron `aside#chat-panel` (right side).
        let r = egui::SidePanel::right("chat")
            .resizable(true)
            .width_range(300.0..=800.0)
            .default_width(400.0)
            .show(ctx, |ui| {
                ui.vertical(|ui| {
                    ui.horizontal(|ui| {
                        ui.heading("Agent");
                        if ui.button("\u{ff0b}").on_hover_text("New chat").clicked() {
                            self.create_session();
                        }
                    });
                    // Session row: select + rename + delete (Electron chat-session-row).
                    ui.horizontal(|ui| {
                        let active_name = self
                            .sessions
                            .iter()
                            .find(|(id, _)| Some(id) == self.active_session.as_ref())
                            .map(|(_, n)| n.clone())
                            .unwrap_or_else(|| "New Chat".to_string());
                        let sessions = self.sessions.clone();
                        egui::ComboBox::from_id_salt("session-select")
                            .selected_text(active_name)
                            .show_ui(ui, |ui| {
                                for (id, name) in sessions {
                                    let active = self.active_session.as_deref() == Some(id.as_str());
                                    if ui.selectable_label(active, &name).clicked() && !active {
                                        self.select_session(&id);
                                    }
                                }
                            });
                        if ui.button("\u{270e}").on_hover_text("Rename chat").clicked() {
                            self.rename_buffer = self
                                .sessions
                                .iter()
                                .find(|(id, _)| Some(id) == self.active_session.as_ref())
                                .map(|(_, n)| n.clone())
                                .unwrap_or_default();
                            self.renaming = true;
                        }
                        if ui.button("\u{1f5d1}").on_hover_text("Delete chat").clicked() {
                            self.delete_session();
                        }
                    });
                    ui.separator();

                    let composer_h = 92.0;
                    egui::ScrollArea::vertical()
                        .id_salt("chat-messages")
                        .auto_shrink([false; 2])
                        .stick_to_bottom(true)
                        .max_height((ui.available_height() - composer_h).max(80.0))
                        .show(ui, |ui| {
                            for bubble in &self.chat.bubbles {
                                let label = match bubble.role {
                                    Role::User => "You",
                                    Role::Assistant => "Byte",
                                    Role::Tool => "Tool",
                                    Role::Notice => "\u{2022}",
                                };
                                ui.group(|ui| {
                                    ui.label(egui::RichText::new(label).strong());
                                    let mut text = bubble.text.clone();
                                    if bubble.live {
                                        text.push_str(" \u{258c}");
                                    }
                                    ui.label(text);
                                });
                            }
                        });

                    ui.horizontal(|ui| {
                        if self.chat.busy {
                            ui.spinner();
                            ui.label(&self.chat.state);
                        } else {
                            ui.weak(&self.status_line);
                        }
                    });

                    let response = ui.add(
                        egui::TextEdit::multiline(&mut self.input)
                            .desired_rows(2)
                            .desired_width(f32::INFINITY)
                            .hint_text("Message the agent \u{00b7} Enter to send \u{00b7} Shift+Enter for newline"),
                    );
                    let enter_send = ui.input(|i| i.key_pressed(egui::Key::Enter) && !i.modifiers.shift);
                    ui.horizontal(|ui| {
                        ui.weak("/");
                        if ui.button("\u{1f4ce}").on_hover_text("Attach image").clicked() {}
                        if ui.button("\u{1f3a4}").on_hover_text("Dictation").clicked() {}
                        ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                            if ui.button("Send").clicked()
                                || (response.lost_focus() && enter_send)
                            {
                                self.send();
                            }
                            if ui.add_enabled(self.chat.busy, egui::Button::new("Stop")).clicked() {
                                self.stop();
                            }
                            if ui.add_enabled(self.chat.busy, egui::Button::new("Steer")).clicked() {
                                self.send(); // busy → daemon::send() routes to /v1/chat/steer
                            }
                        });
                    });
                });
            });
        rects.push(("chat".into(), r.response.rect));

        // Main area — editor surface (Electron `main#main-area`).
        let r = egui::CentralPanel::default().show(ctx, |ui| {
            ui.horizontal(|ui| {
                ui.heading("MonkeyByte");
            });
            ui.separator();
            ui.weak(
                "Editor \u{2014} open a file from the workspace tree. File viewing and editing land in a later phase.",
            );
        });
        rects.push(("editor".into(), r.response.rect));
        self.layout_rects = rects;

        // Rename popup (Electron rename flow).
        if self.renaming {
            let mut open = true;
            let mut close = false;
            egui::Window::new("Rename chat")
                .collapsible(false)
                .resizable(false)
                .open(&mut open)
                .show(ctx, |ui| {
                    ui.horizontal(|ui| {
                        ui.label("Name:");
                        ui.text_edit_singleline(&mut self.rename_buffer);
                    });
                    ui.horizontal(|ui| {
                        if ui.button("Save").clicked() && !self.rename_buffer.trim().is_empty() {
                            let name = self.rename_buffer.trim().to_string();
                            self.rename_session(&name);
                            close = true;
                        }
                        if ui.button("Cancel").clicked() {
                            close = true;
                        }
                    });
                });
            if !open || close {
                self.renaming = false;
            }
        }

        if self.inspect {
            ctx.set_debug_on_hover(true);
            egui::Window::new("UI Inspector")
                .default_pos(ctx.screen_rect().left_top() + egui::vec2(60.0, 60.0))
                .show(ctx, |ui| {
                    let pointer = ctx.pointer_latest_pos().unwrap_or(egui::Pos2::ZERO);
                    ui.monospace(format!("pointer: ({:.0}, {:.0})", pointer.x, pointer.y));
                    ui.monospace(format!("screen:  {:?}", ctx.screen_rect()));
                    ui.separator();
                    ui.label("regions");
                    for (name, rect) in &self.layout_rects {
                        ui.monospace(format!(
                            "{name:9} x {:.0}..{:.0}  y {:.0}..{:.0}  w {:.0} h {:.0}",
                            rect.min.x, rect.max.x, rect.min.y, rect.max.y,
                            rect.width(), rect.height(),
                        ));
                    }
                    ui.separator();
                    ui.label("interaction");
                    let keys: Vec<egui::Key> = ui.input(|i| i.keys_down.iter().copied().collect());
                    ui.monospace(format!("keys: {:?}", keys));
                });
        } else {
            ctx.set_debug_on_hover(false);
        }

        ctx.request_repaint_after(std::time::Duration::from_millis(120));
    }
}