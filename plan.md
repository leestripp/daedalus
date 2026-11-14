# GTK retirement — DONE (2026-11-14)

- Removed the legacy `monkeybyte-gtk` source tree and old installed
  `~/.local/bin/monkeybyte` executable.
- Electron is the sole desktop app. The primary `monkeybyte.desktop` launcher
  is named MonkeyByte, executes `monkeybyte-electron`, and retains the original
  byte-identical `monkeybyte.svg` icon.
- Packaging, installation, CMake options, tests, and active documentation no
  longer depend on or recreate the GTK application.
- Verification passed: 113/113 remaining C++ tests, 38/38 Node tests, static
  analysis, packaged application tests, launcher/Debian checks, and clean close.

## Electron browser client

- Electron tool cards now render once per call: running arguments appear
  immediately, then the same expandable card gains a success tick or failure
  cross and its result JSON. Turn-end canonical history merges into the live
  transcript rather than clearing prompts, responses, notices, and tool cards.
  Packaged tests cover immediate display, success, denial/failure, JSON, and
  transcript preservation.


- Git panel/status bar now reports branch, changed/staged/unstaged/untracked
  counts and numeric ahead/behind commits to push/pull, replacing the workspace
  path in the bottom bar. Non-Git workspaces return a consistent empty status
  instead of breaking the renderer. Real upstream fixture and repository tests
  cover all counts and formatting.


- Workspace hidden-files toggle now uses the real settings action rather than a
  Settings-page DOM checkbox. Its compact header icon shows ○ when off and ●
  when on, with packaged tests proving dotfiles reveal/hide and persistence.
# MonkeyByte Electron replica — COMPLETE (2026-11-14)

## Goal and boundaries

Create a new Electron implementation of MonkeyByte in `monkeybyte/`, with
feature and behaviour parity with the existing GTK application in
the retired GTK implementation. It was the reference implementation and
remains buildable and usable until the Electron replica passes the complete
acceptance gate. This is a separate frontend implementation, not a wrapper
that embeds the GTK widgets and not a reduced chat-only demo.

The historical behaviour inventory was the GTK design document;
plus the post-design behaviour recorded in root `AGENTS.md` and the GTK tests.
The Electron implementation must also preserve the intentionally absent
features in design §16: no git staging/commit UI, diff viewer, tab drag
reordering, notifications, drag-and-drop upload, collaboration, or WebSocket
transport. Multiple independent processes are supported, but two processes
must not share one workspace's `.daedalus/` agent state.

## Architecture

- **Electron main process — trusted system boundary.** Own windows and
  lifecycle, native dialogs, workspace-confined filesystem operations,
  read-only git queries, local UI-cache persistence, backend process
  lifecycle, and loopback HTTP. No remote page receives Node or privileged
  IPC access.
- **Preload — narrow typed bridge.** Expose explicit allowlisted methods for
  workspace, editor, settings, chat, and browser operations. Keep
  `contextIsolation` and sandboxing on and `nodeIntegration` off. Never expose
  generic `ipcRenderer`, arbitrary shell execution, secrets, or unrestricted
  filesystem paths to the renderer.
- **Renderer — vanilla ES modules, semantic HTML, and CSS.** Match the GTK
  layout and Catppuccin Mocha/Latte themes without introducing a framework or
  bundler in the foundation. Use `textContent`/DOM construction for untrusted
  text. Keep modules separated by workspace, editor, chat, settings, and
  navigation responsibilities. Any later editor/markdown dependency requires
  explicit approval before it is added.
- **Agent backend — existing Daedalus engine.** Evolve the session/SSE design
  in `daedalus/demos/daedalus-webui/` into the production loopback backend
  used by Electron. Preserve one persistent agent per workspace, streaming
  sink events, queued turns, approvals, history snapshots, compression,
  statistics, skills/tools/MCP, and shared `daedalus.db` settings. A2A is not
  the primary chat transport because its per-task agent lifecycle does not
  match MonkeyByte's persistent workspace session.
- **Persistence.** Keep authoritative provider/model/MCP/agent state in the
  existing Daedalus store and access it through backend APIs rather than
  creating a second registry. Reuse the MonkeyByte settings contract for
  host settings; keep Electron-only window geometry and ephemeral UI state in
  Electron's user-data cache. Never put API keys in renderer localStorage.
