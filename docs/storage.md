# Daedalus Storage Layout

> **Source of truth for the library's filesystem conventions, env-var
> resolution, and migration recipes.** If anything in `Readme.md` or
> other docs conflicts with this file, this file wins.
>
> *Last updated 2026-07-15.*

---

## TL;DR — the two-tier rule

daedalus stores data in **exactly two tiers**, and the boundary is non-negotiable:

> **If the data is about the USER, it goes in XDG.**
> **If the data is about THE PROJECT, it lives under
> `<project_root>/.daedalus[-<agent_name>]/`.**

| Tier | Location | Examples |
|---|---|---|
| **User** (per user, shared across every project) | `$XDG_*_HOME/daedalus/...` | LLM API keys, MCP server registry, global skill packs, logs, cache |
| **Project** (per project, may have many per `agent_name`) | `<project_root>/.daedalus[-<agent_name>]/...` | Long-term memory, session transcripts, scratch, per-project overrides |

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

For each of the seven user-tier roots, the same algorithm applies:
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
| `daedalus.db` (settings store) | `<data_root>/daedalus.db` | **v0.4.0+**: SQLite-backed settings. Replaces `config.json`, `mcp/servers.json`, `mcp/.env`. See [`settings.md`](settings.md). |
| `config.json` (LLM keys + model list) | `<config_root>/config.json` | **Legacy**: read as fallback during v0.4.0 migration. Migrated to `daedalus.db` on first run. |
| `mcp/servers.json` (MCP registry) | `<config_root>/mcp/servers.json` | **Legacy**: read as fallback during v0.4.0 migration. |
| `mcp/.env` (MCP secrets) | `<config_root>/mcp/.env` | **Legacy**: read as fallback during v0.4.0 migration. |
| Globally-installed skill packs | `<data_root>/skills/installed/` | Unchanged. |
| Library logs | `<state_root>/logs/daedalus.log` | Unchanged. |
| Conversation history (v0.3.5+) | `daedalus.db` under `msg.<session_id>` keys | SQLite, not a JSON file. |
| Session memory (v0.3.5+) | `daedalus.db` under `mem.<session_id>` keys | SQLite, not a JSON file. |
| Response replay cache (future) | `<cache_root>/responses/` | Future. |
| Unix socket (future daemon mode) | `$XDG_RUNTIME_DIR/daedalus/agent.sock` | Future. |

`<config_root>` defaults to `$XDG_CONFIG_HOME/daedalus` or `~/.config/daedalus`.
`<data_root>` defaults to `$XDG_DATA_HOME/daedalus` or `~/.local/share/daedalus`.
And so on.

> **Migration note:** Original JSON files are renamed to `*.migrated` after
> successful import into `daedalus.db`. You can delete them after verifying
> the new layout works. See [`settings.md`](settings.md) §4.

### 1.3 The legacy fallback

Per the legacy-readback policy below, the legacy `~/.daedalus/` layout
is **honoured as a read-fallback for one release cycle**. Specifically:

- If `$DAEDALUS_HOME` AND `$XDG_DATA_HOME` are both unset, AND
  `$HOME/.daedalus/` exists on disk,
- then `user_root()` returns `$HOME/.daedalus/` (legacy wins).

When this happens, `Agent::initialize()` emits a one-shot `[DEPRECATION]`
warning via the sink (not to stderr — the no-stdio rule applies to the
library; the warning goes through `AgentSink::on_log`). The warning tells
the user to run `daedalus-cli --migrate-data` to move to XDG.

`Agent::initialize()` also calls `Paths::ensure_directories()` which
auto-creates the four XDG roots with the secure permissions
(`0750` for config/data, `0700` for state/cache). Hosts can call it
explicitly earlier if they need to:

```cpp
#include <daedalus/paths.h>

daedalus::Paths::global().ensure_directories();
// The four XDG roots now exist with correct permissions.
```

`ensure_directories()` is idempotent — safe to call multiple times — and
silently no-ops on filesystems that don't support `chmod` (e.g. FAT32
mounts).

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

- Empty string: `<project_root>/.daedalus/` (default, backward-compat).
- Non-empty: `<project_root>/.daedalus-<name>/`.

Names are validated by `is_valid_agent_name()`:
- Match `^[A-Za-z0-9_.-]{1,64}$`.
- Must not start with `.` (hidden) or `-` (CLI flag).
- Empty string is permitted (means "no namespace").

### 2.2 Project-tier files

Everything below lives under `agent_dir()` — the namespaced subdir. Two
`Agent` instances in the same project with different `agent_name` values
get different `agent_dir()` paths, so:

| Path | Purpose | Safe to delete between sessions? |
|---|---|---|
| `agent_dir/memory/memory.sqlite` | Long-term facts (THE cardinal file) | No — losing this loses project memory |
| `agent_dir/memory/memory.json` | Legacy memory format | No |
| `agent_dir/sessions/<sid>/messages.sqlite` | Conversation transcripts per session | Yes (rebuilds from the agent loop) |
| `agent_dir/sessions/<sid>/scratch.json` | Per-session scratch state | Yes |
| `agent_dir/cache/` | Project-specific regenerable caches | Yes |
| `agent_dir/config.json` | Per-project overrides of global settings | No — losing this drops overrides |
| `agent_dir/skills/active/` | Project-local skill overrides | No — losing this drops overrides |
| `agent_dir/agents/<sub_name>/` | Sub-agent state (for `daedalus-manager` / `daedalus-subagent`) | Depends on sub-agent |
| `agent_dir/.lock` | Lock file (always present, prevents concurrent writes) | No — recreated on next write |

### 2.3 Legacy read-fallback (Phase 12 rename)

