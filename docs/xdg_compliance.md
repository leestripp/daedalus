# XDG Base Directory Specification Compliance

> This document describes how Daedalus follows the XDG Base Directory
> Specification for proper Linux/Unix integration.

*Last updated: 2026-07-18*

---

## 1. Overview

Daedalus follows the [XDG Base Directory Specification](https://specifications.freedesktop.org/basedir-spec/basedir-spec-latest.html)
to ensure proper integration with Linux/Unix systems and compatibility
with distribution packaging guidelines.

## 2. XDG Environment Variables

### Standard XDG Variables

| Variable | Default | Daedalus Usage |
|----------|---------|----------------|
| `$XDG_CONFIG_HOME` | `~/.config` | Config files (MCP servers, legacy settings) |
| `$XDG_DATA_HOME` | `~/.local/share` | Settings database, skills catalog |
| `$XDG_STATE_HOME` | `~/.local/state` | Logs, session history |
| `$XDG_CACHE_HOME` | `~/.cache` | Response cache (future) |
| `$XDG_DATA_DIRS` | `/usr/local/share:/usr/share` | System skill packs |
| `$XDG_CONFIG_DIRS` | `/etc/xdg` | System-wide config |

### Daedalus-Specific Overrides

| Variable | Purpose |
|----------|---------|
| `$DAEDALUS_HOME` | Single-root override (all user dirs under one parent) |
| `$DAEDALUS_CONFIG_HOME` | Override config root only |
| `$DAEDALUS_DATA_HOME` | Override data root only |
| `$DAEDALUS_STATE_HOME` | Override state root only |
| `$DAEDALUS_CACHE_HOME` | Override cache root only |
| `$DAEDALUS_SYSTEM_DATA_DIRS` | Override system data search path |
| `$DAEDALUS_SYSTEM_CONFIG_DIRS` | Override system config search path |

## 3. Resolution Priority

### User Root (shared state)

```
1. $DAEDALUS_HOME              (single-root override)
2. $XDG_DATA_HOME/daedalus   (XDG-compliant)
3. $HOME/.daedalus            (LEGACY - deprecated)
4. $HOME/.local/share/daedalus (XDG fallback)
```

### User Config Root

```
1. $DAEDALUS_CONFIG_HOME
2. $XDG_CONFIG_HOME/daedalus
3. $HOME/.config/daedalus
```

### User Data Root

```
1. $DAEDALUS_DATA_HOME
2. $XDG_DATA_HOME/daedalus
3. $HOME/.local/share/daedalus
```

### User State Root

```
1. $DAEDALUS_STATE_HOME
2. $XDG_STATE_HOME/daedalus
3. $HOME/.local/state/daedalus
```

### User Cache Root

```
1. $DAEDALUS_CACHE_HOME
2. $XDG_CACHE_HOME/daedalus
3. $HOME/.cache/daedalus
```

## 4. File Locations

### User Tier (XDG-Compliant)

```
$XDG_DATA_HOME/daedalus/              # ~/.local/share/daedalus/
├── daedalus.db                       # Settings store (models, MCP, kv)
└── skills/installed/                # Agent skills catalog

$XDG_CONFIG_HOME/daedalus/            # ~/.config/daedalus/
├── config.json                       # Legacy LLM config (migrated)
├── mcp/
│   ├── servers.json                  # MCP server configs
│   └── .env                         # API keys
└── settings.json                     # Legacy (deprecated)

$XDG_STATE_HOME/daedalus/            # ~/.local/state/daedalus/
└── logs/
    └── daedalus.log                  # Runtime logs

$XDG_CACHE_HOME/daedalus/            # ~/.cache/daedalus/
└── responses/                        # Response cache (future)
```

### Project Tier (Per-Project)

All project-tier state lives under a single hidden `.daedalus/` base:

```
<project>/.daedalus/                  # Base directory (hidden)
├── daedalus/                         # Default agent
│   ├── messages.json                 # Conversation transcript
│   ├── messages.json.crash          # Crash recovery
│   ├── memory/
│   │   ├── memory.sqlite            # Long-term facts
│   │   └── memory.json              # Legacy format
│   ├── sessions/<session_id>/
│   │   └── scratch.json
│   └── skills/active/                # Project-local skills
└── daedalus-<name>/                  # Named agent
    └── (same structure as above)
```

## 5. Two-Tier Rule

Daedalus enforces a strict two-tier storage model:

| Tier | Location | What Goes Here |
|------|----------|----------------|
| **User** | `$XDG_*_HOME/daedalus/` | Settings, models, MCP servers, skills, logs |
| **Project** | `<project>/.daedalus/` | Memory, messages, sessions |

**Critical:** Memory MUST never be stored in the user tier. This prevents
data leakage between agents working on the same project.

## 6. Directory Permissions

| Directory | Permissions | Reason |
|-----------|-------------|--------|
| `user_config_root()` | 0750 | Group-readable for shared daemon access |
| `user_data_root()` | 0750 | Group-readable for skill pack sharing |
| `user_state_root()` | 0700 | Private - logs may contain API key fragments |
| `user_cache_root()` | 0700 | Private - cache may contain PII |
| Project directories | 0755 | Standard project permissions |

## 7. Legacy Fallback

For users migrating from older versions:

1. If `$HOME/.daedalus/` exists and no XDG vars are set
2. Daedalus emits a deprecation warning via `AgentSink::on_log`
3. User should run `daedalus-cli --migrate-data-apply`
4. After verification, user removes `~/.daedalus`

## 8. Container/Sandbox Usage

### Rootless Podman

```bash
podman run --rm -it \
    --userns=keep-id \
    -e XDG_CONFIG_HOME=/home/user/.config \
    -e XDG_DATA_HOME=/home/user/.local/share \
    -e XDG_STATE_HOME=/home/user/.local/state \
    -e XDG_CACHE_HOME=/home/user/.cache \
    -v daedalus-config:/home/user/.config/daedalus \
    -v daedalus-data:/home/user/.local/share/daedalus \
    -v daedalus-state:/home/user/.local/state/daedalus \
    -v daedalus-cache:/home/user/.cache/daedalus \
    localhost/daedalus:latest
```

### Single-Root Override (CI/Sandboxing)

```bash
DAEDALUS_HOME=/tmp/daedalus-test daedalus-cli /path/to/project
```

## 9. Systemd Service

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

## 10. Verification

### Check Resolved Paths

```bash
# View resolved paths
XDG_DATA_HOME=~/.local/share \
XDG_CONFIG_HOME=~/.config \
XDG_STATE_HOME=~/.local/state \
XDG_CACHE_HOME=~/.cache \
./build/demos/daedalus-cli/daedalus-cli --version
```

### Check Storage Locations

```bash
# User tier
ls -la ~/.local/share/daedalus/
ls -la ~/.config/daedalus/
ls -la ~/.local/state/daedalus/

# Project tier
ls -la <project>/.daedalus/
```

## 11. References

- [XDG Base Directory Specification](https://specifications.freedesktop.org/basedir-spec/basedir-spec-latest.html)
- [Arch Wiki: XDG Base Directory](https://wiki.archlinux.org/title/XDG_Base_Directory)
- `docs/storage.md` - Detailed storage documentation
- `docs/settings.md` - SettingsStore API reference
- `src/library/paths.cpp` - Implementation
- `include/daedalus/paths.h` - Public interface