- **Security.** Filesystem APIs resolve beneath the selected workspace and
  reject traversal/symlink escapes. Backend and automation endpoints bind to
  loopback only. Browser tabs are isolated from the privileged editor window.
  Dictation remains review-before-send, and ChimpFlow remains loopback-only.

## Implementation decisions locked for the full build

- The browser is a true Electron `WebContentsView` replacement for WebKit,
  not an iframe and not a screenshot surface. Main-process browser managers
  implement open/navigate/read/eval/links/console/upload/list/close, while
  renderer controls provide navigation, device presets, rotation, and fit.
- One backend process serves one workspace and uses a stable agent name so
  history persists across restarts. The session worker drains FIFO turns and
  honours `on_turn_requested()` without resubmitting queued work.
- The backend reuses the existing Daedalus and MonkeyByte GTK stores. Host
  tools call Electron's authenticated loopback UI endpoint; browser operations
  stay in the main process and privileged APIs never enter web contents.
- Continue through Phases 2–7 in this task. Do not claim completion until the
  agent browser loop, packaging, isolated settings, full tests, and user-assisted
  visual comparison have all been addressed.

## Phases

### Phase 0 — Preserve the GTK reference — DONE

- Renamed `monkeybyte/` to `monkeybyte-gtk/` with Git history preserved.
- Updated the superbuild, launcher, ignore rules, and documentation paths.
- Kept application targets, `src/monkeybyte/` includes, installed data paths,
  and runtime databases unchanged.
- Fresh superbuild configure and full compilation passed; all 113 CTests and
  umbrella static analysis passed after the rename.

### Phase 1 — Secure Electron foundation — DONE

- Add `monkeybyte/package.json` and lockfile with Electron as the only runtime
  tooling dependency; use Node's built-in test runner.
- Create the main/preload/renderer process split and a versioned IPC surface.
- Reproduce the IDE shell: activity bar, sidebar pages, resizable editor/chat
  regions, tab strip, status bar, and full-window Settings behaviour.
- Implement the first real workspace baseline: native folder selection,
  sandboxed lazy directory listing, hidden-file toggle, refresh, text-file
  opening, dirty tabs, save/close, and preserved tabs across workspace changes.
- Add live dark/light/system theme and basic editor preferences.
- Make Search, Git, agent chat, and unimplemented settings pages visibly
  report their later phase; do not present placeholders as completed parity.
- Add filesystem/settings/unit tests and an automated Electron DOM smoke test.

### Phase 2 — Complete workspace, editor, Search, and Git — DONE

- Lazy tree expansion memory, reveal/select, symbolic file icons, all file/folder
  context actions, new/from-template/import/rename/delete/copy-path operations,
  and tab retarget/close coordination after rename/delete.
- Custom confined folder picker and new-folder/new-file/template dialogs.
- Text/binary routing through system associations; 5 MB read cap and safe
  external open/terminal/reveal actions.
- Editor find/replace/next/previous, line numbers, bracket matching, syntax
  highlighting, configurable indentation/wrap/font, tab context menus, dirty
  close confirmation, and all keyboard shortcuts.
- Debounced workspace search with exclude globs, line previews, and jump-to-line.
- Read-only branch/change display and three-second non-overlapping status poll.

### Phase 3 — Persistent agent chat and approvals — DONE

- Productionize the loopback session/SSE backend and its lifecycle from Electron.
- Restore the actual Daedalus history snapshot eagerly; do not maintain a second
  divergent transcript.
- Implement streaming assistant bubbles, tool cards with live bounded output,
  `tell_user`/stop-summary/compaction/info bubbles, and scroll intent pinning.
- Implement FIFO mid-turn sends, repeat-safe Stop, clear/wipe semantics,
  workspace-change cancellation, and background turn wake-ups without duplicate
  submission.
- Add model and thinking-level selectors, stats/error display, slash commands,
  `/compress`, `/usage`, `/tools`, `/skills`, `/mcp`, and `/init`.
- Add image attachment markup/thumbnails and vision-capability gating.
- Add approval dialogs (Deny / Approve / Always) and classify terminal versus
  transient provider errors with actionable hints and durable logging.

### Phase 4 — Full Settings and shared Daedalus configuration — DONE

- Interface, workspace/project types, editor, theme, explorer, browser, chat
  appearance, Search excludes, and resolved Paths pages.
