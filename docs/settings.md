# Daedalus Settings System

> Complete reference for the `daedalus::SettingsStore` API (v0.4.0+) and
> the migration path from the legacy JSON-based `SettingsManager`.

*Last updated 2026-07-20.*

---

## 1. Overview

daedalus v0.4.0 introduced **`daedalus::SettingsStore`**: a SQLite-backed
settings store that replaces the scattered JSON files (`config.json`,
`mcp/servers.json`, `mcp/.env`) with a single database.

**Key benefits:**
- **Atomic transactions** — model writes are all-or-nothing
- **Type-safe API** — no raw JSON parsing in host code
- **Thread-safe** — SQLite opened in serialized mode
- **Per-agent isolation** — `SettingsStore::for_home()` returns an isolated DB
- **Auto-migration** — existing JSON config is imported on first run

**Storage location:**
```
$XDG_DATA_HOME/daedalus/daedalus.db
# e.g. ~/.local/share/daedalus/daedalus.db
```

---

## 2. Public API

### 2.1 Header

```cpp
#include <daedalus/settings.h>
```

### 2.2 Getting a SettingsStore

```cpp
namespace daedalus {

// Global default (one DB per user)
SettingsStore& store = SettingsStore::instance();

// Per-agent isolated store (lives at <home>/daedalus.db)
SettingsStore& agent_store = SettingsStore::for_home("/path/to/agent/home");
```

Both return a reference to a singleton-per-path. The underlying SQLite
handle is managed by a connection pool (`DbPool`) — multiple
`SettingsStore` wrappers can share the same handle without extra overhead.

### 2.3 Provider and Model Configuration

As of v0.6.0/v0.7.0, endpoint and credential information lives in a
`ProviderConfig`, and each `ModelConfig` references a provider by name.
This lets multiple models share one provider's host, API key, and
endpoints.

```cpp
// Shared endpoint/credential configuration for one provider
struct ProviderConfig {
    std::string name;               // provider label, e.g. "minimax"
    std::string host;               // https://api.example.com
    std::string api_key;            // provider-level API key
    std::string chat_endpoint;      // default "/v1/chat/completions"
    std::string discovery_endpoint; // default "/v1/models"
    int         timeout_seconds = 30;
    std::string plugin;             // provider plugin name; empty = auto-detect
                                    // from name/host (see docs/provider-plugins.md)
};

// One configured model; references a ProviderConfig for host/key/endpoints
struct ModelConfig {
    std::string name;              // settings key, e.g. "minimax-m3"
    std::string provider;          // ProviderConfig::name
    std::string model_id;          // actual model id, e.g. "MiniMax-M3"
    int    max_context_size = 0;   // 0 = use built-in default (1M fallback)
    double temperature     = 0.0;
};
```

#### Provider CRUD

```cpp
// Create or update a provider
store.upsert_provider(ProviderConfig{
    .name               = "minimax",
    .host               = "https://api.minimax.chat",
    .api_key            = "YOUR_API_KEY",
    .chat_endpoint      = "/v1/chat/completions",
    .discovery_endpoint = "/v1/models",
    .timeout_seconds    = 30,
    // .plugin          = "minimax",  // optional; auto-detected from
                                     // name/host when left empty
});

// Read
std::optional<ProviderConfig> p = store.get_provider("minimax");
std::vector<ProviderConfig> providers = store.list_providers_detailed();

// Delete
store.delete_provider("minimax");
```

#### Model CRUD

```cpp
// Create or update a model that uses the provider above
store.upsert_model(ModelConfig{
    .name      = "minimax-m3",
    .provider  = "minimax",
    .model_id  = "MiniMax-M3",
    .temperature = 0.7
});

// Read
std::optional<ModelConfig> m = store.get_model("minimax-m3");
std::vector<ModelConfig>   all = store.list_models();

// Default model
std::string default_name = store.get_default_model_name();
store.set_default_model_name("minimax-m3");

// Delete
store.delete_model("old-model");
```

#### Model Discovery

Query a provider's OpenAI-compatible `/v1/models` endpoint to discover
available models. The model-based overload looks up the configured model,
resolves its provider, and uses the provider's host/key/discovery endpoint:

```cpp
#include <daedalus/Agent.h>

auto result = daedalus::Agent::discover_models("minimax-m3");
if (result.success) {
    for (const auto& m : result.models) {
        std::cout << m.id
                  << " owned_by=" << m.owned_by
                  << " ctx=" << m.context_length
                  << " vision=" << m.supports_vision
                  << " reasoning=" << m.supports_reasoning
                  << "\n";
    }
} else {
    std::cerr << result.error_message << "\n";
}
```

For ad-hoc discovery without a stored model, use the lower-level overload
from `daedalus/model_discovery.h`:

```cpp
#include <daedalus/model_discovery.h>

auto result = daedalus::discover_models(
    "https://api.minimax.chat", "YOUR_API_KEY", "/v1/models");
```

