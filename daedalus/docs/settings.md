# Daedalus Settings System

`daedalus::SettingsStore` is the public SQLite-backed settings API. It stores
providers, models, MCP servers, web-search providers, agent limits, and several
internal indexes/mirrors. Project transcripts and runtime files still belong in
the per-project agent directory; see [`storage.md`](storage.md).

The old `SettingsManager` and automatic JSON-to-SQLite migration have been
removed. `servers.json` remains an explicit MCP fallback, not a general settings
format.

## Database location and lifetime

```text
$XDG_DATA_HOME/daedalus/daedalus.db
# normally ~/.local/share/daedalus/daedalus.db
```

```cpp
#include <daedalus/settings.h>

// Process-wide default store.
daedalus::SettingsStore& global = daedalus::SettingsStore::instance();

// Lightweight handle for <home>/daedalus.db (returned by value).
auto isolated = daedalus::SettingsStore::for_home("/tmp/agent-home");
```

Handles for the same path share a pooled SQLite connection. Methods are safe to
call from multiple threads. Writes are immediate; there is no `load()`/`save()`
cycle. `db_path()` reports the selected path and `has_data()` reports whether
user configuration is present.

Connections use SQLite serialized mode, a five-second busy timeout, and WAL.
`daedalus.db-wal` and `daedalus.db-shm` are part of the live database and must be
considered when copying or backing it up. Schema creation and migration are
transactional. A filesystem that cannot provide the required WAL behavior is
reported as an open error rather than silently treated as multi-process safe.

## Providers and models

Provider configuration owns shared connection details. A model references one
provider by name.

```cpp
struct ProviderConfig {
    std::string name;
    std::string host;
    std::string api_key;
    std::string chat_endpoint;
    std::string discovery_endpoint;
    int timeout_seconds = 30;
    std::string plugin;          // empty = auto-detect from name/host
};

struct ModelConfig {
    std::string name;            // settings key
    std::string provider;        // ProviderConfig::name
    std::string model_id;        // provider's actual model identifier
    int max_context_size = 0;    // 0 = model/plugin default
    double temperature = 0.0;
};
```

Example:

```cpp
auto& store = daedalus::SettingsStore::instance();
store.upsert_provider(daedalus::ProviderConfig{
    .name = "minimax",
    .host = "https://api.minimax.io",
    .api_key = "YOUR_API_KEY",
    .chat_endpoint = "/v1/chat/completions",
    .discovery_endpoint = "/v1/models",
    .timeout_seconds = 30,
    // .plugin = "minimax",     // optional
});
store.upsert_model(daedalus::ModelConfig{
    .name = "minimax-m3",
    .provider = "minimax",
    .model_id = "MiniMax-M3",
    .max_context_size = 200000,
    .temperature = 0.0,
});
store.set_default_model_name("minimax-m3");
```

Public operations:

| Operation | API |
|---|---|
| Read one provider/model | `get_provider(name)`, `get_model(name)` |
| List | `list_providers()`, `list_providers_detailed()`, `list_models()` |
| Merge model with provider | `resolve_model(name)` |
| Insert/update | `upsert_provider(p)`, `upsert_model(m)` |
| Delete | `delete_provider(name)`, `delete_model(name)` |
| Default/current selection | `get/set_default_model_name()`, `get/set_current_provider()` |

Deleting a provider also removes its models. `ResolvedModelConfig` contains the
merged host, credentials, endpoints, timeout, model values, and plugin hint.
Provider plugin selection is documented in
[`provider-plugins.md`](provider-plugins.md).

### Discovery

```cpp
#include <daedalus/Agent.h>

auto result = daedalus::Agent::discover_models("minimax-m3");
if (result.success) {
    for (const auto& model : result.models) {
        // model.id, context_length, supports_vision, supports_reasoning, ...
    }
} else {
    // result.error_message
}
```

The model-based overload resolves the provider's host, key, and discovery
endpoint. Lower-level ad-hoc discovery is available from
`daedalus/model_discovery.h`:

```cpp
auto result = daedalus::discover_models(
    "https://api.example.com", "API_KEY", "/v1/models");
```

### Runtime model/provider changes

`Agent::switch_model(name)` validates the stored model, makes it the default,
reconfigures the LLM client, and **clears conversation history**. This avoids
carrying incompatible tool/schema state or an oversized context into the new
model. `Agent::switch_provider(name)` persists the current provider and selects
a model belonging to it when necessary. `active_provider_plugin()` reports the
resolved plugin after the LLM client exists.

## Key/value and agent settings

Generic accessors are:

```cpp
store.get_string(key, fallback);
store.get_bool(key, fallback);
store.get_int(key, fallback);
store.set_string(key, value);
store.set_bool(key, value);
store.set_int(key, value);
```

Typed helpers cover the settings consumed by the agent:

| Setting | Default / behavior |
|---|---|
| `auto_approve` | `false`; `get_auto_approve()` / `set_auto_approve()` |
| Maximum agent iterations | 100 |
| Maximum tool result | 10,000 bytes |
| Maximum request | 4,000,000 bytes |
| Maximum response | 5,000,000 bytes |
| MCP schema soft cap | 5 percent |
| Model context size | Explicit model row, then known model default, then 1,000,000 |

The limit helpers are `get/set_max_agent_iterations`,
`get/set_max_tool_result_size`, `get/set_max_request_size`,
`get/set_max_response_size`, and `get/set_mcp_tool_schema_soft_cap_percent`.
Hosts can override several of these per agent in `AgentConfig`; those overrides
are not automatically written back to the store.

