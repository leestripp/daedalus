# icarus CLI Reference

> Complete reference for all command-line flags, in-session commands, environment
> variables, exit codes, and terminal behaviour.
> For usage examples see `Readme.md`. For design rationale see `docs/design.md`.

---

## 1. Quick Reference

### Invocation syntax

```
icarus [options] [directory]
icarus --a2a-agent [--a2a-port N]
icarus --help
icarus --version
```

### One-liners

| Task | Command |
|---|---|
| Interactive REPL in current directory | `icarus` |
| Interactive REPL in a specific project | `icarus /path/to/project` |
| Select a model | `icarus --model minimax-m3` |
| Auto-approve all tool calls | `icarus --auto` |
| List available models | `icarus --model-list` |
| Discover model endpoints | `icarus --discover-models minimax-m3` |
| Run as A2A agent HTTP server | `icarus --a2a-agent` |
| Load skills from a directory | `icarus --skills /path/to/skills` |
| Use a specific daedalus home | `icarus --home ~/.local/share/daedalus` |
| Preview legacy-data migration | `icarus --migrate-data` |
| Print help | `icarus --help` |
| Plain mode (no color, no unicode, no HUD) | `icarus --plain` |

---

## 2. Global Flags

### `-h`, `--help`

Print the built-in help summary and exit (exit code 0). Does not initialize the agent.

### `-v`, `--version`

Print the version string and exit (exit code 0).

```
$ icarus --version
icarus 0.7.0 (daedalus 0.8.9)
```

### `--home <dir>`

Set the **daedalus home directory**. This is the root under which daedalus stores all runtime state.

```
--home ~/.local/share/daedalus
--home /etc/icarus/daedalus.d   # multi-user shared config
```

When omitted, icarus resolves `daedalus_home` automatically:

```
$XDG_DATA_HOME/daedalus/            (if XDG_DATA_HOME is set)
$HOME/.local/share/daedalus/         (Linux default)
```

**Important:** the settings database (`daedalus.db`) lives in the XDG *data*
hierarchy, **not** config. Using `--home $HOME/.config/daedalus` will appear to
work but `--model-list` will report `(none configured)` because the database
is at `$HOME/.local/share/daedalus/daedalus.db`.

