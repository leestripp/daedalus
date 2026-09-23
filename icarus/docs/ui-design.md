# icarus Terminal UI Design — Phase 1

## Approach: ANSI widget layer (not full TUI)

The terminal stays in normal scrollback mode. We add rich inline widgets
using ANSI escape sequences and Unicode box-drawing characters, with ASCII
fallbacks for `--plain` / `--no-unicode`.

## Why this design

- **Copy-paste friendly.** Users can select and copy code, logs, and responses.
- **Subprocess friendly.** Bash/tool output streams naturally into the scrollback.
- **Keyboard first.** No mouse required, no full-screen redraw.
- **Progressive enhancement.** `--plain`, `--no-color`, `--no-hud`, and `NO_COLOR`
  degrade gracefully.

## Visual elements

### 1. Startup banner

**In a real terminal (colors + Unicode):**

```text
╔══════════════════════════════════════════════════════════════════════════╗
║  icarus — AI coding agent powered by daedalus                            ║
║  Library: 0.7.2-1-gec1ab27                                                 ║
╠══════════════════════════════════════════════════════════════════════════╣
║  Active model: MiniMax-M2.7                                                ║
║  Model id:     MiniMax-M2.7                                                ║
║  Base URL:     https://api.minimax.io                                      ║
║  Chat path:    /v1/chat/completions                                        ║
║  Max context:  200000                                                      ║
║  Temperature:  0.7                                                         ║
╠══════════════════════════════════════════════════════════════════════════╣
║  daedalus_home: /home/ubuntu/.local/share/daedalus                       ║
║  project_root: /workspace/icarus                                         ║
║  ...                                                                       ║
╚══════════════════════════════════════════════════════════════════════════╝
```

- Border: cyan (`#4fc1ff`)
- Labels (`Active model:`): muted gray
- Values: white / bold for the active model name
- Section separators: `╠ ... ╣`
- Width: 76 columns, shrinking to fit when the terminal is narrower

**With `--plain`:** borders become `+` and `-`, colors are stripped.

### 2. HUD / spinner line

During a turn the spinner shows a braille glyph and the current agent state:

```text
⠋ Planning
```

- Spinning glyph: braille pattern (or ASCII fallback)
- State name: colored by state (blue for thinking states, green for
  Done/Idle, red for Error/Cancelled, yellow for AwaitingApproval)

The spinner stops when the first assistant delta or tool call arrives; a
watchdog restarts it if the agent goes quiet for more than a few seconds
mid-turn.

### 3. Tool call cards

Tool calls render as dimmed bordered cards, wrapped to the live terminal
width:

```text
╭────────────────────────────╮
│ 🔧 TOOL CALL: execute_bash │
│ action: run                │
│ command: ls -la            │
╰────────────────────────────╯
```

- Border: dim/muted, always left-justified
- Long argument values are flattened (newlines → spaces) and truncated
- Tool results render a matching card: status line, command, exit code,
  and up to 30 lines of output (first 15 + last 15 with a skip marker)
- Todo tools (`todo_add`/`todo_edit`/`todo_delete`) additionally show the
  affected item, e.g. `#3: Buy milk (pending)`

### 4. Code blocks

Assistant code fences are detected and highlighted:

```text
▸ code: cpp
// Add todo via Agent API
nlohmann::json result = agent.add_todo("Build UI", false);
std::cout << result.dump(2) << std::endl;
▸ end code
```

- Header/footer: muted gray (`▸ code: <lang>`)
- Comments: green
- Strings: orange
- Keywords: blue
- Numbers: light green

### 5. Log messages

```text
[Warn] [SKILLS] Requested skill not found: 'foo'
[Error] [TOOL] execute_bash failed: ...
```

- Level tag colored by severity (Warn/yellow, Error/red)
- Message body: muted gray so it does not compete with assistant text
- **Info-level messages are not shown in the terminal at all** — they go to
  the session log file (`logs.txt`) only, so informational chatter never
  interleaves with the conversation

### 6. Approval prompt

```text
╭─ Command Approval ─────────────────────────────────────────╮
│ Execute command:                                           │
│ rm -rf build                                               │
│                                                            │
│ Approve? (y)es / (n)o / (a)lways this session              │
╰────────────────────────────────────────────────────────────╯
```

- Title: yellow / warning color
- Command: white on its own line
- `y` approves once, `n` (or anything else / Ctrl+D) refuses, `a`
  auto-approves all further commands for the rest of the session

### 7. Turn separator

Before each prompt a subtle frame is drawn — a dim hint line and a
horizontal rule:

```text
✨ Type /help for available commands
────────────────────────────────────────────────────────────────
>
```

The rule uses the live terminal width on every draw, so resizing or
zooming the terminal mid-session takes effect immediately.

## Color palette

| Semantic | 256-color code | Hex approx | Usage |
|----------|----------------|------------|-------|
| Primary  | 39  | `#4fc1ff` | Borders, prompts, active state |
| Success  | 76  | `#4ec9b0` | Done/Idle, success messages |
| Warning  | 220 | `#cca700` | Warnings, approval prompts, todos |
| Error    | 196 | `#f48771` | Errors, cancelled state |
| Info     | 75  | `#569cd6` | Info logs, thinking states |
| Muted    | 245 | `#808080` | Labels, secondary text, separators |

## 8. Sticky status footer (designed, deferred)

A one-line status bar anchored to the bottom of the terminal was designed
for Phase 3:

```text
MiniMax-M2.7 · Acting… · tok 12% · iter 3/600 · 2 todos
```

- Shows the active model, current agent state, token usage percentage,
  iteration count, and pending todo count.
- Would use ANSI cursor positioning in the normal scrollback buffer, so
  scrollback and copy-paste still work.

**Status: not shipped.** Streaming assistant text and subprocess output
fight with any full-width redraw, so the single-line spinner is the status
surface for now. The `StatusHud` class already formats this line
(`footer_line()` / `summary_line()`); nothing renders it yet. Width
handling would follow the same live-width query used everywhere else (no
SIGWINCH handler).

## CLI flags

- `--no-color` — strip all SGR escapes
- `--no-unicode` — use ASCII box chars (`+|-` instead of `╔║═`)
- `--no-hud` — disable the spinner/status HUD
- `--plain` — alias for all three
- `NO_COLOR=1` — respected automatically

## Completed phases

### Phase 2

- `/todo` command family wired to `Agent::add_todo`, `list_todos`, etc.
- `/settings` opens the embedded daedalus settings TUI (FTXUI tabs for
  providers, models, MCP servers, agent options; `Ctrl+C` returns to the
  REPL) — see `docs/design.md` §9.3 for the runtime-sync model
- `/memory`, `/history`, `/export`, `/tool`, `/last`, `/continue`
- Formatted tables for `/providers`, `/models`, `/skills`, `/tools`

### Phase 3

- Spinner shows the current agent state (model/token details stay out of
  the spinner line); the sticky footer itself is deferred — see §8.

### Later additions

- `DrawTable` as the single table builder, shrink-to-fit columns
- Resize-aware rendering (live width query per draw, no 80-col cap)
- All boxes left-justified
- Ghost-text hints + tab-completion for slash commands
- `/paths` command
