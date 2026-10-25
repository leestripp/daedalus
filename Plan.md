# Plan — ChimpFlow voice support for MonkeyByte (2026-10-25)

> **Status**: COMPLETE (2026-10-25). PipeWire dependency and ChimpFlow repairs
> approved by lee (2026-10-25). The completed K3.1 changes
> in the working tree are unrelated and must remain untouched.

> **Urgent interruption resolved (2026-10-25):** §STATUS-KIND-BREAKER
> now treats `set_status("Done")` as an intentional terminal signal and
> suppresses only the third consecutive status-only iteration of the SAME
> non-terminal state. Mixed phases reset the streak. Full superbuild,
> 109/109 CTest entries, and static analysis passed; the tested MonkeyByte
> binary was installed to `~/.local/bin/monkeybyte` with matching SHA-256.
> ChimpFlow resumes from its tested settings/HTTP/WAV foundation below.

## Goal and user-visible behavior

Add fully local ChimpFlow STT/TTS support to MonkeyByte:

1. **Settings → ChimpFlow** tab with a master on/off switch, local server
   address/port, masked scoped API key, discovered TTS model and voice lists,
   STT model, and separate read-aloud/dictation switches.
2. Selecting a voice speaks a short sample in that voice. A Preview button
   replays the sample; Refresh reloads models/voices without blocking the UI.
3. **Agent panel microphone button**: first click starts recording, second
   click stops and transcribes. The transcript is inserted at the input cursor
   for review; it is **not auto-sent**, so dictation cannot accidentally invoke
   agent tools. A third state shows transcription in progress.
4. Every newly finalized **assistant** bubble is read aloud in the selected
   voice when master enable + read-aloud are on. Multiple prose bubbles around
   tool cards are queued FIFO. Restored history, user bubbles, tool cards, and
   host/status messages are not spoken.

## Confirmed ChimpFlow contract

The live `http://127.0.0.1:8090/skill/manifest.json` and sibling
`../ChimpFlow` source agree:

- `GET /healthz` — unauthenticated liveness.
- `GET /v1/models` and `GET /v1/voices` — bearer key, runtime discovery.
- `POST /v1/audio/speech` — `tts` scope; JSON `{model,input,voice,response_format:"wav"}` → WAV.
- `POST /v1/audio/transcriptions` — `stt` scope; multipart WAV + `model=whisper` → `{text}`.
- 401 means bad key, 403 missing scope, 502 engine unavailable, 503 voices catalog unavailable.
  Surface these errors; never retry-loop them.

The installed gateway initially answered `/v1/voices` with 503 because its
deployed directory (`/projects/audio-cpp-test/voice_samples`) no longer existed.
With lee's approval, the deployed setting was repaired to the installer's
existing default, `/projects/AI/ChimpFlow/engine/voice_samples`, and only the
gateway was restarted. `/v1/voices` now returns HTTP 200 with 26 voices. No
ChimpFlow source change was needed; its worktree is unchanged. Live tests may
use this verified gateway with a dedicated MonkeyByte test key.

## Settings and security

Add `ChimpFlowSettings` to MonkeyByte's existing SQLite key/value
`SettingsStore` (no schema-version migration needed):

- `enabled = false` (opt-in; startup never speaks unexpectedly)
- `read_assistant = true`, `dictation_enabled = true`
- `server_url = "http://127.0.0.1"`, `port = 8090` (derived base origin;
  endpoint paths remain the fixed OpenAI-compatible contract)
- `api_key = ""` (empty means read `CHIMPFLOW_KEY` at request time)
- `tts_model = "pocket-tts"`, `stt_model = "whisper"`
- `voice = ""` (first discovered voice becomes the selection; never hardcode a voice)

Persist under the `chimpflow.*` namespace with full round-trip tests. The UI
uses a password entry and never logs the resolved key. Accept only loopback
HTTP URLs (`localhost`, `127.0.0.1`, `[::1]`) and ports 1–65535: transcripts,
agent text, and the scoped key must not leave this machine. Prefer a dedicated
`tts` + `stt` key; never fall back to another app's key or the admin key.

## Components