- Providers, quick presets/discovery, models/default/prompt binding, system
  prompts, MCP servers/import, agent skills/tools/limits/MCP policy.
- Rebuild or reconfigure the agent with the same wipe-versus-preserve semantics
  as GTK; apply thinking-level changes without losing history.
- Share provider/model/MCP configuration with Icarus and MonkeyByte GTK through
  Daedalus; keep secrets masked and never log them.

### Phase 5 — Markdown, previews, and isolated browser tools — DONE

- Safe assistant Markdown rendering and syntax highlighting, including fenced
  code and all existing bubble behaviour.
- Read-only Markdown/HTML preview tabs keyed by path+mode with relative assets.
- Real browser tabs with navigation, bounded long-URL display, device presets,
  rotation, fit-to-window, and mobile user-agent behaviour.
- Implement `browser.open/navigate/read/eval/links/console/upload/list/close`
  through isolated `WebContentsView` instances, bounded console capture, and
  explicit agent/user navigation only.
- Preserve tab-less `web_fetch` and `web_search`; do not add browser snapshots.

### Phase 6 — ChimpFlow voice parity — DONE

- Implement loopback discovery/settings, scoped-key handling, preview, and
  validation without leaking credentials.
- Capture microphone audio, transcribe, and insert review text without
  auto-send; disable controls until configuration is complete.
- Stream sentence/paragraph segmentation, FIFO speech, direct `tell_user`
  narration, remainder-only final flush, and no history/tool narration.
- Apply 0.12 RMS normalization with 4x gain cap and 0.98 peak limit; contain
  cancellation/stale completions and prevent preview/recording overlap.

### Phase 7 — Packaging, migration, and final parity gate — DONE

- Add repeatable development/production launch and Linux packaging without
  overwriting the GTK binary during coexistence; decide final installation
  naming/migration with the user before replacement.
- Reuse MonkeyByte resources and desktop integration; preserve multiple-process
  behaviour and per-workspace state isolation.
- Complete automated unit/integration/Electron smoke coverage and manual GTK-vs-
  Electron comparison of every design §17 item and post-design feature.
- Run Node tests/checks, clean C++ superbuild, full CTest, static analysis, and
  package smoke tests. Ask the user for screenshots for visual comparison; do
  not capture the desktop autonomously.
- Update root `AGENTS.md`, both app READMEs, and this plan; only then mark the
  replica complete. GTK removal, if desired, is a separate explicit decision.

## Final verification checkpoint

All seven implementation phases are complete. Electron is now the only
MonkeyByte desktop host, with one MonkeyByte launcher and the original icon. The
standalone package and a clean-prefix install both passed the complete smoke
suite. Final verification covers 29 Node unit tests, JavaScript syntax checks,
the C++ session catalog and full superbuild/CTest/static-analysis suites, and
the packaged browser/agent/editor/voice/project-type/Fountain integration
suite with isolated settings and secrets. No `--no-sandbox` path is permitted.

Visual desktop screenshots were not captured autonomously, in accordance with
the screenshot policy. Any further visual polish should use a user-supplied
GTK/Electron screenshot comparison; it does not change the tested functional
completion recorded here.

### Live-provider acceptance — DONE (2026-11-14)

- Opened the installed Electron app on `/tmp/test` with the real configured
  Kimi provider. The agent created `index.html`, `styles.css`, and `script.js`
  for a sell-item page, streamed tool progress/results, and completed strict
  validation. A native-browser click test confirmed Buy shows its confirmation
  and becomes disabled.
- Fixed three issues found only in live use: workspace settings were compared
  to a function rather than its returned workspace, `tell_user` called an
  undefined renderer append helper, and shutdown did not reliably save the
  actual geometry. Final live turns have zero renderer errors.
- Matched GTK startup by using Daedalus's sanitized `history_snapshot()` and
  filtering system/empty entries. A backend regression seeds dangling tool
  calls and orphaned results and proves only valid renderable history returns.
- Close now awaits actual window/sidebar/chat geometry persistence. A real
  close/restart restored `1377×833`, sidebar `321px`, and chat `452px` exactly.
- Final gates passed: full build, 114 CTests, static analysis, 26 Node tests,
  JavaScript checks, and packaged smoke including `tell_user` and busy-turn
  settings regressions.

### Persistence, first-run, and shutdown hardening — DONE (2026-11-14)

