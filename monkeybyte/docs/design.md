# MonkeyByte — Current Design

MonkeyByte is a native C++23 GTK4/gtkmm desktop AI code editor. The current
application is a **single-process native host**: GTK widgets, workspace
services, the Daedalus agent, and WebKitGTK run in one application. The retired
browser frontend and its workspace/chat/settings HTTP backend are not part of
the current runtime contract.

For user-facing features and build instructions see [`../README.md`](../README.md).
For detailed contributor invariants and regression history see the MonkeyByte
section of [`../../AGENTS.md`](../../AGENTS.md).

## 1. Architecture

```text
Gtk::Application / MainWindow (GTK main thread)
├── AppServices
│   ├── MonkeyByte SettingsStore (monkeybyte.db)
│   ├── FileService / ProjectTypeLibrary / ThemeManager
│   └── VoiceService → ChimpFlow + PipeWire
├── Workspace tree, search, Git panel, editor, previews
├── ChatPanel → ChatController → daedalus::Agent
│   ├── AppUiTool      (app_ui_control)
│   ├── WebFetchTool   (web_fetch)
│   └── core web_search, execute_bash, memory, MCP, …
├── UiControl command registry
│   ├── embedded app_ui_control tool
│   ├── GTK/D-Bus actions
│   └── loopback ControlServer
└── WebKitGTK browser/preview tabs
```

`AppServices` is the shared owner for application-wide services. The UI is not
an HTTP client for its own workspace operations. `http/ApiClient.*` and
`http/EventSourceClient.*` remain in the source tree for compatibility, but the
old `/api/workspace`, `/api/files/*`, `/api/chat/*`, and `/api/settings/*`
routes are not served by MonkeyByte.

## 2. UI layout and behavior

### Activity bar and sidebar

The activity bar is the only sidebar-panel switcher. Panels cover workspace,
search, Git, and settings. Sizer drags and direct width controls update panel
widths; widths are persisted in GSettings. Keyboard shortcuts and the command
palette use the same actions as visible controls. The selected activity button
is an inset rounded tile with an accent-coloured icon and symmetric 1 px
borders; keyboard focus uses a complete outline. Do not use a one-sided active
border: Adwaita adds a bottom border to focused flat buttons, which otherwise
combines with the active stripe to produce an L-shaped artifact.

### Settings

Settings replaces the editor/chat area and uses a vertical section rail beside
the active page. Each page is centred in a responsive column capped at 900 px;
the column shrinks with the available width. Shared form rows use a 220 px
label and a fixed 20 px gap rather than allowing labels to expand across the
window. Entries and dropdowns fill the remaining row width, while switches and
compact controls stay end-aligned. Keep this geometry independent of the
settings persistence and backend handlers.

### Workspace and editor

`FileService` is the workspace filesystem boundary. All workspace paths must
pass through `FileService::resolve()`, which rejects `..` escapes and paths
outside the workspace. Text reads are capped at 5 MB. The workspace tree uses
GIO content classification plus a text sniff for source-like files. Binary and
media files open through the system-default application rather than being
loaded into the source editor.

The editor is GtkSourceView with tabs, dirty markers, save/save-as, line
numbers, current-line highlight, word wrap, and configurable fonts/tab width.
With no documents open, a solid editor-background surface fills the available
body and centres the prompt to open a file from the Workspace.
Markdown and HTML can open as rendered preview tabs. Fountain screenplay files
have a dedicated preview and external-tool PDF export.

The workspace tree watches for filesystem changes. Refresh logic must avoid
clobbering a dirty editor and must tolerate agent-generated changes. Search is
performed by `SearchService`, with persisted exclude patterns. Git status is
read through `GitService`; staging/commit UI is intentionally not implied by
the read-only panel. The bottom status bar also shows the upstream branch,
incoming commits (`↓`, remote ahead), outgoing commits (`↑`, local ahead), and
the working-tree change count. Local state is polled every three seconds;
remote-tracking refs are fetched off-thread at most once a minute with a
15-second, non-interactive bound. Hovering the label explains the counts.

### Workspace types

