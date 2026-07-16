# Session Memory

> **Updated (2026-07-13):** this document has been rewritten from
> scratch to reflect the per-Agent `daedalus_home` design (§Phase 8.8).
> The old file described a monolithic `~/.daedalus/session_memory.db`
> shared across all agents. Each agent now has its own database under
> `~/.daedalus/{agent_name}/session_memory.db`.

## What Session Memory Is

Session Memory is the agent's persistent scratch-pad — a SQLite database
that survives between sessions so the agent doesn't have to rediscover
the same patterns every time it starts up. It lives alongside the
messages history and settings in the per-agent data directory.

The agent uses it to store:
- **Build patterns** — commands + flags that worked (`cmake --build build -j8`)
- **Error patterns** — known errors and their solutions
- **Preferences** — inferred from repeated behaviour (e.g. "user prefers `git switch` over `git checkout`")
- **Patterns** — file locations, naming conventions, style preferences
- **Session summaries** — what happened at end of session for next session's context

The host can also write entries directly via the `remember()` tool.

## Storage location

```
~/.local/share/daedalus/{agent_name}/
├── daedalus.db               ← SettingsStore (v0.4.0+)
└── session_memory.db         ← SQLite database for memories

# Per-project runtime state (separate from the user-level data above):
<project_root>/.daedalus[-<agent_name>]/
├── messages.json
└── (other session files)
```

`agent_name` defaults to `""` (no namespace) when not specified in
`AgentConfig`. The database is created automatically on first use;
its parent directory is `cfg.daedalus_home/{agent_name}/`.

## Schema

```sql
CREATE TABLE memories (
    id           TEXT PRIMARY KEY,
    category     TEXT NOT NULL,   -- builds | errors | preferences | patterns | sessions
    content      TEXT NOT NULL,
    metadata     TEXT,            -- JSON: extra key-value pairs
    tags         TEXT,            -- JSON: array of strings
    created_at   INTEGER,
    updated_at   INTEGER,
    confidence   INTEGER DEFAULT 3
);
```

The `id` is assigned by the agent (e.g. `err_001`, `bld_abc`). The host
does not need to manage IDs — the `remember()` tool handles ID assignment.

## API (host-facing)

The `SessionMemory` class is constructed by `AgentImpl`. Hosts don't
construct it directly unless they need to query the database without the
agent (e.g. a "show memory" UI panel):

```cpp
// include/daedalus/SessionMemory.h
namespace daedalus {
struct MemoryEntry { /* id, category, content, metadata, tags, timestamps, confidence */ };

class SessionMemory {
public:
    // Construct — opens (or creates) the per-agent DB
    explicit SessionMemory(const std::string& db_path);

    // ---- agent writes ----
    bool remember(const MemoryEntry& entry);
    bool forget(const std::string& id);
    bool update(const MemoryEntry& entry);

    // ---- agent reads ----
    std::vector<MemoryEntry> get_all() const;
    std::vector<MemoryEntry> get_by_category(const std::string& cat) const;
    std::optional<MemoryEntry> get_by_id(const std::string& id) const;

    // ---- context injection ----
    std::string inject_as_context(size_t max_entries = 10) const;
    // Returns a string like:
    //   ## Session Memory\n- [builds] bld_001: cmake --build build -j8 (src/)\n...
};
}
```

## What the agent writes vs what the host controls

| Who | What | How |
|-----|------|-----|
| Agent | Learned build commands | Writes `BuildEntry` on successful builds |
| Agent | Learned error solutions | Writes `ErrorEntry` on first fix |
| Agent | Session summary | Writes `SessionSummary` when `stop` fires |
| Host | Explicit preferences | Calls `SessionMemory::remember()` via `remember()` tool |
| Host | Manual entries | Calls `SessionMemory::remember()` directly |
| Host | Clear memory | Calls `forget()` or wipes the DB |

The agent's `remember` / `forget` tools translate to direct calls on the
`SessionMemory` singleton owned by `AgentImpl`.

## What the host does NOT control

- The agent decides what patterns to remember (e.g. after a successful
  build, after a build error is fixed). The host cannot veto a memory
  write.
- The agent decides how to format `inject_as_context`. Hosts should
  treat the returned string as opaque and insert it into the system
  prompt verbatim.

## UTF-8 safety

The `SessionMemory` implementation sanitises all stored strings to valid
UTF-8 before writing to the database. Invalid bytes are replaced with
`?`. This prevents the database from becoming unreadable on non-UTF-8
systems.

## Session end

On `stop` tool, the agent writes a `SessionSummary` entry with:
- Session ID
- Summary of what was accomplished
- Files touched
- Tags (`["session-end"]`)

This gives the next session a one-paragraph recap without re-reading the
full conversation.
