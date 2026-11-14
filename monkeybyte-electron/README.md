# MonkeyByte Electron

MonkeyByte is a standalone Electron AI code editor built directly on the
existing Daedalus agent library. It includes the workspace/editor, Search and
read-only Git panels, persistent agent chat, isolated browser tools, complete
shared settings, project types, Fountain preview/export, and local ChimpFlow
voice. Electron is now the only MonkeyByte desktop application; the retired
GTK implementation is not embedded or linked. The agent header keeps its session, model, and thinking controls on responsive
rows; model and thinking share the panel's full current width and grow when it
is resized. Steer and Stop always occupy the same compact row above a
full-width Send button; they are greyed out while idle and enabled while busy,
so the input layout never jumps when a turn starts or ends.
During a busy turn Send keeps its label and remains enabled so additional
messages can still be queued. Steer is a true mid-turn correction: it enters
the provider chain only after every result for the current assistant tool
calls has completed, immediately before the next model request. It never
splits a tool call/result pair or an unfinished assistant response. If the
turn ends or is cancelled first, Steer safely becomes the next queued user
message instead of being lost.
The bottom status bar mirrors
GTK's model, live token/context usage, cache hits, iterations, and tool counts;
the chat activity strip has distinct thinking/streaming/tool text and an actual
animated spinner while work is active.

## Requirements

- Node.js 22+ and npm for development.
- CMake 3.28+, a C++23 compiler, and Daedalus's normal native dependencies for
  the loopback backend.
- A working Chromium sandbox. **Never use `--no-sandbox`.**
- Optional: `wrap` or `screenplain` on `PATH` for Fountain preview/PDF export.
- Optional: a local ChimpFlow gateway for speech transcription/narration.

## Develop

From the repository root:

```sh
cmake -S . -B build
cmake --build build --target monkeybyte-electron-backend -j4
cd monkeybyte
npm ci
npm start
```

The C++ backend is a loopback-only HTTP/SSE subprocess. It uses the existing
Daedalus engine and shared `daedalus.db`; chat transcripts are not duplicated in
renderer storage. Startup uses Daedalus's post-load, post-sanitization history
snapshot: dangling tool calls and orphaned results are removed by the engine,
and pure tool plumbing or empty entries are not rendered. Successful `tell_user`
content is restored as assistant text. Each named chat has its own transcript
file and SQLite key; legacy default history migrates once into the first named
session that loads it, and deleting a temporary chat cannot clear another chat.
ChimpFlow reads GTK's canonical `chimpflow.*` settings with Electron's legacy
JSON as fallback, and saving from Electron updates both. Load Voices and Preview
use the current form values, including a newly typed key before the first save.
The TTS model dropdown discovers the host's actual models and defaults to
`kokoro-82m`; Pocket TTS is legacy only because of intermittent garbled output.
Changing the model queries `/v1/voices?model=<model-id>` and refreshes the
compatible voices. Discovered catalogs survive Save, and background settings
updates cannot clobber active ChimpFlow edits/discovery. Voice playback permits
local blob audio through CSP. Microphone recordings are decoded and converted
to 16 kHz mono WAV before transcription, matching the reliable GTK/Whisper path. Electron's
`userData/electron-settings.json` contains only non-secret interface state.
Closing the window awaits persistence of its actual bounds and the rendered
sidebar/chat widths before shutdown continues. Geometry, backend, event-stream,
and UI-server shutdown are bounded; after both windows and backend close,
`app.exit(0)` lets the launching bash command return instead of hanging.
API keys, MCP bearer tokens,
search credentials, and ChimpFlow keys stay in the trusted backend/main-process
boundary.

### Workspace tree context menus

Right-click a file or folder, or focus it and press **Shift+F10** / the Menu
key. The GTK-aligned actions include Open, system-application opening,
format-specific previews, New File Here, New Folder Here for directories,
Rename, Delete, and Copy Path. Electron also retains Copy Absolute Path,
Reveal in File Manager, import, and terminal actions. Arrow keys/Home/End
navigate the menu and Escape restores focus to the tree item. The menu is a
top-level overlay so the hidden Settings panel cannot suppress it, and its
position/height are clamped to the viewport. Clipboard writes are awaited as
required by Electron 45.