Project types are Markdown definitions under
`$XDG_DATA_HOME/monkeybyte/project-types/`. Each type can provide an agent
prompt, detection rules, and starter templates. New workspaces can be
scaffolded from a type. Existing workspaces store an explicit type stamp in
`AGENTS.md`; otherwise detection heuristics apply. Per-model prompt bindings
remain the highest-priority prompt override.

## 3. Settings and persistence

Two stores have separate responsibilities:

| Store | Default location | Owns |
|---|---|---|
| MonkeyByte `SettingsStore` | `$XDG_DATA_HOME/monkeybyte/monkeybyte.db` | Interface/chat settings, last workspace, search excludes, disabled host tools/skills, ChimpFlow, custom prompts, model→prompt bindings |
| Daedalus `SettingsStore` | `$XDG_DATA_HOME/daedalus/daedalus.db` | Providers/models, MCP servers, web-search providers, agent scalar settings, encrypted vault, brain index/mirrors |
| GSettings/dconf | Desktop settings service | Window/sidebar/editor caches and theme/window geometry |

GSettings is not the source of truth for provider, model, MCP, or ChimpFlow
configuration. Both SQLite stores use WAL and busy timeouts. Settings handlers
must use guarded load-modify-save: if loading current values fails, abort the
save rather than replacing unrelated fields with defaults.

`ModelBridge` performs the one-time legacy MonkeyByte model migration into the
Daedalus provider/model registry. `McpBridge` edits Daedalus MCP rows. API keys
support `${ENV_VAR}` placeholders; never print or log resolved credentials.

Project/agent state belongs under
`<workspace>/.daedalus/daedalus[-<agent>]/`, including messages, memory,
scratch, and scheduled/todo state. These directories are runtime-owned and must
not be cleaned up as ordinary project cruft.

## 4. Agent and chat lifecycle

`ChatController` owns the Daedalus agent and a turn worker. `ChatPanel` owns
presentation. Agent callbacks can arrive on worker/HTTP threads and are
marshalled to GTK with dispatchers; widgets are only touched on the GTK main
thread.

The agent configuration is rebuilt when relevant provider/model, MCP, prompt,
skill, or tool policy changes. A rebuild normally preserves the persisted
conversation. `/clear`, the clear button, and workspace changes additionally
wipe the persisted transcript and crash/temp mirrors so old messages cannot
reappear on the next agent construction.

### Input queue and cancellation

Users may continue typing while a turn runs. Submitted messages are FIFO. The
host pairs submissions with `run_one_turn()` and must account for Daedalus's
queue-join behavior: messages accumulated before a dispatch can be joined into
one model input, with image attachments concatenated and origin metadata
retained. Never leave an idle submission without a driving turn.

The composer has separate **Send** and **Steer** buttons. Send always uses the
normal FIFO above. Steer is enabled only while processing and calls
`Agent::submit_steer_message()` through its own synchronized queue. Daedalus
injects it before the next model cycle, only after the current complete
assistant tool-call batch and every matching result are committed. It never
injects inside per-tool execution/cleanup or between results in a multi-tool
batch. The worker includes `pending_steer_count()` in its final drain check,
closing the submission-vs-Idle race. Cancellation and terminal errors clear
uninjected steers; normal queued sends retain their existing semantics.

Stop cancels the current turn and clears later queued user intent. Cancellation
still emits tool results for any interrupted tool-call
chain. Error, completion, and queued-turn transitions are delivered through
`AgentSink`, not inferred by scraping terminal output.

### Commands

The `/` menu exposes `/help`, `/clear`, `/init`, `/compress`, `/usage`,
`/tools`, `/skills`, and `/mcp`. Commands run through `UserCommands`.
`/compress` calls `Agent::compress_context()` on the turn thread and reports
success, deferral, or an incompressible/empty history. It never submits a fake
summary as a user message.

### Tool policy and safety