- Kept GTK permanently at `~/.local/bin/monkeybyte`; Electron remains optional
  as `monkeybyte-electron`. Package assertions reject any Electron-owned GTK
  executable or desktop file, and Electron-only reinstalls preserve GTK's hash.
- Removed stale Phase 1 loading UI and settings-driven agent-status clobbering.
  No-workspace startup now has an honest idle state and disabled Send/Steer.
- Restored successful `tell_user` answers from Daedalus history. Real restart
  checks recover the full answer rather than only the lead-in.
- Fixed a session-storage data-loss bug: named chats previously shared
  `messages.json` and `msg.default`, so New Chat inherited history and deleting
  it erased the original. Chats now have independent files/SQLite keys with
  one-time legacy migration. Live create/rename/delete/restart preserved
  `ORIGINAL_SESSION_KEEP`; low-level and backend isolation regressions pass.
- Bounded geometry, backend, event-stream, and UI-server shutdown. After all
  resources close, the main process exits explicitly. `npm run test:close`
  launches the packaged app, requests a normal close, and returns in about
  5.5 seconds with exit code zero instead of leaving bash stuck.
- Updated the suite to 29 Node tests and added the repeatable packaged-close
  test. Interactive tests use isolated `XDG_CONFIG_HOME`, not just Chromium's
  non-isolating `--user-data-dir`.

### GTK status/activity and ChimpFlow parity — DONE (2026-11-14)

- Replaced Electron's bare `Agent Idle` footer with GTK-equivalent model, token/
  context percentage, cached/prompt tokens, iteration, and tool/failure counts.
  Daedalus stats now stream through the loopback backend and initial status.
- Added an actual animated working spinner plus GTK-style Thinking, Streaming,
  Running-tool, Listening, Transcribing, Compacting, Done, and Cancelled states.
- Matched GTK's shared ChimpFlow configuration: canonical `chimpflow.*` keys
  win over Electron's legacy JSON, and Electron saves both representations.
  Discovery and preview use the current form's key/server/model/voice before
  saving, not stale persisted state.
- Fixed two app-side voice failures not visible in settings: CSP now permits
  local blob audio, and dictation converts WebM/recorded audio to 16 kHz mono
  WAV before Whisper upload. The microphone only enables when enabled, scoped
  key, dictation, and STT model are all available.
- Real local acceptance used the saved key and `alba`: authenticated discovery,
  actual MP3 preview playback, and STT of the synthesized speech all passed.
  Automated coverage grew to 34 Node tests plus backend shared-settings/stats
  assertions and packaged spinner/status/ChimpFlow integration checks.

### ChimpFlow 0.3.4 model selection — DONE (2026-11-14)

- Read the live `/skill/` and `/skill/manifest.json` contract. New defaults are
  `kokoro-82m`; Pocket TTS is legacy only due to intermittent garbled output.
  Chatterbox remains an optional high-end narration model, not the default.
- Electron's TTS model is now a dropdown populated from `/v1/models`. Model
  changes fetch `/v1/voices?model=<model-id>`; GTK discovery uses the same
  scoped contract. Settings text explains the model tradeoffs.
- Real testing found and fixed two renderer lifecycle bugs: startup settings
  refreshes could overwrite an active discovery, and Save collapsed the runtime
  catalogs to one option. Active edits now resist background re-renders and
  discovered models/voices survive Save.
- Real acceptance discovered three models and 54 Kokoro voices, selected and
  played `af_heart`, saved `kokoro-82m` + `af_heart` to both GTK and Electron
  settings, preserved all options after Save, and passed real STT. The Node
  suite is now 35 tests; GTK's default/scoped-request regression also passes.

### Workspace tree context menus — DONE (2026-11-14)

- Matched GTK's file/folder context actions: New File Here on files and folders,
  New Folder Here on directories, Open/system application, format previews,
  Rename/Delete, and relative Copy Path. Retained Electron's absolute-path,
  reveal, import, and terminal extras.
- Found the existing menu's root cause: it was nested inside the hidden
  Settings section, so it could not become visible or receive focus. It is now
  a top-level overlay with viewport-safe placement and scrolling.
- Added Shift+F10/Menu-key invocation, arrow/Home/End navigation, menu roles,
  and Escape/action focus restoration. Packaged tests create a Markdown file,
  create a nested folder, rename the file, exercise both copy actions, and
  verify keyboard/focus/position behavior against a real temporary workspace.