See also: [`DAEDALUS_HOME`](#daedalus_home) environment variable.

### `--model <name>`

Select the active model by its configured name (as listed by `--model-list`).

```
--model minimax-m2.7
--model qwen-3.6-35b
--model minimax-m3
```

The model must be registered in `daedalus.db` (see [Setup](#setup)). When
omitted, the model configured as `default_model` in settings is used.

### `--model-list`

List all models registered in `daedalus.db` and exit without starting the REPL.

```
$ icarus --model-list
Available models:
  minimax-m2.7 (provider=MiniMax, model_id=MiniMax-M2.7)
  minimax-m3 (provider=MiniMax, model_id=MiniMax-M3)
```

Initializes the agent (loading settings, skills, memory), then shuts down
cleanly. Exit code 0.

### `--discover-models <name>`

Query the provider's `/v1/models` endpoint for the configured model `<name>`
and exit. This short-circuits before the agent is constructed — no REPL, no
session state. Prints one model id per line. Exit code 1 on error.

```
$ icarus --discover-models minimax-m3
MiniMax-M2.7
MiniMax-M3
```

### `--agent-name <name>`

Per-agent namespace for runtime state files (`messages.json`, session logs,
crash recovery files).

```
--agent-name coding-agent
--agent-name review-agent
```

When omitted, session files live in `<project_root>/.daedalus/daedalus/`.
With `--agent-name my-agent` they live in
`<project_root>/.daedalus/daedalus-my-agent/` instead. Run `/paths` in a
session to see the resolved locations.

### `--threads <n>`

Number of worker threads for the agent's thread pool. `<n>` must be a positive
integer; a missing or invalid value is an error (exit code 1). When omitted,
the daedalus library default is used.

### `--stream`

Enable SSE token streaming. Tokens arrive in real-time as the model generates
them. **This is the default.**

### `--no-stream`

Disable SSE token streaming. The complete response is returned as a single
block after generation. Slightly lower latency per turn after the first, but
no streaming feedback during generation.

### `--auto`

Auto-approve all bash commands and tool executions without prompting. The agent
can run arbitrary shell commands without requiring confirmation.

```
icarus --auto /path/to/project   # useful for CI, scripting
```

**Warning:** in `--auto` mode, the agent has full access to the user's shell
environment. Always review the project directory before running. The approval
prompt's session-scoped `a` (always) answer is an interactive alternative —
see [Approval prompt](#approval-prompt).

### `--no-memory`

Disable persistent session memory for this session. The agent will not read or
write conversation history. Each invocation starts fresh.

```bash
icarus --no-memory   # one-off question, no history
```

Note: this does not prevent daedalus from loading crash-recovery files
(`messages.json.crash`) if a previous session crashed.

### `--clear-memory`

Clear all session memory for the current `--agent-name` and exit. Does not
start the REPL.

```bash
icarus --clear-memory
icarus --clear-memory --agent-name my-agent  # clear a specific namespace
```

Exit code 0 on success. Initializes and immediately shuts down the agent.

### `--skills <dir>`

Add `<dir>` to the skill discovery search path. This option can be passed
multiple times. When omitted, skills are loaded from the global skills
directory (`$XDG_DATA_HOME/daedalus/skills/`).

```
--skills /path/to/skills
--skills ./my-skills --skills /usr/local/share/icarus/skills
```

### `--system-prompt <file>`

Load a custom system prompt from `<file>`, prepended to the default daedalus
system prompt. Without this flag, icarus loads `<project_root>/AGENTS.md` if it
exists. The legacy `.daedalus/daedalus/system_prompt.txt` overlay and the
`<exe_dir>/system_prompt.txt` sidecar are no longer read.

```bash
# Apply a custom system prompt
icarus --system-prompt /path/to/prompt.txt /project

# Per-project instructions (auto-loaded from AGENTS.md)
echo "You are a code reviewer specializing in security." > /project/AGENTS.md
icarus /project
```

### `--a2a-url <url>`

Set the default A2A server URL for the `a2a` tool. This is passed to
`A2aTool` at registration time and can be overridden per-call in tool
invocations.

```
--a2a-url http://localhost:8000    # default
--a2a-url https://a2a.example.com
```

The `a2a` tool uses this URL to call remote A2A agents. It does not affect
the `--a2a-agent` daemon mode.

The legacy `--acp-url` flag is accepted as a deprecated alias.

### `--a2a-port <n>`

Port for the A2A agent HTTP server when running `--a2a-agent`. Use `0`
(the default) to let the operating system assign an ephemeral port. The
bound URL is printed to stderr on startup.

```
--a2a-port 8080
--a2a-port 0
```

### `--a2a-agent`

Run icarus as an A2A agent HTTP server spoken to by an IDE or another A2A
client. Each incoming task drives a dedicated `daedalus::Agent` that
auto-approves all commands (there is no interactive terminal to ask).

```
$ icarus --a2a-agent
[a2a] icarus A2A agent mode
[a2a] Server ready at http://127.0.0.1:45678
```

The per-task agents honour `--model`, `--home`, `--skills`, `--system-prompt`,
`--no-memory`, and `--agent-name`, so the daemon can be pointed at a specific
model and configuration just like the interactive mode. The agent card served
at `/.well-known/agent-card.json` advertises the real bound URL.

If the server fails to bind, icarus prints an error and exits with code 1.
Send SIGINT (Ctrl+C) or SIGTERM to shut down cleanly.

The legacy `--acp-agent` flag is accepted as a deprecated alias, but the
protocol is now A2A over HTTP rather than ACP v1 over stdio JSON-RPC.

See `docs/a2a-protocol.md` for the protocol notes.

### `--migrate-data`, `--migrate-data-apply`

Copy legacy `~/.daedalus` data (settings, MCP servers, skills) into the
XDG-compliant locations. `--migrate-data` is a dry run that only prints the
plan; `--migrate-data-apply` performs the copy. The legacy directory is never
removed automatically.

```
icarus --migrate-data                 # preview
icarus --migrate-data-apply           # copy
icarus --migrate-data-from /old/path  # override the source root
icarus --migrate-data-verbose         # log each copied entry
```

Exit codes: `0` = migrated (or dry run), `1` = nothing to migrate,
`2` = migration failed.

### `--no-color`

Strip all ANSI color and style (SGR) escape sequences from output. Useful
when redirecting to a file, piping to tools that are confused by escapes,
or running in environments without color support.

### `--no-unicode`

Replace Unicode box-drawing characters with ASCII equivalents (`+`, `-`, `|`
instead of `╭`, `─`, `│`). Does not affect color escapes — combine with
`--no-color` to strip both.

### `--no-hud`

Disable the spinner during agent thinking states. The terminal stays in
normal scrollback mode throughout.

### `--plain`

Alias for `--no-color --no-hud --no-unicode`. Useful for CI environments,
automated tests, or terminals that do not support ANSI escapes.

The `NO_COLOR=1` environment variable is respected automatically.

### `<directory>`

Positional argument: the target project directory. The agent uses this as its
working directory for file operations and tool executions.

```
icarus                          # current directory
icarus /path/to/project
icarus -- /path/with-dashes     # use -- to stop flag parsing
```

Defaults to the current working directory if omitted. The path is resolved
with `fs::weakly_canonical`; if it cannot be resolved, icarus falls back to
`$HOME` or `/tmp`.

---

## 3. In-Session Commands

These are typed at the `> ` prompt inside the interactive REPL. They are not
shell commands and do not invoke a subshell. Slash commands have ghost-text
hints and tab-completion as you type them.

### Session control

#### `/quit`, `/exit`

Exit icarus cleanly. Saves session state, runs `shutdown()`, and terminates.

```
> /quit
Exiting. Goodbye!
```

#### `/help`

Print the built-in help summary (identical to `--help`).

#### `/init`

Create or update the project's `AGENTS.md`. The request is handed to the
agent itself (it explores the codebase and writes the file), so it can be
cancelled with ESC like any other turn.

#### `/clear`

Clear session memory for the current project/agent namespace and reset the
last-message buffer used by `/continue`. Equivalent to `/memory clear`.

```
> /clear
[CLEAR] Session memory cleared
```

#### `/compress`

Compress the conversation context (daedalus `Agent::compress_context()`).
Uses the same 70/30 split + LLM summary as the automatic 90%-threshold
trigger. When no turn is running it happens immediately; during a turn it is
deferred to the end of the current agent cycle. No-op when no model is
configured.

```
> /compress
[COMPRESS] Compressing conversation context...
╭ Compact ──────────────╮
│ Context compacted: …  │
╰───────────────────────╯
```

#### `/continue`

Re-submit the previous user message (e.g. after a cancelled turn).

#### `/last`

Print the last assistant response again.

### Providers and models

#### `/providers`

List configured providers in a table (`#`, name, host, plugin, timeout,
masked key). The current provider is marked with `*`. The plugin column
shows the explicit plugin set on the row, or `(auto: <name>)` for the
plugin auto-detected from the provider name/host (daedalus
§ProviderPlugins: `openai-compatible`, `minimax`, `moonshot`, `lmstudio`).

#### `/provider-add <name> <host> <api_key|-> [plugin]`

Add or update a provider non-interactively. Pass `-` as the API key for
auth-less providers (e.g. a local LM Studio server). The optional plugin
must name a registered daedalus provider plugin; when omitted it is
auto-detected from the host. For the full form (chat/discovery endpoints,
timeout) open the `/settings` TUI (Providers tab).

#### `/provider-delete <name|#>`

Delete a provider by name or by its 1-based index in the `/providers` table.

#### `/provider <name|#>`

Switch the current provider by name or index. Clears the discovered-model
cache used by `/model <#>`.

#### `/models`

Discover models exposed by the current provider's `/v1/models` endpoint and
print them as a table. The active model is marked `[current]`. When the
provider has no discovery endpoint (or the query fails), the fallback table
shows only the **current provider's** configured models — use
`/models --configured` for the cross-provider list.

```
> /models
#   Model ID              Context   Features
1   MiniMax-M2.7          200000    reasoning
2   MiniMax-M3            200000    reasoning [current]
```

#### `/models --configured [provider]`

List the models stored in `daedalus.db` (name, provider, model id, context
size, temperature) instead of querying the provider. With an optional
provider name, only that provider's models are listed.

#### `/model-add <name> <provider> <model_id>`

Add or update a configured model. The provider must already exist.

#### `/model-delete <name|#>`

Delete a configured model by name or by its 1-based index in the
`/models --configured` table. Reports an error if the name is not configured.

#### `/model <name|#>`

Switch the active model. By name: switches to a configured model (also
matches on `model_id`). By 1-based index: picks from the last `/models`
discovery list (auto-discovers if empty), reusing an existing configured
entry or adding a new one on the fly.

### Skills, tools, MCP

#### `/skills`

List all loaded skills (enabled and disabled) with descriptions and tags.

#### `/skills enable|disable <name|#>`

Enable or disable a skill by name or by its 1-based index in the `/skills`
table.

#### `/tools`

List all tools currently registered in the agent's tool registry, including
built-in daedalus tools and the `a2a` tool, with an enabled flag for each.

#### `/tools enable|disable <name|#>`

Enable or disable a tool by name or by its 1-based index in the `/tools`
table.

#### `/mcp-servers`

List configured MCP servers with an enabled flag for each.

#### `/mcp-servers enable|disable <name|#>`

Enable or disable an MCP server by name or by its 1-based index in the
`/mcp-servers` table.

### Todos

#### `/todo [list|pending|done]`

List todos (default: all). `pending` and `done` filter by state.

#### `/todo add <text>`

Add a new todo.

#### `/todo done <id>` / `/todo undone <id>`

Mark a todo as done / pending.

#### `/todo edit <id> <text>`

Replace a todo's text.

#### `/todo delete <id>`

Delete a todo.

#### `/todo clear`

Delete all todos.

#### `/todo max <N>`

Set the maximum number of todos.

### Settings

#### `/settings`

Opens the interactive settings TUI (the embedded daedalus settings editor):
fullscreen FTXUI tabs for **Providers**, **Models**, **MCP Servers** and
**Agent** options, editing the same `daedalus.db` the agent uses. Keys:
`←`/`→` switch tabs, `a` add, `j` import MCP server(s) by pasting a
provider JSON snippet (`{ "mcpServers": { ... } }` or a bare
`{ "<name>": { ... } }` map; an `Authorization: Bearer …` header maps to
`bearer_token`), `e`/`Enter` edit, `d` delete, `s` set default
model, `Esc` close dialog, `Ctrl+C` quit back to the icarus prompt. When
stdin/stdout is not a terminal (piped), `/settings` instead prints the
read-only overview: active model plus the providers and configured-models
tables. Changes made in the TUI apply to the store immediately; use
`/model <name>` or `/provider <name>` to activate a new default. When the TUI
closes, changes are synced into the running agent where daedalus exposes a
runtime path: `auto_approve` is applied immediately (unless `--auto` was
passed, which stays authoritative); a changed `current_provider` triggers a
provider switch; edits to the active model or its provider (credentials,
endpoints, temperature) are re-applied by re-selecting the model — this
resets conversation history and says so; MCP server enable/disable toggles
are applied live. The Agent-tab `thinking_level` field is applied to the
running agent when the TUI closes (same as `/level`). Added/edited/removed
MCP servers and the remaining
Agent-tab values (iteration/size limits, retry, Serper key) are read at
startup only — icarus prints a note when those need a relaunch. If
`current_provider` is empty when the TUI opens it is seeded from the
active model's provider (daedalus only writes it on a provider switch).

The old `/settings` subcommands (`providers`, `models`, `add-provider`,
`add-model`, `delete-provider`, `delete-model`, `switch-provider`) were
removed: everything they did is covered by the TUI tabs or by the
standalone `/providers`, `/models --configured`, `/provider` and `/model`
commands. `/settings` takes no arguments.

### Memory, history, export

#### `/memory [clear]`

Show whether memory is enabled and the path of the memory store
(`memory/memory.sqlite` inside the per-project agent directory).
`/memory clear` clears session memory.

#### `/history [N]`

Show the last `N` messages (default: all) in a box, each truncated to
120 characters.

#### `/export [file]`

Copy the session `messages.json` to `file` (default:
`icarus-export-YYYYMMDD-HHMMSS.json` in the current directory).

#### `/tool <name> [json]`

Execute a registered tool directly with JSON arguments and print the result.

```
> /tool execute_bash {"command":"ls -la"}
```

### Information

#### `/usage`

Token/context usage summary with a progress bar. When the provider reports
server-side context-cache hits (daedalus §ContextCache), a `Cached:` line
shows how many prompt tokens were served from the cache on the last
response.

#### `/stats`

Raw token and iteration statistics for the current session. Includes
`cached=<n>` when the provider reported context-cache hits.

```
> /stats
Stats: iteration=3 / 600 | tokens=4821 (prompt=2104, completion=2717, cached=1800) | ctx=4821/200000
```

#### `/state`

Print the current daedalus agent state, plus the resolved provider plugin
once an LLM session exists.

```
> /state
State: Acting
Provider plugin: moonshot
```

Possible states: `Idle`, `Understanding`, `Planning`, `Acting`, `Verifying`,
`Documenting`, `AwaitingApproval`, `Compressing`, `Done`, `Cancelled`,
`Error`.

#### `/levels`

List the thinking/reasoning levels the current model honors as a table,
with `[active]` marking the selection. Row `0` is always `Default` (send
nothing — the provider/model decides), so on/off models like MiniMax-M3
show both states. Models without thinking control print a note.

```
> /levels
╭─────┬─────────┬──────────╮
│ #   │ Level   │ Status   │
├─────┼─────────┼──────────┤
│ 0   │ Default │ -        │
│ 1   │ Low     │ -        │
│ 2   │ High    │ -        │
│ 3   │ Max     │ [active] │
╰─────┴─────────┴──────────╯
(Default = send nothing; the provider/model decides)
```

#### `/level <name|#>`

Set the thinking level at runtime (daedalus `Agent::set_thinking_level()`),
taking effect on the next request. Accepts `default`, `none`, `low`, `high`,
`max` (case-insensitive) or an index from `/levels` (`0` = default). Levels
the current model does not support are rejected with the supported list.
The selection is persisted (kv row in `daedalus.db`) and re-applied at
startup. Bare `/level` shows the current value.

```
> /level max
[LEVEL] Thinking level set to Max
```

#### `/paths`

Table of every filesystem path in use: daedalus config/data/state/cache,
database, logs, skills, MCP files, OmniNet config/data, the icarus input
history, and the per-project session files (messages, memory, sessions,
logs).

---

## 4. Environment Variables

### `DAEDALUS_HOME`

Overrides `--home`. Equivalent to `--home "$DAEDALUS_HOME"`.

```
export DAEDALUS_HOME=~/.local/share/daedalus
icarus
```

### `XDG_DATA_HOME`

Sets the base for the daedalus data directory when `DAEDALUS_HOME` is not set.

```
# Without DAEDALUS_HOME:
# daedalus_home = $XDG_DATA_HOME/daedalus/
# Default (XDG_DATA_HOME unset): $HOME/.local/share/daedalus/

export XDG_DATA_HOME=/opt/data
icarus   # daedalus_home = /opt/data/daedalus/
```

### `XDG_CONFIG_HOME`

Used by daedalus to resolve the legacy config read-fallback path
(`$XDG_CONFIG_HOME/daedalus/config.json`). The active settings database
(`daedalus.db`) is **never** stored here.

### `NO_COLOR`

When set (any non-empty value), all ANSI color/style escapes are stripped,
as if `--no-color` was passed. See <https://no-color.org/>.

### `COLUMNS`

Fallback terminal width when the kernel has no window size for stdout
(e.g. output redirected to a pipe). The live `TIOCGWINSZ` ioctl is always
preferred, so interactive resizing works without touching `COLUMNS`.

### `LINENOISE_ASSUME_TTY`

When set (any non-empty value), the input reader uses the full linenoise
line editor even when stdin is not a TTY (debugging aid; normally piped
input is read in a plain byte-stream mode).

### `HOME`

Used as a fallback when resolving paths. If `$HOME` is unset, falls back to
the system temp directory.

---

## 5. Terminal Output Reference

### Banner (interactive mode only)

On every interactive start, icarus prints a full configuration banner:

```
╔══════════════════════════════════════════════════════════════════════════╗
║  icarus — AI coding agent powered by daedalus                            ║
║  Library: 0.7.6
╠══════════════════════════════════════════════════════════════════════════╣
║  Active model:    minimax-m2.7                                          ║
║  Model id:        MiniMax-M2.7                                          ║
║  Base URL:        https://api.minimax.io                                 ║
║  Chat path:       /v1/chat/completions                                   ║
║  Max context:     200000                                                ║
║  Temperature:     0.7                                                   ║
╠══════════════════════════════════════════════════════════════════════════╣
║  daedalus_home:    /home/user/.local/share/daedalus                     ║
║  project_root:    /home/user/project                                    ║
║  session_id:      sess_abc123                                           ║
║  data_dir:        /home/user/project/.daedalus/daedalus                 ║
║  agent_name:      (none — legacy .daedalus/)                            ║
║  memory:          enabled                                               ║
║  auto_approve:    no                                                    ║
║  stream:          yes (SSE tokens)                                      ║
║  skills:          7 loaded                                              ║
║  iterations:      600                                                   ║
╠══════════════════════════════════════════════════════════════════════════╣
║  Type /help for commands. /quit to exit.                                ║
╚══════════════════════════════════════════════════════════════════════════╝
```

The banner shrinks to fit when the terminal is narrower than 78 columns.

The banner is **not** shown for `--help`, `--version`, `--model-list`,
`--clear-memory`, `--migrate-data*`, or `--a2a-agent`.

The daedalus library's own startup banner (the `╔═══ ... Autonomous Coding Agent ... ╚═══` box) is dropped entirely by the log filter; icarus prints its own banner.

### Prompt

In interactive mode the prompt is `> `, framed by a hint line and a
horizontal rule drawn at the live terminal width:

```
✨ Type /help for available commands
────────────────────────────────────────────────────────────────────────────
>
```

While you type a slash command, a ghost-text hint completes the command name
(and shows a short summary once it is unambiguous); TAB cycles completions.

### Log levels

Every `on_log` message from the daedalus library is written to the
timestamped log file `<data_dir>/logs.txt`. Only warnings, errors, and debug
messages are also shown on the terminal (stderr) — Info-level chatter is
kept out of the terminal so it does not interleave with assistant output.

| Message | Level | Terminal | `logs.txt` |
|---|---|---|---|
| *(assistant text)* | — | stdout (deltas) | no |
| `[PERSIST] …`, `[SKILLS] …`, `[LLM] …` | Info | no | yes |
| `(library banner)` | Info | no | no (dropped by filter) |
| `[WARN] …` | Warn | stderr | yes |
| `[ERROR] …` | Error | stderr | yes |
| `[FATAL] …` | Error | stderr | yes (process exit 1) |

In `--a2a-agent` mode the log path is not set, so messages go to stderr only.

### Approval prompt

Unless `--auto` is in effect, every shell command needs approval:

```
╭─ Command Approval ─────────────────────────────────────────╮
│ Execute command:                                           │
│ rm -rf build                                               │
│                                                            │
│ Approve? (y)es / (n)o / (a)lways this session              │
╰────────────────────────────────────────────────────────────╯
```

- `y` — run this command, ask again next time
- `n` (or anything else, or Ctrl+D) — refuse this command
- `a` — run this command and auto-approve all further commands this session

### Thinking spinner

During agent thinking states, a single-line spinner shows the current agent
state (e.g. `⠋ Planning`). It stops as soon as assistant text or a tool
call arrives, and a watchdog restarts it if the agent goes quiet for more
than a few seconds mid-turn.

### Resize behaviour

Boxes, tables, rules, and the prompt header all query the terminal width at
render time. Resizing or zooming the terminal mid-session takes effect on
the next draw — no restart needed. Over-wide tables shrink their widest
columns (with `…` truncation) to fit.

### Terminal state restoration

icarus uses raw terminal mode for line editing and ESC key detection. On
exit (normal, SIGINT, or crash), it restores the terminal: shows the cursor,
resets ANSI attributes. The restoration runs via `std::atexit`, the SIGINT
handler, and the chained crash handlers.

---

## 6. Data Files and Directories

### Directory layout

```
$DAEDALUS_HOME/                         # or ~/.local/share/daedalus
├── daedalus.db                         # SQLite: models, MCP servers, settings
├── skills/
│   └── installed/                      # migrated skills from legacy layout
└── .upgrade-guard                      # daedalus internal

$XDG_DATA_HOME/icarus/
└── input_history.txt                   # linenoise line-edit history

<project_root>/.daedalus/daedalus/      # unnamed default agent
├── messages.json                       # current session history
├── messages.json.tmp                   # auto-save temp (overwritten each save)
├── messages.json.crash                 # crash recovery copy
├── logs.txt                            # all on_log messages (ISO timestamped)
├── memory/
│   └── memory.sqlite                   # persistent session memory
├── sessions/                           # saved sessions
└── skills-active/                      # skills enabled for this project

<project_root>/.daedalus/daedalus-<agent_name>/  # when --agent-name is used
└── (same files as above)

<project_root>/AGENTS.md                # per-project agent instructions (if present)
```

Run `/paths` in a session for the authoritative, fully-resolved list
(including the OmniNet config/data locations).

### Crash recovery

If icarus crashes (SIGSEGV/SIGABRT/SIGFPE/SIGILL), the last saved session
state is written to `messages.json.crash` before the process terminates. On
the next launch, daedalus reads `messages.json.crash` if `messages.json` is
missing or corrupt, recovering the in-flight turn with a "drop last malformed
message" heuristic.

### Legacy paths (pre-v0.4.0)

```
~/.daedalus/settings/settings.json     → $XDG_CONFIG_HOME/daedalus/config.json
~/.daedalus/mcp/servers.json           → $XDG_CONFIG_HOME/daedalus/mcp/servers.json
~/.daedalus/mcp/.env                   → $XDG_CONFIG_HOME/daedalus/mcp/.env
~/.daedalus/skills/                    → $XDG_DATA_HOME/daedalus/skills/installed/
```

These legacy paths are kept as **read fallbacks** during the migration window.
New writes always go to the SQLite database. The `--migrate-data` tool copies
(old → new) without deleting the old files.

---

## 7. Exit Codes

| Code | Meaning |
|---|---|
| `0` | Normal exit (`/quit`, `--help`, `--version`, `--model-list`, `--clear-memory`, migrate dry run, migration applied) |
| `1` | Fatal error during agent run, `--discover-models` failure, A2A server bind failure, invalid flag value, or `--migrate-data-apply` with nothing to migrate |
| `2` | Migration failed (`--migrate-data-apply`) |
| `128+n` | Fatal signal (SIGINT=130, SIGSEGV=139, etc.) — if crash handler fails |

---

## 8. A2A Tool Reference

The `a2a` tool is registered automatically at startup (when not in
`--a2a-agent` mode). It is available as a daedalus HostTool — the agent
can call it by name, passing JSON arguments. The implementation uses the
A2A client built into the daedalus library (`daedalus::a2a::A2aClient`).

### Tool definition

```json
{
  "type": "function",
  "function": {
    "name": "a2a",
    "description": "Interact with remote A2A (Agent2Agent Protocol) agents. "
                 + "Use get_agent_card to discover an agent, send_message to start "
                 + "a task, and get_task/list_tasks/cancel_task/subscribe_to_task "
                 + "to manage task lifecycle. Returns structured JSON.",
    "parameters": {
      "type": "object",
      "properties": {
        "action": {
          "type": "string",
          "enum": ["get_agent_card","send_message","send_streaming_message",
                   "get_task","list_tasks","cancel_task","subscribe_to_task"]
        },
        "message": { "type": "object" },
        "task_id": { "type": "string" },
        "context_id": { "type": "string" },
        "configuration": { "type": "object" },
        "a2a_url": { "type": "string" }
      },
      "required": ["action"]
    }
  }
}
```

### Actions

#### `get_agent_card`

Fetch the agent card from the A2A server.

```json
{ "action": "get_agent_card" }
{ "action": "get_agent_card", "a2a_url": "https://a2a.example.com" }
```

#### `send_message`

Send a message and start a task (non-streaming).

```json
{
  "action": "send_message",
  "message": {
    "role": "user",
    "parts": [{ "text": "Hello" }]
  }
}
```

#### `send_streaming_message`

Send a message and collect streaming A2A events synchronously.

```json
{
  "action": "send_streaming_message",
  "message": {
    "role": "user",
    "parts": [{ "text": "Hello" }]
  }
}
```

#### `get_task`

Poll the status of an existing task.

```json
{ "action": "get_task", "task_id": "task_1" }
```

#### `list_tasks`

List tasks, optionally filtered by `context_id` or `status`.

```json
{ "action": "list_tasks" }
{ "action": "list_tasks", "context_id": "ctx-1", "status": "completed" }
```

#### `cancel_task`

Cancel a running task.

```json
{ "action": "cancel_task", "task_id": "task_1" }
```

#### `subscribe_to_task`

Subscribe to streaming updates for an existing task.

```json
{ "action": "subscribe_to_task", "task_id": "task_1" }
```

---

## 9. Signal Handling

| Signal | Behaviour |
|---|---|
| `SIGINT` (Ctrl+C) | Graceful cancel: tells daedalus to stop, restores terminal, exits cleanly. A second Ctrl+C forces immediate exit. |
| `SIGTERM` | Same as SIGINT |
| `SIGPIPE` | Ignored (prevents crash when writing to a closed pipe) |
| `SIGSEGV/SIGABRT/SIGFPE/SIGILL` | Writes `messages.json.crash`, then re-raises — crash recovery on next launch |

Pressing **Ctrl+C** during the REPL sends SIGINT, which gracefully cancels the current turn.
Pressing **ESC** at the prompt cancels an in-flight turn without exiting.

---

## 10. Setup

### Prerequisites

- C++23 compiler (gcc/g++ preferred; clang also works)
- cmake ≥ 3.28
- SQLite3
- OpenSSL
- libpng
- uuid

### Build

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
./build/icarus --version
```

### Install

```bash
cmake --build build --target install   # installs to ~/.local/bin/
```

### First-run model configuration

Models are stored in `daedalus.db` via the daedalus `SettingsStore`. The
easiest way to add one is inside icarus itself — open the settings TUI:

```
> /settings   # Providers tab: name, host, api key, endpoints, timeout
              # Models tab: name, provider, model id, temperature, context
```

or non-interactively:

```
> /provider-add MiniMax https://api.minimax.io <api-key>
> /model-add minimax-m3 MiniMax MiniMax-M3
```

### Migrating from legacy settings

```bash
# Preview what will be copied
icarus --migrate-data

# Apply the migration
icarus --migrate-data-apply

# Verify the new locations
ls ~/.local/share/daedalus/
ls ~/.config/daedalus/

# Remove the legacy directory manually
rm -rf ~/.daedalus
```