MonkeyByte registers `AppUiTool` and `WebFetchTool`; Daedalus supplies its core
tools including `execute_bash` and unified `web_search`. Configured MCP tools
appear as `mcp__<server>__<tool>`. Settings → Agent controls disabled tools,
skills, and MCP servers. Its **Context Compression** slider selects 50–90% of
old history to summarize in 10% steps (default 80%); the shared Daedalus setting
is also available in icarus as a dropdown. The **Second Brain** section edits
the shared absolute Markdown directory, with a folder picker and immediate
persistence. The next brain operation uses and reindexes that directory; notes
are not moved automatically. Registration is replace-by-name, and runtime
disables change the live tool surface.

Bash approval is interactive unless auto-approve or a command-specific session
approval applies. Auto-approve is not a sandbox. Web results and fetched pages
are untrusted content. `browser.eval` executes agent-provided JavaScript in the
selected tab and therefore inherits that tab's authenticated web authority.

## 5. Browser and previews

WebKitGTK browser tabs support navigation, arbitrary JavaScript-rendered pages,
local/remote URLs, device viewport presets, rotation/fit-to-window, console
capture, link listing, JavaScript evaluation, file upload, and closing/listing
tabs. Browser tabs are ephemeral and are not persisted as workspace files.

`editor.open_as` opens Markdown/HTML in a **custom read-only preview viewer**.
These previews are a separate tab type, not browser tabs: they do not appear in
`browser.list` and cannot be driven by `browser.navigate`, `browser.read`,
`browser.eval`, `browser.links`, `browser.console`, `browser.upload`,
`browser.device`, or `browser.close`. Use `browser.open` when the agent
needs `browser.*` control.

Long URLs must be ellipsized in the tab header. Console forwarding is batched
and size-bounded to prevent WebKit IPC or a chatty page from flooding the GTK
main thread. Preview rendering escapes untrusted content before converting it
to Pango markup or HTML. Popup creation uses the WebKitGTK 6 `related-view`
property and the `create` signal.

## 6. Unified UI control

`UiControl` is the single command registry used by the agent, D-Bus/GActions,
and local HTTP automation. Calls from non-GTK threads are marshalled through
`Glib::signal_idle` and block until the main thread returns JSON.

Current commands are:

| Group | Commands |
|---|---|
| Layout | `sizer.begin`, `sizer.delta`, `sizer.end`, `panel.width`, `panel.switch` |
| Editor | `editor.open`, `editor.open_as`, `editor.selection` |
| Browser | `browser.open`, `browser.navigate`, `browser.read`, `browser.eval`, `browser.links`, `browser.console`, `browser.upload`, `browser.device`, `browser.list`, `browser.close` |
| Introspection/testing | `app.geometry`, `app.measure`, `app.screenshot`, `app.themeprobe`, `app.widgettree`, `app.commands` |
| Chat | `chat.send`, `chat.cancel` |

Argument details live beside each registration in `ui/MainWindow.cpp` and in
`app.commands`. Commands return structured JSON, normally `{"ok": ...}` or
`{"error": ...}`. Browser evaluation is asynchronous at the WebKit boundary but
presented through the same command result contract.

`browser.device` exposes the 📱 menu's responsive-testing state. With no
action it returns all presets (stable `id`, menu `label`, dimensions, and
`mobile` UA flag) plus `current`. `{preset:"iphone-14-15"}` selects a
preset; add `rotate:true` to swap its dimensions. `{rotate:true}` without
a preset rotates the current fixed size, and `{fit:true}` restores the
allocated tab size. Actions return the selected dimensions and rotation
state. Preset selection and rotation reload the page, as the menu does, so
UA-sniffing sites see the updated User-Agent. The actual viewport can then
be verified with `browser.eval` using `window.innerWidth` and
`window.innerHeight`.

### Loopback HTTP control server

`ControlServer` binds **127.0.0.1 only**, default port **17878**. Set
`MONKEYBYTE_CONTROL_PORT` to choose another port or `0` to disable it. A port
conflict logs a warning and leaves the application running without the server;
it must not abort a secondary instance.

The complete HTTP API is:

| Method | Path | Behavior |
|---|---|---|
| `GET` | `/api/ui/geometry` | Execute `app.geometry` |
| `GET` | `/api/ui/commands` | Execute `app.commands` |
| `POST` | `/api/ui/control` | Body `{"command":"name","args":{...}}`; execute on GTK main thread |