### 2.4 Switching Models at Runtime

Use `Agent::switch_model()` to change the model for subsequent turns.
On success the new model becomes the default in `SettingsStore`, the
`AgentConfig::model_name` is updated, the internal `LLMClient` is
reconfigured for the new endpoint / credentials / context size, and
**the conversation history is cleared**.

```cpp
daedalus::Agent agent(cfg, sink);
agent.initialize();

// Start with a lightweight model for planning...
agent.switch_model("minimax-m2.7");
agent.submit_user_message("Plan the refactor");
agent.run_one_turn();

// ...then switch to a larger model for implementation.
// History is cleared automatically so the bigger model's context
// window is not consumed by the earlier planning turn.
agent.switch_model("minimax-m3");
agent.submit_user_message("Implement the plan");
agent.run_one_turn();
```

**Behaviour:**

| Step | Effect |
|---|---|
| Validation | Returns `false` immediately if the requested model is not in `SettingsStore`. |
| Default update | `SettingsStore::set_default_model_name()` is updated so future sessions start with the new model. |
| Config refresh | The `LLMClient` resolves the model's `provider`, then picks up the provider's `host`, `api_key`, `chat_endpoint`, the model's `model_id`, `temperature`, and `max_context_size`. |
| History cleared | `Messages::clear()` + `save()` removes the on-disk transcript so the new model starts fresh. |

**Rationale:** different models have different context windows and may
also differ in API endpoint or provider. Clearing history avoids a
`context_length_exceeded` error when moving from a large-context model
to a smaller one, and guarantees the new model's first turn is not
polluted by tool-call pairs generated for a different model's schema.

### 2.5 Key-Value Settings

```cpp
// String
std::string val = store.get_string("my_key", "default");
store.set_string("my_key", "value");

// Boolean
bool flag = store.get_bool("auto_approve", false);
store.set_auto_approve(true);

// Integer
int count = store.get_int("max_retries", 3);
store.set_int("max_retries", 5);
```

### 2.6 Agent Settings

```cpp
// Max iterations (default 100)
int max_iter = store.get_max_agent_iterations();

// Tool result size limit (default 10,000 bytes)
size_t tool_result_size = store.get_max_tool_result_size();

// Request/response size limits (defaults 4MB / 5MB)
size_t max_req = store.get_max_request_size();
size_t max_res = store.get_max_response_size();

// Auto-approve
bool auto_approve = store.get_auto_approve();
```

### 2.7 MCP Tool Schema Soft Cap

The MCP tool schema size is monitored against the model's context window:

```cpp
// Default 5% — warns if MCP tools exceed this fraction of context
int cap_pct = store.get_mcp_tool_schema_soft_cap_percent();
```

### 2.8 Server Error Settings

```cpp
struct ServerErrorSettings {
    bool enabled = true;
    int initial_wait_seconds = 30;
    int max_wait_seconds = 300;
    double backoff_multiplier = 2.0;
    int max_retries_per_call = 10;
};

ServerErrorSettings ses = store.get_server_error_settings();
```

### 2.9 Model Context Size Resolution

```cpp
int ctx = store.get_max_context_size_for_model("minimax-m3");
// Returns (in priority order):
//   1. User's explicit setting from models table (if non-zero)
//   2. Built-in default for known models (MiniMax-M3 → 500K, M2.7 → 200K, etc.)
//   3. 1,000,000 (ultimate fallback)
```

Built-in defaults:

| Model | Context Size |
|-------|-------------|
| MiniMax-M3 / minimax-m3 | 500,000 |
| MiniMax-M2.7 / minimax-m2.7 | 200,000 |
| GPT-4o | 128,000 |
| Claude 3.5 Sonnet | 200,000 |
| DeepSeek Chat | 128,000 |
| (unknown) | 1,000,000 |

### 2.10 Atomic Transactions

```cpp
store.transaction([](SettingsStore& s) {
    s.upsert_model(model1);
    s.upsert_model(model2);
    s.set_default_model_name("minimax-m3");
    // All committed together, or all rolled back on any exception.
});
```

### 2.11 Introspection

```cpp
std::filesystem::path path = store.db_path();  // e.g. ~/.local/share/daedalus/daedalus.db
bool has_data = store.has_data();              // true if models or kv_settings exist
```

---

## 3. Per-Project Persistence (v0.3.5+)

`SettingsStore` also stores per-project session state (session memory and
messages) under namespaced keys:

```cpp
// Session memory
std::string mem_json = store.get_session_memory(session_id);
store.set_session_memory(session_id, json_blob);
store.delete_session_memory(session_id);

// Conversation messages
std::string msg_json = store.get_messages_json(session_id);
store.set_messages_json(session_id, json_blob);
store.delete_messages_json(session_id);
```

Keys are prefixed `mem.<session_id>` and `msg.<session_id>` in the
`kv_settings` table.

---

## 4. Migration from JSON

Run the CLI migrator once to move data from legacy JSON files to the
SQLite store:

