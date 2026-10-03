# Daedalus Storage Layout

> **Source of truth for the library's filesystem conventions, env-var
> resolution, and migration recipes.** If anything in `Readme.md` or
> other docs conflicts with this file, this file wins.
>
> *Last updated 2026-07-18.*

---

## TL;DR — the two-tier rule

daedalus stores data in **exactly two tiers**, and the boundary is non-negotiable:

> **If the data is about the USER, it goes in XDG.**
> **If the data is about THE PROJECT, it lives under
> `<project_root>/.daedalus/daedalus[-<agent_name>]/`.**

| Tier | Location | Examples |
|---|---|---|
| **User** (per user, shared across every project) | `$XDG_*_HOME/daedalus/...` | LLM API keys, MCP server registry, global skill packs, logs, cache |
| **Project** (per project, may have many per `agent_name`) | `<project_root>/.daedalus/daedalus[-<agent_name>]/...` | Long-term memory, session transcripts, scratch, per-project overrides |

**The cardinal sin we're avoiding:** putting memory in the global XDG dir. If
agent "alice" and agent "bob" both work on the same repo and both store their
memories in `~/.local/share/daedalus/memory/`, every fact alice learns bleeds
into bob's session. That bug is enforced away by `tests/test_two_tier_rule.cpp`.

---

## 1. User tier — XDG-compliant locations

The library resolves every user-tier path through `daedalus::Paths::global()`.
Hosts can inject a custom implementation for testing via
`Paths::set_override(...)`.

### 1.1 Env-var resolution chain

For each of the four user-tier roots, the same algorithm applies:
*explicit daedalus var → standard XDG var → XDG default*.

| Variable (read in this order) | Default value (no env vars set) | Purpose |
|---|---|---|
| `$DAEDALUS_HOME` | (none — single-root override) | Collapses the whole user tier to one dir |
| `$DAEDALUS_CONFIG_HOME` | — | Override `user_config_root()` |
| `$XDG_CONFIG_HOME` | `~/.config` | User-specific config |
| `$DAEDALUS_DATA_HOME` | — | Override `user_data_root()` |
| `$XDG_DATA_HOME` | `~/.local/share` | User-specific data (skills, future caches) |
| `$DAEDALUS_STATE_HOME` | — | Override `user_state_root()` |
| `$XDG_STATE_HOME` | `~/.local/state` | User-specific state (logs, history) |
| `$DAEDALUS_CACHE_HOME` | — | Override `user_cache_root()` |
| `$XDG_CACHE_HOME` | `~/.cache` | User-specific cache (regenerable) |

`$DAEDALUS_HOME` is the *single-root convenience* — it overrides every other
user-tier variable. If set, all four subdirs (config, data, state, cache) live
under `$DAEDALUS_HOME/{config,data,state,cache}/`. Useful for CI / sandboxing:

```bash
DAEDALUS_HOME=/tmp/daedalus-test daedalus-cli --migrate-data-apply
```

### 1.2 What goes where

| Concrete file / dir | Resolves to | Notes |
|---|---|---|
| `daedalus.db` (settings store) | `<data_root>/daedalus.db` | **v0.4.0+**: SQLite-backed settings. Stores models, MCP servers, Serper key. |
| `mcp/.env` | `<config_root>/mcp/.env` | MCP server environment variables (env vars for subprocesses) |
| Globally-installed skill packs | `<data_root>/skills/` | User-global skill packs |
| Library logs | `agent_dir/logs/` (**project tier**, see §2.2) | `core/Logger.h` diagnostics. Note: `Paths::log_file()` (`<state_root>/logs/daedalus.log`) exists in the API but nothing currently writes to it. |
| Response replay cache (future) | `<cache_root>/responses/` | Future |
| Unix socket (future daemon mode) | `$XDG_RUNTIME_DIR/daedalus/agent.sock` | Future |

`<config_root>` defaults to `$XDG_CONFIG_HOME/daedalus` or `~/.config/daedalus`.
`<data_root>` defaults to `$XDG_DATA_HOME/daedalus` or `~/.local/share/daedalus`.
And so on.

### 1.3 XDG directory permissions

`Agent::initialize()` calls `Paths::ensure_directories()` which
auto-creates the four XDG roots with the secure permissions shown below.
Hosts can call it explicitly earlier if they need to:

```cpp
#include <daedalus/paths.h>

daedalus::Paths::global().ensure_directories();
// The four XDG roots now exist with correct permissions.
```

`ensure_directories()` is idempotent — safe to call multiple times — and
silently no-ops on filesystems that don't support `chmod` (e.g. FAT32
mounts).

| Directory | Permissions | Reason |
|-----------|-------------|--------|
| `user_config_root()` | 0750 | Group-readable for shared daemon access |
| `user_data_root()` | 0750 | Group-readable for skill pack sharing |
| `user_state_root()` | 0700 | Private — logs may contain API key fragments |
| `user_cache_root()` | 0700 | Private — cache may contain PII |
| Project directories | 0755 | Standard project permissions |

