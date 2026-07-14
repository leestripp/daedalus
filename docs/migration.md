# Migration: `codemax` → `daedalus`

This document is for users of the **old** `codemax` CLI binary. The
daedalus library is a drop-in replacement with five behavioural
differences you should know about.

---

## TL;DR

| What changed | Old `codemax` | New `daedalus` |
|---|---|---|
| Executable | `codemax-cli` | `daedalus-cli` |
| Config dir | `~/.daedalus/` | `~/.daedalus/` (auto-migrated on first run) |
| Env var | `DAEDALUS_HOME` | `DAEDALUS_HOME` (still reads `DAEDALUS_HOME` for back-compat) |
| Headers | `#include <codemax.h>` | `#include <daedalus.h>` |
| Namespace | `codemax::` | `daedalus::` |
| Mode flags | `--manager-mode`, `--sub-agent`, `--sub-agent-looping` | Removed; run `daedalus-manager` or `daedalus-subagent` instead |
| MCP servers dir | `~/.daedalus/mcp/` | `~/.daedalus/mcp/` |

Everything else (skills, memory format, model lookup in settings.json,
the REPL keybindings) is identical.

---

## 1. Config directory migration

The library auto-detects old `~/.daedalus/` and copies it to
`~/.daedalus/` on first run. Manual migration:

```bash
# If you have existing config you want to keep:
cp -r ~/.daedalus ~/.daedalus

# Verify the new path:
DAEDALUS_HOME=~/.daedalus daedalus-cli --version
```

After migration, you can safely delete the old directory:

```bash
rm -rf ~/.daedalus
```

The library does **not** delete `~/.daedalus/` itself — that's your
call. We keep the old name for one release cycle so any tooling
referencing it by path keeps working.

---

## 2. Environment variables

| Old | New | Notes |
|---|---|---|
| `DAEDALUS_HOME` | `DAEDALUS_HOME` | `DAEDALUS_HOME` is still read as a fallback during the migration window. |
| `DAEDALUSLIB_HOME` | `DAEDALUS_HOME` | This was the library-specific env var; now consolidated to `DAEDALUS_HOME`. |

In bash:

```bash
# Old:
export DAEDALUS_HOME=~/.config/codemax

# New:
export DAEDALUS_HOME=~/.config/daedalus
```

---

## 3. Removed CLI flags

The following flags existed in `codemax-cli` and have been removed:

| Flag | Replacement |
|---|---|
| `--manager-mode` | Run `demos/daedalus-manager/daedalus-manager` instead. The manager pattern is now a standalone demo, not a flag on the CLI. |
| `--sub-agent` | Run `demos/daedalus-subagent/daedalus-subagent` for the parent/child demo. |
| `--sub-agent-looping` | Same as `--sub-agent`; the looping variant was the only variant. |

If your shell script piped `--manager-mode` to a model, the upgrade
path is:

```bash
# Old:
codemax-cli --manager-mode --model minimax-m3 "decompose this"

# New:
demos/daedalus-manager/daedalus-manager \
    --models planner=minimax-m3,actor=minimax-m3,verifier=minimax-m3 \
    "decompose this"
```

(The exact CLI surface for the demos is documented in their respective
README / `--help`.)

---

## 4. Embedder API: `codemax::` → `daedalus::`

If you wrote a host against the static library, the rename is purely
mechanical:

```cpp
// Old:
#include <codemax.h>
codemax::Agent agent(std::move(cfg), sink);

// New:
#include <daedalus.h>
daedalus::Agent agent(std::move(cfg), sink);
```

Every public symbol moves namespace, every header moves directory:

| Old include | New include |
|---|---|
| `<codemax.h>` | `<daedalus.h>` |
| `<codemax/Agent.h>` | `<daedalus/Agent.h>` |
| `<codemax/types/AgentConfig.h>` | `<daedalus/types/AgentConfig.h>` |
| `<codemax/AgentSink.h>` | `<daedalus/AgentSink.h>` |

The library target is `daedalus::daedalus` in CMake:

```cmake
# Old:
find_package(codemax REQUIRED CONFIG)
target_link_libraries(my_host PRIVATE codemax::codemax)

# New:
find_package(daedalus REQUIRED CONFIG)
target_link_libraries(my_host PRIVATE daedalus::daedalus)
```

---

## 5. settings.json (unchanged)

The format of `settings.json` did not change. The library reads the
same JSON shape, in the same location (now `~/.daedalus/settings/`
instead of `~/.daedalus/settings/` — same file format, new directory).

If you have an existing `~/.daedalus/settings/settings.json`, copy it
verbatim into `~/.daedalus/settings/settings.json`:

```bash
mkdir -p ~/.daedalus/settings
cp ~/.daedalus/settings/settings.json ~/.daedalus/settings/settings.json
```

`~/.daedalus/mcp/servers.json` follows the same pattern.

---

## 6. Skills (unchanged)

`~/.daedalus/skills/*/SKILL.md` — same YAML frontmatter, same metadata
fields. If you had `~/.daedalus/skills/foo/SKILL.md`:

```bash
mkdir -p ~/.daedalus/skills
cp -r ~/.daedalus/skills/* ~/.daedalus/skills/
```

---

## 7. Conversation / memory (unchanged)

`<project>/.daedalus/messages.json` and `.daedalus/memory.json` are
**not** auto-migrated (they're project-local, not global). To move
them into the new namespace:

```bash
cd /path/to/your/project
mv .daedalus .daedalus
```

---

## 8. Where to get help

| Question | Where |
|---|---|
| "How do I use the library as a host?" | `docs/embedding.md` |
| "What events does the sink receive?" | `docs/embedding.md §The AgentSink Contract` |
| "How does the new prompt composition work?" | `docs/agent_system_prompts.md` |
| "What's the daemon-mode config story?" | `docs/session_memory.md` |
| "What's not yet ported?" | `CHANGELOG.md` (look for any open items) |

---

## 9. Rollback (if you need it)

The new `daedalus-cli` reads from `~/.daedalus/`, but if you still have
the old `codemax-cli` binary, it reads `~/.daedalus/` as it always did.
They are independent — switching binaries does not touch the other
binary's config dir. To roll back:

```bash
# Stop using daedalus-cli
# Re-install / rebuild the old codemax-cli
# Point DAEDALUS_HOME at the old config dir if needed
```

There's no shared state to corrupt; both versions can coexist on the
same machine during the transition period.
