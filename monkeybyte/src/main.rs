//! MonkeyByte-Rust — Phase 1/2: daemon bridge + chat UI shell.
//! Full plan: PLAN.md. Reference: ../monkeybyte-electron (retired).

mod chat;
mod daemon;
mod log;
mod narration;
mod tts;

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
        };
        let home = std::env::var("HOME").unwrap_or_default();
        let executable = format!("{home}/.local/lib/monkeybyte-electron/resources/backend/monkeybyte-electron-backend");
        let data_dir = format!("{home}/.config/monkeybyte-electron/backend");
        log::log(&format!("spawning daemon: {executable}"));
        match daemon::spawn(&executable, &data_dir, 0) {
            Ok((daemon, _ui_token)) => {
                log::log(&format!("daemon up: port={}", daemon.port));
                daemon.events(app.events_cancel.clone(), tx);
                app.status_line = format!("daemon ready on port {}", daemon.port);
                app.narrator = narrator_from_settings(&daemon);
                log::log(&format!("narrator: {}", if app.narrator.is_some() { "enabled" } else { "disabled" }));
                let (sessions, active) = load_sessions(&daemon);
                app.sessions = sessions;
                app.active_session = active;
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
        egui::SidePanel::left("sessions")
            .width_range(100.0..=300.0)
            .default_width(200.0)
            .show(ctx, |ui| {
                ui.horizontal(|ui| {
                    ui.label("Chats");
                    if ui.button("+").clicked() {
                        self.create_session();
                    }
                });
                ui.separator();
                egui::ScrollArea::vertical().show(ui, |ui| {
                    let sessions = self.sessions.clone();
                    for (id, name) in sessions {
                        let active = self.active_session.as_deref() == Some(id.as_str());
                        if ui.selectable_label(active, &name).clicked() && !active {
                            self.select_session(&id);
                        }
                    }
                });
            });

        egui::CentralPanel::default().show(ctx, |ui| {
            ui.horizontal(|ui| {
                ui.heading("MonkeyByte (Rust)");
                ui.separator();
                if self.chat.busy {
                    ui.spinner();
                    ui.label(&self.chat.state);
                    if ui.button("Stop").clicked() {
                        self.stop();
                    }
                } else {
                    ui.label("ready");
                }
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    ui.label(format!("context {:.0}%", self.chat.context_percent));
                });
            });
            ui.separator();
            egui::ScrollArea::vertical()
                .auto_shrink([false; 2])
                .stick_to_bottom(true)
                .show(ui, |ui| {
                    for bubble in &self.chat.bubbles {
                        let label = match bubble.role {
                            Role::User => "You",
                            Role::Assistant => "Byte",
                            Role::Tool => "Tool",
                            Role::Notice => "•",
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
            ui.separator();
            ui.horizontal(|ui| {
                let response = ui.add(
                    egui::TextEdit::singleline(&mut self.input)
                        .desired_width(f32::INFINITY)
                        .hint_text("Message Byte…"),
                );
                let send_clicked = ui.button("Send").clicked()
                    || (response.lost_focus() && ui.input(|i| i.key_pressed(egui::Key::Enter)));
                if send_clicked {
                    self.send();
                }
            });
            ui.label(egui::RichText::new(&self.status_line).small().weak());
        });
        ctx.request_repaint_after(std::time::Duration::from_millis(120));
    }
}
