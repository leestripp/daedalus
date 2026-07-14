# System Prompt Composition

> **TL;DR** — You write the identity, rules, and workflow. daedalus
> appends the live tool surface, MCP status, skill catalog, memory,
> and context stats. You control all prose via config; daedalus
> handles all mechanics.

---

## The Contract

The system prompt is a **composite document** assembled in this order:

```
┌─────────────────────────────────────────────────────────────┐
│  [system_prompt_prefix]         ← developer (optional)     │
│  [system_prompt]                ← developer (your rules)  │
│  [tool definitions]              ← daedalus (live JSON)   │
│  [MCP servers + tools]           ← daedalus (live)        │
│  [active skills]                 ← daedalus (live)        │
│  [available skills]              ← daedalus (live)        │
│  [session memory]                ← daedalus (if enabled)  │
│  [context status]                ← daedalus (per-turn)    │
│  [system_prompt_suffix]         ← developer (optional)    │
└─────────────────────────────────────────────────────────────┘
```

**You** write everything the agent needs to *know about its role*.
**daedalus** appends everything the agent needs to *know about its environment*.

---

## 1. Developer's Job: Write the Prompt

Set `AgentConfig::system_prompt` to the full role/persona/rules text.
daedalus takes it verbatim — no transformation, no defaults.

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
| **Identity** | "You are a senior C++ engineer…" |
| **Task focus** | "Stay on task. Do not modify unrelated code." |
| **Workflow** | "Understand → Plan → Act → Verify → Stop" |
| **Tool policies** | "Always run `cmake --build` after editing." |
| **Error handling** | "On a build error, read the output before acting." |
| **Tone/persona** | "Explain reasoning before acting." |

### Prefix and suffix

For text that should appear *before* the role section or *after* the
daedalus sections, use:

```cpp
cfg.system_prompt_prefix = "You are in a secure sandbox.\n";
cfg.system_prompt_suffix = "\n---\nRemember: safety over speed.\n";
```

Both are optional. Both are inserted verbatim without any daedalus
processing.

---

## 2. daedalus's Job: Append Dynamic Sections

daedalus appends the following sections in fixed order. You cannot
reorder them, but you can disable some via `AgentConfig` flags.

### 2.1 Tool Definitions (JSON Schema)

daedalus appends a fenced JSON block containing every tool the agent
can call — built-in tools plus any MCP tools currently loaded.

```json
# Tool Definitions (JSON Schema)

```json
[
  {
    "type": "function",
    "function": {
      "name": "execute_bash",
      "description": "Execute a shell command...",
      "parameters": { ... }
    }
  },
  ...
]
```
```

**How to control it:** Tools are registered by the host (built-in
tools are always present). MCP tools appear here only when
`cfg.mcp_enabled = true` and the MCP server is running.

**Why it is JSON:** The agent receives machine-readable schemas it can
parse without the library needing to maintain a parallel prose
description. The host does not need to write this section.

---

### 2.2 MCP Servers and Tools

If `cfg.mcp_enabled = true`, daedalus appends a section listing each
active MCP server and its available tools:

```
# MCP Servers

## server-name
Connected via: stdio
Tools:
  - mcp_read_image   — read and describe an image file
  - mcp_web_search  — search the web
```

**How to control it:** Set `cfg.mcp_enabled = false` to suppress this
section entirely (sandboxed agent with no external tool integrations).

---

### 2.3 Active Skills

Skills are domain-specific knowledge packages the agent can load at
runtime. daedalus appends the list of *currently loaded* skills:

```
# Active Skills

| Skill | Description |
|-------|-------------|
| gdb-crash-debugger | Run GDB in batch mode to get crash backtraces |
| memory-leak | Detect memory leaks using Valgrind Memcheck |
```

The full skill content (`SKILL.md`) is **not** inlined here. The agent
sees the name, description, and a short usage note. The agent must
call `load_skill` to bring the full content into context.

**How to control it:** Pass `cfg.skills` to name skills to pre-load,
or set `cfg.skills_dirs` to directories to scan. Empty = no skills.

---

### 2.4 Available Skills Catalog

daedalus also appends every *available-but-not-loaded* skill it
discovers, so the agent can decide to load more at runtime:

```
# Available Skills (call load_skill to activate)

| Skill | Description |
|-------|-------------|
| codemax-container-setup | Set up and run daedalus inside a Podman container |
| mcp-standard | How to build MCP servers and MCP-aware agents |
| firefox | Access Firefox via WebDriver BiDi |
```

The agent uses `load_skill <name>` to bring one into the Active
Skills list above.

---

### 2.5 Session Memory

If `cfg.memory_enabled = true` (the default), daedalus appends a
fact dump from the long-term memory store:

```
# Session Memory

- remember 2026-07-12: the project's CMake uses -j8 for parallel builds
- forget 2026-06-01: stale note about gcc version (now using clang++)
```

**How to control it:** Set `cfg.memory_enabled = false` to suppress
this section.

---

### 2.6 Context Status (per-turn)

On every turn, daedalus appends a live token-usage and iteration
table:

```
# Context Status

| Metric | Value |
|--------|-------|
| Token Usage | 12,345 / 200,000 (6.2%) |
| Messages | 8 |
| Iterations | 3 / 300 |
```

The agent uses this to decide when to call `compress_context`.

---

## 3. The Complete Flow

