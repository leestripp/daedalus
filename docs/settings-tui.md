# daedalus-settings-tui — Settings Editor TUI

> Terminal UI for editing the daedalus SQLite settings store
> (`daedalus::SettingsStore`). Aimed at developers building CLI hosts on
> `libdaedalus.a` who need to configure providers, models, MCP servers,
> and agent options without hand-editing SQLite.

*Last updated 2026-07-27.*

---

## 1. Overview

`daedalus-settings-tui` is a demo executable built with
[FTXUI](https://github.com/ArthurSonzogni/FTXUI) (fetched by CMake into
`build/_deps/`). It talks to the settings database **only** through the
public `<daedalus/settings.h>` API — it never constructs an `Agent` and
never touches the schema directly, so it stays in lock-step with the
library's own storage logic.

It edits the same store the agent uses at runtime:

```
$XDG_DATA_HOME/daedalus/daedalus.db
# e.g. ~/.local/share/daedalus/daedalus.db
```

All writes apply immediately (the store is write-through SQLite).

## 2. Build

```bash
cmake -S . -B build                 # DAEDALUS_DEMO_SETTINGS_TUI is ON by default
cmake --build build -j$(nproc)
```

Disable with `-DDAEDALUS_DEMO_SETTINGS_TUI=OFF` (also skipped entirely
when `DAEDALUS_BUILD_DEMOS=OFF`).

## 3. Usage

```bash
daedalus-settings-tui [--home <path>] [--dump] [--help]
```

| Flag | Effect |
|------|--------|
| *(none)* | Open the interactive TUI against the global user store. |
| `--home <path>` | Edit the per-agent store at `<path>/daedalus.db` (`SettingsStore::for_home`). |
| `--dump` | Print every section to stdout with secrets masked, then exit. Works without a TTY — useful for scripts and CI. |
| `--help` | Show usage. |

Environment overrides honoured by the store itself (`DAEDALUS_HOME`,
`XDG_DATA_HOME`, …) apply here too — see
[`storage.md`](storage.md).

## 4. Tabs

| Tab | What you can edit |
|-----|-------------------|
| **Providers** | `ProviderConfig` rows: name, host, api_key (masked input), chat/discovery endpoints, timeout. Add / edit / delete. |
| **Models** | `ModelConfig` rows: name, provider (dropdown of configured providers), model_id, max_context_size, temperature. Add / edit / delete, mark the **default model**, and **Discover** model ids from the provider's OpenAI-compatible `/v1/models` endpoint. |
| **MCP Servers** | `McpServer` rows: name, transport (`stdio` / `streamable-http`), command, args, env (`KEY=VALUE` lines), url, bearer_token (masked), description, enabled switch. |
| **Agent** | kv-backed agent options: auto_approve, current_provider, max_agent_iterations, max_tool_result_size, max_request_size, max_response_size, MCP schema soft-cap percent, Serper API key (masked), and the `server_error` retry group (enabled, initial/max wait, backoff multiplier, max retries). |

## 5. Key bindings

| Key | Action |
|-----|--------|
| `←` / `→` | Switch section (Providers / Models / MCP Servers / Agent) |
| `↑` / `↓` | Navigate lists and form fields |
| `a` | Add a new entry (entity tabs) |
| `e` | Edit the selected entry |
| `Enter` | Edit the selected entry; inside a form field, save the form |
| `d` | Delete the selected entry (asks for confirmation) |
| `s` | Set the selected model as default (Models tab) |
| `Esc` | Close the current dialog without saving |
| `Ctrl+C` | Quit |

## 6. `--dump` example

```
db: /home/user/.local/share/daedalus/daedalus.db

[providers]
  minimax
    host               = https://api.minimax.chat
    api_key            = sk-…7890
    ...

[models]
* minimax-m3            # '*' marks the default model
    provider         = minimax
    ...
```

Secrets (provider api_key, bearer tokens, Serper key, MCP env values)
are always masked as `<first 3>…<last 4>`.

## 7. Testing

A hermetic smoke test runs `--dump` against a throwaway
`XDG_DATA_HOME`:

```bash
ctest --test-dir build --output-on-failure -R settings_tui_dump
```

## 8. References

- [`settings.md`](settings.md) — the `SettingsStore` API this tool edits
- [`storage.md`](storage.md) — storage layout and env-var overrides
- `demos/daedalus-settings-tui/` — source
