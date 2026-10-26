# MonkeyByte — Design Document for GTK4 / C++23 Port

> **Goal of this document:** specify every visible widget, page, panel,
> state machine, persistence layer, and backend contract in enough detail that
> a separate team (or future-you) can rebuild the application **as a native
> desktop app** — GTK4 widget tree as the UI layer, C++23 classes wrapping
> every GObject, the existing C++23 / OmniNet / daedalus / SQLite backend
> preserved unchanged.
>
> This is a **pure design specification**. No code. No build instructions.
> It describes *what* the app looks like, *what* it does, and *which
> widgets/state/data back each piece*.

---

## 1. Application Identity

| Field | Value |
|---|---|
| Name | MonkeyByte |
| Binary | `monkeybyte` |
| Tagline | "AI Code Editor" |
| Core loop | User picks a workspace → opens files in a CodeMirror-like editor → talks to an LLM agent in a side chat → the agent can read/edit files, run bash, run git, and load MCP tools |
| UI paradigm | IDE-shaped: vertical Activity Bar on the far left, a Sidebar with multiple stacked panels (Workspace, Search, Source Control, Settings), a center Editor with tabs, a Chat panel docked on the right, and a thin Status Bar across the bottom |
| Visual identity | Catppuccin Mocha-style dark theme by default; a light-theme override exists; accent color is `#89b4fa` (catppuccin blue); monospace font for code (`Fira Code`, `Consolas`) |
| Windowing | Single top-level window (`GtkApplicationWindow`); no multi-document-interface tabs at the OS level — file tabs live inside the app |
| Backend to keep | C++23 HTTP server (OmniNet) + daedalus agent + SQLite settings + SettingsStore + McpClient — unchanged. GTK4 is a *replacement for the browser-based UI*, not the agent engine |

---

## 2. Top-Level Window Anatomy

```
┌─ title bar (handled by compositor / GTK) ─────────────────────────────┐
│ ┌─ activity bar (48 px wide) ─┐                                     │
│ │ [Explorer] [Search] [Git]   │  sidebar (260 px, resizable 150–500)│
│ │              ↑               │  ┌──────────────────────────────┐ │
│ │            (spacer)          │  │ panel content                │ │
│ │              ↓               │  │                              │ │
│ │ [Settings]                   │  │                              │ │
│ └──────────────────────────────┘  └──────────────────────────────┘ │
│ ╳ resizer (6 px)                                                     │
│ ┌─ main area ─────────────────────────────┐  ╳ resizer (6 px) ┌────┐│
│ │ Tab bar (36 px)                          │                   │ Ch ││
│ │ ╔══════════════════════════════════════╗ │                   │ at ││
│ │ ║ File header (filename, dirty dot,    ║ │                   │ Pa ││
│ │ ║  save/delete/close buttons)          ║ │                   │ ne ││
│ │ ╠══════════════════════════════════════╣ │                   │ l  ││
│ │ ║ Editor (CodeMirror instance)         ║ │                   │    ││
│ │ ║                                      ║ │                   │    ││
│ │ ╚══════════════════════════════════════╝ │                   └────┘│
│ └──────────────────────────────────────────┘                          │
│ ┌─ status bar (24 px, accent background) ──────────────────────────┐ │
│ │ left: branch info                       right: model / tokens │ │
│ └──────────────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────────────┘
```

### Window-level rules

* The window is `GtkApplicationWindow` with a single child: a `GtkBox`
  vertical. Top child is `GtkBox` horizontal that holds `[activity_bar,
  sidebar, resizer-sidebar, main, resizer-chat, chat_panel]`. Bottom child is
  the `status_bar`.
* The application window enforces a sensible default size (≈ 1100 × 720),
  is resizable, and remembers its size in GSettings.
* Two **resize grips** are real `GtkGestureDrag` widgets between
  `sidebar | main` and `main | chat_panel`. While dragging, the grip turns
  accent color. Persist each width to GSettings on `gesture-end`.
* When **Settings** is the active sidebar panel, the editor and chat panel
  are hidden (`visible=false`) and the sidebar takes 100 % of the client
  width. This is exactly what the CSS class `body.settings-active` does in
  the web app.

---

## 3. The Activity Bar (leftmost 48 px column)

The activity bar is a vertical stack of icon buttons (VS Code style). It is
the **only** navigation control that switches which sidebar panel is shown.