Packaged smoke coverage exercises real file/folder creation, rename, both copy
actions, keyboard navigation/focus restoration, preview availability, and
viewport containment in a temporary workspace. Requested smoke-test failure
codes survive normal window shutdown instead of being overwritten with zero.

### Built-in browser controls and agent evaluation

Open a browser yourself with the **🌐 Open Browser** button beside New File
in the tab bar. It loads Settings → Interface → Browser home URL, or Google when
that setting is empty, then focuses the address bar. The agent can open other
browser tabs independently.

The browser toolbar keeps its address, navigation, viewport, and color-scheme
controls within the available width. Navigate with Enter or **Go**. The
**Dark/Light** switch defaults to **Dark** and changes the page's
`prefers-color-scheme` without changing MonkeyByte's editor theme. Dark mode
also sends `Sec-CH-Prefers-Color-Scheme: dark` and enables Chromium's painted
auto-darkening for light-only pages. The selection persists across navigation
and applies to newly opened tabs. Browser backgrounds follow the selected
scheme while pages load.

`browser.eval` uses Chromium's isolated debugger evaluation, so agent scripts
may contain multiple statements and return Promises. Promise results are
awaited, and page exceptions retain their actual messages for debugging. The
existing bounded `browser.open/navigate/read/eval/links/console/upload/list/close`
operations remain available through `app_ui_control`; browser pages never
receive the privileged editor preload.

Live Electron-agent acceptance used a dedicated workspace outside the source
checkout. The agent opened and cross-navigated controlled sites, then used DOM,
console, links, and asynchronous evaluation to reproduce a null-element load
exception, incorrect string-concatenated totals, an unlabeled quantity input,
and two missing HTTP endpoints. External acceptance also opened Google and Wikipedia in the same browser tab,
read their live content, and listed their links successfully.

### Chromium sandbox on this checkout

`/projects` is `nosuid` and this Ubuntu host restricts unprivileged user
namespaces with AppArmor. Copy Electron to a setuid-capable home directory and
reuse the existing root-owned helper for checkout development:

```sh
RUNTIME="$HOME/.cache/monkeybyte-electron-runtime"
rm -rf "$RUNTIME"
cp -a node_modules/electron/dist "$RUNTIME"
rm -f "$RUNTIME/chrome-sandbox"
ln -s "$HOME/Applications/hivemind-client/chrome-sandbox" \
  "$RUNTIME/chrome-sandbox"
MONKEYBYTE_ELECTRON_EXECUTABLE="$RUNTIME/electron" npm start
```

Do not use sudo from an unattended agent session. The launcher fails closed
with repair instructions when no sandbox is available. Installed packages use
their own root-owned helper or another already trusted root-owned helper.

## Tests

```sh
npm run check                 # syntax-check every JavaScript source file
npm test                      # 35 Node unit tests
npm run test:smoke            # real sandboxed Electron integration suite
npm run test:package          # build and test the actual packaged application
npm run test:close            # normal packaged close returns within 12 seconds
```

For checkout smoke tests on this host, use the copied runtime above and run:

```sh
MONKEYBYTE_ELECTRON_EXECUTABLE="$HOME/.cache/monkeybyte-electron-runtime/electron" \
  npm run test:smoke
```