`ServerErrorSettings` controls retry behavior:

```cpp
struct ServerErrorSettings {
    bool enabled = true;
    int initial_wait_seconds = 30;
    int max_wait_seconds = 300;
    double backoff_multiplier = 2.0;
    int max_retries_per_call = 10;
};
```

Read it with `get_server_error_settings()`. Retry writes use the generic
key/value API and the keys consumed by `SettingsStore`/the settings UIs.

## Web-search providers

The `web_search_providers` table is the active registry for the unified
`web_search` tool. Each row has this public shape:

```cpp
struct WebSearchProviderSettings {
    std::string name;       // brave|tavily|exa|firecrawl|serper|serpapi|searxng|duckduckgo
    bool enabled = false;
    std::string api_key;
    std::string base_url;   // SearXNG instance URL
    std::string username;   // optional SearXNG Basic auth
    std::string password;
    int priority = 0;       // lower runs earlier
};
```

Read the ordered chain with `get_web_search_providers()` and update one row
with `set_web_search_provider(settings)`. The tool reads the chain per call, so
settings edits do not require an agent restart. Enabled providers are tried in
priority order until one succeeds; forcing `provider` in the tool call bypasses
the chain.

- Brave, Tavily, Exa, Firecrawl, Serper, and SerpApi use `api_key`.
- SearXNG uses `base_url` and optional Basic credentials. A bearer/API-key value
  may also be supplied where supported by the current provider adapter.
- DuckDuckGo needs no key and is enabled by default on a fresh store.

`serper_config` and `searxng_config`, along with
`get/set_serper_api_key()` and `get/set_searxng_settings()`, are legacy
compatibility surfaces. Schema migration copies their values into the unified
registry; `web_search` does not consult them at request time. New host code
should use only `WebSearchProviderSettings`.

## MCP servers

```cpp
using McpServer = daedalus::SettingsStore::McpServer;

store.upsert_mcp_server(McpServer{
    .name = "example",
    .transport = "stdio",        // or streamable-http
    .command = "uvx",
    .args = {"example-server"},
    .env = {{"TOKEN", "${TOKEN}"}},
    .description = "Example",
    .enabled = true,
});
```

Use `get_mcp_server`, `list_mcp_servers`, `upsert_mcp_server`,
`delete_mcp_server`, and `has_mcp_servers`. The struct also has `url` and
`bearer_token` for streamable HTTP. SQLite is authoritative over the legacy
JSON fallback. See [`mcp_server.md`](mcp_server.md).

## Atomic transactions

```cpp
store.transaction([](daedalus::SettingsStore& tx) {
    tx.set_string("key-a", "one");
    tx.set_int("key-b", 2);
    // Other store operations participate in the same transaction.
});
```

Use a transaction when related writes must succeed or fail together. Do not
perform blocking network or UI work inside the callback.

## Persistence mirrors and indexes

The header also exposes low-level storage used internally:

- `get/set/delete_session_memory()` and `get/set/delete_messages_json()` store
  JSON mirrors under `mem.<session_id>` and `msg.<session_id>`. They are not a
  replacement for the normal project `SessionMemory` and `Messages` lifecycle.
- `VaultRow` accessors move encrypted, base64-encoded rows only. Use
  `daedalus::SecretsVault` / the `vault` tool for plaintext operations; see
  [`vault.md`](vault.md).
- `BrainNoteRow` and search/index accessors maintain the rebuildable Second
  Brain metadata and FTS5 cache. Markdown under the user brain directory is the
  source of truth. Use `daedalus::SecondBrain` / the `brain` tool rather than
  editing index rows directly.

Brain metadata and FTS updates are transactional. Independent processes sharing
the brain also coordinate through its file lock; see [`storage.md`](storage.md).

## Schema and migration

The current schema version is **10**. Its major tables are:

| Table | Purpose |
|---|---|
| `providers` | Shared provider endpoint/credential configuration |
| `models` | Model key, provider reference, model id, context size, temperature |
| `kv_settings` | Scalar settings and internal JSON mirrors |
| `mcp_servers` | MCP stdio/HTTP server registry |
| `web_search_providers` | Unified web-search failover configuration |
| `vault_entries` | Encrypted vault payloads and public metadata |
| `brain_notes`, `brain_fts` | Rebuildable Second Brain index |
| `schema_version` | Migration marker |

Legacy `serper_config` and `searxng_config` definitions remain in the migration
path so older databases can be upgraded. Existing databases are migrated in
version order; new databases are seeded directly. The eight web-search rows are
also reseeded if the registry is empty.

There is **no automatic import of legacy general JSON settings**. The Icarus
migration command is the supported interactive migration path:

```bash
icarus --migrate-data         # preview
icarus --migrate-data-apply   # apply
```

It copies legacy data without deleting the originals. MCP `servers.json` is
separately retained as a read fallback when no same-named SQLite row exists.
Placeholder credentials are not a substitute for configuring a real provider.

## Source references

- Public API: `include/daedalus/settings.h`
- Provider/model types: `include/daedalus/types/ProviderConfig.h`
- Implementation/migrations: `src/core/SettingsStore.cpp`
- Store tests: `tests/test_settings_store*.cpp`,
  `tests/test_mcp_servers_store.cpp`, and web-search tests
- Storage paths: [`storage.md`](storage.md)
