![MonkeyByte banner](resources/banner.png)

# MonkeyByte

A native desktop **AI code editor** written in C++23 with GTK4 / gtkmm —
a GTK4 port of a previously browser-based UI. The full behavioral
specification lives in [`docs/design.md`](docs/design.md).

Pick a workspace folder, edit files in a GtkSourceView editor, and talk to
an LLM agent in a side chat panel. The agent backend is the
[daedalus](https://gitlab.com/leestripp/daedalus) agent library; LLM
providers are configured through an OpenAI-compatible chat API.

## Requirements

- CMake ≥ 3.28, Ninja, a C++23 compiler
- gtkmm-4.0, gtksourceview-5, libcurl, sqlite3, nlohmann_json ≥ 3.11,
  libpipewire-0.3
- Network access on first configure (fetches daedalus + OmniNet)

## Build & run (development)

```bash
# §Monorepo (2026-09-24): PREFERRED — build everything from the repo root:
#   (cd .. && cmake -S . -B build && cmake --build build -j$(nproc))
# One ./build for library + hosts. The standalone build below still works
# (dual-mode) but creates a per-component build/ you must clean up yourself.
# (run.sh auto-detects both build trees; superbuild binaries land in
# build/icarus/ and build/monkeybyte/).
cmake -B build -G Ninja
cmake --build build
./run.sh
```

> **Note:** running `./build/monkeybyte` directly aborts with
> `Settings schema 'io.github.monkeybyte.*' is not installed` — the
> GSettings schemas are compiled into `build/gschemas` but not installed.
> `run.sh` exports `GSETTINGS_SCHEMA_DIR` so the app finds them. To run
> the binary yourself:
>
> ```bash
> GSETTINGS_SCHEMA_DIR=$PWD/build/gschemas ./build/monkeybyte
> ```

## Install (no sudo required)

```bash
./install.sh            # installs to ~/.local
./install.sh /some/dir  # or a custom prefix you can write to
```

This installs the binary to `bin/`, themes to `share/monkeybyte/data/`,
the compiled GSettings schema to `share/glib-2.0/schemas/`, a
freedesktop entry to `share/applications/monkeybyte.desktop`, and the
monkey-head icon (`resources/monkeybyte.svg`) to
`share/icons/hicolor/scalable/apps/` — so the app
shows up in your desktop's launcher. Under the
default `~/.local` prefix, GSettings finds the schema automatically via
XDG data dirs, so the installed binary runs from anywhere:

```bash
~/.local/bin/monkeybyte
```

(Make sure `~/.local/bin` is in your `PATH` if you want to just type
`monkeybyte`.)

## Optional local voice (ChimpFlow)

MonkeyByte can use a local [ChimpFlow](../ChimpFlow) gateway for assistant
read-aloud and microphone dictation. Audio and transcripts stay on the
machine; MonkeyByte accepts only loopback HTTP URLs (`localhost`,
`127.0.0.1`, or `[::1]`).

1. Start ChimpFlow (default `127.0.0.1:8090`) and create a dedicated key
   with **both `tts` and `stt` scopes**. Set it as `CHIMPFLOW_KEY`, or enter
   it in Settings; an empty key field uses the environment variable.
2. Open **Settings → ChimpFlow**. Keep the server at `http://127.0.0.1`
   and port `8090`, then click **Refresh Models / Voices**.
3. Select the discovered TTS model, STT model, and voice. Selecting a voice
   plays a short preview; **Preview Voice** replays it.
4. Enable local voice support, read-aloud, and/or dictation as desired,
   then click **Save Settings**. The master switch is off by default.

All playback is loudness-normalised to a consistent RMS level, with capped
gain and peak limiting so quiet voices are not excessively amplified and
loud voices do not clip.

The chat microphone records on the first click and transcribes on the
second. The transcript is inserted at the input cursor for review and is
**never sent automatically**. With read-aloud enabled, each newly finalized
live assistant bubble is spoken in FIFO order; restored history, user
messages, tool cards, and status notices are not spoken.

## Multiple instances

MonkeyByte is a non-unique GApplication: you can run as many instances as
you like side by side, one per workspace. Pass a folder to open that
workspace in the new instance (a file path opens its parent folder):

```bash
monkeybyte ~/src/project-a &
monkeybyte ~/src/project-b &
```

With no folder argument each instance restores the most recently used
workspace. Global settings (theme, providers/models, interface options)
are shared between instances — the settings DBs use WAL + busy timeouts
and GSettings/dconf is multi-process safe — but window state is per
instance. Only the first instance owns the HTTP control port; later
instances log a warning and run without it (override with
`MONKEYBYTE_CONTROL_PORT`). Running two instances on the *same* workspace
is not supported (they would share its `.daedalus/` agent state).

## Chat agent

The side-panel chat is a full agent session, not a one-shot prompt box:

- **Keep typing while it works** — the input is never disabled. Messages
  sent mid-turn are queued (FIFO) and run back-to-back when the current
  turn finishes. Stop interrupts only the *current* turn; queued messages
  still run. A cancelled turn never leaks a stale message into the next
  one.
- **Session continuity** — the conversation persists across restarts
  (per workspace). On startup the transcript you see is the agent's own
  restored history, so what you read is exactly what the model sees —
  even after context compression has rewritten the middle of it.
- **/commands** — via the `/` button: `/help`, `/clear`, `/init`,
  `/compress`, `/usage`, `/tools`, `/skills`, `/mcp`. `/compress` reports
  what actually happened (compressed, too short, deferred to the next
  turn) instead of silently no-oping.
- **Live feedback** — streamed replies, per-tool progress cards with live
  command output, context/token stats in the status bar, and error
  banners with actionable hints (bad API key, rate limit, …) while
  transient failures retry in the background.
- **Approvals** — bash commands ask before running (Deny / Approve /
  Always for that command), with a global auto-approve toggle in
  Settings → Interface.
- **Skills, tools & MCP** — the agent loads workspace `.agents/skills/`,
  built-in tools (file ops, search, git, web fetch/search, browser
  control, image reading) and configured MCP servers; each group can be
  disabled in Settings → Agent.

## Project types

Workspaces can have a **project type** that focuses the agent: a tailored
system prompt, starter-file templates, and detection rules. Built-in types:
Generic, C++ (C++23), Web (HTML/JS), Filmmaking.

- **Creating a workspace**: Workspace panel '+' menu → "New Workspace…"
  creates the folder, applies the chosen type, scaffolds the type's
  starter files plus a stamped AGENTS.md, and opens it. (Open Workspace
  stays a pure folder picker — it never creates or types anything.)
- **Choosing a type for an existing folder**: Workspace panel '+' menu →
  "Set Workspace Type…". Type is auto-detected from the folder's files
  (e.g. `CMakeLists.txt` → C++, `*.fountain` → Filmmaking); setting it
  writes a `<!-- monkeybyte:type: <id> -->` stamp into AGENTS.md so the
  choice sticks across sessions and tools.
- **Managing types**: Settings → Workspace lists the library with
  Edit Prompt… (rewrites the type's .md body, front matter preserved),
  Rescan, Open Types Folder and New Type. Types are plain Markdown files
  with front matter in `~/.local/share/monkeybyte/project-types/`
  (`<id>.md` + optional `<id>.templates/` starter files), editable and
  shareable like any file. The same tab also holds the advanced per-model
  prompt overrides, which always win over the workspace type prompt.
- **Filmmaking**: screenplays are written in Fountain (`.fountain`), the
  open plain-text screenplay format; everything else (treatments, shot
  lists, notes) is Markdown. Right-click a `.fountain` file for "Open as
  Screenplay Preview" and "Export to PDF…" — both need an external
  converter on PATH: `wrap` (preferred) or `screenplain`. The preview has
  a zoom bar (− / percentage / +, or Ctrl+- / Ctrl++ / Ctrl+0); since
  screenplay HTML is styled for print, the default zoom is display-aware
  (monitor DPI and scale factor plus the desktop text-scaling setting), so
  text stays readable on high-DPI screens. The zoom you pick is shared by
  all screenplay previews for the session.

## Opening files

Clicking a file in the Workspace tree opens it in the editor when it's a
text-like format (anything GIO classifies as `text/plain` or a subtype —
code, Markdown, JSON, shell scripts, … — with a content sniff as fallback).
Media and other binary formats (`.mp4`, `.mp3`, `.mov`, images, PDFs,
archives, …) are instead handed to the **system-default application** via
the freedesktop file associations (the in-process equivalent of
`xdg-open`) — no internal viewer, no giant binary dumped into an editor
tab. The tree's right-click menu also has **"Open with System
Application"** on every file, handy for opening e.g. a `.md` in a real
Markdown editor or an `.svg` in Inkscape. If a file type has no
application registered, MonkeyByte tells you instead of failing silently.

## Built-in browser

MonkeyByte has a real web browser built into the editor area (WebKitGTK —
JavaScript enabled, arbitrary local/remote URLs). Open one from the ☰
tab-bar menu (**Open Browser**, loads the home URL from Settings →
Interface) or let the agent open one for you — typically the dev server it
just started.

Features:

- **Navigation bar** — back/forward/reload plus an editable address entry
  (bare hosts like `localhost:3000` are normalized automatically).
- **📱 device presets** — screen-size menu with Phone / Tablet / Laptop /
  Desktop presets (iPhone, Galaxy, Pixel, iPad, 1366–2560 px) plus
  **Rotate** and **Fit to Window**. A preset constrains the *real* WebKit
  viewport, so width-based media queries respond exactly as on the device;
  phone presets also spoof a mobile User-Agent for UA-sniffing sites.
  Presets smaller than the tab sit centered; ones larger than the window
  (e.g. a 1920×1080 desktop on a small screen) get scrollbars to pan
  across the full viewport.
- **Preview tabs** — workspace markdown and HTML files open as rendered
  previews alongside code tabs ("Open as …" in the Workspace tree).
- **Agent integration** — the chat agent can drive and inspect browser
  tabs (`browser.open / navigate / read / eval / links / console /
  upload`), e.g. open your app, read the rendered page, click around and
  report console errors. Also available over the HTTP control port for
  external automation.

Browser tabs keep no history across restarts and can't be saved/deleted —
closing the tab closes the session.

## Tests

```bash
cd build && ctest --output-on-failure   # all tests (incl. daedalus')
./build/unit_tests                      # only monkeybyte's own suite
```

## Project layout

- `src/monkeybyte/` — application code (see `AGENTS.md` for the tour)
- `tests/` — assert-based unit tests (no framework)
- `data/` — GSettings schema + dark/light CSS themes
- `docs/design.md` — full design specification (authoritative)
- `AGENTS.md` — contributor/agent guidance: conventions, testing, security

## License

[MIT](../LICENSE)