The smoke suite uses isolated workspaces, settings, a fake OpenAI-compatible
provider, and a fake ChimpFlow/Fountain converter. It covers workspace/editing,
browser navigation/read/eval/links/console/upload, Markdown/Fountain previews,
persistent sessions and damaged-history restore, real SSE deltas, FIFO turns,
approval/resume, cancellation, `tell_user` rendering,
host browser/editor tools, settings secret redaction, voice/transcription IPC,
project-type scaffolding, PDF export, and real file writes. Agent integration
also checks token/context status text, live stats events, the running CSS
spinner, blob-audio CSP, and discovery with a newly typed ChimpFlow key. No cloud
account or desktop screenshot is used. Real local acceptance additionally passed
authenticated discovery, MP3 preview playback, and speech-to-text after
16 kHz mono WAV conversion. ChimpFlow 0.3.4 acceptance also discovered three
models, switched from Pocket TTS to Kokoro, loaded all 54 Kokoro voices, played
`af_heart`, saved the compatible pair, and confirmed the catalog remained intact. Interactive/provider tests must set a temporary
`XDG_CONFIG_HOME`; `--user-data-dir` by itself does not isolate Electron's real
`electron-settings.json`.

Run everything from the component directory:

```sh
npm run test:all
```

## Build standalone packages

```sh
npm run package
```

Outputs under `release/`:

- `MonkeyByte-linux-x64/` — portable application directory;
- `monkeybyte-electron-<version>-linux-x64.tar.gz` — portable archive;
- `monkeybyte-electron_<version>_amd64.deb` — Debian package when `dpkg-deb`
  is present; it installs `monkeybyte-electron` and the primary `monkeybyte.desktop` launcher.

The package includes Electron, all renderer/main/preload sources, built-in
project types, and `resources/backend/monkeybyte-electron-backend`. Packaging
uses the pinned Electron runtime directly; it adds no bundler or packaging
dependency. The Debian post-install step installs the root-owned mode-4755
sandbox helper. The portable launcher never disables sandboxing: it uses a
trusted root-owned helper where required, or the namespace sandbox where the
host permits it.

## Install

From the repository root:

```sh
./install.sh                 # ~/.local
./install.sh /custom/prefix  # custom prefix
```

This installs Icarus and Electron MonkeyByte as `bin/monkeybyte-electron`.
The single desktop entry is named **MonkeyByte**, points to Electron, and keeps
the original icon. The legacy GTK executable is removed. Existing user
settings, provider credentials, sessions, skills, and workspaces are preserved.

## Source map

```text
backend/           C++ loopback HTTP/SSE server, Daedalus agent, session catalog
src/main/          Trusted lifecycle, IPC, filesystem, browser, voice, packaging services
src/preload/       Narrow allowlisted contextBridge API
src/renderer/      Vanilla ES modules, semantic HTML/CSS, editor and chat UI
project-types/     Built-in Markdown project definitions and starter templates
scripts/           Sandbox launcher, checks, packaging, smoke drivers
tests/             Node unit tests and C++ catalog/restored-history tests
```

The renderer has no Node integration, generic IPC, arbitrary shell execution,
or unrestricted network/filesystem API. Browser pages run in separate isolated
`WebContentsView` instances. Dictation is always inserted for review, never
auto-sent. Narration is segmented into a FIFO sentence/paragraph queue, uses a
0.12 RMS target with 4× maximum gain and a 0.98 lookahead limiter, and cancels
stale playback on Stop/new narration.

## Keyboard shortcuts

Ctrl+Q is disabled; Quit remains menu-only. Ctrl+W closes only the active document or browser tab, preserves dirty-file confirmation, and does nothing when no tab is active.

## Workspace hidden files

The workspace header keeps its compact hidden-files icon button: **○** means off and **●** means on. It updates the real saved setting directly and refreshes the tree; it does not depend on the Settings page being open.

## Git status

The Git panel and bottom status bar show the branch, changed-file count, staged/unstaged/untracked counts, and numeric ahead/behind counts (commits to push/pull). Non-Git workspaces show **Not a Git repository** without renderer errors; the workspace path no longer replaces this information.

## Agent tool cards and history

Tool cards appear immediately in the running state with expandable argument JSON. The same card updates to **✓ success** or **✕ failure** and adds result JSON; call/result events never create duplicate cards. End-of-turn history merges preserve the visible prompts, responses, notices, and tool cards instead of clearing the live transcript.