### 1.4 System tier (read-only)

The library also looks for read-only system-shipped data:

| Variable | Default | Purpose |
|---|---|---|
| `$DAEDALUS_SYSTEM_DATA_DIRS` | `/usr/local/share/daedalus:/usr/share/daedalus` | Search path for default skill packs, prompt templates |
| `$DAEDALUS_SYSTEM_CONFIG_DIRS` | `/etc/xdg/daedalus` | System-wide config (admin-editable) |

These are searched in priority order; user data always wins on name collision.

---

## 2. Project tier — namespaced per agent

Every `Agent` instance has its own per-project namespace, namespaced by
`agent_name`. Multiple `Agent` instances on the same project never collide on
disk because they each get their own subdir.

### 2.1 agent_name rules

All project-tier state lives under a single hidden base directory:

- Empty string: `<project_root>/.daedalus/daedalus/` (default).
- Non-empty: `<project_root>/.daedalus/daedalus-<name>/`.

Names are validated by `is_valid_agent_name()`:
- Match `^[A-Za-z0-9_.-]{1,64}$`.
- Must not start with `.` (hidden) or `-` (CLI flag).
- Empty string is permitted (means "default namespace").

> **Why the extra `.daedalus/` base?** It keeps the project root tidy:
> only one hidden directory is created, and the per-agent namespaces are
> visible non-hidden sub-folders that are easy to `.gitignore`.

### 2.2 Project-tier files

Everything below lives under `agent_dir()` — the namespaced subdir. Two
`Agent` instances in the same project with different `agent_name` values
get different `agent_dir()` paths, so:

| Path | Purpose | Safe to delete between sessions? |
|---|---|---|
| `agent_dir/messages.json` | Conversation transcript. Atomic write via temp + rename + `fsync`; survives `kill -9` mid-save. Write-through auto-flush keeps it within ~100 ms of every `add_*()` call. §MSG-HEADERS (2026-10-04): each message carries a `_meta` origin envelope (`source` + `received_at`, always UTC with an explicit ` UTC` indicator) — see `docs/how_compress_works.md` → "Message origin envelope". | Yes (rebuilds from the agent loop on next turn) |
| `agent_dir/messages.json.crash` | Crash mirror — written by the SIGSEGV/SIGABRT/SIGFPE/SIGILL signal handlers and re-written on every save. Read on next launch if `messages.json` is missing/corrupt. Defensive "drop last message" recovery. | Yes |
| `agent_dir/memory/memory.json` | Long-term facts. Source of truth for `SessionMemory`. | No — losing this loses project memory |
| `agent_dir/memory/daedalus.db` | SQLite mirror of `memory.json` under `mem.<session_id>` keys. Used for recovery if the JSON file is corrupt. | Yes (regenerable from `memory.json`) |
| `agent_dir/sessions/<sid>/` | Per-session scratch state | Yes |
| `agent_dir/logs/<timestamp>_session.log` | Internal diagnostics written by the `LOG_*` macros (`core/Logger.h`). One file per session; rotated to `.old` past 10 MB; files older than 30 days are deleted on startup. | Yes |
| `agent_dir/cache/` | Project-specific regenerable caches | Yes |
| `agent_dir/skills/active/` | Project-local skill overrides | No — losing this drops overrides |
| `agent_dir/agents/<sub_name>/` | Sub-agent state | Depends on sub-agent |
| `agent_dir/.lock` | Lock file (prevents concurrent writes) | No — recreated on next write |

---

## 3. Migration recipes

### 3.1 From old JSON config to SQLite

If you have an old `config.json` or `mcp/servers.json`:

```bash
# 1. See what will migrate.
daedalus-cli --migrate-data

# 2. Apply (copies, doesn't remove).
daedalus-cli --migrate-data-apply

# 3. Verify the new layout works.
daedalus-cli /path/to/your/project

# 4. When satisfied, manually remove the old files.
rm -rf ~/.config/daedalus/config.json
rm -rf ~/.config/daedalus/mcp/servers.json
```

You can override the destinations:

```bash
XDG_CONFIG_HOME=/custom/cfg XDG_DATA_HOME=/custom/data \
    daedalus-cli --migrate-data-apply
```

### 3.2 Setting per-project agent_name

```bash
daedalus-cli --agent-name planner /path/to/project
# → /path/to/project/.daedalus/daedalus-planner/memory/...
```

Multiple agents on the same project, each with their own memory:

```bash
daedalus-cli --agent-name planner  /path/to/project   # in one terminal
daedalus-cli --agent-name actor    /path/to/project   # in another
daedalus-cli --agent-name verifier /path/to/project   # in a third
```

---

## 4. Deployment shapes

### 4.1 Local user install (no env vars)

Defaults to `~/.config/daedalus/`, `~/.local/share/daedalus/`, etc.

### 4.2 System service (systemd)

