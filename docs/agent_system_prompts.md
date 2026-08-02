# System Prompt Composition

> **TL;DR** — You write the identity, rules, workflow, build conventions,
> and tone. daedalus appends the live tool surface, MCP status, skill
> catalog, memory dump, context stats, the **state-machine instructions**,
> and the **reserved-directory contract**. You control all prose via
> config; daedalus handles all mechanics — and emits two always-on
> sections that protect the runtime.

---

## The Contract

The system prompt is a **composite document** assembled by
`SystemPromptBuilder::compose()` in this fixed order. There are 9 slots
(plus a transient trailing Context Status message that is not a slot):

```
┌─────────────────────────────────────────────────────────────┐
│  1. system_prompt          ← developer (your identity /      │
│                                rules / workflow — verbatim)  │
│  2. system_info_note       ← developer (optional 1-line     │
│                                reminder about system_info)  │
│  3. mcp_servers            ← daedalus (live servers + tools, │
│                                when MCP enabled)             │
│  4. tool_definitions       ← daedalus (live JSON Schema of   │
│                                every callable tool)          │
│  5. active_skills          ← daedalus (skills currently      │
│                                loaded — name + desc only)    │
│  6. available_skills       ← daedalus (catalog of skills the │
│                                agent can load_skill)         │
│  7. State Machine          ← daedalus (ALWAYS appended —     │
│                                see §2.7)                     │
│  8. Reserved Agent Dirs    ← daedalus (ALWAYS appended —     │
│                                see §2.8)                     │
│  9. session_memory         ← daedalus (fact dump, opt-in,    │
│                                ALWAYS LAST — §ContextCache)  │
└─────────────────────────────────────────────────────────────┘
│  ~ Context Status          ← transient trailing message, NOT │
│                                part of the prompt (§2.6)     │
```

**You** write everything the agent needs to *know about its role and
rules*. **daedalus** appends everything the agent needs to *know about
its environment* — and the two always-on sections that protect the
runtime.

Slots 1–2 are **yours**. Slots 3–6 and 9 are daedalus's dynamic,
data-driven additions (each can be suppressed by leaving the
corresponding field empty or toggling a flag). Slots 7 and 8 are
**non-optional** daedalus prose sections — see §2.7 and §2.8 for why
they exist and how to augment (not replace) them in your prose. Slot 9
is emitted **last** because it is the most volatile section — emitting
it after the byte-static mandatory sections keeps them inside the
server-side prompt-cache prefix (§ContextCache).

---

## 1. Developer's Job: Write the Prompt

Set `AgentConfig::system_prompt` (or, for fine-grained control,
`SystemPromptOptions::system_prompt`) to the full role / persona /
rules text. daedalus takes it verbatim — no transformation, no
defaults, no validation.

```cpp
daedalus::AgentConfig cfg;
cfg.project_root = "/work/myproject";

cfg.system_prompt = R"(
# Role

You are a senior C++ engineer who specialises in writing clean,
maintainable code. You explain your reasoning before acting and
verify your changes compile before finishing.

## Behaviour

- Stay on task. Do not refactor or "improve" code unrelated to
  the user's request.
- Prefer small, focused commits over large rewrites.
- When uncertain, ask the user before proceeding.
)";
```

### What goes in `system_prompt`

| Content | Example |
|---|---|
| **Identity / persona** | "You are a senior C++ engineer…" |
| **Task focus rules** | "Stay on task. Do not modify unrelated code." |
| **Workflow** | "Understand → Plan → Act → Verify → Stop" |
| **Tool policies** | "Always run `cmake --build` after editing." |
| **Build conventions** | "Configure with `-S . -B build`, never `cd build`." |
| **Error handling** | "On a build error, read the output before acting." |
| **Style / tone** | "Be terse; cite file:line." |

### `system_info_note` — optional 1-line reminder

If your agent should be nudged to call the `system_info` tool at
session start (so it knows the host environment), set
`LLMClient::Config::system_info_note` to a one-liner like:

```
Call `system_info` once at the start of each session.
```

The library appends it verbatim after your `system_prompt` but before
the daedalus-managed sections if `include_system_info_note = true`
(default) and the string is non-empty. Empty (the default) → the slot
is skipped entirely.

