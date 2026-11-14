//! Debug log — ~/.monkeybyte/rust-debug.log, like the Electron app's.
//! Instrumentation exists from the very start this time: every boundary
//! (spawn, handshake, SSE, request, narration) writes a line.

use std::fmt::Write as _;
use std::fs::{self, OpenOptions};
use std::io::Write as _;
use std::path::PathBuf;
use std::sync::Mutex;
use std::time::{SystemTime, UNIX_EPOCH};

static LOG: Mutex<Option<PathBuf>> = Mutex::new(None);

pub fn init() {
    let dir = dirs_home().join(".monkeybyte");
    let _ = fs::create_dir_all(&dir);
    let path = dir.join("rust-debug.log");
    *LOG.lock().expect("log lock") = Some(path.clone());
    log("=== MonkeyByte Rust starting ===");
    eprintln!("debug log: {}", path.display());
}

fn dirs_home() -> PathBuf {
    std::env::var_os("HOME").map(PathBuf::from).unwrap_or_else(|| PathBuf::from("/tmp"))
}

pub fn log(msg: &str) {
    let path = LOG.lock().expect("log lock").clone();
    let Some(path) = path else { return };
    let now = SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default();
    let secs = now.as_secs();
    let ts = format!(
        "{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}",
        1970 + secs / 31_557_600,
        (secs / 2_592_000) % 12 + 1,
        (secs / 86_400) % 31 + 1,
        (secs / 3600) % 24,
        (secs / 60) % 60,
        secs % 60,
        now.subsec_millis(),
    );
    let mut line = String::with_capacity(msg.len() + 32);
    let _ = write!(line, "{ts} {msg}\n");
    if let Ok(mut f) = OpenOptions::new().create(true).append(true).open(&path) {
        let _ = f.write_all(line.as_bytes());
    }
}
