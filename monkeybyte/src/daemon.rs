//! Daemon bridge — spawn the agent backend, token-auth HTTP, SSE events.
//! Protocol reference: ../monkeybyte-electron/src/main/backend-client.js
//! The daemon is the proven agent brain; this app replaces only the shell.

use std::io::{BufRead, BufReader};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{mpsc, Arc};
use std::time::{Duration, Instant};

pub struct Daemon {
    #[allow(dead_code)]
    child: Child,
    pub port: u16,
    bridge_token: String,
}

#[derive(Debug)]
pub enum Event {
    UserMessage(serde_json::Value),
    AssistantDelta(serde_json::Value),
    Status(serde_json::Value),
    Other(String, serde_json::Value),
    BackendError(String),
}

fn hex_token() -> anyhow::Result<String> {
    use std::io::Read;
    let mut bytes = [0u8; 32];
    std::fs::File::open("/dev/urandom")?.read_exact(&mut bytes)?;
    Ok(bytes.iter().map(|b| format!("{b:02x}")).collect())
}

/// Spawn `executable --data-dir D --ui-port P`; read the daemon's chosen port
/// from the first stdout JSON line (15s handshake budget, as Electron had).
pub fn spawn(executable: &str, data_dir: &str, ui_port: u16) -> anyhow::Result<(Daemon, String)> {
    let bridge = hex_token()?;
    let ui = hex_token()?;
    let mut child = Command::new(executable)
        .args(["--data-dir", data_dir, "--ui-port", &ui_port.to_string()])
        .env("MONKEYBYTE_BRIDGE_TOKEN", &bridge)
        .env("MONKEYBYTE_UI_TOKEN", &ui)
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .stdin(Stdio::null())
        .spawn()?;
    let stdout = child.stdout.take().expect("piped stdout");
    let mut reader = BufReader::new(stdout);
    let deadline = Instant::now() + Duration::from_secs(15);
    loop {
        if Instant::now() > deadline {
            anyhow::bail!("daemon handshake timed out");
        }
        let mut line = String::new();
        if reader.read_line(&mut line)? == 0 {
            anyhow::bail!("daemon exited during handshake");
        }
        if let Ok(ready) = serde_json::from_str::<serde_json::Value>(&line) {
            if let Some(port) = ready["port"].as_u64() {
                return Ok((Daemon { child, port: port as u16, bridge_token: bridge }, ui));
            }
        }
    }
}

impl Daemon {
    pub fn request(&self, method: &str, path: &str, body: &serde_json::Value) -> anyhow::Result<serde_json::Value> {
        let url = format!("http://127.0.0.1:{}{}", self.port, path);
        let client = reqwest::blocking::Client::builder()
            .timeout(Duration::from_secs(30))
            .build()?;
        let req = match method {
            "POST" => client.post(&url).json(body),
            _ => client.get(&url),
        };
        let resp = req.header("X-MonkeyByte-Token", &self.bridge_token).send()?;
        let status = resp.status();
        let value: serde_json::Value = resp.json().unwrap_or(serde_json::Value::Null);
        if !status.is_success() {
            anyhow::bail!("daemon HTTP {status}: {value}");
        }
        Ok(value)
    }

    /// SSE event stream on its own thread, reconnecting on drops (§SSE-RECONNECT).
    pub fn events(&self, cancel: Arc<AtomicBool>, tx: mpsc::Sender<Event>) {
        let url = format!("http://127.0.0.1:{}/v1/events", self.port);
        let token = self.bridge_token.clone();
        std::thread::spawn(move || {
            while !cancel.load(Ordering::Relaxed) {
                if let Err(error) = stream_events(&url, &token, &cancel, &tx) {
                    if cancel.load(Ordering::Relaxed) {
                        break;
                    }
                    let _ = tx.send(Event::BackendError(format!("event stream lost ({error}); reconnecting")));
                    std::thread::sleep(Duration::from_secs(1));
                }
            }
        });
    }
}

fn stream_events(
    url: &str,
    token: &str,
    cancel: &AtomicBool,
    tx: &mpsc::Sender<Event>,
) -> anyhow::Result<()> {
    let client = reqwest::blocking::Client::builder()
        .timeout(Duration::from_secs(300))
        .build()?;
    let resp = client.get(url).header("X-MonkeyByte-Token", token).send()?;
    if !resp.status().is_success() {
        anyhow::bail!("events HTTP {}", resp.status());
    }
    let reader = BufReader::new(resp);
    let mut event_type = String::from("message");
    for line in reader.lines() {
        if cancel.load(Ordering::Relaxed) {
            anyhow::bail!("cancelled");
        }
        let line = line?;
        if let Some(t) = line.strip_prefix("event:") {
            event_type = t.trim().to_string();
            continue;
        }
        if let Some(data) = line.strip_prefix("data:") {
            if event_type == "ready" {
                continue;
            }
            let payload: serde_json::Value =
                serde_json::from_str(data.trim_start()).unwrap_or(serde_json::Value::Null);
            let event = match event_type.as_str() {
                "user.message" => Event::UserMessage(payload),
                "assistant.delta" => Event::AssistantDelta(payload),
                "status" => Event::Status(payload),
                other => Event::Other(other.to_string(), payload),
            };
            if tx.send(event).is_err() {
                anyhow::bail!("receiver dropped");
            }
        }
    }
    Ok(())
}