* Visual width: 48 px.
* Background: `--activity-bg` (#11111b dark / #e6e9ef light).
* Buttons are 40 × 40 with a 2 px left "active rail" that lights up in
  `--accent` (#89b4fa) on the currently active icon.
* One `GtkBox` `orientation=vertical` with a `GtkSeparator` / `GtkBox` `vexpand`
  spacer separating the top icons from the bottom (settings) icon.

### Icon buttons (top → bottom)

| Order | `data-panel` (in HTML) | SVG source (Material/Heroicons-like) | Title (tooltip) |
|---|---|---|---|
| 1 | `workspace` | `folder` outline (Heroicons `M10 4H4 ...h-8l-2-2z`) | Workspace |
| 2 | `search`   | magnifying glass with circle            | Search |
| 3 | `git`      | git branch (nodes + links)              | Source Control |
| — | spacer | `GtkBox` with `vexpand=true` | — |
| 4 | `settings` | `cog / cog-6-tooth`                     | Settings |

* `data-panel` is just a key: when clicked, set the matching sidebar page
  visible and all others hidden. There is no separate "selected" widget for
  pages; **all 4 sidebar panels are attached simultaneously**, and visibility
  is toggled (`visible`/`hidden`) — same trick the HTML uses.
* Icons are symbolic SVGs (`mb-*-symbolic.svg`) in `data/icons/`, loaded via
  `GtkIconTheme` (the directory is added to its search path). Being symbolic,
  GTK recolors them to the button's CSS foreground color, so the muted /
  hover / accent-active states come from the theme for free.

### Keyboard & accessibility

* `Ctrl+1` / `Ctrl+2` / `Ctrl+3` / `Ctrl+4` to switch panels (the web app
  doesn't bind these, but adding them here is cheap and natural).
* Each button has a tooltip (`GtkWidget::tooltip-text`) matching the web
  `title` attribute.
* Active icon sets `aria-pressed=true` (or, in GTK terms,
  `GtkAccessible::state-checked`).

---

## 4. The Sidebar

A `GtkBox` `orientation=vertical` that holds four child pages. Only one is
visible at a time. Width is stored in GSettings as `sidebar-width`
(default 260 px, range 150–500).

```
┌─ sidebar header ─────────────────────────────────┐
│ WORKSPACE                             [📁][↻][+] │   <- varies per panel
├─ workspace path bar (Explorer only) ──────────────┤
│ /home/user/project                                  │
├─ panel body ───────────────────────────────────────┤
│ ...                                                 │
└────────────────────────────────────────────────────┘
```

### 4.1 Common sidebar header

A `GtkBox` horizontal with:
* Left: a `GtkLabel` title (uppercase, 11 px, letter-spacing 0.5 em,
  `--text-secondary`).
* Right: a `GtkBox` horizontal of action buttons (`btn-icon` 24 px). The
  specific buttons depend on the panel (see below).

The CSS rule `.sidebar-header` translates 1:1 to a `GtkBox` with the same
padding/font/colors.

### 4.2 Sidebar panel: **Workspace Explorer** (default)

This is the largest and most-used panel.

**Header buttons (right):**
1. `📁` — Open Workspace Folder Picker (modal).
2. `↻` — Refresh the tree.
3. `+` — Popup menu (`GtkMenuButton`), three sections:
   * **Add File** — create a new empty file in the workspace root (uses a
     `GtkEntry` dialog with title "New file").
   * **Add Folder** — New Folder modal (§5.2) rooted at the workspace, so
     the target folder can be changed with the in-dialog folder selector
     (same dialog as the tree's "New Folder" context action, which
     pre-selects the right-clicked folder).
   * **Add File from Template…** — template picker (C++ header/source,
     CMakeLists.txt, .gitignore, README.md, MIT LICENSE from
     `backend/FileTemplates`); creates in the workspace root, then refreshes
     and opens the file.
   * **Import File(s)…** — multi-select `GtkFileDialog`; copies the chosen
     files into the workspace root (no overwrite; failures logged), then
     refreshes.
   * **Reveal in File Manager** — opens the workspace root in the OS file
     manager (`GtkFileLauncher`).
   * **Open Terminal Here** — launches an external terminal emulator with
     the workspace root as cwd (`$TERMINAL`, else first of
     kgx/gnome-terminal/konsole/xfce4-terminal/alacritty/kitty/xterm on PATH).
   * **Show Hidden Files** — checkmark toggle; same setting as Settings →
     Explorer → Show Hidden Files (the settings store is authoritative),
     applies live and refreshes the tree.

**Workspace path bar:** a single clickable bar showing the current absolute
workspace path (`/home/lee/...` etc.). Clicking it also opens the folder
picker.

**Body: lazy-loaded file tree.**

* Root: a `GtkTreeView` (or, more idiomatic in GTK4, a `GtkColumnView` /
  `GtkListView` of `GtkTreeListModel` — but `GtkTreeView` with custom renderers
  is fine and matches the simple symbolic icons).
* Tree state lives in `WorkspaceTree` (C++ class). It is defined in the
  `static/js/workspace.js` original. Mirror its public API exactly:

```
class WorkspaceTree {
    setWorkspace(path);          // clears expansion memory, schedules refresh
    refresh() -> Promise<void>;  // rebuilds root from /api/files/
    select(path);                // sets selection highlight, scrolls in
    refreshFolder(path);         // refresh only a single open folder
    revealPath(path);            // expand parents then select
    destroy();

    // signals
    onOpenFile(path);            // user clicked a file
    onShowPicker();              // workspace is empty → open folder picker
    onRefresh();                 // tree has been rebuilt
};
```

* Lazy loading: each folder is fetched on first expand with a 10 s fetch
  timeout. Expand state is held in a `std::unordered_set<std::string>`
  keyed by folder path so that a refresh of the root preserves the user's
  open folders (this is exactly the
  `expandedPaths` Set in the JS).
* Selected file is tracked separately (`selectedPath`). Render a different
  background (`--bg-active`) when set.
* Each row shows (left → right): an expand/collapse arrow (▶, rotates 90°
  on expand; visibility:hidden on files), a symbolic SVG icon (see icon
  table), the filename. Files are not collapsible; clicking a file calls
  `onOpenFile(path)` for text-like files (GIO content type text/plain or a
  subtype, with a NUL-byte sniff fallback — see
  `backend/FileKind::is_text_like_file`); media and other binary formats
  are instead opened in the system-default application
  (`launch_external`, the in-process equivalent of `xdg-open`).
* Sort: directories first, then case-insensitive by name.
* Dotfiles (`.git`, `.env`, …) are hidden by default; the Settings →
  Interface → Explorer → "Show Hidden Files" toggle controls this.
* **Right-click context menu** on a row (`GtkPopoverMenu`):
  * File rows: Open (routed like click — internal editor or system app,
    depending on the file kind) · Open with System Application (always
    offered; launches the freedesktop association default) · Open as
    Markdown Preview (`.md` / `.markdown` only) · Open as HTML Preview
    (`.html` / `.htm` only) · New File Here · Rename… · Delete… · Copy
    Path.
  * Folder rows: New File Here · New Folder Here · Rename… · Delete… ·
    Copy Path.
  * Rename/Delete go through `FileService::rename` / `FileService::remove`
    (sandbox-validated); Delete confirms first. After either, the tree
    refreshes and open editor tabs on the affected path are closed
    (delete) or retargeted (rename). The tree also follows the active
    editor file: switching tabs expands + selects it (`revealPath`).

**File icon set** — symbolic monochrome SVGs in `data/icons/`
(`mb-*-symbolic.svg`, 24×24, `fill="currentColor"`), recolored by GTK from
the CSS foreground so they follow the active theme. The icon theme search
path is registered once in `main()`. `ui/FileIcons.h::file_icon_name()`
maps lowercase extension / exact filename → icon name; the same icons are
used in the file tree rows and the editor tab titles:

```
folder            mb-folder-symbolic      folder-open  mb-folder-open-symbolic
default           mb-file-symbolic
code (c/cpp/h/rs/go/java/kt/cs/php/rb/swift/js/ts/html/css/…)  mb-file-code-symbolic
script (sh/bash/zsh/fish/py)    mb-file-script-symbolic
docs (md/txt/pdf/README/LICENSE)            mb-file-doc-symbolic
data (json/xml/sql + package/cargo/go.mod/requirements)  mb-file-data-symbolic
image (png/jpg/gif/svg/webp/bmp/ico)        mb-file-image-symbolic
archive (zip/tar/gz/rar/7z)                 mb-file-archive-symbolic
config (yaml/toml/ini/cfg/cmake + Makefile/CMakeLists/Dockerfile)  mb-file-config-symbolic
lock (.env/.gitignore/*.lock)               mb-file-lock-symbolic
```

**Empty / loading / error states** render as one centered muted-italic label
("No workspace selected", "Loading workspace…", "Error loading workspace: …").

**Live-refresh rule:** the tree does **not** auto-refresh when the chat
agent mutates files. The web app disabled auto-refresh for the same reason —
it caused churn during agent runs. The tree only refreshes on:
* the `↻` button,
* after the user creates a file,
* after the user deletes a file,
* after `setWorkspace()` is called.

This behaviour must be preserved.

### 4.3 Sidebar panel: **Search**

* Header has only a title (no actions).
* Body is a `GtkBox` vertical with two regions:
  * Top: a `GtkSearchEntry` (`#search-input`) full-width in a
    `GtkBox` `padding=8`. Placeholder "Search files…".
  * Bottom: a `GtkScrolledWindow` containing a `GtkListView`
    (`#search-results`).
* Behavior:
  * 300 ms debounce on `search-changed`. Send `GET /api/search?q=...`.
  * Only text files are searched: hidden dirs, files > 5 MB, and binary
    files (first 8 KB sniffed for NUL / non-printable bytes) are skipped.
  * Entries matching the Settings → Search exclude patterns (globs vs file
    name and workspace-relative path) are skipped; matching directories are
    not descended into.
  * `Enter` triggers an immediate search (no debounce).
  * When empty: render a muted hint "Type to search files…".
  * When searching: render "Searching…".
  * On results: render one row per result, two lines each — first line
    is the file path with the match line appended as `path:line` (monospace,
    accent color), second is the line preview (ellipsized to 200 chars).
    Clicking a row calls `openFile(item.path, item.line)` — the editor opens
    the file, jumps to the matching line, selects it, and centers it in the
    viewport (the current-line highlight marks the spot).
    A muted "N results" header row is shown above the list.
  * On zero results: render a muted "No results found".
  * On error: render a muted "Error: …".

### 4.4 Sidebar panel: **Source Control (Git)**

* Header has only a title.
* Body is a `GtkScrolledWindow` containing a `GtkBox` vertical (`#git-status`).
* On open, call `GET /api/git/status` and render one of:

  | Condition | Render |
  |---|---|
  | No workspace | "No repository — open a workspace that contains a .git directory." |
  | `isRepo:false` | "No repository — open a workspace that contains a .git directory." |
  | `isRepo:true`, no changes | Branch label + "No changes detected" |
  | `isRepo:true`, changes | Branch pill + one row per change with status letter (M/A/D/?/…) and the file path (clickable — opens the file) |
  | Error | "Error loading git status: …" |

* This panel is read-only in MonkeyByte. No diff viewer, no commit button,
  no staging. (Those operations live in the agent's tools.) Don't add them.

### 4.5 Sidebar panel: **Settings**

The biggest panel. Has its own internal tab bar and ten tab pages.

**Outer layout:** a `GtkBox` vertical containing, top-to-bottom:
1. Outer header: only the title `"Settings"`.
2. **Settings sub-tabs** (`#settings-tabs`): one row of small buttons
   (Interface / Providers / Models / Workspace / MCP Servers / Agent /
   Web Search / ChimpFlow / Search / Paths), styled as a
   segmented control. Active tab has the accent underline and
   `bg-panel` background.
3. **Settings content** (`#settings-content`): a `GtkStack` with one
   page per tab, only one visible at a time. Each page is a
   `GtkScrolledWindow` wrapping a `GtkBox` vertical.
4. **Settings footer**: a `GtkBox` horizontal pinned to the bottom. Holds
   a status label (`#settings-status` — empty / "Settings saved
   successfully" / "Failed to save …") and a primary `Save Settings`
   button.

#### Tab 1 — **Interface**

A `GtkBox` vertical of "settings groups", each group is:

```
<h4> Editor / Theme </h4>           <- uppercase 12 px muted header
─────────────────
  Label: Font Size       [ GtkDropDown 12..20 px ]
  Label: Tab Size        [ GtkDropDown 2 / 4 / 8 spaces ]
  Label: Word Wrap       [ GtkDropDown Off / On ]
  Label: Color Theme     [ GtkDropDown System / Dark / Light ]
```

Groups in the GTK port (top → bottom):

* **Workspace** — current workspace path + "Change…" button (opens the
  Folder Picker; this is the "Settings Edit button" of §12.1).
* **Editor** — Font Size, Tab Size, Word Wrap (as above), plus Line
  Numbers and Highlight Current Line (Off/On).
* **Theme** — Color Theme (System / Dark / Light). **System** (the
  default) follows the desktop's color-scheme preference, live;
  Dark/Light are explicit overrides. Resolution order: the
  xdg-desktop-portal `org.freedesktop.appearance` `color-scheme`
  key (uint 1 = dark, 2 = light, 0 = no preference), then the
  GNOME `org.gnome.desktop.interface` `color-scheme` enum
  (`prefer-dark` / `prefer-light` / `default`), then dark.
* **Explorer** — Show Hidden Files (Off/On; hides dotfiles in the tree).
* **Browser** — Home URL (free-text entry; the tab-bar ☰ menu's "Open
  Browser" item opens this URL — normalized like the nav bar's address
  entry, so a bare `localhost:3000` works — or a blank "New Tab" when
  empty; persisted as `browser_home_url`).
* **Chat** — chat area appearance overrides: Bubble Font Size
  (Default/10–18 px) and color pickers (`GtkColorDialogButton`) for Chat
  Background, User Bubble Background, User Text, Assistant Bubble
  Background, Assistant Text. Each is override-only: unset (0 px / no
  color) means "use the active theme", so the default look is unchanged.
  Set values are applied by `ThemeManager::set_chat_overrides()`, which
  generates a CSS provider at `GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1`
  — one rule per set value, layered above the theme and surviving theme
  switches (one override set applies to both themes). Persisted as
  `chat_font_size` + `chat.bg` / `chat.user_bg` / `chat.user_text` /
  `chat.assistant_bg` / `chat.assistant_text`; a "Reset Chat Appearance"
  button clears them all. Picker swatches with no override set re-point at
  the new theme's default on a theme switch.
  Note: markdown syntax-highlight spans in assistant bubbles (code, strings,
  keywords, comments, code background) are Pango colors, not CSS, so the
  Assistant Text override does not recolor them — they follow the active
  theme via `Markdown::set_palette()` (Mocha/Latte), switched from
  `ThemeManager::apply()` (§7.4).

When auto-approve is off, each bash command triggers an approval dialog
explaining that the agent wants to run a shell command in the workspace
(with the user's permissions, able to read or modify files), showing the
command. Three choices: **Deny** (refuse this run), **Approve** (run
once, ask again next time — daedalus's `remember_approvals` is disabled
so approvals are not auto-cached), **Always** (run and never prompt again
for this exact command string, tracked in-memory for the session).

Live apply: changing a value here calls `applyEditorSettings()` immediately
(no Save needed) and persists to `SettingsStore` via
`PUT /api/settings/interface`. The Save button still exists for parity
with the web UI but most settings are auto-saved.

**Theme handling.** The web app sets `data-theme="dark|light"` on the root
and the CSS variables on `:root` flip via a media query. In GTK4 we'll
swap the named `GtkStyleProvider` (`gtk.css`) or set a `style-class`
(`dark`/`light`) on the root widget.

#### Tab 2 — **Providers**

Providers are configured once and referenced by name from models
(§10.2); endpoint and credential details are never re-entered per model.

**Section "Configured Providers":** a `GtkListView` of provider cards.
Each card shows, top-to-bottom:

```
┌─────────────────────────────────────────────────────────────┐
│ [icon] MiniMax       [ Fetch Models ] [ Edit ] [ Delete ]   │   ← header
├─────────────────────────────────────────────────────────────┤
│ BASE URL         https://api.minimax.io                     │
│ ENDPOINT         /v1/chat/completions                       │
│ PLUGIN           minimax  or  (auto: openai-compatible)      │
│ API KEY          •••••••• (32 chars)  or  env: ${X}          │
└─────────────────────────────────────────────────────────────┘
```

* Provider-icon map (used for the leading emoji):
  MiniMax 🔷, OpenAI ⚡, Claude 🟣, Ollama 🦙, Gemini ✨, Mistral 🌬️,
  Grok 🚀, Kimi 🌙. Default: 🤖.
* `Env-key` display if `apiKey` contains `${` (the placeholder pattern).
* **Plugin** (daedalus §ProviderPlugins): each provider resolves to a
  daedalus provider plugin that shapes requests, endpoints, auth and
  model knowledge (`openai-compatible`, `minimax`, `moonshot`,
  `lmstudio`). The card shows the explicit choice, or the registry's
  auto-detection (from provider name/host) as `(auto: <name>)`.
* **Delete** removes the provider *and all models that reference it*
  (daedalus cascades), after a confirmation prompt.
* **Fetch Models** queries the provider's discovery endpoint
  (`GET host + discovery_endpoint`, via `daedalus::discover_models`,
  `${ENV_VAR}` keys resolved first) on a worker thread, then opens a
  picker modal listing the reported model ids (with context length when
  the provider reports it). Checked rows are added as models referencing
  this provider (name = model id).

**Action row** (flex wrap):
* `+ Add Provider` — opens the Provider Edit Modal in create mode.
* `+ MiniMax` / `+ OpenAI` / `+ Ollama` / `+ Kimi` / `+ LM Studio` —
  quick-fill buttons. They open the same modal pre-populated with that
  provider's default URL and endpoints; user only enters the API key
  (LM Studio needs none — its plugin is auth-less).

The `+ MiniMax`, `+ OpenAI`, `+ Ollama`, `+ Kimi`, `+ LM Studio` provider
defaults (each preset also pins its daedalus provider plugin explicitly so
host auto-detection can't surprise — e.g. the lmstudio plugin claims every
loopback host):

| Provider | host                       | chat endpoint                | discovery endpoint | plugin           |
|----------|----------------------------|------------------------------|--------------------|------------------|
| MiniMax  | https://api.minimax.io     | /v1/chat/completions         | /v1/models         | minimax          |
| OpenAI   | https://api.openai.com     | /v1/chat/completions         | /v1/models         | openai-compatible |
| Ollama   | http://localhost:11434     | /api/chat                    | /v1/models         | openai-compatible |
| Kimi     | https://api.kimi.com       | /coding/v1/chat/completions  | /coding/v1/models  | moonshot         |
| LM Studio | http://localhost:1234     | /v1/chat/completions         | /v1/models         | lmstudio         |

Note: Kimi **Code subscription** keys are served under `/coding/v1`; the
bare host answers `/v1/models` (so discovery appears to work with the
generic defaults) but chat completions must use the `/coding/v1` path.

#### Tab 3 — **Models**

Two parts plus an action row.

**Section "Active Model":** `Label: Default Model` + a `GtkDropDown`
populated from the daedalus model registry. Each row shows
`providerName - modelId`. The selected row in this dropdown is the
default model.

**Section "Configured Models":** a `GtkListView` of model cards (`#models-list`).
Each card shows, top-to-bottom:

```
┌─────────────────────────────────────────────────────────────┐
│ [icon] minimax-m3           [ Edit ] [ Delete ] [Default]   │   ← header
│        MiniMax-M3                                           │
├─────────────────────────────────────────────────────────────┤
│ PROVIDER         MiniMax                                    │
│ CONTEXT / TEMP   4096 / 0.7                                 │
│ SYSTEM PROMPT    [ GtkDropDown: Default / <prompt name> ]    │
└─────────────────────────────────────────────────────────────┘
```

* `Default` badge if the model is the registry's default model.
* The **System Prompt** dropdown is per-card; the binding lives in
  `monkeybyte.db` (`model_prompts` table, §10.2).

**Action row:**
* `+ Add Model` — opens the Model Edit Modal in create mode. The modal
  has a provider dropdown; if no providers exist yet, add one in the
  Providers tab first.

#### Tab 4 — **System Prompts**

A list of cards (`#system-prompts-list`), one per saved prompt:

```
┌─────────────────────────────────────────────────────────────┐
│ Code Reviewer                            [ Edit ] [ Delete ] │
│ [ Default ]                                                 │
│ preview text… max 300 chars then …                          │
└─────────────────────────────────────────────────────────────┘
```

* Default-flag badge if `isDefault=true`.
* Preview truncated to ~60 px height with a fade gradient at the bottom.
* A `+ New Prompt` button below opens the System Prompt Modal in create mode.

#### Tab 5 — **MCP Servers**

Same card layout pattern but the card shows:

```
┌─────────────────────────────────────────────────────────────┐
│ filesystem                     ( Switch )  [ ✎ ] [ × ]       │
├─────────────────────────────────────────────────────────────┤
│ COMMAND                                                    │
│ npx -y @modelcontextprotocol/server-filesystem /path      │
│ ENVIRONMENT                                                │
│ MY_KEY, OTHER_VAR                                          │
└─────────────────────────────────────────────────────────────┘
```

* Add button: `+ Add MCP Server` opens MCP Server Modal in create mode.
* The header **switch** enables/disables the server (daedalus
  `McpServer.enabled`). Disabled servers are also passed to the agent via
  `AgentConfig::mcp_servers_disabled` so the MCP manager skips them.
* The tab lists the daedalus MCP registry (`daedalus::SettingsStore` in
  `daedalus.db`) — the servers the agent actually loads — not a
  monkeybyte-local list. Edits trigger an agent rebuild so they take
  effect on the next chat message.

#### Tab 6 — **Agent**

Host-policy switches and tuning for the daedalus agent. Every change
persists immediately and triggers an agent rebuild on the next message.

* **Behavior** — Auto-approve Bash (on/off switch; when on, daedalus runs
  bash commands without the approval dialog — persisted as the
  `agent.auto_approve` key; the approval dialog itself is described under
  Tab 1 — Interface).
* **Skills** — one switch per skill discovered in the global skills
  directory (`daedalus::Paths::global().global_skills_dir()`, each
  immediate subdirectory with a `SKILL.md`; name/description parsed from
  the YAML front matter). Disabled names are stored in
  `agent.disabled_skills` (monkeybyte.db kv) and seeded into
  `AgentConfig::skills_disabled` — daedalus does not persist these.
* **Tools** — one switch per tool. The full tool list comes from the
  live agent session; without a session only the currently-disabled
  tools are listed (so they can be re-enabled). Disabled names live in
  `agent.disabled_tools`, seeded into `AgentConfig::tools_disabled`.
* **Limits** — Max agent iterations, Max tool result (KB), Max
  request/response (MB); read/written through daedalus's `SettingsStore`
  kv settings and applied to `AgentConfig` on agent build.
* **MCP** — master switch (`agent.mcp_enabled`, default on) mapped to
  `AgentConfig::mcp_enabled`; off disables all MCP tool integrations.

#### Tab 8 — **ChimpFlow** (§CHIMPFLOW)

Local voice support is opt-in (master switch defaults off). The page has
master, **Read assistant replies aloud**, and **Enable microphone dictation**
switches; loopback server URL + port; a masked `tts` + `stt` API-key entry
(empty resolves `CHIMPFLOW_KEY` at request time); discovered TTS-model and
voice dropdowns; an STT-model entry; Refresh and Preview buttons; and a
busy/error status label.

* Refresh runs health/model/voice discovery on a worker without blocking
  GTK. Discovery and preview use the current widget values even before Save.
  List population is suppression-guarded and never auto-plays. An actual
  user voice selection plays a short sample; Preview replays it.
* Save validates loopback HTTP (`localhost`, `127.0.0.1`, `[::1]`) and the
  port, persists all fields under `chimpflow.*` in `monkeybyte.db`, and calls
  the shared `VoiceService::reload()` immediately—no agent rebuild. Enabling
  requires a scoped key, TTS model, and voice; dictation also needs STT model.
* Cancelling discovery/preview invalidates stale callbacks and clears busy
  state. Async callbacks carry a panel-lifetime token. Keys are masked and
  never logged; transcripts and assistant text never leave the machine.

#### Tab 9 — **Search**

* A monospace multi-line editor with the search **exclude patterns**, one
  glob per line, auto-saved on edit (SQLite `search.excludes`).
* Patterns match against file names and workspace-relative paths; `*` and
  `?` wildcards (`fnmatch`, `*` may cross `/`). Matching directories are not
  descended into; matching files are skipped.
* Seeded with defaults on first run (`build`, `_build`, `cmake-build-*`,
  `node_modules`, `__pycache__`, `target`, `dist`, `*.o`, `*.a`, `*.so`,
  `*.dylib`, `*.exe`, `*.dll`, `*.class`, `*.pyc`, `*.min.js`, `*.min.css`).
* A `Reset to Defaults` button restores the seeded list.

#### Tab 10 — **Paths**

A read-only, grouped listing of every filesystem location the app and its
embedded libraries use, so users can find their data. Each entry shows a
name, a one-line description, and the resolved absolute path in a
selectable (copyable) monospace label; paths that do not exist on disk yet
are dimmed and marked "(not created yet)". Not affected by the Save
button.

Groups (top → bottom):

* **MonkeyByte** — settings database (`$XDG_DATA_HOME/monkeybyte/monkeybyte.db`,
  §10.2), current workspace, per-workspace daedalus agent data
  (`<workspace>/.daedalus/daedalus/`, shown only when a workspace is set),
  and the GSettings/dconf UI cache location.
* **Daedalus** — the user-tier roots from `daedalus::Paths::global()`:
  config (`~/.config/daedalus`), data (`~/.local/share/daedalus`, incl.
  `daedalus.db`), state (`~/.local/state/daedalus`, logs/history/stats),
  cache (`~/.cache/daedalus`).
* **OmniNet** — config file (`~/.config/omninet/config.json`) and data
  dir (`~/.local/share/omninet`), as resolved by `daedalus::Paths`.

---

## 5. Modal Dialogs

All modals share visual style: centered overlay with 75 % black + 4 px
backdrop blur, a `GtkWindow` (or a `GtkPopover` from the parent button) with
a `12 px` rounded corner, header (title + `×` close), body (`GtkBox`
vertical scrolling), and footer (`GtkBox` horizontal with Cancel / Save).

GTK4 note: prefer `GtkDialog` (with `GtkHeaderBar`) for simplicity and
proper a11y modal behavior. All six modals are siblings of the main window;
they are not children of any panel. (The web app does this by stacking
them in `<body>`.)

### 5.1 Workspace Folder Picker (modal)

The single most complex dialog. It is a "file chooser" custom-built rather
than a native `GtkFileChooserDialog` because it needs to stay within the
configurable workspace root.

Components:

```
┌ Open Workspace ──────────────────────────────┐ [📁+ ] [×] ┐
├ /home/lee                                              ┤  <- breadcrumb
├ ┌─────────────────────────────────────────────────────┐│
│ │ .. (parent folder)                                  ││
│ │ 📁 Documents                       ▶                ││
│ │ 📁 Downloads                       ▶                ││
│ │ 📁 Projects                        ▶                ││
│ └─────────────────────────────────────────────────────┘│
├┌──────────────────────────────────────────────────────┐┤
││ [/path/to/workspace                          ]       ││   <- manual entry
│├──────────────────────────────────────────────────────┤┤
││[● Show hidden]                       [Cancel] [Open]││
│└──────────────────────────────────────────────────────┘┘
```

* Clicking the `📁+` (Create new folder) button at the top opens the
  **New Folder modal** as a child of this dialog.
* Clicking any row navigates into that folder (single click = expand).
* The breadcrumb shows the absolute path (or the text "(workspace root)"
  when at the configured root).
* The path text input is editable; Enter triggers Open.
* Open's logic: prefer the explicitly selected row's path; otherwise use
  the breadcrumb.
* "Show hidden" is a `GtkSwitch`. Its state persists to GSettings
  (`folder-picker-show-hidden`).
* Empty / loading / error states mirror the sidebar file tree.

### 5.2 New Folder modal (§4.2 Workspace panel / nested inside §5.1)

```
┌─ Create New Folder ──────────────────────────── [×] ┐
│ [/home/lee/project/src                      ]       │   <- breadcrumb
│┌──────────────────────────────────────────────────┐│
││ 📁 ..                                            ││
││ 📁 include                                       ││   <- subfolder list,
││ 📁 src                                           ││      double-click to
│└──────────────────────────────────────────────────┘│      navigate
│ [/home/lee/project/src                      ]       │   <- manual entry
│  Folder name:                                     │
│ [ my-new-folder                            ]      │
│                       [ Cancel ] [ Create ]       │
└───────────────────────────────────────────────────┘
```

* The top half is the shared **FolderPicker** widget (also used by the New
  File modal, §4.2): breadcrumb + navigable subfolder list + editable path
  entry, confined to a root — the workspace root when launched from the
  Workspace panel, the folder being browsed when nested inside §5.1.
* The list always starts with a `..` parent row (except at the root); as in
  §5.1 it stays mounted even in empty/error states so the user can always
  navigate back up.
* Create emits (parent folder, name); the caller performs the mkdir —
  `FileService::mkdir` + tree refresh for the Workspace panel, plain
  `std::filesystem::create_directories` + picker refresh for §5.1.
* The name must be a plain folder name (no path separators).
* Create is disabled until a name is typed. Enter in the entry triggers
  Create. Escape closes.

### 5.3 Provider & Model Edit modals

**Provider Edit modal** (Settings → Providers):

```
┌─ Edit Provider ─────────────────── [×] ┐
│                                        │
│  PROVIDER NAME      [ MiniMax      ]   │
│  BASE URL           [ https://...  ]   │
│  CHAT ENDPOINT      [ /v1/chat/completions] │
│  DISCOVERY ENDPOINT [ /v1/models   ]   │
│  API KEY            [ ●●●●●●●●     ]   │
│  TIMEOUT (SECONDS)  [   30         ]   │
│  PLUGIN             [ Auto-detect  ▼ ] │
│                                        │
│                [ Cancel ] [ Save ]     │
└────────────────────────────────────────┘
```

* The `Provider Name` field is the unique ID; it is read-only in edit mode.
* API keys support `${ENV_VAR}` placeholders, resolved at agent start.
* The `Plugin` dropdown (daedalus §ProviderPlugins) lists **Auto-detect**
  (the default — daedalus resolves from provider name/host) plus every
  plugin registered in `daedalus::ProviderRegistry`. An explicit choice is
  stored in the provider row's `plugin` column and wins over detection.

**Model Edit modal** (Settings → Models): endpoint and credential fields
come from the selected provider — they are not re-entered here.

```
┌─ Edit Model ──────────────────────── [×] ┐
│                                          │
│  PROVIDER         [ MiniMax        ▼ ]   │
│  NAME (UNIQUE ID) [ minimax-m3     ]    │
│  MODEL ID         [ MiniMax-M3     ]    │
│  ┌─────────────┬──────────────────────┐  │
│  │ MAX CONTEXT │ TEMPERATURE          │  │
│  │ [   4096 ]  │ [   0.7 ]            │  │
│  └─────────────┴──────────────────────┘  │
│                                          │
│                [ Cancel ] [ Save ]       │
└──────────────────────────────────────────┘
```

* The `Name` field is the unique ID stored in the registry; it is
  read-only in edit mode, as is the provider dropdown.
* `Max Context` = 0 falls back to the model's default.
* Both modals are used for create and edit; in create mode the form is
  empty (or preset-filled), in edit mode the values are pre-loaded.

### 5.4 MCP Server modal

```
┌─ Add MCP Server ──────────────────── [×] ┐
│                                          │
│  SERVER NAME      [ filesystem      ]    │
│  COMMAND          [ npx -y @model…  ]    │
│  ENVIRONMENT (JSON)                       │
│  ┌────────────────────────────────────┐  │
│  │ { "KEY": "value" }                  │  │
│  └────────────────────────────────────┘  │
│  [×] Enabled                              │
│                                          │
│                [ Cancel ] [ Save ]       │
└──────────────────────────────────────────┘
```

* On save: `POST /api/settings/mcp` (create) or
  `PUT /api/settings/mcp/<name>` (edit). `env` is parsed as JSON; if it's
  invalid, show a status error under the Settings footer and don't close.
* Default: enabled=true.
* The tab also offers **Add from JSON…**, which accepts the raw JSON MCP
  providers publish: a full `{"mcpServers": {…}}` block, a single keyed
  object (`{"name": {…}}`), or a bare single-server config with a `name`
  field. All servers in one paste are imported at once (stdio `command`/
  `args`/`env`, or http `url` with an optional bearer token); invalid
  JSON keeps the dialog open with an inline error.

### 5.5 System Prompt modal

```
┌─ New System Prompt ──────────────── [×] ┐
│                                          │
│  PROMPT NAME  [ Code Reviewer       ]    │
│  SYSTEM PROMPT                          │
│  ┌────────────────────────────────────┐  │
│  │                                    │  │
│  │                                    │  │
│  │                                    │  │
│  │                                    │  │
│  │                                    │  │
│  └────────────────────────────────────┘  │
│  [ ] Set as default prompt               │
│                                          │
│                [ Cancel ] [ Save ]       │
└──────────────────────────────────────────┘
```

* On save: `POST /api/settings/system-prompts {id, name, prompt, isDefault}`.
* In **create mode**, the textarea is pre-filled with the current default
  prompt body (so users can branch from it).

---

## 6. Main Area: Tab Bar + Editor

### 6.1 Tab Bar

A `GtkBox` horizontal, full width of the main area, height 36 px.

* Left part: a `GtkScrolledWindow` `hscrollbar-policy=automatic` containing
  `GtkBox` horizontal of tabs. Tabs scroll horizontally if there are many.
* Right part: a single `+` icon button (`#new-file-main-btn`).

**Each tab** is a `GtkBox` horizontal, min-width 0, padding 0 12 px:

```
[icon] filename[●]    x
```

* `icon` is the file's symbolic icon (same `file_icon_name()` map as the
  file tree; browser tabs use `mb-globe-symbolic`).
* `filename` is the file's basename; ellipsizes at 120 px.
* `●` (dirty dot) appears only when `isDirty`, accent-colored.
* `x` is a close button.
* Active tab: bg `--bg-primary`, text `--text-primary`, 2 px bottom border in
  `--accent`.
* Inactive tab: bg `--bg-secondary`, text `--text-secondary`.
* Hover: bg `--bg-hover`.

Behavior:
* Click on the label/icon part: switch to that tab.
* Click on `x`: close that tab.
  * If `isDirty`, show a confirmation dialog
    (`"<name>" has unsaved changes. Close without saving?`).
* Right-click on a tab: context menu — Close · Close Others · Close All ·
  Reveal in Explorer · Copy Path.
* Closing the active tab picks the tab at the previous index, or
  the new last tab if the closed tab was last.
* No tab drag-and-drop reordering in the web app — don't add it.
* A single global `Ctrl+S` (and `Cmd+S` on macOS) saves the active tab.
* `Ctrl+W` closes the active tab.

**State** (mirrors `state.openTabs` in `static/app.js`):

```
struct Tab {
    std::filesystem::path path;       // absolute, sandbox-validated
    std::string name;                 // basename
    std::string content;              // current buffer
    std::string original_content;     // last-saved content
    bool is_dirty = false;
    bool is_saving = false;
};
std::vector<Tab> tabs;
size_t active_tab = kNoActive;
```

### 6.2 Editor

Uses **GtkSourceView 5** (`GtkSourceView` from `libgtksourceview-5`) as a
direct replacement for CodeMirror. (CodeMirror is JS-only; in the GTK port we
gain better integration, proper IME, print margins, etc., for free.)

* A single `GtkSourceView` instance is reused across tab switches — exactly
  like the `codeMirror` singleton in `static/app.js`. Switching tabs re-binds
  the buffer. The editor body is a `GtkStack`: the shared source view is the
  `"text"` page, and each read-only **preview tab** (see below) gets its own
  page. Switching to a preview tab leaves the source buffer untouched.
* **Preview tabs** (GTK-port addition): the tree context menu can open a
  `.md` file "as Markdown Preview" or an `.html` file "as HTML Preview" —
  read-only tabs rendered by an embedded **WebKitGTK** web view
  (`ui/HtmlView`; markdown is converted to a full HTML document via
  **md4c**/GitHub-flavored markdown in `backend/MarkdownToHtml` with an
  embedded Mocha/Latte stylesheet, loaded with a `file://` base URI so
  relative images resolve). Preview tabs never go dirty, disable Save and the
  find/replace bar (both are bound to the single source buffer), and are
  keyed by path+mode so the same file can be open as text and as preview at
  once. The open mode travels through the UI signals as `FileOpenMode`
  (`src/monkeybyte/FileOpenMode.h`); the palette is baked in at open time,
  so reopen a preview after a theme switch. The chat message renderer (§7.4)
  is unaffected — it does not use WebKit.
* **Browser tabs**: HTML previews and URL tabs get a navigation bar
  (back/forward/reload + editable address entry + 📱 screen-size menu)
  above the web view. The 📱 menu lists device viewport presets grouped
  Phones / Tablets / Laptops / Desktops (iPhone SE…Pro Max, Galaxy S,
  Pixel, iPads, Galaxy Tab, 1366–1536 laptops, 1920/2560 desktops) plus
  **Rotate** (swaps width/height) and **Fit to Window** (reset). Picking a
  preset size-constrains the web view widget inside a scrolled container —
  the WebKitGTK viewport is the widget size, so width-based media queries
  respond immediately. Presets smaller than the tab sit centered; presets
  LARGER than the tab (e.g. a desktop size on a small window) get GTK
  scrollbars so the full viewport can be panned horizontally/vertically.
  Phone presets also spoof a mobile User-Agent (iPhone/Android) for
  UA-sniffing sites, applied via reload. The
  agent (or HTTP automation) can open any URL — typically a dev server it
  just started — via the `browser.open` UI-control command
  (`EditorArea::open_url`), which stores the URL in `Tab::path` (never
  collides with workspace-file coordination, which matches absolute paths).
  Browser tabs disable Save/Delete, skip tree-reveal, show a globe icon, and
  keep no history across restarts. It is a real web view: arbitrary
  remote/local URLs load with JavaScript enabled, always by explicit user or
  agent action.
* **Agent browser tools**: the `browser.*` UI-control commands let the
  agent drive the tab it opened — `browser.navigate {url}` (same address
  normalization as the nav bar), `browser.list {}` (open browser tabs with
  live titles), `browser.close {url?}`, and `browser.read {selector?,
  max_chars?}` which extracts `{url, title, text}` from the live DOM via
  `webkit_web_view_evaluate_javascript` (`document.body.innerText`, or the
  `selector` element's), truncated to `max_chars` (default 8000, capped at
  20000). Because the text comes from the rendered page, JavaScript-heavy
  pages (SPA dev servers) read fine. The evaluation is async, so the
  handler runs a bounded (~5 s) nested main loop while the calling agent
  worker thread stays blocked in `UiControl::execute`. Commands act on the
  active browser tab, falling back to the most recently opened one when a
  text tab is active (`EditorArea::browser_tab_index`); an optional
  `{tab: url}` arg targets a specific open tab instead
  (`EditorArea::browser_view_for`, matching the tab's live or initial
  URL). Three more commands close the debugging/research loop:
  `browser.eval {script, max_chars?, tab?}` evaluates an arbitrary JS
  expression in the page and returns the JSON-decoded result (page
  sandboxed — same trust level as agent-driven navigation);
  `browser.links {selector?, max?, tab?}` returns the page's
  de-duplicated `[{text, href}]` links for crawling documentation sites;
  `browser.console {max_entries?, clear?, tab?}` reads back console.* and
  page-error entries captured by a document-start user script
  (`ui/HtmlView`, `mbConsole` script message handler, bounded 200-entry
  buffer cleared on every navigation start).
* **File uploads in browser tabs**: a page's `<input type="file">` works
  out of the box — `ui/HtmlView` does not handle
  `WebKitWebView::run-file-chooser`, so WebKitGTK's default handler shows
  its own GTK file-chooser dialog. For automated/agent-driven testing,
  `browser.upload {path | paths: [...], tab?}` queues local file paths on
  the tab (`HtmlView::queue_upload_files`); the next `run-file-chooser`
  request — e.g. triggered by clicking the input via `browser.eval` —
  completes with the queued paths instead of opening a dialog (the queue
  is consumed on first use; `paths` suits `multiple` inputs). Paths must
  exist or the command errors.
* **Tab-less web research**: two daedalus host tools run on the agent's
  tool thread with no tab and no UI marshalling — `web_fetch {url,
  max_chars?}` (`ui/chat/WebFetchTool`; synchronous GET in
  `backend/WebFetch` built on OmniNet's `HttpClient` — the project's HTTP
  stack — with redirects, 10 s timeout, 2 MB cap; HTML bodies
  are converted to plain text by `backend/HtmlToText`, markdown/JSON pass
  through) and `web_search {query, max_results?}`
  (`ui/chat/WebSearchTool`; key-less DuckDuckGo HTML endpoint in
  `backend/WebSearch`, redirect `uddg` links decoded to real URLs). Both
  are registered once per process alongside `AppUiTool`
  (`ChatController::ensure_agent`). Deliberately not built (future idea):
  `browser.snapshot` (screenshot → image attachment for vision models) —
  chat image attachments exist now (§7.5 §READ-IMAGE), but a snapshot
  tool would need scratch-file handoff; still a future idea.
* Line numbers: `show-line-numbers=true`.
* Bracket matching: `GtkSourceBracketsMatch`.
* Auto-close brackets: enable via `GtkSourceSpaceDrawer` is **not** the right
  feature — use sourceview's built-in "insert matching bracket" through
  `GtkSourceCompletion` or a small custom keypress handler. (Note: do this
  with a `GtkEventControllerKey` because sourceview doesn't auto-close
  out of the box.)
* Tab size / indent: pulled from settings every time a tab activates
  (`update_indent_unit`, `set_tab_width`, `set_insert_spaces_instead_of_tabs`).
* Word wrap: `set_wrap_mode(GTK_WRAP_WORD)` or off.
* Font: monospace, size from settings, default 14 px.
* Keybindings (preserve from CodeMirror config):
  * `Ctrl+F` / `Cmd+F` — open incremental search (use `GtkTextSearchDialog`
    from gtksourceview or a manual popover if not built-in).
  * `Ctrl+H` / `Cmd+H` — replace.
  * `Ctrl+G` / `Shift+Ctrl+G` — find next / prev.
* Syntax highlighting: `GtkSourceLanguageManager` with a language detected
  from extension. Map the CodeMirror mode table 1:1 to GLib-file languages:

```
.js .mjs .jsx               → javascript
.ts .tsx                    → typescript
.py                          → python
.cpp .cc .cxx .hpp          → cpp
.h                          → c
.c                          → c
.java                       → java
.go                         → go
.rs                         → rust
.html .htm                  → html
.css .scss .sass .less      → css
.xml                        → xml
.md .markdown               → markdown
.json                       → json (built-in)
.yaml .yml                  → yaml
.sh .bash .zsh .fish        → sh
.sql                        → sql
```

* Theme: pick `Adwaita-dark`/`Adwaita` or load a custom `.xml` style scheme
  matching catppuccin Mocha for dark and catppuccin Latte for light.

### 6.3 File header (above editor body)

`GtkBox` horizontal padding 6×12 px, bg `--bg-secondary`, border-bottom.

```
[ filename ] [●]  Saving… / Saved      [ 💾 Save ] [ 🗑 Delete ] [ × Close ]
```

* `Save` button is disabled when `isSaving || !isDirty`.
* `Save` calls `POST /api/files/write { path, content }`. On 200, update
  `original_content`, mark `isDirty=false`, show "Saved" for 2 s.
* `Delete` shows a confirmation dialog (different wording if isDirty). On
  confirm: `DELETE /api/files/<path>`, then close any tabs on the path
  (`close_tabs_under` — re-resolved by path since tabs may shift while
  the dialog is open, and a preview tab of the deleted file is stale
  too) and `workspace.refresh()`.
* `Close` = close the tab (same as `×` on the tab).

---

## 7. Chat Panel

Right-docked panel; width 320 px default, minimum 250 px. There is no fixed
maximum: the panel may take any width the user drags it to (the window bounds
it; the editor keeps ≥ 200 px on restore). Persisted in
GSettings as `chat-panel-width`.

### 7.1 Header

`GtkBox` horizontal padding 12×16 px:

```
[ Agent ]   [ <model dropdown> ] [ <thinking dropdown> ]   [ ⏹ Stop ]   [ 🗑 Clear ]
```

* `<model dropdown>` is `GtkDropDown` populated from the daedalus model
  registry (via `model_bridge`). Selection persists to GSettings
  (`selected-model`) and updates the **status bar**'s model label.
* `<thinking dropdown>` (daedalus §ThinkingLevel) sits right of the model
  dropdown and is always visible. Entries are `Default` (= send nothing)
  plus the levels the selected model's provider plugin reports via
  `model_bridge::supported_thinking_levels()` (queried without building an
  agent — `ProviderRegistry` hint-then-autodetect); e.g. MiniMax-M3 gets
  `Default`/`None`, while Kimi K3.1 Pro gets `Default`/`Low`/`High`/`Max`
  (`kimi-k3.1-pro[-highspeed]` on the platform, `k3.1-pro[-highspeed]` on
  Kimi Code). A model with no
  thinking support gets a single inert `None` entry so the dropdown never
  disappears. The selection is persisted per model in monkeybyte.db kv
  (`agent.thinking_level.<model name>`) and handed to the controller via
  `set_thinking_level()`, which switches the live session at runtime
  (daedalus `Agent::set_thinking_level`: the provider plugin maps the
  level into each outgoing payload, so the change applies from the very
  next request with no rebuild). The level is also seeded into
  `AgentConfig::thinking_level` on every agent build so a fresh session
  keeps the saved selection. For K3.1, `Default` preserves the endpoint
  default (`Max` platform / `High` Kimi Code) and thinking cannot be
  disabled. Moonshot recommends a fresh session after effort changes because
  they invalidate the prefix cache.
* `Stop` is hidden until the panel enters `processing` state. Click:
  `POST /api/chat/cancel { session_id }` followed by closing the
  EventSource stream and resetting state.
* `🗑 Clear` clears local messages, removes `chatHistory` from GSettings,
  and shows the welcome block again. It also tears down the agent and wipes
  daedalus's persisted message chain (`messages.json` + mirrors + SQLite
  mirror row via `ChatController::wipe_agent_session`) — daedalus restores
  that chain on every agent build, so without the wipe the next session
  would silently resume the cleared conversation. The settings-edit agent
  rebuild (`reset_agent`) tears down WITHOUT the wipe so the rebuilt agent
  resumes the visible conversation.

### 7.2 Error banner

Above the message list. Hidden by default. When shown:
* bg `--error` (#f38ba8), black text.
* Types with different backgrounds: `error-auth` (peach), `error-rate`
  (yellow), `error-server` (teal). In the GTK port, just use `--error`
  for all; the differentiation is cosmetic.
* When a turn ends in an agent error, the banner shows the real cause —
  the agent's recent Error/Fatal log lines (e.g. the HTTP status and
  error body returned by the provider), not a generic message.
* Terminal LLM failures are shown IMMEDIATELY, not at turn end:
  `AgentSink::on_error(will_retry=false)` is the failure signal (daedalus
  feeds fatal 4xx responses back into the conversation and ends the turn
  as Done only after its retry budget is spent). Each new terminal error
  updates the banner and adds a ❌ bubble as soon as it arrives. The
  turn-end outcome no longer has to be reconstructed from sink callbacks:
  the drain loop queries `Agent::turn_had_terminal_error()` /
  `Agent::last_error()` (daedalus §TURN-ERROR, 2b25f27) to flag the turn
  as failed. Transient errors the library retries (`will_retry=true`)
  are not shown.
* Errors carry daedalus's machine-readable `ErrorKind` classification
  (`on_error(kind, message, will_retry)`): the banner line and the log
  mirror are prefixed with it (e.g. `[RateLimited] …`), and actionable
  kinds append a hint — `Auth` → check the API key in Settings →
  Providers; terminal `RateLimited` → retry or switch model.
* Queued main-loop callbacks from controller signals are dropped once the
  panel is destroyed (shutdown mid-turn must not touch dead widgets).
* Every reported error (terminal and transient `on_error`, Error/Fatal
  log lines, turn exceptions) is also appended untruncated to
  `$XDG_DATA_HOME/monkeybyte/monkeybyte.log` (`backend/Logger`) so
  failures can be diagnosed after the fact. daedalus additionally writes
  its own per-session diagnostics to
  `<workspace>/.daedalus/daedalus/logs/` (gitignored).

### 7.3 Messages area

`GtkScrolledWindow` containing a `GtkBox` vertical of message bubbles. Each
bubble has a class `user` or `assistant`. Wakes up via `GtkAdjustment`
changed to auto-scroll to bottom on append.

**Welcome block** (when no messages):

```
┌──────────────────────────────────────────┐
│           👋 Welcome to MonkeyByte!       │
│  I'm monkey-agent, your AI coding        │
│  assistant.                              │
└──────────────────────────────────────────┘
```

Replaced after the first message.

**User bubble:**
* bg `--accent`, padding 8×12 px, border-radius 8 px, font-size 13 px.
* `margin-start = 16 px`.
* Body: plain text with newlines preserved.

**Assistant bubble:**
* bg `--bg-tertiary`, padding 8×12 px, border-radius 8 px, font-size 13 px.
* `margin-end = 16 px`.
* Body: rendered from markdown (see §7.4 below).
* When streaming, the bubble gets class `streaming`, which adds a CSS
  `::after { content: "|" }` blinking caret (we'll use a `GtkLabel` with a
  recurring `gtk_widget_queue_draw` or a simple `GtkLabel`-with-cursor
  shown by the controller).

**Think-block stripping:** some models (e.g. MiniMax M3 served by a
provider that ignores MiniMax's `reasoning_split` flag, §BUG-MINIMAX-105,
or qwen3-style models) leak their chain-of-thought into the `content`
stream wrapped in `<think>…</think>` tags. This is handled INSIDE daedalus
(its stream-safe `ThinkTagFilter` in the shared SSE parser drops complete
blocks, holds chunk-split tags until disambiguated, strips stray closers,
and strips before persisting history) — the host performs no think-tag
handling of its own.

**Stop-summary bubble:** the daedalus `stop` tool result carries a
user-facing `summary` of the turn (`{"status":"stop","summary":"…",…}`).
When present and non-empty, it is emitted from the turn epilogue — after
the final tool event — and shown as its own finalized assistant bubble
(markdown-rendered) below all of the turn's tool cards. It is
session-only: daedalus's history snapshot (§13) elides tool results, so
the summary bubble does not reappear after a restart (the turn's
assistant text around it does).

**tell_user bubble:** the daedalus `tell_user` tool result carries a
user-facing message (`{"status":"success","content":"…",…}`) that is meant
to be read, not inspected as raw JSON. The panel therefore skips the tool
card for `tell_user` entirely and, on the tool result, emits the `content`
as its own finalized assistant bubble (markdown-rendered) — visually
indistinguishable from the agent's other bubbles. Like the stop-summary
it is session-only (§13: tool results are elided from the history
snapshot), so the bubble does not reappear after a restart.

**Compaction bubble:** when daedalus's context compression rebuilds the
history — automatically (the 90%-of-context loop, mid-turn) or manually
(`/compress`, §7.8) — its `AgentSink::on_compaction(before, after)` is
surfaced as a transient info bubble — "🗜 Context compressed: 16.1k →
9.8k tokens" — and the activity strip returns to "Thinking…" as the turn
continues. The bubble is not persisted to chat history (info bubbles are
session-only).

**Scroll pinning:** the view follows new content only while the user is at
the bottom. `stick_to_bottom_` tracks user intent exclusively: the
adjustment's `value-changed` handler ignores value changes caused by our own
`set_value()` calls (guarded by `programmatic_scroll_`), because transient
mid-layout states (a wrapped bubble renegotiating height across several
layout passes) otherwise land the value >12 px above a stale bottom and
latch the follow behavior off. On `changed` (content grown, e.g. a bubble
appended) the view re-scrolls to the true bottom while sticking — this also
corrects the idle `scroll_to_bottom`, which can fire before the new row is
allocated. Streaming respects the latch (a user who scrolled up is not
yanked back down); the user's own message and history restore force the pin
(`scroll_to_bottom_forced`). The forced pin is protected by a 1 s
stick-lock (`stick_lock_until_us_`) during which residual inertial-scroll
value changes (touchpad fling on Wayland) cannot clear the latch, plus a
second re-pin 150 ms later to catch late multi-pass layouts.

**§SCROLL-KINETIC (2026-10-26):** the long-standing clipped user-bubble
failure was NOT fixed by more delayed pins. GTK 4.22's deceleration callback
keeps writing the previous scroll position after each pin, so the viewport
could briefly reach the bottom and then return to 0 even with
`stick_to_bottom_ == true`. `cancel_scroller_deceleration()` (ScrollPin.h)
uses the public kinetic-scrolling toggle to cancel that callback and restores
the user's setting immediately; every forced pin calls it before scheduling
the layout pins. The regression test drives GTK's real scroll/decelerate
controllers and proves the bottom stays stable, and the same reproduction
passed live on X11 and Wayland. The stick-lock and 150 ms pass still handle
latch intent and late multi-pass layouts, respectively.

**Tool card** (appended below the current assistant bubble when a tool call
starts; never inside it). Starting a tool call **finalizes the active
assistant bubble**; assistant text streamed after the tool call opens a
*fresh* bubble below the tool cards. Bubbles and cards therefore interleave
in chronological order (text → cards → text → …) and the newest agent output
always lands at the bottom of the messages area:

```
┌──────────────────────────────────────────────┐
│ [icon] execute_bash    ● running        [⌄]  │
├──────────────────────────────────────────────┤
│ {                                              │
│   "command": "ls -la",                       │
│   "timeout": 30000                            │
│ }                                              │
└──────────────────────────────────────────────┘
```

* Header (`GtkButton` with revealer): a symbolic tool-kind icon
  (`mb-tool-terminal/file/search-symbolic`, `mb-globe-symbolic`, or the
  `mb-tool-symbolic` wrench fallback, matched by name substring), tool name,
  status (spinner while running → `mb-status-ok/error-symbolic` ✓/✗ +
  status text), expand/collapse chevron (`mb-chevron-right/down-symbolic`).
* Body (`GtkRevealer`): monospace `<pre>` text of the args (or result).
* States:
  * `running` (left border accent) → tool is in flight.
  * `success` (left border green #22c55e) → `tool_done` with
    `result.success === true`.
  * `error` (left border red #ef4444) → otherwise.
* Status text format on success: `done` or `done (450ms)` or `done (1.3 s)`.
* Result body truncated at 2000 chars with `(truncated)` suffix.
* Live output: while a tool runs, `on_tool_progress` events with
  `{"stage":"output","output_chunk":"…"}` (emitted by `execute_bash`)
  stream the command's sanitised stdout/stderr into the card body — an
  8 KB per-call tail is kept by the sink, the last ~2 KB is displayed
  (ANSI SGR colour sequences stripped, UTF-8 boundary safe), and the
  view auto-scrolls to follow it. The body switches to the result JSON
  when the call completes.

### 7.4 Markdown renderer

Lightweight, dependency-free — the JS code already implements this in
`renderMarkdown()` of `static/js/chat.js`. In C++ port it should produce
Pango markup. Rules:

* Strip `<!--…-->` comments and `<!--…-->`.
* Strip `<!--…-->` and `<!--…-->` blocks via regex before tagging.
* Strip `<!--…-->` and `<!--…-->` tags.
* Fenced code ```lang … ``` → `PangoAttrList` with monospace bg; the code's
  own text gets tokenized by `highlightCode()` (the existing tokenizer in
  `static/app.js`) and converted to Pango spans.
* Inline `` `…` `` → monospace inline.
* `**bold**` → `<b>`.
* `*italic*` → `<i>` (must not be confused with bold).
* `- list` / `* list` lines → consecutive grouping into `<ul><li>…</li></ul>`.
* Newlines elsewhere → `<br>`.
* Strip XML special chars before parsing.

For the GTK port we can implement this as a `Markdown::render(const
std::string&)` returning either a `GtkTextBuffer` or Pango markup. Pair it
with a small syntax highlighter port (`highlightCode()`) and you're done.

### 7.5 Input bar

`GtkBox` horizontal, padding 12 px, border-top, gap 8 px.

```
┌───┐ ┌───┐ ┌───┐ ┌─────────────────────────┐  ┌──────┐
│ / │ │ 📎 │ │ 🎤 │ │ Ask me anything…        │  │ Send │
└───┘ └───┘ └───┘ │                         │  └──────┘
                  └─────────────────────────┘
```

* The input is a `GtkTextView` (or `GtkEntry` with multi-line) that grows
  from 1 to ~6 lines as the user types Enter.
* `Enter` (without Shift) sends; `Shift+Enter` inserts a newline.
* The input area is never disabled: daedalus has user-message queue
  support, so mid-turn sends are queued host-side
  (`ChatController::queued_messages_`, mutex-guarded) and the turn thread
  drains them one `submit_user_message` + `run_one_turn` each when the
  agent exits its current loop. daedalus's own queue is internally
  synchronized since 2b25f27 (§QUEUE-MUTEX — the race reported in
  `docs/upstream-repro/queue_thread_safety.md` is fixed upstream); the
  host relay remains for its FIFO and state-atomicity semantics, not for
  thread safety. A cancel landing between submit and
  consume can strand a message in daedalus's own queue, so a cancelled
  turn ends with `Agent::clear_pending_messages()` (d73d1ba) before the
  next message is submitted. Stop only interrupts the CURRENT turn —
  queued tasks still run; an errored turn ends the drain and the rest of
  the queue goes first on the next send (FIFO). `clear()` drops the
  queue with the session. When `isProcessing=true`, Stop becomes
  visible.
* Empty input doesn't send.
* A `/` menu button at the left edge of the bar opens a popover listing
  the user commands (§7.8). Commands are reachable mid-turn now that the
  input is never disabled: idle-only ones (`/clear`, `/init`, read-only
  inspectors) are rejected with an info bubble; `/help` and `/compress`
  work while the agent runs.
* §READ-IMAGE — an image button (`mb-file-image-symbolic`, between `/`
  and the input) opens a `GtkFileDialog` filtered to png/jpeg/gif/bmp/
  webp. The chosen file is appended to the input as `![name](path)` on a
  fresh line — visible and editable before sending — which daedalus's
  `LLMClient` (≥0.10.0) turns into an `image_url` content part for
  vision-capable models (scaling, history stripping, and SVG rejection
  all live in the library). The button is gated on
  `ChatPanel::current_model_supports_vision()`, which mirrors
  `LLMClient::model_supports_vision()` — the active provider plugin's
  static model table, unknown models assumed capable — so a text-only
  model gets an info bubble instead of a silently dropped image. Allowed
  while processing (it only edits the input buffer; the queued message
  carries the `![name](path)` markdown as usual).
* §CHIMPFLOW — the microphone sits between Attach and the input. It is
  insensitive unless master enable + dictation + scoped key + STT model are
  configured. First click records (red record icon / Listening), second stops
  and enters the insensitive Transcribing state. Completion inserts trimmed
  text at the TextView cursor and focuses it for review; it **never
  auto-sends**. Empty/short audio returns to Ready with non-error feedback.

* Sent attachments are shown in the user bubble: `ChatMessage::set_text`
  (user role) lifts `![alt](path)` references to existing local files out
  of the text and renders them as rounded `GtkPicture` thumbnails (≤320px
  longest edge, tooltip = full path, `.message-image` CSS) above the
  remaining text; an attachments-only bubble hides its label. Unreadable
  or missing files keep their markup in the text. This runs on both live
  sends and history restore, since both go through `set_text`.

### 7.6 Chat controller (state machine)

A class encapsulating the agent lifecycle — mirrors `ChatController` in
`static/js/chat.js`.

> §STOP-BTN-FIX (2026-10-22, upstream "Stop works once then dead"
> report): `cancel()` accepts `Processing` **or** `Cancelling` and
> re-fires the agent cancel on every press — the first press sets
> `Cancelling` and, with a queued follow-up draining behind the
> cancelled turn, nothing moved the state back, so the old
> `!= Processing` guard turned every later press into a no-op. The
> drain loop also transitions back to `Processing` when it actually
> dispatches a queued message from a `Cancelling` state, and a cancelled
> turn clears `queued_messages_` (Stop drops the pipeline, it does not
> just interrupt the current turn). A `chat.cancel` control-server
> command drives the same path as the Stop button for headless tests.
>
> §RESTORE-RETRY (2026-10-22, "no old messages load on a fresh project"):
> `restore_history()` ran once at construction; anything not ready by
> then (workspace picked later, model added in Settings) left the chat
> empty for the whole session. The restore is now re-entrant
> (`teardown_messages()` rebuilds the view without wiping the persisted
> session) and re-runs on panel map when nothing rendered yet and on
> workspace switch (`reload_history()`). §RESTORE-SCROLL: the startup
> pin fired before the scroller had an allocation (upper()≈0), so a
> history taller than the viewport opened at the TOP; the pin now fires
> on the scroller's first map, with the 150 ms pass and the
> `signal_changed` re-pin absorbing late layout growth.

```
enum class ChatState {
    Idle,         // no session
    Processing,   // user submitted, waiting on SSE events
    Cancelling,   // POST /api/chat/cancel in flight
    Error         // last turn errored; clear before resending
};

class ChatController {
    void send_message(const std::string& text);
    void cancel();
    void clear();

    // SSE event handlers (mirror JS map)
    void on_turn_start();             // empty
    void on_state(const std::string&); // drives the activity spinner (below)
    void on_text(const std::string&);   // appends to active assistant bubble
    void on_tool_start(id, name, args);
    void on_tool_progress(id, progress);
    void on_tool_done(id, name, result, duration_ms);
    void on_log(level, message);
    void on_error(message);
    void on_done(reason);

    // signals
    sigc::signal<void(ChatState)> state_changed;
    sigc::signal<void(Message&)>   message_appended;
    sigc::signal<void(ToolCard&)>  tool_card_added;
    sigc::signal<void()>           history_changed;
};
```

**Activity strip.** The chat panel keeps a fixed activity strip
(`GtkSpinner` + muted state label, class `chat-activity`) **above the input
bar**, outside the scroller, so it can never be clipped. It is **always
visible** and mirrors the agent state at all times: the spinner spins while
a turn is running and the label names the current activity — "Thinking…"
(`Processing`), "Streaming…" (text deltas arriving), "Running \<tool\>…"
(tool call in flight), "Cancelling…" (`Cancelling`), "Compacting…"
(context compression rebuilding the history — daedalus's 90% auto loop
or a manual `/compress` — `AgentState::Compressing`). When idle it shows a
resting state instead: "Ready" (no turn yet / cleared), and after a turn
ends "Done", "Cancelled", or "Error" according to the finish reason
(`finished` is emitted after `state_changed`, so its resting text wins).
The spinner reserves its space while stopped so the label never shifts.

### 7.7 Session lifecycle

1. **First message.** `isProcessing=true`. `POST /api/chat/start
   {"message", "model"}` → JSON `{ "session_id": "..." }`. Connect an
   `EventSource` (via libsoup `SoupSession`, or use the in-process
   daedalus backend directly if the GTK port is single-process — see
   §10). Save `session_id`.
2. **Subsequent messages** (same session). `POST /api/chat/message
   {"session_id", "message"}`. The same SSE connection stays open.
3. **Stop.** `POST /api/chat/cancel {"session_id"}` → close the stream.
4. **Reconnect.** The web app uses `EventSource` which auto-reconnects.
   In the GTK port we should use a `SoupMessage` streaming body or a
   long-poll on `GET /api/chat/events?session=...&since=<seq>` as the
   fallback. The web app documents `/api/chat/events` as the polling
   fallback; use it when WebTransport/SSE over HTTP/2 isn't possible.
5. **Done event.** Closes the active assistant bubble, persists history,
   sets `isProcessing=false`.

### 7.8 User commands

In-session slash commands, modeled on the icarus CLI's in-session commands.
Typed into the input (`/cmd`, no arguments) or picked from the `/` menu
button's popover (§7.5). `ChatPanel::on_send` intercepts them before the
text reaches the agent (`UserCommands::parse_command`); unknown `/foo`
input produces an info bubble instead of being sent to the model. Since
the input is never disabled (§7.5), `run_command` idle-gates everything
except `/help` and `/compress` while a turn runs (feedback bubble, not
silent). The
command table lives in `UserCommands` (GTK-free, unit-tested); execution
lives in `ChatPanel::run_command`.

| Command   | Behavior |
|-----------|----------|
| `/init`     | Sends a fixed AGENTS.md-generation prompt as a normal user message (cancellable via Stop) |
| `/compress` | Host-triggered context compression (below) |
| `/clear`    | Same as the header 🗑 button: `clear_history()` |
| `/usage`    | Info bubble with `daedalus::AgentStats` (tokens, context used/max, tool calls); appends server-side prompt-cache hits (`cached_tokens`, daedalus §ContextCache) when the provider reports any, plus the resolved provider plugin name |
| `/tools`    | Info bubble listing `Agent::list_tools()` |
| `/skills`   | Info bubble listing `Agent::list_skills()` |
| `/mcp`      | Info bubble listing `Agent::mcp_configured_servers()` |
| `/help`     | Info bubble listing all commands |

Commands run only while idle. Listing commands show a "no session yet"
note when the agent has not been created (first message not sent).

* **Info bubbles** are transient assistant-style notes rendered through
  the normal markdown path; they are **not** persisted to history.
* **`/compress`**: daedalus compresses the session internally — the host
  calls `Agent::compress_context()` (daedalus §AutoCompress) and nothing
  else: no user message, no summary prompt, no reseed. When a turn is
  running the call sets a flag daedalus consumes at the compression
  checkpoint at the end of the current agent cycle (the same 70/30
  LLM-summary path as the automatic 90 % trigger, never mid-tool-call,
  honored even if the turn is cancelled). When the agent is idle daedalus
  compresses **immediately and synchronously on the caller's thread**, so
  the controller offloads that blocking summary call to the turn thread
  (state `Processing`, strip shows "Compacting…"). The panel confirms the
  request with a transient info bubble; the result arrives through the
  same `on_compaction` signals as automatic compression — a "🗜 Context
  compressed: before → after tokens" info bubble (§7.3). Every other
  outcome comes from the `CompressResult` return value (daedalus
  §COMPRESS-RESULT, 2b25f27 — the host no longer duplicates daedalus's
  internal message-count threshold): `NothingToCompress` → a "too short,
  will compress at the start of the next turn" bubble (the request IS
  carried over by daedalus); `Failed` → a "did not complete — will retry
  at the start of the next turn" bubble; `NoModel` → a "no model
  configured" bubble.

---

### 7.9 ChimpFlow voice orchestration (§CHIMPFLOW)

`AppServices` owns one `VoiceService`, shared by Settings and Chat.
`ChimpFlowClient` is a synchronous worker-called libcurl client with an
injectable transport, binary-safe bodies, cancellation and no automatic
error retries. `Wav` provides bounded PCM WAV/multipart helpers.
`AudioEngine` exposes an `AudioBackend` interface for fake-audio tests and a
native PipeWire implementation for 16 kHz mono capture and sample-rate mono
playback. Every utterance is RMS-normalised to 0.12 with a 4x gain cap and
0.98 peak limit, so voices have consistent loudness without noise boosting
or clipping.

* Public service calls and widget signals are UI-thread affairs. HTTP runs
  on `std::jthread` workers; completions return via `Glib::Dispatcher`.
  Generation counters plus lifetime tokens suppress stale work. Destruction
  cancels and joins workers. Exceptions are contained per operation so one
  failed utterance does not head-of-line-block later speech.
* Live `text_delta` feeds `SpeechStreamSegmenter`: complete sentences call
  `speak()` while the model is still writing, and bubble finalization flushes
  only the unspoken remainder (no duplicate narration). `tell_user` creates a
  complete bubble without deltas, so its handler calls `speak(msg)` directly.
  Markdown becomes speech-friendly UTF-8 prose (no byte truncation).
  Utterances synthesize/play one at a time in FIFO order, including prose
  around tool cards. History restore, user messages, tool cards and host
  notices never enter this path.
* Preview and recording stop pending speech to avoid overlap/microphone
  feedback. Clear History, reset/reload and teardown call `stop_all()`.
  Master-off cancels active work. No audio device opens until requested.
* PipeWire streams are destroyed under the loop lock; the lock is released
  before `pw_thread_loop_stop()`. The `pw_stream_events` tables have static
  lifetime because PipeWire retains their pointers (stack tables caused a
  realtime-thread crash). Completion is the **drained** event, not merely
  queued samples, so speech tails are not clipped.

---

## 8. Status Bar

Bottom bar, height 24 px, accent background, fixed text. Two halves:

```
[ git branch info (left) ]     [ Model · 12.4k/200k ctx (6%) · cached 4.1k · iter 3 · tools 7 (right) ]
```

* **Left:** when workspace is empty, hidden. When set but not a git repo,
  shows "Not a git repo". When inside a repo, shows the branch name plus a
  dirty count (`main · 3 changes`) from `GitService::status` (read-only).
  Refreshed on workspace set, and re-polled every 3 s on a timer —
  branch switches, commits, and working-tree edits happen outside the
  app's own signals, so polling is the only way to see them. The poll
  runs the git subprocesses on a worker thread and marshals only the
  label text back to the main loop; ticks are skipped while a previous
  poll is still running, and a transient spawn failure keeps the
  previous label.
* **Right:** the active model name followed by live daedalus `AgentStats`,
  pushed by `ChatController::stats_changed` (emitted from the sink's
  tool-result and state-change callbacks and at turn end; zeroed on
  `clear()`): total/max context tokens with a percentage, `cached N/M`
  (prompt-cache hits vs prompt tokens of the **last response** — shown only
  when the provider reports them), the real
  agent iteration count, and completed tool calls (`+M failed` when any).
  With no session, only the model name shows.

---

## 9. Keyboard Shortcuts & Gestures

| Shortcut | Action |
|---|---|
| `Ctrl+S` / `Cmd+S` | Save active file |
| `Ctrl+W` | Close active tab |
| `Ctrl+F` / `Cmd+F` | Find in editor |
| `Ctrl+H` / `Cmd+H` | Replace in editor |
| `Ctrl+G` / `Shift+Ctrl+G` | Find next/prev |
| `Escape` (editor search bar) | Close search bar |
| `Enter` (chat, no Shift) | Send |
| `Shift+Enter` (chat) | Newline |
| `Escape` (any modal) | Close modal |
| `Ctrl+1..4` | Switch sidebar panel (suggested) |

Drag gestures: two draggable resizers between sidebar/main and main/chat.

---

## 10. Backend Contract — API Surface to Preserve

The HTTP server and all the routes below must continue to work; the GTK
port only changes the front-end that consumes them. (In a single-process
variant you can also wrap `daedalus::Agent` directly — but that's an
optimization, not a requirement.)

| Method | Path | Purpose |
|---|---|---|
| GET    | `/api/workspace`                     | Return current workspace + home/user info |
| POST   | `/api/workspace`                     | Set workspace |
| GET    | `/api/files/`                        | List workspace root |
| GET    | `/api/files/<path>`                  | List directory or read file (size limit 5 MB) |
| POST   | `/api/files/write`                   | Create or overwrite file |
| POST   | `/api/files/mkdir`                   | Create directory |
| DELETE | `/api/files/<path>`                  | Delete file |
| GET    | `/api/browse[/<path>]`               | List child directories only (for folder picker) |
| GET    | `/api/parent/<path>`                 | Get parent path |
| GET    | `/api/search?q=...`                  | Search workspace files for a query string |
| GET    | `/api/git/status`                    | Branch + porcelain changes |
| POST   | `/api/chat/start`                    | Start a new streaming session |
| POST   | `/api/chat/message`                  | Submit follow-up |
| GET    | `/api/chat/stream?session=...`       | SSE event stream |
| GET    | `/api/chat/events?session=...&since=`| Polling fallback |
| POST   | `/api/chat/cancel`                   | Cancel the running session |
| GET    | `/api/settings`                      | All settings (interface + models + MCP + system prompts) |
| PUT    | `/api/settings/interface`            | { fontSize, tabSize, wordWrap, theme } |
| GET    | `/api/settings/providers`            | List providers (shared host/key/endpoints) |
| POST   | `/api/settings/providers`            | Add/update provider by name |
| DELETE | `/api/settings/providers/<name>`     | Remove provider (cascades to its models) |
| GET    | `/api/settings/models`               | List models (reference providers by name) |
| POST   | `/api/settings/models`               | Add/update by name |
| DELETE | `/api/settings/models/<name>`        | Remove model |
| PUT    | `/api/settings/models/<name>/system-prompt` | Bind prompt id (0 = default) |
| GET    | `/api/settings/system-prompts`       | List system prompts |
| POST   | `/api/settings/system-prompts`       | Add/update prompt |
| DELETE | `/api/settings/system-prompts/<id>`  | Delete prompt |
| GET    | `/api/settings/mcp`                  | List MCP servers |
| POST   | `/api/settings/mcp`                  | Add MCP server |
| PUT    | `/api/settings/mcp/<name>`           | Update MCP server |
| DELETE | `/api/settings/mcp/<name>`           | Delete MCP server |

### 10.1 SSE event schema

The `MonkeybyteSink` translates daedalus callbacks into these UI-facing
events. The C++ GTK port must consume the same vocabulary:

| Event          | Payload                                       | Meaning |
|----------------|-----------------------------------------------|---------|
| `turn_start`   | `{role:"user"}`                               | New turn begins |
| `state`        | `{state, previous}`                           | Agent state changed (`waiting`, `responding`, `acting`, `done`) |
| `text`         | `{text}`                                      | Assistant text delta; append to current bubble |
| `tool_start`   | `{id, name, args}`                            | Tool call starting |
| `tool_progress`| `{id, progress}`                              | Intermediate progress (long-running tool) |
| `tool_done`    | `{id, name, result, duration_ms?}`            | Tool finished; `result.success` flag |
| `log`          | `{level, message}`                            | Agent log message |
| `error`        | `{message}`                                   | Turn errored |
| `done`         | `{reason}`                                    | Terminal: `done` / `cancelled` / `error` / `timeout` |

### 10.2 Persistence

Two SQLite databases are read and written by the backend; the GTK port
treats these as opaque storage:

* `monkeybyte.db` — settings (workspace, font size, theme, etc.), chat
  history, tool traces, per-model system-prompt bindings
  (`model_prompts` table, keyed by daedalus model name), and agent
  host-policy lists (`agent.disabled_skills`, `agent.disabled_tools`,
  `agent.mcp_enabled` kv keys — daedalus does not persist skill/tool
  enablement, so the host seeds them via `AgentConfig`).
* `daedalus.db` — at `$XDG_DATA_HOME/daedalus/daedalus.db`. Holds shared
  **providers** (`providers` table: host, API key, endpoints), configured
  **models** referencing a provider by name (`models` table; effective
  config is the merge via `SettingsStore::resolve_model`), key/value agent
  settings in `kv_settings` (including the default model name), and the
  **MCP server registry** in `mcp_servers`. The agent loads MCP servers and
  resolves models from this store, so Settings → Providers / Models / MCP
  Servers read/write it directly (via `model_bridge` / `mcp_bridge` over
  `daedalus::SettingsStore::instance()`); monkeybyte.db does NOT keep its
  own provider/model/MCP lists. Legacy monkeybyte.db `models` rows
  (pre-split schema) are migrated into this store once at startup.

GSettings on the GTK side should be **cache-only** (window size, sidebar
width, chat panel width, theme, selected model name, hidden-files toggle).
Authoritative state lives in SQLite.

---

## 11. Layout, Theming & Style Mapping

The web app's `:root` CSS variables map roughly 1:1 to a tiny CSS stylesheet
loaded by the GTK app via `GtkStyleContext::add_provider_for_display` with
`GTK_STYLE_PROVIDER_PRIORITY_APPLICATION`. Two named color schemes
(`dark` = catppuccin Mocha, `light` = catppuccin Latte) ship as `.css`
files; we toggle by attaching the provider matching the user's `theme`
setting and removing the other. The `system` theme setting (default)
resolves through a fallback chain: first the xdg-desktop-portal
`org.freedesktop.appearance` `color-scheme` key (uint: 1 = dark,
2 = light, 0 = no preference), then the GNOME
`org.gnome.desktop.interface` `color-scheme` enum
(`prefer-dark` / `prefer-light` / `default`), and finally
defaults to dark. This re-applies live when either preference
changes (`signal_changed` is wired on whichever schema is
present). The `org.freedesktop.appearance` schema is only
present when xdg-desktop-portal-gnome is installed and a portal
is running; the GNOME schema is the always-present source of
truth on a real GNOME install.

Once resolved, the app must also point the *base* GTK theme at the same
scheme or the chrome (buttons, dropdowns, popovers, separators) stays
light while only our CSS-painted widgets go dark. GTK 4.20 deprecated
`gtk-application-prefer-dark-theme`; the base theme now follows the
`gtk-interface-color-scheme` setting (`GtkInterfaceColorScheme`:
dark = 2, light = 3), which is portal-fed by default but writable
per-app. `ThemeManager::set_color_scheme()` sets it via `g_object_set`
(guarded by `GTK_CHECK_VERSION(4, 20, 0)`) and still sets the old
prefer-dark property for GTK < 4.20 hosts. Note the compositor-drawn
title bar (SSD under mutter) follows the *session* color-scheme, so it
only goes dark when the desktop itself is dark.

| CSS variable (web) | Used for |
|---|---|
| `--bg-primary`   | Main area / editor background |
| `--bg-secondary` | Sidebars / tab inactive / chat background |
| `--bg-tertiary`  | Chat user bubble / workspace path bar |
| `--bg-hover`     | Buttons hover, tree row hover, tab hover |
| `--bg-active`    | Selection highlight, active row |
| `--bg-input`     | Entry background |
| `--text-primary`, `--text-secondary`, `--text-muted` | 3-level text |
| `--border`       | All 1 px lines and resizers |
| `--accent`       | Active icon rail, selection underline, primary button bg, focus ring, status bar bg |
| `--error`        | Error banner / failure labels |

The resizer is a 6 px wide `GtkBox` with cursor `col-resize`. On hover or
active drag, swap its CSS class from `resizer` to `resizing` (accent
background) and draw a 24 px tall 2 px wide grip glyph vertically centered.

The status bar uses the accent color directly (not the standard
`--bg-tertiary`), inheriting from the web app's design choice.

`prefers-color-scheme` (CSS media query) is replicated by reading the
desktop's `Gdk::Monitor` `color-scheme` or by an explicit user setting
via the Settings / Theme dropdown.

---

## 12. State Machines & Data Flows

### 12.1 Workspace change flow

1. User clicks 📁 in the sidebar header (or the path bar, or the Settings
   Edit button, or the Folder Picker modal's Open button).
2. Folder Picker modal shows.
3. User navigates / types a path / clicks Open.
4. The app calls `POST /api/workspace { workspace }`.
5. **If a chat session is in flight, the app MUST `POST /api/chat/cancel`
   first and poll `/api/chat/state` until it returns `idle` or
   `cancelled`.** This is a strict requirement — switching workspaces
   mid-agent is dangerous.
6. SQLite `settings.workspace` is updated.
7. The chat history is wiped (per-session state, not persisted anyway).
8. Workspace tree re-renders.
9. Editor tabs are NOT auto-closed. The user keeps their tabs. (Preserves
   the web behaviour.)

### 12.2 File lifecycle

* **Open:** click file in tree → fetch content (if not in tabs) → push
  Tab → activate. Each tab holds the raw string content; the editor
  binds to it.
* **Edit:** `change` signal from source view → recompute `isDirty` (compare
  to `original_content`).
* **Save:** `POST /api/files/write { path, content }` → on 200, update
  `original_content = content`, clear dirty, flash "Saved".
* **Create:** `+` in sidebar asks for filename → `POST /api/files/write
  { path: <ws>/<name>, content: "" }` → refresh tree → open the new file.
* **Delete:** confirm → `DELETE /api/files/<path>` → close tab (skipping
  dirty check) → refresh tree.
* **Close tab:** confirm if dirty → remove from `tabs` → pick new active or
  show placeholder.

### 12.3 Agent flow

1. User types into chat input → `Enter`.
2. `ChatController::send_message(text, model)`:
   * Empty → no-op.
   * No model selected → append error message "No model selected…", set
     `isProcessing=false`.
3. Disable input, show Stop button.
4. POST `/api/chat/start` (or `/api/chat/message` if existing session).
5. Open EventSource on `/api/chat/stream?session=<id>`.
6. For each event:
   * `turn_start` — reset pending tool cards.
   * `text` — append to active assistant bubble (create bubble if absent).
   * `tool_start` — finalize the active assistant bubble, then append the
     tool card below it (later text opens a fresh bubble under the cards).
   * `tool_progress` — update status text in matching tool card.
   * `tool_done` — set card state (success/failure), duration, fill body
     with result (truncate 2000 chars).
   * `log` — log only (no UI).
   * `error` — append "❌ <message>" to assistant bubble.
   * `done` — finalize turn (close bubble, persist history,
     `isProcessing=false`).
7. On user Stop:
   * `POST /api/chat/cancel` → close EventSource.
   * Append "⚠️ Operation cancelled by user." message.
   * Finalize bubble.
   * `isProcessing=false`.

### 12.4 Theme switch flow

`Settings → Interface → Color Theme` change:
1. Update `SettingsStore.interface.theme`.
2. Apply immediately: set/unset `style-class=dark|light` on the root
   widget, swap the CSS provider.
3. No restart required.

---

## 13. Local UI Cache (GSettings)

Suggested keys and defaults:

```
io.github.monkeybyte.window             / width              1100
io.github.monkeybyte.window             / height             720
io.github.monkeybyte.sidebar            / width              260
io.github.monkeybyte.chat-panel         / width              320
io.github.monkeybyte.sidebar            / active-panel       0   (workspace)
io.github.monkeybyte.folder-picker      / show-hidden        false
io.github.monkeybyte.editor             / font-size          14
io.github.monkeybyte.editor             / tab-size           2
io.github.monkeybyte.editor             / word-wrap          true
io.github.monkeybyte.theme              / name               "dark"
io.github.monkeybyte.chat               / selected-model     ""
io.github.monkeybyte.chat               / session-id         ""   (in-memory)
```

There is deliberately NO host-side chat-history key. The transcript is
owned by daedalus's persisted session (`<workspace>/.daedalus/`), and the
startup view renders from `Agent::history_snapshot()` (daedalus
§HISTORY-SNAPSHOT, 2b25f27) — the same conversation the agent resumes,
post-compression and post-sanitization. This replaced the old
`chat/history` JSON blob (`[{role, content, timestamp}]`), which silently
diverged from what the agent actually saw after compression. Building the
agent at panel startup is what makes the snapshot available, so the agent
is now constructed eagerly rather than on first send.

---

## 14. C++23 Class / Module Decomposition (Suggested)

These mirror the JS modules one-for-one. Names are suggestions; adjust to
your house style.

```
src/monkeybyte/
  main.cpp                          (≈ Launch.cpp — wires OmniNet + daemon)
  launch.h                          (Daemon lifecycle)

  ui/
    MainWindow.{h,cpp}              (GtkApplicationWindow subclass)
    ActivityBar.{h,cpp}             (left 48 px column)
    Sidebar.{h,cpp}                 (host for the 4 sidebar panels)
    StatusBar.{h,cpp}
    Resizer.{h,cpp}                 (GtkGestureDrag wrapper)

    panels/
      WorkspacePanel.{h,cpp}        (Explorer)
      SearchPanel.{h,cpp}
      GitPanel.{h,cpp}
      SettingsPanel.{h,cpp}         (tabs host)
        InterfaceTab.{h,cpp}
        ModelsTab.{h,cpp}           (cards list, system-prompt dropdown)
        PromptsTab.{h,cpp}
        McpServersTab.{h,cpp}

      TabBar.{h,cpp}                (file tab strip)
      Editor.{h,cpp}                (GtkSourceView wrapper)

    chat/
      ChatPanel.{h,cpp}
      ChatController.{h,cpp}        (Agent lifecycle, SSE/polling)
      ChatMessage.{h,cpp}           (Message bubble widget)
      ToolCard.{h,cpp}              (collapsible tool result card)
      Markdown.{h,cpp}              (lightweight md -> Pango)
      Highlighter.{h,cpp}           (token colorizer)

    dialogs/
      WorkspacePickerDialog.{h,cpp} (folder picker)
      NewFolderDialog.{h,cpp}       (nested modal)
      ModelEditDialog.{h,cpp}
      ProviderEditDialog.{h,cpp}
      McpServerDialog.{h,cpp}
      SystemPromptDialog.{h,cpp}

    theme/
      ThemeManager.{h,cpp}          (loads dark/light css providers)
      Colors.h                      (named CSS variable constants)

  tree/
    WorkspaceTree.{h,cpp}           (lazy file tree; mirrors workspace.js)

  http/
    ApiClient.{h,cpp}               (libcurl or libsoup wrapper)
    EventSourceClient.{h,cpp}       (SSE / polling hybrid)

  models/
    Model.{h,cpp}                   (provider config)
    SettingsStoreAdapter.{h,cpp}    (= DaedalusSettingsAdapter; read/write
                                      through the existing bridge)
```

Each class should:
* Use `sigc::signal` for events that need to reach other widgets.
* Hold a `weak_ptr<GtkWidget>` back to the widget it drives, never a
  raw `GtkWidget*` (the existing pattern in libdaedalus uses smart pointers
  everywhere — keep it).
* Expose a synchronous refresh hook for tests.
* Have a paired unit test if it owns complex state (notably `WorkspaceTree`,
  `Markdown`, `ChatController`).

### Headers / libraries assumed

* GTK 4.10+ (the build is Ubuntu LTS, 26.04 ships gtk4 4.18+ as of late
  2025 — verify).
* libgtksourceview-5.
* libsigc++-3 (already required by gtkmm).
* glibmm / gtkmm are *optional*; the project so far uses raw GLib / GTK via
  OmniNet style. Decide up front: gtkmm (idiomatic, faster to write)
  versus raw GTK (consistent with existing code, smaller binary). My
  recommendation is gtkmm because it makes the C++23 idioms
  (`std::expected`, `std::span`, ranges) much more readable in widget
  callbacks.

### Existing backend stays untouched

`FileService`, `ChatService`, `ChatSession`, `MonkeybyteSink`,
`DaedalusSettingsAdapter`, `Settings`, `McpClient`,
`ToolFactory` continue to compile and link. The HTTP layer (OmniNet) is no
longer on the critical path for the UI — the C++ GTK client either calls
the same HTTP routes via `ApiClient` (recommended — keeps the same
deployment story) **or** embeds the same services directly in-process for
snappier UI. Either is fine; **don't change the AI agent loop**.

---

## 15. Panel-by-Panel Recap (for QA)

| Panel / Page | Showed by | Hide others | Lazy-loads | Persists |
|---|---|---|---|---|
| Activity bar | always | n/a | n/a | active icon in GSettings |
| Sidebar header | always | n/a | n/a | panel index in GSettings |
| Workspace path bar | Explorer only | n/a | n/a | path from `Settings` |
| File tree | Explorer only | yes | folders on expand | expansion set in memory only |
| Search input + results | Search only | yes | on input (300 ms) | last query in GSettings optional |
| Git status | Source Control only | yes | on each open | n/a |
| Settings header / tabs / footer / content stack | Settings only | editor + chat hidden | on each tab open | interface settings on Save |
| Tab bar | always (except Settings) | n/a | n/a | open tabs in memory only |
| Editor file header | always (active tab) | n/a | n/a | n/a |
| Editor body | always (active tab) | placeholder when none | source language on tab open | font/size/wrap in GSettings |
| Chat header / messages / input | always (except Settings) | n/a | message history on first open | selected model + history blob in GSettings |
| Status bar | always | n/a | on workspace set | n/a |

---

## 16. What To Skip From The Web App

Things intentionally absent or stub. Don't try to "complete" them in the
GTK port:

* **WebSocket support** (future). The GTK port doesn't need a browser; SSE
  or polling is fine.
* **Git staging/commit UI**. The web app's Git panel is read-only; the
  agent has git_* tools.
* **Multi-window**. There's one top-level window.
* **Drag-to-reorder tabs**. The web app doesn't have it; don't add.
* **Diff viewer**. Not in the web app.
* **Drag-and-drop file upload**. Not in the web app.
* **Notifications**. Not in the web app.
* **Collaboration / multi-user**. Not in the web app.

---

## 17. Definition of Done — Acceptance Checklist

A GTK4 / C++23 port matches this design when, for each item below, a
fresh developer can point at the running binary and see the documented
behavior:

1. **Activity bar** switches between four sidebar pages; only one
   visible at a time; icons are 40×40 with an accent rail.
2. **Resizers** drag smoothly, persist widths, and the central main area
   stays usable in [min, max] px.
3. **Workspace Explorer** shows the workspace path bar, three icon
   buttons, and a lazy-loaded symbolic-icon tree whose expand state
   survives a refresh.
4. **Folder picker modal** is a model window centered over the app with
   breadcrumb, lazy-loaded directory list, "Show hidden" toggle, path
   text entry, Cancel and Open buttons, and a "Create New Folder" button
   that opens a nested modal.
5. **Search panel** debounces, renders file path + line preview, opens a
   file when clicked.
6. **Git panel** renders branch + changes (or the right empty / error
   message) on open.
7. **Settings → Interface** has the four controls listed; they apply
   live, persist, and change theme + editor behaviour without a restart.
8. **Settings → Models** has dynamic cards, an `Add Model` button, three
   quick-fill buttons (`+ MiniMax` / `+ OpenAI` / `+ Ollama`), per-model
   System Prompt dropdown, Edit/Delete actions, and a Default model
   dropdown at the top.
9. **Settings → System Prompts** lists prompt cards, has a `+ New Prompt`
   button opening the modal with a pre-filled default.
10. **Settings → MCP Servers** lists configured servers with a `+ Add MCP
    Server` button opening the modal.
11. **Tab bar** supports opening, switching, dirty marking, saving, and
    closing tabs.
12. **Editor** shows line numbers, syntax highlighting per file type,
    bracket matching, configurable font/tab/wrap, and the keybindings
    listed in §9.
13. **Chat panel** sends a message, opens an SSE stream (or polling),
    streams text deltas into assistant bubbles (a new bubble per text
    segment, finalized when a tool call starts), interleaves tool
    cards that show args → running → result (success / error) with
    timing, and supports Stop and Clear.
14. **Status bar** updates branch info (+ dirty count), active model name,
    and live agent stats (tokens, cached, iterations, tool calls) on change.
15. **Theme switching** flips dark↔light without restarting.
16. **Workspace change** cancels any running agent before changing and
    wipes the in-memory chat history.
17. **Backend HTTP API** (`/api/...`) and SQLite (`monkeybyte.db`,
    `daedalus.db`) continue to work exactly as documented in
    `docs/AGENTS.md`.

When all 17 pass, the port is feature-equivalent to the web UI.
