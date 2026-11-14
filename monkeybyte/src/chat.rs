//! Chat transcript model — applies daemon events to bubbles.

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Role {
    User,
    Assistant,
    Tool,
    Notice,
}

#[derive(Debug, Clone)]
pub struct Bubble {
    pub role: Role,
    pub text: String,
    /// Still streaming — the UI may render a cursor.
    pub live: bool,
}

#[derive(Default)]
pub struct Chat {
    pub bubbles: Vec<Bubble>,
    pub busy: bool,
    pub state: String,
    pub context_percent: f32,
}

impl Chat {
    pub fn apply_delta(&mut self, payload: &serde_json::Value) {
        let text = payload["text"].as_str().unwrap_or("");
        if let Some(last) = self.bubbles.last_mut() {
            if last.role == Role::Assistant && last.live {
                last.text.push_str(text);
                return;
            }
        }
        self.bubbles.push(Bubble { role: Role::Assistant, text: text.to_string(), live: true });
    }

    pub fn user_message(&mut self, payload: &serde_json::Value) {
        // Any new user message ends the previous assistant bubble.
        if let Some(last) = self.bubbles.last_mut() {
            last.live = false;
        }
        let text = payload["text"].as_str().unwrap_or("").to_string();
        self.bubbles.push(Bubble { role: Role::User, text, live: false });
    }

    pub fn apply_status(&mut self, payload: &serde_json::Value) {
        self.busy = payload["busy"].as_bool().unwrap_or(false);
        self.state = payload["state"].as_str().unwrap_or("").to_string();
        if let Some(card) = payload["compression"].as_object().or_else(|| payload["context"].as_object()) {
            if let Some(pct) = card["percent"].as_f64().or_else(|| card["usage_percent"].as_f64()) {
                self.context_percent = pct as f32;
            }
        }
        if !self.busy {
            if let Some(last) = self.bubbles.last_mut() {
                last.live = false;
            }
        }
    }

    pub fn seal(&mut self) {
        if let Some(last) = self.bubbles.last_mut() {
            last.live = false;
        }
    }
}