```
Host sets AgentConfig:
  cfg.system_prompt         = role + rules text          (you write)
  cfg.system_prompt_prefix  = optional pre-role text     (you write)
  cfg.system_prompt_suffix  = optional post-stats text    (you write)
  cfg.mcp_enabled           = true / false
  cfg.memory_enabled        = true / false
  cfg.skills_dirs           = { paths to scan }
  cfg.skills                = { names to pre-load }

Agent::initialize()
  → loads settings, MCP (if enabled), memory, skills
  → SkillManager.discover_all_skills()
  → SkillManager.activate(cfg.skills)

Agent::run_one_turn(user_message)
  → SystemPromptBuilder.compose(options)
       1. system_prompt_prefix          (verbatim)
       2. system_prompt                 (verbatim — your text)
       3. tool_definitions JSON schema   (live from ToolHandler)
       4. MCP servers + tools            (live from McpServerManager)
       5. active_skills                 (live from SkillManager)
       6. available_skills catalog       (live from SkillManager)
       7. session_memory fact dump        (live from SessionMemory, if enabled)
       8. context_status table           (live: token count, iteration)
       9. system_prompt_suffix           (verbatim)

  → final_prompt = composed string
  → messages.add(role="system", content=final_prompt)
  → messages.add(role="user",   content=user_message)
  → LLMClient.call(messages)
  → LLMClient.on_assistant_delta → sink_->on_assistant_delta(token)
  → LLMClient.on_tool_call     → sink_->on_tool_call(name, args)
  → ToolHandler.execute(name, args) → sink_->on_tool_result(name, result)
  → messages.add(role="assistant", content=response)
```

---

## 4. MCP Server Discovery

When `cfg.mcp_enabled = true`, daedalus reads the MCP server
definition file (default: `~/.daedalus/mcp/servers.json`) and
launches each server process. The tool names and schemas advertised
in the Tool Definitions section (§2.1) and the MCP Servers section
(§2.2) come from the servers' `initialize` response — they are
entirely server-controlled. daedalus does not interpret or filter
them; it just passes them through.

To disable MCP entirely for a given Agent:

```cpp
cfg.mcp_enabled = false;          // no MCP servers started
cfg.mcp_servers_path.clear();    // also suppress the path
```

---

## 5. Skills Discovery

Skills live on disk as a directory tree. Each skill directory contains
a `SKILL.md` file (the full content the agent sees after loading)
and an optional `skill.json` metadata file.

```
~/.daedalus/skills/
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
containing `SKILL.md`. To configure skills:

```cpp
// Scan this directory for available skills
cfg.skills_dirs = { "/home/user/.daedalus/skills/" };

// Pre-load these skills (they appear in §2.3 immediately)
cfg.skills = { "gdb-crash-debugger", "memory-leak" };
```

The agent uses these tools at runtime:

| Tool | Effect |
|---|---|
| `discover_skills` | Re-scan skills_dirs and return the catalog |
| `load_skill <name>` | Load a SKILL.md into the Active Skills section |
| `get_active_skills` | List currently active skills |
| `get_skill_context <name>` | Return the full SKILL.md text for a skill |
| `deactivate_skill <name>` | Remove from active list |
| `search_skills <query>` | Full-text search across skill names and descriptions |

---

## 6. Example: Three Different Agent Configurations

### CLI demo — interactive coding agent

```cpp
cfg.system_prompt = R"(
# Role

You are daedalus, an autonomous coding agent designed to help
users with software development tasks.

## Task Focus — CRITICAL

DO NOT modify code that is unrelated to the user's request.

## Workflow

Understand → Plan → Act → Build/Test → Stop
)";
```

### Manager demo — orchestrates sub-agents

```cpp
cfg.system_prompt = R"(
# Role

You are a PLANNING AGENT. Your job is to decompose a goal into
ordered, independent sub-tasks that can be handed off to
sub-agents for execution.

## Rules

- Break the task into tasks that can run in parallel.
- Each sub-task must be independently verifiable.
- Do NOT edit files or run commands yourself.
- Hand off to sub-agents only.
)";
```

### Webui demo — isolated session per HTTP connection

```cpp
cfg.system_prompt = R"(
# Role

You are a helpful coding assistant in a web-based IDE session.
Each session is isolated — you have no memory of previous sessions.
)";
cfg.memory_enabled = false;   // no session memory in the webui
```

---

## 7. Backward Compatibility

The old monolithic system prompt builder (which generated all prose
in-library, including agent identity, task-focus text, workflow
instructions, and build-system rules) has been deprecated. The
deprecated API still returns empty strings — the library no longer
generates any prose identity text.

New hosts **must** supply their own `system_prompt`. Existing hosts
should migrate by:

1. Copying the prose from the old `daedalus/system_prompt.txt` into
   `AgentConfig::system_prompt`.
2. Removing any calls to the deprecated `SystemPromptBuilder` static
   methods (`getAgentIdentitySection`, `getTaskFocusSection`, etc.).
3. Setting `cfg.skills_dirs` and `cfg.skills` if the old agent used
   skill discovery.

---

## 8. Reference: AgentConfig Fields

| Field | Type | Default | Section injected |
|---|---|---|---|
| `system_prompt` | `std::string` | *(empty)* | §1 (verbatim) |
| `system_prompt_prefix` | `std::string` | *(empty)* | before §1 |
| `system_prompt_suffix` | `std::string` | *(empty)* | after §8 |
| `mcp_enabled` | `bool` | `true` | suppresses §2.2 and §2.1 MCP tools |
| `memory_enabled` | `bool` | `true` | suppresses §2.5 |
| `skills_dirs` | `vector<path>` | *(empty)* | drives §2.3 + §2.4 discovery |
| `skills` | `vector<string>` | *(empty)* | pre-loads skills into §2.3 |
| `daedalus_home` | `path` | `$DAEDALUS_HOME` / `$HOME/.daedalus` | path resolution for all daedalus-managed files |
