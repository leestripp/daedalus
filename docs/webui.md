# daedalus-webui Demo

The `demos/daedalus-webui` example hosts one or more `daedalus::Agent`
instances behind an HTTP + SSE server and serves an embedded single-page
browser UI. It demonstrates a non-stdio host: all library output reaches
the browser through `AgentSink` callbacks broadcast as Server-Sent Events.

## Building

The webui demo is **not** built by default. Enable it at configure time:

```bash
cmake -S . -B build -DDAEDALUS_DEMO_WEBUI=ON
cmake --build build --target daedalus-webui -j$(nproc)
```

The executable is `build/demos/daedalus-webui/daedalus-webui`.

## Running

```bash
./build/demos/daedalus-webui/daedalus-webui [port] [project_dir]
```

| Positional | Default | Purpose |
|------------|---------|---------|
| `port` | `8080` (or `DAEDALUS_WEBUI_PORT`) | HTTP port |
| `project_dir` | current working directory | Project root passed to each `Agent` |

Common options:

```bash
# Use an isolated settings store
./build/demos/daedalus-webui/daedalus-webui 8080 . --home /tmp/daedalus-webui-home

# Auto-approve bash commands (use with caution)
./build/demos/daedalus-webui/daedalus-webui 8080 . --auto
```

Open `http://localhost:8080` in a browser.

## Browser UI

The page has three main areas:

- **Header** — current model, project directory, and a **Settings** button.
- **Chat log** — assistant deltas, tool calls/results, logs, and state changes.
- **Bash approval modal** — appears when the agent runs a command and
  `auto_approve` is false.

### Settings page

Click **⚙ Settings** to open the full-page overlay. Settings are persisted
in the same SQLite `daedalus.db` that the library reads.

| Tab | What you can do |
|-----|-----------------|
| **Models** | List configured models. Add/edit a model by selecting a **Provider** and filling in the model id, context size, and temperature. Discover models from a configured model or provider and add them with one click. |
| **Providers** | List configured providers. Add/edit a provider (name, host URL, API key, chat endpoint, discovery endpoint, timeout). |
| **General** | Default model, auto-approve bash, iteration/size limits, MCP schema soft cap. |
| **MCP Servers** | List, add, edit, and delete MCP servers (stdio or streamable-http). |

The **API key** and **bearer token** fields are masked on read; leaving them
blank when editing preserves the existing secret.

## REST Endpoints

### Sessions and chat

| Method | Path | Description |
|--------|------|-------------|
| `POST` | `/sessions` | Create a new agent session. Returns `{session_id, model}`. |
| `POST` | `/sessions/<id>/messages` | Submit a user message. Returns `{status:"queued"}`. |
| `POST` | `/sessions/<id>/cancel` | Cancel the current turn. |
| `POST` | `/sessions/<id>/approve` | Approve/deny a pending bash command. Body: `{approved: bool, sticky?: bool}`. |
| `GET`  | `/sessions/<id>/state` | Current `AgentState` as JSON. |
| `GET`  | `/sessions/<id>/stats` | `AgentStats` (tokens, iterations). |
| `GET`  | `/sessions/<id>/events` | SSE stream of agent events. |

### Settings

| Method | Path | Description |
|--------|------|-------------|
| `GET`  | `/settings/providers` | List configured providers. |
| `POST` | `/settings/providers` | Add/update a provider. |
| `DELETE` | `/settings/providers/<name>` | Delete a provider. |
| `GET`  | `/settings/providers/discover?name=<provider>` | Discover models from a provider's `/v1/models`. |
| `GET`  | `/settings/models` | List configured models and default model name. |
| `POST` | `/settings/models` | Add/update a model. |
| `DELETE` | `/settings/models/<name>` | Delete a model. |
| `GET`  | `/settings/models/discover?name=<model>` | Discover models via a configured model's provider. |
| `POST` | `/settings/models/default` | Set default model. Body: `{name}`. |
| `GET`  | `/settings/general` | Read general/agent settings. |
| `POST` | `/settings/general` | Update general settings. |
| `GET`  | `/settings/mcp` | List MCP servers. |
| `POST` | `/settings/mcp` | Add/update an MCP server. |
| `DELETE` | `/settings/mcp/<name>` | Delete an MCP server. |

### Metadata

| Method | Path | Description |
|--------|------|-------------|
| `GET`  | `/` | Embedded single-page HTML/JS/CSS. |
| `GET`  | `/healthz` | Liveness probe. |
| `GET`  | `/info` | Server metadata, including `cwd`. |

## SSE Event Stream

Each session has its own SSE stream at `/sessions/<id>/events`. Events are
plain text lines prefixed with `data: ` and separated by blank lines. The
browser reconnects automatically if the connection drops.

## See also

- [`webui_testing.md`](webui_testing.md) — Firefox BiDi end-to-end testing recipe.
- [`embedding.md`](embedding.md) — General embedding guide for `daedalus::Agent`.
- [`settings.md`](settings.md) — `SettingsStore` API reference.