```bash
daedalus-cli --migrate-data
```

The migrator (implemented in `src/tools/migrate.cpp`):
1. Reads `config.json`, `mcp/servers.json`, `mcp/.env`
2. Writes rows to `daedalus.db`
3. Renames originals to `*.migrated`

After migration, all subsequent runs read exclusively from SQLite. No
auto-migration happens at runtime — if the DB is empty, daedalus starts
with an empty config rather than trying to read JSON again.

Placeholder API keys (`YOUR_API_KEY_HERE`) are skipped by the migrator.
## 5. Legacy SettingsManager (Deprecated)

`SettingsManager` (in `src/core/SettingsManager.h`) is a **deprecated shim**
that delegates to `SettingsStore`. It exists only for backward compatibility
with code not yet updated to the new API.

**Migration guide for internal code:**

| Old (SettingsManager) | New (SettingsStore) |
|----------------------|---------------------|
| `sm->get_model(name)` | `store->get_model(name)` |
| `sm->get_models()` | `store->list_models()` |
| `sm->get_default_model_name()` | `store->get_default_model_name()` |
| `sm->set_default_model(name)` | `store->set_default_model_name(name)` |
| `sm->get_auto_approve()` | `store->get_auto_approve()` |
| `sm->is_loaded()` | *(removed — SQLite is always ready)* |
| `sm->load()` | *(removed — DB is auto-opened)* |
| `sm->save()` | *(removed — writes are immediate)* |

**Deletion timeline:** `SettingsManager` will be removed in v0.6.0.

---

## 6. Schema

The database schema is created idempotently on first open:

```sql
CREATE TABLE schema_version (version INTEGER PRIMARY KEY);
CREATE TABLE models (
    name             TEXT PRIMARY KEY,
    host             TEXT NOT NULL,
    api_key          TEXT NOT NULL,
    model_id         TEXT NOT NULL,
    chat_endpoint    TEXT NOT NULL DEFAULT '/v1/chat/completions',
    max_context_size INTEGER NOT NULL DEFAULT 0,
    temperature      REAL    NOT NULL DEFAULT 0.0
);
CREATE TABLE kv_settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE mcp_servers (
    name      TEXT PRIMARY KEY,
    transport TEXT NOT NULL,
    command   TEXT,
    args_json TEXT,
    env_json  TEXT,
    url       TEXT
);
CREATE TABLE serper_config (id INTEGER PRIMARY KEY CHECK (id = 1), api_key TEXT NOT NULL);
```

---

## 7. Thread Safety

All `SettingsStore` methods are safe to call from multiple threads. The
underlying SQLite handle is opened in **serialized mode** (SQLite's
default thread-safe mode). The `DbPool` uses a mutex to ensure one handle
per unique `db_path`.

---

## v0.5.0 — MCP server registry in SettingsStore

The `mcp_servers` SQLite table replaces `~/.config/daedalus/mcp/servers.json`:

```cpp
#include <daedalus/settings.h>

auto& store = daedalus::SettingsStore::instance();
for (const auto& s : store.list_mcp_servers()) {
    std::cout << s.name << " (" << s.transport << ")\n";
}
```

### Schema v2

The schema was bumped to v2 to add columns for full parity with `McpServerConfig`:

```sql
CREATE TABLE mcp_servers (
    name         TEXT PRIMARY KEY,
    transport    TEXT NOT NULL DEFAULT 'stdio',
    command      TEXT,
    args_json    TEXT,
    env_json     TEXT,
    url          TEXT,
    bearer_token TEXT,
    description  TEXT,
    enabled      INTEGER NOT NULL DEFAULT 1
);
```

Pre-v2 databases get an in-place ALTER TABLE migration on first open.

### Public API

```cpp
struct McpServer {
    std::string name;
    std::string transport;        // stdio | streamable-http
    std::string command;
    std::vector<std::string> args;
    std::map<std::string, std::string> env;
    std::string url;
    std::string bearer_token;
    std::string description;
    bool enabled = true;
};

std::optional<McpServer> get_mcp_server(std::string_view name) const;
std::vector<McpServer>   list_mcp_servers() const;
void upsert_mcp_server(const McpServer& s);
void delete_mcp_server(std::string_view name);
bool has_mcp_servers() const noexcept;
```

### McpServerManager integration

`mcp::McpServerManager::load_from_store(store)` ingests McpServer rows
into the same `configs_` map that `load_config(servers.json)` populates.
Both APIs are interchangeable.

---

## 8. File Locations Summary

---

## 9. References

- `include/daedalus/settings.h` — public SettingsStore API
- `src/core/SettingsStore.cpp` — implementation
- `src/tools/migrate.cpp` — CLI JSON→SQLite migration tool
- `tests/test_settings_store.cpp` — unit tests
- `tests/test_settings_store_e2e.cpp` — end-to-end wire test

*Last updated 2026-07-18 (v0.5.2: removed auto-migration, removed SettingsMigrator).*