For one release cycle, the pre-rename `.codemax[-<name>]/` paths are honoured
as a read-fallback. NEW writes always go to the new `.daedalus*` location.
Migration is a manual `mv` (or `daedalus-cli --migrate-data` if you want the
library to do it).

---

## 3. Migration recipes

### 3.1 Laptop / desktop user — `~/.daedalus/` → XDG

If you've been running daedalus for a while and have a populated
`~/.daedalus/` directory:

```bash
# 1. See what will migrate.
daedalus-cli --migrate-data

# 2. Apply (copies, doesn't remove).
daedalus-cli --migrate-data-apply

# 3. Verify the new layout works (run the agent normally).
daedalus-cli /path/to/your/project

# 4. When satisfied, manually remove the legacy dir.
rm -rf ~/.daedalus
```

You can override the destinations:

```bash
XDG_CONFIG_HOME=/custom/cfg XDG_DATA_HOME=/custom/data \
    daedalus-cli --migrate-data-apply
```

### 3.2 Setting per-project agent_name

```bash
daedalus-cli --agent-name planner /path/to/project
# → /path/to/project/.daedalus-planner/memory/...
```

Multiple agents on the same project, each with their own memory:

```bash
daedalus-cli --agent-name planner  /path/to/project   # in one terminal
daedalus-cli --agent-name actor    /path/to/project   # in another
daedalus-cli --agent-name verifier /path/to/project   # in a third
```

(That's exactly what `daedalus-manager` does internally.)

---

## 4. Deployment shapes

### 4.1 Local user install (no env vars)

Defaults to `~/.config/daedalus/`, `~/.local/share/daedalus/`, etc. The
legacy `~/.daedalus/` fallback kicks in if the XDG dirs don't exist yet —
one-time migration prompt via the deprecation warning.

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

systemd's `ConfigurationDirectory=` / `StateDirectory=` directives
auto-create the dirs at `/etc/daedalus/`, `/var/lib/daedalus/`, etc.,
with correct ownership for the daemon user.

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

Or with the minimum-effort bind-mount:

```bash
podman run --rm -it \
    --userns=keep-id \
    -v $HOME/.daedalus:/home/me/.daedalus:rw \
    -w /work -v /path/to/your/project:/work \
    localhost/daedalus:latest
```

(The second form skips XDG entirely — `$HOME/.daedalus/` is still honoured
as the legacy fallback for one release cycle.)

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

These sandboxes rewrite `$HOME`. Set the XDG vars explicitly to canonical
locations the sandbox maps back to the real host:

```bash
XDG_CONFIG_HOME=/home/user/.config \
XDG_DATA_HOME=/home/user/.local/share \
XDG_STATE_HOME=/home/user/.local/state \
XDG_CACHE_HOME=/home/user/.cache \
daedalus-cli /path/to/project
```

In a Nix flake, wrap the binary with `wrapProgram` to bake these in.

---

## 5. FAQ

### Q. Where do my LLM API keys live?

`$XDG_DATA_HOME/daedalus/daedalus.db` (or `~/.local/share/daedalus/daedalus.db`
by default). The `SettingsStore` API in `include/daedalus/settings.h` provides
type-safe access — no raw JSON. See [`settings.md`](settings.md) for the
full API reference.

### Q. Where is memory stored?

**Per-project, always.** Two storage modes:

1. **v0.4.0+** (default): Session memory stored in `daedalus.db` under
   `mem.<session_id>` keys. The `SessionMemory` class falls back to the
   JSON file at `<project>/.daedalus[-<agent_name>]/memory/memory.json` if
   the DB key is empty.

2. **Legacy**: `<project_root>/.daedalus[-<agent_name>]/memory/memory.json`.
   Read as fallback during v0.4.0 migration.

**Never** in XDG. See `tests/test_two_tier_rule.cpp` for the test that
enforces this. See [`settings.md`](settings.md) §3 for the DB schema.

### Q. How do I run two agents on the same project without their memory mixing?

Use `--agent-name` (CLI) or `cfg.agent_name` (API). Each name gets its own
subdir under `<project_root>/`. See `daedalus-manager` for the canonical
3-agent composition.

### Q. I have an old `~/.codemax/` from before the rename — what happens?

The library reads from `.codemax[-<name>]/` as a fallback if `.daedalus[-<name>]/`
doesn't exist (Phase 12 backward-compat). NEW writes go to the new
`.daedalus*` path. To migrate manually: `mv ~/.codemax ~/.daedalus` (or
`mv <project>/.codemax <project>/.daedalus`). The `--migrate-data` CLI
handles only the user tier; project-tier rename is a manual `mv`.

### Q. Why is `~/.daedalus/` deprecated?

It violates the [XDG Base Directory Specification 0.8](https://specifications.freedesktop.org/basedir-spec/basedir-spec-latest.html)
— every modern distro (Fedora, Debian, Ubuntu, Arch, openSUSE) enforces this
in their packaging guidelines. It forces `--userns=keep-id` bind-mounts in
containers; XDG-compliant tools work with named volumes. It conflates config,
state, data, and cache. Distribution packagers will write autoload migration
scripts per distro if we don't fix it ourselves.

### Q. Why does my agent emit a `[DEPRECATION]` warning?

Because `~/.daedalus/` exists on your system and you haven't set `$DAEDALUS_HOME`
or any `$XDG_*` var. Run `daedalus-cli --migrate-data-apply` to copy your
state to the new XDG-compliant locations, then `rm -rf ~/.daedalus` once
you've verified the new layout works.

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
- `tests/test_legacy_deprecation_warning.cpp` — deprecation warning tests.
- `tests/test_migrate_data.cpp` — `--migrate-data` tests.
- `tests/test_ensure_directories.cpp` — XDG-permissions test.
- `demos/daedalus-cli/migrate.cpp` — migration CLI implementation.

*Last updated 2026-07-15.*