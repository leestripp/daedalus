# MonkeyByte-Rust — build plan

Functionally identical to `../monkeybyte-electron`, except the functions work.
Reference implementation (read-only): `../monkeybyte-electron/src/{main,renderer}`.

## Architecture (why this won't repeat Electron's failures)

- **One Rust binary.** No renderer process, no IPC bridge, no JS event loop.
- **UI: egui** — immediate-mode, pure Rust, no webview/Chromium anywhere.
- **Audio: rodio.** A `Sink` is a real audio thread queue: samples play
  back-to-back gapless BY CONSTRUCTION; enqueue = schedule. No 'ended'-event
  timing, no AudioContext suspension, no autoplay policy. Cancel = `sink.stop()`.
- **Agent streaming:** one `std::thread` per turn running a blocking SSE read,
  events over `mpsc` channel to the UI. Stop = atomic flag + stream drop.
  The UI thread never blocks on I/O and I/O never blocks on UI.
- **TTS/STT:** HTTP to the system ChimpFlow service (same as before).

## Phases

- [x] **0. Scaffold** — cargo project, eframe window shell, this plan.
- [x] **1. Settings store** — JSON at `~/.config/monkeybyte-rust/settings.json`
      (agent base URL/key, model, system prompt, context %, chimpflow config,
      voice on/off). All panes read/write through it.
- [x] **2. Agent client + chat UI** — POST /v1/agent with session, SSE-parse
      `assistant.delta`/`status`/`turn_complete`, streaming markdown bubbles
      (egui_commonmark or minimal renderer), Stop button that actually stops,
      busy/spinner state, compression card (context %), multi-block tool
      interleave rendered as tool bubbles, error banner.
- [x] **3. TTS narration** — chimpflow `/v1/audio/speech` + `/v1/models` +
      `/v1/voices` (key from settings); sentence segmentation on punctuation
      AND newlines; speech-text sanitizer (strip bullets/backticks/hashes/URLs);
      up to 2 concurrent synths, rest FIFO; rodio Sink queue = gapless; cancel
      on new send; per-segment debug log with timings.
- [x] **4. STT dictation** — cpal mic capture → WAV → chimpflow whisper
      endpoint → text into input box; push-to-talk button with level meter.
- [~] **5. Projects/workspaces** (workspace tree ✅ 2026-11-14; `.mb/` files + file ops pending) — workspace tree, `.mb/` markdown project
      files, file create/rename/delete, per-workspace sessions.
- [ ] **6. Parity + hardening** — parity checklist vs electron app
      (narration gapless, stop-instant, dictation, settings persist, restart
      clean), debug log pane, `cargo test` coverage of segmentation/sanitizer/
      queue logic.

## Build deps (Ubuntu)
`libasound2-dev libxcb-render0-dev libxcb-shape0-dev libxcb-xfixes0-dev libxkbcommon-dev libwayland-dev` (egui + cpal).