- Electron 45 made clipboard operations asynchronous; Copy Path now awaits the
  write and the test awaits the read. Smoke exit codes also survive normal
  window shutdown rather than being reset to zero, preventing false-green
  package runs.

---

# Scheduled agent tasks — DONE (2026-11-11)

Implemented persistent per-project/per-agent scheduling with a
`std::jthread` dispatcher and the `schedule_task` add/list/edit/delete tool.
One-shot UTC tasks delete themselves before dispatch; recurring tasks advance
by their fixed interval and coalesce missed intervals. Due prompts use the
normal pending queue with source `trigger:task-<id>` and wake idle hosts
through `AgentSink::on_turn_requested()`. If the agent is actively busy, the
occurrence is dropped permanently instead of queued; recurring tasks retain
their next future slot, preventing unbounded backlogs.

- Storage: `agent_dir/scheduled_tasks.json`, with atomic temp-file + rename
  mutations and fail-closed malformed-file handling.
- Lifecycle: scheduler starts on agent initialization and joins on shutdown
  or destruction; overdue work fires on startup only when a host is active.
- Verification: full superbuild, all 113 CTests, umbrella static analysis,
  and repeated timing/routing suites passed, including 20 busy/drop runs.
- Contract documented in `daedalus/docs/tools.md`, `docs/storage.md`, root
  `AGENTS.md`, and `daedalus/CHANGELOG.md`.

# Second Brain multi-process hardening — DONE (2026-11-09)

The Global Second Brain is now safe for cooperating MonkeyByte/Daedalus
processes sharing one user data root. The original isolated repro lost 20 of 40
successful concurrent append markers; the new suite verifies every append,
unique same-title IDs, consistent readers, mixed mutations, transactional
rollback, and kill/reopen recovery.

- Added bounded RAII `flock(2)` at `<brain>/.lock`: shared reads, exclusive
  mutations/reindex, explicit five-second timeout errors.
- Notes now use same-directory temp-file + `fsync` + atomic rename + directory
  `fsync`; stale temporary files are ignored by reindex.
- Metadata/FTS upserts, deletes, and complete reindexes are transactional.
- `SettingsStore` verifies WAL and serializes schema initialization with
  `BEGIN IMMEDIATE`, closing the startup race found during stress testing.
- Reindex uses nanosecond mtime plus size; the Markdown tree remains the source
  of truth and repairs file/index crash windows.
- Added `second_brain_concurrency_tests` for independent/same-title capture,
  concurrent append/read, update/move/delete/backlinks/reindex, lock timeout,
  forced index rollback, and `SIGKILL` + WAL recovery. All paths are temporary;
  the real brain was never touched.
- Verification: concurrency/crash suite passed 20 consecutive runs; full
  superbuild succeeded; all 111 CTests passed (three live-LLM tests skipped as
  designed); umbrella static analysis passed.
- Contract documented in `daedalus/docs/storage.md`, `settings.md`, `tools.md`,
  root `AGENTS.md`, and `daedalus/CHANGELOG.md`.

## ChimpFlow voice support for MonkeyByte — DONE (2026-10-25)

Implemented local ChimpFlow STT/TTS settings and discovery, native PipeWire
capture/playback, microphone dictation with review-before-send, voice previews,
and FIFO assistant narration. Verification included the full 109/109 CTest
suite, clean static analysis, live gateway/audio tests, isolated UI smoke, and a
matching tested/installed MonkeyByte binary. The durable documentation is in
`monkeybyte-gtk/README.md`, the MonkeyByte section of root `AGENTS.md`,
and `monkeybyte-gtk/docs/design.md` (§CHIMPFLOW).

### True Steer tool-boundary safety — DONE (2026-11-14)

- Deep-dive found the old Electron `/v1/chat/steer` only changed the source to
  `steer:lee`; it used the ordinary next-turn queue and was not mid-turn.
- Added `Agent::submit_steer_message()` and a separate synchronized steering
  queue. Send retains its independent FIFO queue and remains enabled busy.
- `LLMClient` drains Steer only after ALL current tool results are appended,
  before the next provider request. It never splits an unfinished assistant
  message or a tool call/result pair. Multiple Steer messages retain FIFO
  joining, origins, and attachments.
- Cancellation before the boundary leaves Steer undrained. Idle submission,
  final-response, stop, and cancellation races move leftovers to the ordinary
  pending queue under the same mutex before `turn_active_` clears.