### Do **NOT** put the following in `system_prompt`

These are daedalus's job — the library will write them for you:

| If you write… | Result |
|---|---|
| **JSON tool schemas** | Library appends live JSON in slot 4 — your copy goes stale |
| **MCP server list** | Library appends live status in slot 3 — your copy goes stale |
| **Skill catalog / active-skill lists** | Library appends live data in slots 5–6 — your copy goes stale |
| **Session-memory fact dump** | Library appends live facts in slot 9 (always last) |
| **Token-usage / iteration table** | Library sends a live transient trailing message every turn — your copy goes stale and breaks prompt caches |
| **`# State Machine` instructions** | Library appends its own version in slot 7 — your copy duplicates |
| **`# Reserved Agent Directories` instructions** | Library appends its own version in slot 8 — your copy duplicates |

If you want to *augment* the library's mandatory slots (7 and 8) with
extra domain-specific rules (e.g. "in this project, the agent must
always run `pytest` before `Done`"), put those *prose* rules in your
`system_prompt` — the library sections are appended **after** yours
and won't overwrite them.

### Demo hosts

The repo ships three reference system prompts in `demos/`:

| Host | File |
|---|---|
| `daedalus-cli` (interactive coding agent) | `demos/daedalus-cli/system_prompt.txt` |
| `daedalus-manager` (planner / orchestrator) | `demos/daedalus-manager/main.cpp` (inline) |
| `daedalus-webui` (per-session web assistant) | `demos/daedalus-webui/main.cpp` (inline) |

Use any of these as a starting point and tailor the identity / rules
section to your use case.

---

## 2. daedalus's Job: Append Dynamic + Mandatory Sections

daedalus appends the following sections in fixed order. Slots 3, 5, 6
opt out by leaving the matching `SystemPromptOptions` field empty.
Slot 9 also has a master `include_*` flag. Slots 7 and 8
are **non-optional** — see each sub-section.

### 2.1 Tool Definitions (JSON Schema)

daedalus appends a fenced JSON block containing every tool the agent
can call — built-in tools plus any MCP tools currently loaded.

```
# Tool Definitions (JSON Schema)
```json
[
  { "type": "function", "function": { "name": "execute_bash", ... } },
  ...
]
```
```

**You don't write this.** It's generated per turn from
`ToolHandler::get_tool_definitions()`. To control it, register /
unregister tools in `AgentConfig::tools` (or via MCP servers); the
JSON surfaces those bindings verbatim.

### 2.2 MCP Servers and Tools

If MCP is enabled and any servers are connected, daedalus appends a
section naming each server, its transport, and the tools it offers:

```
# MCP Servers

## filesystem
Connected via: stdio
Tools:
  - filesystem__read_file   — read a file by absolute path
  - filesystem__write_file  — write to a file by absolute path
```

**You don't write this.** It is generated from
`McpServerManager::list_connected_servers()` +
`McpServerManager::get_all_tools()`. The MCP **tools** also surface
inside `# Tool Definitions` (slot 4) — this slot is the human-readable
"what servers are connected and what they offer" summary. Leave
`mcp_servers` empty to suppress.

### 2.3 Active Skills

Skills are domain-specific knowledge packages (e.g.
`gdb-crash-debugger`, `memory-leak`). daedalus appends a table of
currently-loaded skills:

```
# Active Skills

| Skill | Description |
|-------|-------------|
| gdb-crash-debugger | Run GDB in batch mode to get crash backtraces |
```

The full `SKILL.md` content is **not** inlined — the agent calls
`get_skill_context <name>` to retrieve the body of a single skill on
demand. This keeps the system prompt small.

**You don't write this.** It's generated per turn from
`SkillManager::get_active_skills()`. Pre-load skills via
`AgentConfig::skills` (names → loaded from disk by the library) or
fill `SystemPromptOptions::active_skills` directly with pre-formatted
prose.

### 2.4 Available Skills Catalog

daedalus also appends every *available-but-not-loaded* skill it has
discovered on disk, so the agent can decide to load more at runtime:

```
# Available Skills (catalog)

The following skills are available. If your task seems related to
one, use the load_skill tool to activate it before proceeding.

| Skill | Description |
|-------|-------------|
| mcp-standard | How to build MCP servers and MCP-aware agents |
| firefox | Access Firefox via WebDriver BiDi |
```

**You don't write this.** It's generated from
`SkillManager::discover_all_skills() - active_set`. Pass
`SystemPromptOptions::available_skills` to override the formatted
prose, or leave empty to suppress.

### 2.5 Session Memory (opt-in)

If `include_session_memory = true` (default) and `session_memory` is
non-empty, daedalus appends a fact dump from the long-term memory
store:

```
# Session Memory

- 2026-07-12: project uses -j8 for parallel builds
- 2026-06-01: stale note about gcc (now on clang++) — pruned
```

**You don't write this.** It's generated each turn from
`SessionMemory::get_system_prompt_addition()` and emitted as the
**last** section of the composed prompt (§ContextCache: the memory dump
is the most volatile section, so it goes after the byte-static
mandatory sections to keep them inside the server-side prompt-cache
prefix). To opt out, set `include_session_memory = false` or leave the
field empty; hosts using `AgentConfig` can set
`cfg.memory_in_prompt = false` to drop the section while memory
collection continues.

### 2.6 Context Status (per-turn, transient trailing message)

daedalus reports a token-usage and iteration table on every turn:

```
# Context Status
<!-- Auto-updated each iteration -->
| Metric | Value |
|--------|-------|
| Token Usage | 12,345 / 200,000 (6%) |
| Iterations | 3 / 300 |
| Messages | 42 |
| Date/Time | 2026-07-31 02:16:56 UTC |
| Prompt Cache | 9,800/12,345 prompt tokens cached |
```

The agent reads this to decide when to compact context.

**You don't write this — and it is NOT part of the system prompt.**
§ContextCache (2026-07): the block changes every iteration, so it is
carried as a transient **trailing system message** appended to the wire
messages (and replaced on the next iteration) by
`LLMClient::inject_context_stats` / `context_status::
upsert_context_status_message` (`src/api/ContextStatus.h`). Embedding it
in the static system prompt invalidated provider prompt caches
(MiniMax / Kimi match on a stable tools → system → messages prefix) for
the entire conversation history. Sessions persisted before this change
have the legacy embedded block stripped automatically on the next turn.

### 2.7 State Machine — **ALWAYS APPENDED** ✋

This section is **mandatory** — there is no flag to turn it off. The
library writes it on every `compose()` call because the agent is the
only party that knows when its own work is done, and we need a stable
set of state-transition rules in every prompt:

```
# State Machine

You drive a state machine. On every transition the library notifies
the host through its `on_state_change` callback; the host UI uses
those events to render the current stage.

**How to change state:** call the `set_status` tool with
`{"state": "<Name>"}` whenever you enter a new phase. This is the
ONLY mechanism — tool calls do not carry an implicit state, and the
host cannot see your phase unless you call `set_status`.

States:
| State | Meaning |
|-------|---------|
| Idle         | No work in flight. |
| Understanding | Reading the request, parsing input, gathering context. |
| Planning     | Designing the approach, choosing which tools to call. |
| Acting       | Running tools (file edits, bash, MCP, etc.). |
| Verifying    | Compiling, testing, inspecting output — a decision gate. |
| Documenting  | Updating README, CHANGELOG, comments, docs after verification. |
| Done         | Turn complete — emit your final assistant message and stop. |
| Cancelled    | Host cancelled the turn; stop work and ack. |
| Error        | Unrecoverable error; report to host and stop. |

Runtime-owned states (informational — the library sets these itself;
never pass them to `set_status`):
| State | Meaning |
|-------|---------|
| AwaitingApproval | A bash command is waiting for host approval. |
| Compressing  | Context compaction in flight. |

Rules:
1. You are the only one who knows when the work is truly done. Do
   not stay in `Acting` after the last tool result — call
   `set_status` to switch to `Verifying` and actually inspect the
   results.
2. If verification fails, loop back via `set_status` to
   `Understanding` (or `Planning`) before re-planning the fix —
   never go to `Done`.
3. If verification passes but docs need updating, switch to
   `Documenting`. Only go to `Done` after docs are complete.
4. Call `set_status` on every phase change. The host UI shows stale
   stage chips until you do.
5. Never stay in `Acting` forever. If you have no more tools to
   call, transition to `Verifying` (and then `Done` or
   `Documenting`) or back to `Planning` if more work is needed.
6. Always end with `Done`. Call `set_status` with `Done`
   immediately before your final assistant message; otherwise the
   host UI will leave the stage chips stuck.
```

**Why the library owns this.** The host UI (webui demo, plus any host
that consumes `AgentSink::on_state_change`) renders state chips from
the transition events. If the agent isn't taught to call `set_status`
on every phase change, the UI stalls on the turn's opening state
(`Understanding`). Embedding the rules in the prompt (rather than
expecting each host to remember them in their own prose) guarantees
consistent behaviour across all hosts.

**If you want to augment it.** You may add complementary *workflow
prose* in your `system_prompt` (e.g. "always plan in detail before
Acting", "treat build warnings as errors"), but the `# State Machine`
table itself is non-negotiable — the library emits it as-is.

### 2.8 Reserved Agent Directories — **ALWAYS APPENDED** ✋

This section is also **mandatory**. It tells the agent that all
project-tier state lives under a single `.daedalus/` base directory,
with non-hidden `daedalus/` (default) and `daedalus-<id>/` (namespaced)
sub-folders:

```
# Reserved Agent Directories (.daedalus*)

The following directories in the project root are owned by the agent
runtime. They are HIDDEN system folders — treat them with the same
care as `.git/`.

| Directory | Purpose |
|-----------|---------|
| `.daedalus/` | Base directory for all agent runtime state. Do not touch. |
| `.daedalus/daedalus/` | Default agent's persistent state. Memory, session scratch, per-project config, skills cache. |
| `.daedalus/daedalus-<id>/` | Per-agent / per-session state when the host namespaces agents (`AgentConfig::agent_name`). The contents are the same as `.daedalus/daedalus/`. |

NEVER:
1. **Delete** any `.daedalus*` directory. Doing so wipes the agent's
   memory and all session scratch — unrecoverable.
2. **Rename** or **move** them.
3. **Edit files inside them directly** with `modify_file` /
   `edit_file_patch` / `create_file` unless the user has EXPLICITLY
   asked you to (and even then, ask first).
4. Include them in any "clean up junk directories" or "remove cruft"
   task. They are not junk.
5. Suggest that the user delete them to "fix" a problem. They are not
   the cause of any problem you can observe.

ALWAYS:
1. Treat them as **out-of-bounds** during normal work — like a
   `.git/` folder: read what's relevant via the agent's own tools
   (`memory_*`, `get_skill_context`, etc.) and never touch the
   directory tree directly.
2. If the user asks you to do something that would affect a
   `.daedalus*` directory, **stop and ask** before doing it.
```

**Why the library owns this.** Without the contract, a well-meaning
"clean up junk directories" task will silently nuke the agent's
memory and the user's session scratch state. See `docs/storage.md`
for the path-resolution rules.

**If you want to teach more.** You may add complementary *prose* in
your `system_prompt` (e.g. "before deleting any directory, verify
it isn't a `.daedalus*` runtime folder"), but the `# Reserved Agent
Directories` block stays put.

---

## 3. The Complete Flow

```text
Host sets AgentConfig (or SystemPromptOptions):
  cfg.system_prompt        = role + rules + workflow text   (you write)
  LLMClient::Config::system_info_note = optional 1-line      (you may write)
  cfg.mcp_enabled          = true / false
  cfg.memory_enabled       = true / false
  cfg.skills_dirs          = { paths to scan }
  cfg.skills               = { names to pre-load }

Agent::initialize()
  → loads settings, MCP (if enabled), memory, skills
  → SkillManager.discover_all_skills()
  → SkillManager.activate(cfg.skills)

Agent::run_one_turn(user_message)
  → SystemPromptBuilder.compose(options)
       1. system_prompt          (verbatim — your prose)
       2. system_info_note       (verbatim, only if non-empty + flag on)
       3. mcp_servers            (live, if any connected)
       4. tool_definitions       (live, JSON schema of all tools)
       5. active_skills          (live, name + desc)
       6. available_skills       (live catalog, excluding active)
       7. State Machine          (ALWAYS — slot §2.7)
       8. Reserved Agent Dirs    (ALWAYS — slot §2.8)
       9. session_memory         (live, LAST, if include_session_memory)

  (the per-iteration Context Status table is sent separately as a
   transient trailing message — §2.6 / §ContextCache)

  → final_prompt = composed string
  → messages.add(role="system", content=final_prompt)
  → messages.add(role="user",   content=user_message)
  → LLMClient.call(messages)
  → LLMClient.on_assistant_delta → sink_->on_assistant_delta(token)
  → LLMClient.on_tool_call      → sink_->on_tool_call(name, args) (with state)
  → ToolHandler.execute(name, args) → sink_->on_tool_result(name, result)
  → messages.add(role="assistant", content=response)
```

The host only sees slots 1–2 land "first". Slots 3–6 and 9 are
mechanical and safe to ignore. Slots 7–8 are guaranteed to be present
on every single turn — that's a contract, not an accident.

---

## 4. MCP Server Discovery

When `cfg.mcp_enabled = true`, daedalus reads the MCP server registry
from the SQLite `SettingsStore` (`$XDG_DATA_HOME/daedalus/daedalus.db`,
overridable with `cfg.daedalus_home` or `$DAEDALUS_HOME`). Legacy
`~/.daedalus/mcp/servers.json` is merged as a read fallback for one
release cycle. The tool names and schemas advertised in slots 3 (MCP
Servers) and 4 (Tool Definitions) come from the servers' `initialize`
response — they are server-controlled. daedalus does not interpret or
filter them; it just passes them through.

To disable MCP entirely for a given Agent:

```cpp
cfg.mcp_enabled = false;          // no MCP servers started
```

---

## 5. Skills Discovery

Skills live on disk as a directory tree. Each skill directory contains
a `SKILL.md` file (the body the agent sees after loading) and an
optional `skill.json` metadata file.

```
~/.local/share/daedalus/skills/
├── codemax-container-setup/
│   ├── SKILL.md
│   └── skill.json
├── firefox/
│   ├── SKILL.md
│   └── skill.json
└── mcp-standard/
    └── SKILL.md
```

daedalus discovers skills by scanning `cfg.skills_dirs` for directories
containing `SKILL.md`. Configure with:

```cpp
// Scan these directories for available skills
cfg.skills_dirs = { daedalus::Paths::global().global_skills_dir() };

// Pre-load these skills (they appear in slot 5 immediately)
cfg.skills = { "gdb-crash-debugger", "memory-leak" };
```

Agent tools that operate on skills at runtime:

| Tool | Effect |
|---|---|
| `discover_skills` | Re-scan skills_dirs and return the catalog |
| `load_skill <name>` | Load a SKILL.md into Active Skills (slot 5) |
| `get_active_skills` | List currently active skills |
| `get_skill_context <name>` | Return the full SKILL.md text for a skill |
| `deactivate_skill <name>` | Remove from active list |
| `search_skills <query>` | Full-text search across skill names and descriptions |

---

## 6. Example: Three Host Configurations

### CLI — interactive coding agent

```cpp
cfg.system_prompt = R"(
# Role

You are daedalus, an autonomous coding agent designed to help
users with software development tasks.

## Task Focus — CRITICAL

DO NOT modify code that is unrelated to the user's request.
)";
```

**What the library will add (in order):**

1. *(none — your prose is the whole identity / rules)*
2. *(any `system_info_note`, if set)*
3. *(any connected MCP servers)*
4. tool JSON schemas (built-in tools)
5. active skills
6. available skills catalog
7. `# State Machine` (mandatory)
8. `# Reserved Agent Directories` (mandatory)
9. session memory *(omitted if `memory_enabled = false`; always last)*

*(The per-iteration context-status table is no longer a compose slot —
it rides as a transient trailing message so the static prompt stays
byte-stable for provider prompt caches. See §2.6.)*

### Manager — orchestrates sub-agents

```cpp
cfg.system_prompt = R"(
# Role

You are a PLANNING AGENT. Your job is to decompose a goal into
ordered, independent sub-tasks that can be handed off to sub-agents.

## Rules

- Break the task into tasks that can run in parallel.
- Each sub-task must be independently verifiable.
- Do NOT edit files or run commands yourself.
)";
```

Library adders as above (1–9). The manager's status flips naturally
land in `Planning`; its sub-agents use `Acting` heavily.

### Webui — isolated session per HTTP connection

```cpp
cfg.system_prompt = R"(
# Role

You are a helpful coding assistant in a web-based IDE session.
Each session is isolated — you have no memory of previous sessions.
)";
cfg.memory_enabled = false;   // no session-memory slot
```

Library adders as above (1–9). Slot 9 (`# Session Memory`) is
skipped because `memory_enabled = false`. Slots 7 and 8 still append.

---

## 7. Backward Compatibility

The old monolithic system prompt builder — which generated all prose
in-library including agent identity, task-focus text, workflow
instructions, and build-system rules — has been deprecated. The
deprecated API still returns empty strings; the library no longer
generates any prose identity text on the host's behalf.

**Migration checklist for existing hosts:**

1. Copy the prose from the old `daedalus/system_prompt.txt` into
   `AgentConfig::system_prompt` (or `SystemPromptOptions::system_prompt`).
2. Stop calling the deprecated `SystemPromptBuilder` static methods
   (they return empty strings now and are slated for removal).
3. Set `cfg.skills_dirs` and `cfg.skills` if the old agent used
   skill discovery.
4. Trust the library to emit `# State Machine` and `# Reserved Agent
   Directories` automatically — **do not duplicate them in your
   prose** (you'd just be adding tokens for the same content).

**Why there is no `# prefix` / `# suffix` slot any more.** Earlier
versions of the library exposed `system_prompt_prefix` and
`system_prompt_suffix` fields wrapping the dynamic sections. Those
have been removed because:

- They encouraged hosts to write prose in slots that should be empty
  (the library adders are exactly enough).
- They broke the order guarantee — the library can't promise "your
  prose appears between the role and the tools" when the host also
  injects prose there.

If you want prose before the role: put it at the top of your
`system_prompt` (slot 1). If you want prose after everything: put it
at the bottom of your `system_prompt` (still slot 1 — the library
always appends after you). If you want prose interleaved with the
dynamic sections, that is **not supported** — the slot order is
fixed because each slot has a defined semantic role (tools come after
identity, skills come after tools, etc.).

---

## 8. Reference: `SystemPromptOptions` Fields

Every field maps 1:1 to a slot in the contract diagram.

| Field | Slot | Default | Notes |
|---|---|---|---|
| `system_prompt` | 1 | `""` | Your prose, verbatim, first section |
| `system_info_note` | 2 | `""` | 1-line reminder; empty = skipped |
| `include_system_info_note` | 2 | `true` | Master switch for slot 2 |
| `mcp_servers` | 3 | `""` | Pre-formatted MCP server status; empty = skipped |
| `tool_definitions` | 4 | `""` | Tool JSON schema; empty = skipped |
| `active_skills` | 5 | `""` | Currently loaded skills; empty = skipped |
| `available_skills` | 6 | `""` | Catalog of available skills; empty = skipped |
| `session_memory` | 9 | `""` | Pre-formatted fact dump; emitted LAST |
| `include_session_memory` | 9 | `true` | Master switch for slot 9 |

Slots 7 (`# State Machine`) and 8 (`# Reserved Agent Directories`)
are library-owned and **not configurable** — see §2.7 and §2.8. The
per-iteration Context Status block (§2.6) is deliberately NOT a compose
slot: it lives outside the static system prompt so provider prompt
caches keep matching (§ContextCache). Slot 9 (`# Session Memory`) is
emitted last for the same reason — it is the most volatile section, so
placing it after the byte-static mandatory sections keeps them inside
the cacheable prefix. Hosts can also suppress it via
`AgentConfig::memory_in_prompt = false` while memory collection
continues.

For ergonomic setup, prefer filling an `AgentConfig` (`cfg.system_prompt`,
`cfg.mcp_enabled`, etc.) and letting `Agent::initialize()` populate the
options for you; reach for raw `SystemPromptOptions` only when you need
fine-grained control (e.g. swapping the pre-formatted prose strings or
suppressing a specific section with a flag).