```ini
# /etc/systemd/system/daedalus.service
[Service]
Type=notify
Environment=XDG_CONFIG_HOME=/etc/daedalus
Environment=XDG_DATA_HOME=/var/lib/daedalus
Environment=XDG_STATE_HOME=/var/log/daedalus
Environment=XDG_CACHE_HOME=/var/cache/daedalus
ConfigurationDirectory=daedalus
StateDirectory=daedalus
CacheDirectory=daedalus
RuntimeDirectory=daedalus
ExecStart=/usr/bin/daedalus-daemon
```

### 4.3 Rootless Podman

```bash
podman run --rm -it \
    --userns=keep-id \
    -e XDG_CONFIG_HOME=/home/me/.config \
    -e XDG_DATA_HOME=/home/me/.local/share \
    -e XDG_STATE_HOME=/home/me/.local/state \
    -e XDG_CACHE_HOME=/home/me/.cache \
    -v daedalus-config:/home/me/.config/daedalus \
    -v daedalus-data:/home/me/.local/share/daedalus \
    -v daedalus-state:/home/me/.local/state/daedalus \
    -v daedalus-cache:/home/me/.cache/daedalus \
    -w /work -v /path/to/your/project:/work \
    localhost/daedalus:latest
```

### 4.4 Rootful Docker

```yaml
services:
  daedalus:
    image: localhost/daedalus:latest
    environment:
      XDG_CONFIG_HOME: /etc/daedalus
      XDG_DATA_HOME:  /var/lib/daedalus
      XDG_STATE_HOME: /var/log/daedalus
      XDG_CACHE_HOME: /var/cache/daedalus
    volumes:
      - daedalus-config:/etc/daedalus
      - daedalus-data:/var/lib/daedalus
      - daedalus-state:/var/log/daedalus
      - daedalus-cache:/var/cache/daedalus
      - ./myproject:/work
    working_dir: /work

volumes:
  daedalus-config:
  daedalus-data:
  daedalus-state:
  daedalus-cache:
```

### 4.5 nix / snap / flatpak

These sandboxes rewrite `$HOME`. Set the XDG vars explicitly:

```bash
XDG_CONFIG_HOME=/home/user/.config \
XDG_DATA_HOME=/home/user/.local/share \
XDG_STATE_HOME=/home/user/.local/state \
XDG_CACHE_HOME=/home/user/.cache \
daedalus-cli /path/to/project
```

---

## 5. FAQ

### Q. Where do my LLM API keys live?

`$XDG_DATA_HOME/daedalus/daedalus.db` (or `~/.local/share/daedalus/daedalus.db`
by default). The `SettingsStore` API in `include/daedalus/settings.h` provides
type-safe access — no raw JSON. See [`settings.md`](settings.md) for the
full API reference.

### Q. Where is memory stored?

**Per-project, always.** The source of truth is the JSON file at
`<project>/.daedalus/daedalus[-<agent_name>]/memory/memory.json`.
`SessionMemory` also writes a best-effort SQLite mirror to
`<project>/.daedalus/daedalus[-<agent_name>]/memory/daedalus.db` under
`mem.<session_id>` keys, which is used as a fallback if the JSON file
is corrupt or missing.

**Never** in XDG. See `tests/test_two_tier_rule.cpp` for the test that
enforces this.

### Q. How do I run two agents on the same project without their memory mixing?

Use `--agent-name` (CLI) or `cfg.agent_name` (API). Each name gets its own
subdir under `<project_root>/`. See `daedalus-manager` for the canonical
3-agent composition.

### Q. How do I point daedalus at a single root for CI / sandboxing?

```bash
DAEDALUS_HOME=/tmp/daedalus-test daedalus-cli /path/to/project
```

`$DAEDALUS_HOME` short-circuits all four XDG vars. The whole user tier
collapses to `$DAEDALUS_HOME/{config,data,state,cache}/`.

### Q. How do I programmatically override the Paths singleton (e.g. in tests)?

```cpp
#include <daedalus/paths.h>

class MyTestPaths : public daedalus::Paths { /* ... */ };
daedalus::Paths::set_override(std::make_shared<MyTestPaths>());
// All Agent instances now use MyTestPaths.
// Reset at end of test:
daedalus::Paths::set_override(nullptr);
```

---

## 6. References

- `include/daedalus/paths.h` — the `Paths` interface.
- `include/daedalus/project_paths.h` — the `ProjectPaths` class.
- [`settings.md`](settings.md) — complete SettingsStore API reference.
- `tests/test_xdg_paths.cpp` — env-var resolution tests.
- `tests/test_project_paths.cpp` — per-agent namespace tests.
- `tests/test_two_tier_rule.cpp` — XDG-vs-project enforcement test.
- `tests/test_migrate_data.cpp` — `--migrate-data` tests.
- `tests/test_ensure_directories.cpp` — XDG-permissions test.
- `demos/daedalus-cli/migrate.cpp` — migration CLI implementation.

*Last updated 2026-07-22 (docs cleanup: memory paths corrected).*