Example:

```bash
curl -s http://127.0.0.1:17878/api/ui/commands
curl -s -X POST http://127.0.0.1:17878/api/ui/control \
  -H 'Content-Type: application/json' \
  -d '{"command":"panel.switch","args":{"panel":0}}'
```

There is no authentication. Never expose this listener on a non-loopback
address. No other MonkeyByte workspace/chat/settings REST endpoints exist.

## 7. Voice and audio

ChimpFlow is optional and loopback-only (`localhost`, `127.0.0.1`, or `[::1]`).
It supplies local TTS and STT through dedicated `tts` + `stt` credentials. An
empty key field resolves `CHIMPFLOW_KEY` at request time. Voice support is
opt-in. Discovery uses `GET /v1/voices?model=<selected-model>`; changing the
TTS model refreshes its voices and discards a saved voice that is unavailable
for that model. A stale saved model falls back to the first discovered model.

PipeWorker capture/playback and ChimpFlow network calls run on worker threads;
`VoiceService` public operations and widget callbacks remain on the UI thread.
`Glib::Dispatcher` returns results to GTK and generation counters suppress stale
completion. Exceptions are contained per utterance/operation.

PipeWire event tables have static lifetime because `pw_stream_new_simple()`
retains their pointers. Streams are destroyed under the thread-loop lock and
unlocked before stopping/joining the loop. Playback completion is based on the
`drained` event.

Playback is normalized to 0.12 RMS with at most 4× gain and a 0.98 peak limit.
The Agent header's speaker popover controls live playback gain for both read-aloud
and voice previews. Mute sets gain to zero and unmute restores the last audible
level. `io.github.monkeybyte.chat playback-volume` is loaded at startup and saved
by the window close handler.
Read-aloud segments streamed assistant/tell_user sentences FIFO; restored
history, user messages, tool cards, and status notices are not spoken.
Dictation inserts text at the input cursor for review and never auto-sends.
The microphone, record, synchronizing, and three playback-volume states use
app-owned `mb-*-symbolic.svg` assets under `data/icons`, resolved through the
icon search path registered at startup. Do not replace them with Adwaita or
other host-theme names: minimal containers do not necessarily ship those
themes. Every symbolic control icon must remain in the app-private set so it
recolours with the active CSS foreground without a host package dependency.

## 8. Threading and error containment

- GTK objects are main-thread-only. Use `Glib::Dispatcher` or the `UiControl`
  main-thread marshaller at every worker-to-UI boundary.
- Daedalus sink callbacks are `noexcept`; post events and return.
- OmniNet control handlers and voice/network workers catch exceptions and
  return structured errors rather than terminating the process.
- Long-running work uses `std::jthread`/stop tokens where available. Shutdown
  cancels work before destroying owning services.
- Settings and agent operations must not leave partially applied state after a
  failed load or save.
- Exceptions from one chat turn, one browser operation, or one voice utterance
  must not terminate the application.

## 9. Multiple instances and workspace changes

MonkeyByte is non-unique: separate instances can edit separate workspaces.
Settings databases and dconf tolerate multiple processes. Only the first
instance normally owns control port 17878. Running two agents on the same
workspace is unsupported because they share `.daedalus/` state.

Changing workspace cancels any in-flight agent turn before replacing
workspace/agent state. Never retarget `FileService` or the chat controller
while a turn can still access the old workspace.

## 10. Tests and verification

Tests are an assert-based C++ runner in `tests/test_main.cpp`, registered with
CTest. They cover services and non-interactive UI logic, with GTK tests using
the project's offscreen/X11/Wayland setup as appropriate.

From the monorepo root:

```bash
cmake -S . -B build
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

For UI changes also exercise the real application, `app_ui_control`, browser
rendering, resize/sizer behavior, and both light/dark themes. For voice changes
verify live ChimpFlow STT/TTS and PipeWire playback/capture, not just mocked
clients. For storage changes run settings/migration/concurrency tests and
preserve WAL/recovery behavior.