### 1. `backend/ChimpFlowClient.{h,cpp}`

Small synchronous, worker-called libcurl client (libcurl is already a
MonkeyByte dependency), with an injectable transport for hermetic tests:

- `health()`, `list_models()`, `list_voices()`, `synthesize()`, `transcribe()`.
- Binary-safe response bodies, JSON error extraction, 3 s connect timeout,
  bounded request timeouts, and `std::stop_token` cancellation through curl's
  progress callback.
- No GTK calls and no automatic retries on HTTP/auth/engine errors.

### 2. `backend/Wav.{h,cpp}`

Pure, tested WAV helpers adapted from ChimpEcho, with stronger bounds checks:

- 16 kHz mono float capture → 16-bit PCM WAV.
- Parse mono PCM 16/24/32-bit WAV at the returned sample rate for playback.
- Build the OpenAI-compatible multipart STT body without corrupting binary data.
- Reject truncated chunks, overflow, invalid rates/channels/formats, and
  out-of-bounds data. Audio buffers are bytes, never UTF-8 strings.

### 3. `backend/AudioEngine.{h,cpp}`

Native PipeWire capture/playback, adapted from the proven ChimpEcho backend:

- 16 kHz mono microphone capture and mono float playback at the WAV rate.
- RAII ownership and idempotent start/stop.
- Preserve ChimpEcho's hard-won rules: destroy stream under the loop lock,
  unlock **before** `pw_thread_loop_stop()`, and re-arm one-shot latches on
  every start.
- Playback completion means **drained/played**, not merely queued, so speech
  tails are not clipped.
- No audio device is opened until recording or playback is requested.

**Dependency approved:** add `pkg_check_modules(PIPEWIRE REQUIRED
IMPORTED_TARGET libpipewire-0.3)` and link it to `monkeybyte_core`. PipeWire is
already installed and is the same backend used by ChimpFlow's ChimpEcho demo.

### 4. `backend/VoiceService.{h,cpp}`

Shared through `AppServices`, so Settings and Chat use one owner and one audio
pipeline. Widgets remain in their panels.

- UI-thread orchestration; HTTP on `std::jthread` workers; results and audio
  callbacks marshalled to the main context with `Glib::Dispatcher`.
- Generation counters + an `alive` token suppress stale completions after
  cancellation/destruction. Destructor cancels HTTP, capture, and playback,
  then joins workers; exceptions stay inside operation boundaries.
- Assistant speech queue: one synthesis/playback chain at a time, FIFO.
- Voice preview and microphone start explicitly stop/clear pending speech to
  prevent overlapping audio and microphone feedback.
- Read settings afresh on `reload()`. Master-off immediately cancels recording,
  transcription, and speech; save applies without rebuilding the agent.
- Empty/whitespace assistant text is ignored. Convert Markdown to
  speech-friendly plain text (preserve prose/link labels; omit image targets
  and fence markers); do not truncate UTF-8 bytes.

## UI wiring

### Settings → ChimpFlow

Add a named stack page and rail button, following existing card/group/two-column
form patterns. Include:

- Master enable, Read assistant replies, Enable microphone dictation switches.
- Server URL, Port, masked API key, TTS model, STT model, Voice.
- Refresh Models/Voices and Preview Voice buttons; busy/status/error labels.
- Discovery and preview use the current widget values, even before Save.
- Populating dropdowns is suppression-guarded; only an actual user voice
  selection auto-plays the sample, never initial load/list rebuild.
- Save persists through the existing Settings footer and calls
  `VoiceService::reload()` immediately.

### Agent chat

- Add `audio-input-microphone-symbolic` beside Attach/Send, with recording and
  transcribing states plus accessible tooltips.
- Mic is insensitive with an explanatory tooltip when ChimpFlow/dictation is
  disabled or required configuration is missing.
- On finalized **live** assistant bubbles, pass the bubble's raw text once to
  `VoiceService::speak()`. Do not call this from history restoration.
- Dictation completion inserts trimmed text at the TextView cursor and focuses
  the input. Empty audio/transcript returns to Ready with non-error feedback.
