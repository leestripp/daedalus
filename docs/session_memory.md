# Session Memory

Session Memory is the agent's persistent scratch-pad — a JSON-backed store
that survives between sessions so the agent doesn't have to rediscover the
same patterns every time it starts up.

> **Storage location:** per-project, never in the XDG user tier. See
> [`docs/storage.md`](storage.md) for the exact path resolution and the
> two-tier rule.

## What is remembered

The agent stores several categories of facts:

- **Builds** — commands + flags that worked (e.g. `cmake --build build -j8`).
- **Errors** — error patterns and their solutions.
- **Preferences** — inferred from repeated behaviour (e.g. "user prefers
  `git switch` over `git checkout`").
- **Patterns** — file locations, naming conventions, style preferences.
- **Session summaries** — a short recap of what happened at the end of a
  session for the next session's context.

The host can also write explicit entries through the `remember()` tool.

## Storage

Session memory lives in the project tier:

```
<project_root>/.daedalus/daedalus[-<agent_name>]/
├── memory/
│   ├── memory.json          ← source of truth
│   └── daedalus.db          ← SQLite mirror (best-effort recovery aid)
├── messages.json
└── ...
```

- `memory.json` is the source of truth. It is written atomically on
  `SessionMemory` destruction and after notable lifecycle events.
- `daedalus.db` (inside the `memory/` directory) is a SQLite mirror keyed
  by `mem.<session_id>`. If `memory.json` is corrupt or missing on the
  next load, the mirror is used as a fallback.

`agent_name` defaults to `""` (the default namespace). Multiple agents on
same project use separate subdirectories and do not share memory.

## Schema

The JSON file contains:

```json
{
  "version": "1.0",
  "updated_at": "2026-07-22T04:00:00Z",
  "builds": [...],
  "errors": [...],
  "preferences": [...],
  "patterns": [...],
  "sessions": [...],
  "project": {...},
  "config": {...}
}
```

Key entry types:

| Type | Fields |
|------|--------|
| `BuildEntry` | `id`, `command`, `success`, `attempts`, `first_used`, `last_used`, `use_count`, `notes` |
| `ErrorEntry` | `id`, `pattern`, `solution`, `occurrences`, `first_seen`, `last_seen`, `resolved`, `confidence` |
| `PreferenceEntry` | `id`, `key`, `value`, `inferred_from`, `first_observed`, `observation_count`, `user_confirmed` |
| `PatternEntry` | `id`, `type`, `description`, `example`, `confidence` |
| `SessionSummary` | `id`, `summary`, `timestamp`, `files_touched`, `iterations` |

All stored strings are sanitized to valid UTF-8 before persistence.

## Who writes what

| Who | What | How |
|-----|------|-----|
| Agent | Learned build commands | Writes a `BuildEntry` on successful builds |
| Agent | Learned error solutions | Writes an `ErrorEntry` when a fix succeeds |
| Agent | Session summary | Writes a `SessionSummary` when the session ends |
| Host / user | Explicit preferences | Calls the `remember()` tool |
| Host / user | Manual entries | Calls the `remember()` tool |
| Host / user | Clear memory | Calls the `forget()` tool or deletes `memory.json` |

## Memory tools

The following tools are available to the agent by default:

- `remember` — store a fact under a category.
- `forget` — remove a fact by ID.
- `memories` — list or search stored facts.
- `memory_stats` — show memory size and category counts.

See [`docs/tools.md`](tools.md) for the exact JSON schemas.

## Configuration

`SessionMemory::Config` controls sizing and auto-learning behaviour:

| Field | Default | Purpose |
|-------|---------|---------|
| `enabled` | `true` | Master switch |
| `max_total_size_kb` | 100 | Soft cap on total memory size |
| `max_entries_per_category` | 50 | Per-category entry limit |
| `max_session_history` | 20 | How many session summaries to keep |
| `max_entry_size_bytes` | 2048 | Max size of a single stored string |
| `memory_age_days` | 30 | Age at which entries are eligible for pruning |
| `auto_detect_errors` | `true` | Learn from errors automatically |
| `auto_detect_preferences` | `true` | Learn preferences automatically |
| `auto_detect_patterns` | `true` | Learn project patterns automatically |
| `auto_learn_builds` | `true` | Record successful build commands |

Hosts can disable memory entirely via `AgentConfig::memory_enabled`.

## See also

- [`docs/storage.md`](storage.md) — path layout and the two-tier rule
- [`docs/tools.md`](tools.md) — tool specifications including memory tools
- `src/core/SessionMemory.{h,cpp}` — implementation