- Exact two-tool outbound-chain, final-response, cancellation, idle fallback,
  and real Electron Steer-button integration tests pass. Full gate: clean
  build, 114/114 C++ tests, static analysis, 35/35 Node tests, packaged
  browser/agent/smoke tests, and clean close.
- Live Kimi K3 acceptance in an isolated Electron workspace used the actual
  Send/Steer controls during two timed real `execute_bash` calls. Both steers
  changed the requested outputs: `steered-one.txt` and `steered-two.txt`
  (contents `steering worked twice`); neither superseded filename was created.
  The raw persisted chain validated every tool-call ID against every result
  across six assistant tool blocks, with each Steer after the completed block.
  A subsequent same-chat provider turn answered exactly `LIVE_CHAIN_OK`; no
  provider/UI errors, pending messages, or executing tools remained.

### Electron agent-panel responsive controls — DONE (2026-11-14)

- Removed the fixed 190 px dropdown cap. Session controls, model, and thinking
  now use responsive rows; model/thinking share and fill the current agent-panel
  width and grow after keyboard resizing.
- Kept Send independent from Stop to preserve Daedalus queued messages. Busy
  state leaves Send labelled and enabled for queued submissions. Steer and Stop
  sit above a full-width Send button in a compact two-column action grid.
  Both remain visible at all times—disabled while idle and enabled while busy—
  with a constant action-area height so starting/stopping never moves the UI.
- Packaged tests verify selector growth, separate busy actions, stacked
  geometry, busy-time Send availability, and Stop cancellation.

### Electron built-in browser live acceptance — DONE (2026-11-14)

- Added a user-visible Open Browser button beside New File, using the configured
  home URL and focusing the address bar; no agent invocation is required.
- Repaired the Electron browser toolbar layout and added Enter/Go navigation.
  Added a Dark-by-default page color-scheme switch in the toolbar; Dark/Light
  changes the browser page's `prefers-color-scheme`, survives navigation, and
  leaves the editor theme unchanged.
- Fixed `browser.eval` after live agent use exposed expression-only evaluation
  and unresolved Promises. Chromium debugger evaluation now accepts statements,
  awaits Promises, and returns actual page exceptions. Packaged tests cover the
  toolbar, both computed page color schemes, navigation, statements, async
  results, exception details, and upload compatibility.
- With the real configured provider, the embedded Electron agent used
  `browser.open/navigate/read/links/console/eval` across controlled sites and a
  deliberately broken local app. It reported all five injected bugs with
  evidence: a null-element TypeError, string-concatenated totals, a missing
  accessible input name, a broken Help route, and a missing analytics endpoint.
  External Google/Wikipedia navigation, live reads, and link listing also
  passed. Interactive acceptance
  used `/tmp/monkeybyte-electron-browser-workspace`, never the source checkout
  as its agent workspace. GTK was not changed.

## Daedalus completed tasks

- **2026-09-21 — Global encrypted vault:** AES-256-GCM secrets storage, global
  key handling, public API, and the non-secret-listing `vault` tool.
- **2026-09-10 — Agent API and queue hardening (commit `2b25f27`):** typed
  `AgentState` sink events, `CompressResult`, read-only history snapshots,
  public auto-flush configuration, terminal turn-error reporting, and the
  synchronized pending-message queue/cancel API.
- **2026-09-02 — MonkeyByte crash and subprocess hangs:** fixed the Messages
  flush-worker race with published snapshots (§PERSIST-RACE), hardened malformed
  chain handling, and bounded both subprocess hang paths. Regression/stress
  testing and 95/95 CTest were recorded in the original completed plan.
- **2026-08-31 — Persistence and PDF follow-ups:** malformed-chain no-throw
  handling (SANITIZE-NOTHROW) and restoration of `read_pdf` as a core tool.

## MonkeyByte completed tasks

- **System file associations:** the FileKind classifier/launcher, workspace-tree
  routing, editor binary guard, and unit-test checklist are complete. Non-text
  files open through the user's associated system application.
- **Flush-worker SIGSEGV investigation — RESOLVED (2026-09-10):** fixed upstream
  in Daedalus by §PERSIST-RACE published snapshots. The evidence remains in
  `monkeybyte-gtk/docs/upstream-repro/`; the separate plan file has been retired.