- Clear History, reset/teardown, and window destruction call `stop_all()` as
  appropriate. No worker callback may touch a dead widget.

## Tests and verification

1. **SettingsStore tests**: defaults, every field round-trip, environment-key
   fallback, loopback/port validation.
2. **Client contract tests** with injected fake transport: exact URLs, auth
   header without logging it, TTS JSON, binary WAV, multipart STT + model part,
   catalog parsing, 401/403/502/503 and malformed JSON handling, cancellation.
3. **WAV tests**: all supported sample widths, round trips, malformed/truncated
   chunks, binary multipart integrity.
4. **Voice orchestration tests** with fake client/audio: FIFO speech, no
   duplicate bubble narration, preview/dictation cancellation, master-off,
   stale callback suppression, empty transcript, repeated start/stop cycles.
5. **Existing suites**: clean full superbuild, full CTest, and umbrella
   `static_analysis`; no warnings from changed files.
6. **Isolated live smoke**: run ChimpFlow on a separate loopback port with the
   valid voices directory and a dedicated test key; verify discovery, voice
   preview WAV + drained playback, STT of a generated spoken phrase, repeated
   record/stop, and assistant read-aloud. Never print or commit the key.
7. **UI smoke** in an isolated MonkeyByte profile: ChimpFlow tab, persistence
   after restart, voice-selection preview, mic state transitions, transcript
   insertion without auto-send, and no speech during history restore.

## Documentation

Update `monkeybyte/docs/design.md` (new §CHIMPFLOW marker), Settings/Chat
sections, architecture/dependency list, and AGENTS.md when durable operational
facts emerge. Document opt-in setup, scoped-key configuration, localhost
restriction, dictation review-before-send behavior, and the separately observed
live-gateway voices-directory issue.

## Implementation result and verification (2026-10-25)

Implemented all planned components: persisted settings, injectable curl
client, bounded WAV/multipart helpers, native PipeWire capture/playback,
shared asynchronous VoiceService, RMS-normalised PipeWire playback,
Settings → ChimpFlow, microphone states,
review-only transcript insertion, and FIFO live-assistant read-aloud.
Documentation is in `monkeybyte/README.md`, `monkeybyte/AGENTS.md`, and
`monkeybyte/docs/design.md` (§CHIMPFLOW).

Verification passed:

- Full superbuild and **109/109 CTest entries** (three normal live-LLM skips).
- Umbrella `static_analysis` clean; `git diff --check` clean. Only existing
  third-party sqlite-vec compiler warnings appeared during the full build.
- Hermetic client/WAV/service tests cover HTTP errors, binary integrity,
  FIFO playback, cancellation/stale completions, preview busy cleanup,
  dictation states, missing configuration, short/empty transcripts, and
  RMS gain/peak/sanitisation normalisation.
- Live gateway: 1 model / 26 voices discovered, valid 24 kHz mono TTS WAV,
  and non-empty STT round trip. A dedicated MonkeyByte `tts` + `stt` key was
  used through the environment and never committed.
- Native PipeWire: three drained playback cycles (including normalized
  playback) and two real microphone start/stop cycles. Testing exposed and fixed a retained
  `pw_stream_events` stack-lifetime segfault; event tables are now static.
- Isolated UI: live discovery, actual voice preview with drained completion,
  save, and restart persistence all passed. The process remains alive after
  preview. External X11 mouse automation could not reliably activate the
  test window for the final mic-button click; mic state/transcript behavior
  is covered by the service tests, real native capture, and live STT instead.
- Installed to `~/.local/bin/monkeybyte`; installed and tested binaries have
  identical SHA-256
  `6e2b87a1a516121b4d4c8f890d2ab55f373418df7e6c28aa0c5ae3bd8c10cdd9`.

Follow-up voice-loudness normalisation targets 0.12 RMS, caps amplification
at 4x, and limits peaks to 0.98 for consistent non-clipping playback.

The ChimpFlow gateway repair remains deployed with the corrected voices
directory; no ChimpFlow source/worktree change was made. Unrelated K3.1 and
§STATUS-KIND-BREAKER changes remain preserved and uncommitted.
