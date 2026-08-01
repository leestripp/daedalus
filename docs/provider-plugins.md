# Provider Plugins

daedalus talks to LLM providers through a **plugin system**. Each provider
(MiniMax, Moonshot AI/Kimi, LM Studio, generic OpenAI-compatible, or your
own) is described by a `ProviderPlugin` that encapsulates everything that
differs between providers, while `LLMClient` remains the single engine
(retry/backoff, compression triggers, size caps, token accounting, tool
loop).

Public headers:

- `include/daedalus/ProviderPlugin.h` — the plugin interface, stream-parser
  interface, and supporting types.
- `include/daedalus/ProviderRegistry.h` — the static registry used to
  register and resolve plugins.

## How a plugin is selected

Every provider row in the settings DB has a `plugin` column
(`ProviderConfig::plugin`):

- **Empty (the default)** — auto-detect: the registry first tries the
  provider *name* as a plugin name (case-insensitive, so provider
  `"MiniMax"` matches plugin `"minimax"`), then each plugin's
  `matches(host)` hook against the provider's base URL. Unmatched
  providers fall back to the `openai-compatible` default plugin.
- **Explicit** — must name a registered plugin; wins over auto-detection.

The active plugin for a running session is logged
(`[ProviderPlugin] active plugin: ...`) and exposed to hosts via
`Agent::active_provider_plugin()`.

## Built-in plugins

| Plugin | Matches | Key behavior |
|--------|---------|--------------|
| `openai-compatible` (default) | — (fallback) | `stream_options.include_usage` when streaming; classic SSE parser; classic error mapping. |
| `minimax` | name `minimax`, host containing `minimax` | Adds `reasoning_split: true` (§BUG-MINIMAX-105) and `stream_options.continuous_usage_stats`; static M3/M2.x context-size table. |
| `moonshot` | name `moonshot`, host containing `moonshot` or `kimi` | Forces `temperature = 1.0` for `kimi-k2.7-code` **and** the Kimi Code endpoint's `kimi-for-coding` IDs (the server rejects other values); sends `prompt_cache_key` from the agent name; static table for the pay-as-you-go platform (`api.moonshot.ai`/`api.moonshot.cn`: kimi-k3, k2.x, moonshot-v1) **and** the Kimi Code subscription endpoint (`api.kimi.com/coding/v1`: `k3`, `k3-256k`, `kimi-for-coding[-highspeed]`). |
| `lmstudio` | name `lmstudio`, loopback hosts (`localhost`, `127.0.0.1`) | `requires_auth() = false`; strips extension fields (`reasoning_split`, `prompt_cache_key`, `thinking`, `reasoning_effort`) so local servers get a plain OpenAI payload, **but** forces `stream_options.include_usage` when streaming — token accounting and auto-compression depend on the usage chunk; no static model table (discovery is the source of truth). |

## The hook interface

`ProviderPlugin` (all hooks have defaults; a minimal plugin overrides only
`name()` and `default_base_url()`):

| Hook | Default | Purpose |
|------|---------|---------|
| `name()` | — (pure) | Canonical plugin name. |
| `matches(host)` | `false` | Host-URL auto-detection. |
| `default_base_url()` | — (pure) | Base URL offered when adding the provider. |
| `chat_endpoint()` / `models_endpoint()` | `/v1/chat/completions`, `/v1/models` | Endpoint paths. |
| `requires_auth()` | `true` | `false` for key-less providers (e.g. local LM Studio). |
| `known_models()` | `{}` | Static model table; empty = discovery is the source of truth. |
| `context_size_for(model_id)` | `0` (unknown) | Context window lookup; consulted by `SettingsStore::get_max_context_size_for_model()` after the user-set value and before the legacy built-in map. |
| `mutate_chat_payload(payload, ctx)` | no-op | Shape the request before send (thinking switches, cache keys, stripping). `ctx` carries model id/name, provider, base URL, `cache_key` (agent name), stream flag, temperature, and the host-selected `thinking_level`. |
| `clamp_temperature(model_id, t)` | `t` | Force model-specific sampling constraints. |
| `sanitize_history(messages)` | no-op | Enforce conversation rules before send. |
| `create_stream_parser(sink)` | `nullptr` | SSE parser for streaming requests (see below). |
| `parse_models_response(body)` | `{}` | Parse the models-endpoint dialect for discovery. |
| `classify_api_error_type(type)` | `ApiError` | Map provider error-type strings to `ErrorKind`. |
| `supported_thinking_levels(model_id)` | `{}` | Thinking levels the model honors (subset of `None`/`Low`/`High`/`Max`); hosts use it to build per-model pickers. |

The shipped `OpenAiCompatiblePlugin` implements every hook with the
library's classic generic behavior and is the recommended base class for
custom plugins.

## Thinking levels (§ThinkingLevel, 2026-08)

Hosts select a `daedalus::ThinkingLevel` (`Default`/`None`/`Low`/`High`/
`Max`) via `AgentConfig::thinking_level`. `Default` sends nothing and
keeps the provider default. Each plugin maps supported levels to its
provider's parameter inside `mutate_chat_payload()` and silently ignores
unsupported ones — the library never sends a parameter the model would
reject (Kimi answers unknown/invalid fields with HTTP 400):

| Model | Wire parameter | Supported levels |
|-------|----------------|------------------|
| `kimi-k3`, `k3`, `k3-*` | `reasoning_effort` (platform: `api.moonshot.*`) / `thinking: {"type":"enabled","effort":...}` (Kimi Code: `api.kimi.com`) | `Low`, `High`, `Max` (default `Max`; `None` ignored — K3 always thinks) |
| `kimi-k2.5`, `kimi-k2.6` | `thinking: {"type":"disabled"}` | `None` only |
| `kimi-k2.7-code`, `kimi-for-coding*` | — (always thinks; `disabled` is a hard error) | none |
| `MiniMax-M3` | `thinking: {"type":"disabled"}` | `None` only |
| MiniMax M2.x | — (cannot disable thinking) | none |

`Agent::supported_thinking_levels()` returns the current model's row from
this table (empty when the model has no thinking control), so host UIs
can show only valid picker entries. `Agent::set_thinking_level()` switches
the level mid-session — the payload hook reads it per request, so the
change applies from the next turn with no agent rebuild. LM Studio's
plugin strips `thinking` and `reasoning_effort` since local servers
reject unknown fields.


## Streaming parsers

`ProviderStreamParser` reassembles one provider's SSE chunk dialect into
the standard non-streaming message shape the pipeline consumes
(`synthesized_message()`), plus `final_event()` (usage/finish_reason),
`accumulated_tool_calls()`, and `had_text_deltas()`.

The tree ships one parser, `MiniMaxSseParser` (internal), which handles
the plain OpenAI delta dialect plus whole-object tool-call chunks,
`audio_content`, usage-only trailer chunks, and `reasoning_content`
accumulation (Kimi requires `reasoning_content` to round-trip in history
during multi-step tool calling). All built-in plugins currently return it
from `create_stream_parser()`; plugins for a genuinely different wire
format supply their own implementation.

## Writing a custom plugin

```cpp
#include <daedalus/ProviderPlugin.h>
#include <daedalus/ProviderRegistry.h>

class MyProviderPlugin : public daedalus::ProviderPlugin {
public:
    std::string name() const override { return "my-provider"; }
    std::string default_base_url() const override { return "https://api.example.com"; }
    bool matches(std::string_view host) const override {
        return host.find("example.com") != std::string_view::npos;
    }
    int64_t context_size_for(std::string_view model_id) const override {
        if (model_id == "my-model") return 128000;
        return 0;
    }
    void mutate_chat_payload(nlohmann::json& payload,
                             const daedalus::ProviderChatContext& ctx) const override {
        if (ctx.stream) payload["stream_options"] = {{"include_usage", true}};
    }
};

// Register once at host startup (replaces any plugin with the same name):
daedalus::ProviderRegistry::register_plugin(std::make_shared<MyProviderPlugin>());
```

From then on, any provider row named `my-provider` or pointing at a
matching host uses the plugin's hooks; setting the row's `plugin` column
explicitly also works.

Escape hatch: if a provider can't fit the hooks at all (e.g. a non-OpenAI
wire format like Anthropic `/v1/messages`), its plugin overrides the
parser + payload hooks to translate both directions rather than forking
`LLMClient`.

## Context caching (§ContextCache, 2026-07)

All three built-in providers support **server-side prompt caching**, but
with different mechanics. The library's job is to (a) keep the request
prefix byte-stable and (b) report cache hits; no provider needs explicit
cache management for an agent loop.

| Provider | Mechanism | What the library sends | Hit reporting |
|----------|-----------|------------------------|---------------|
| MiniMax | **Automatic** prefix cache (≥ 512 input tokens; prefix order tools → system → messages; load-adjusted expiry). Cache reads bill at ~0.1× input. | Nothing extra — eligibility is automatic. | `usage.prompt_tokens_details.cached_tokens` |
| Moonshot / Kimi | Automatic cache-hit/miss pricing + **cache affinity key**. On the Kimi Code Plan endpoint (`api.kimi.com/coding`) `prompt_cache_key` is *required* for good hit rates; use a stable per-session value that survives resume. | `prompt_cache_key` = agent name, falling back to `AgentConfig::session_id` (MoonshotPlugin). | top-level `usage.cached_tokens` |
| LM Studio (local) | Engine-level KV reuse (llama.cpp/MLX) for shared prefixes. No billing — the win is prefill latency. | Nothing extra (plugin strips extension fields); `stream_options.include_usage` **is** sent so token counts flow. | Cache hits not reported; prompt/completion/total tokens are |

Both usage shapes are folded into `AgentStats::cached_tokens` (and the
CLI `/stats` + `/usage` displays) by
`context_status::extract_cached_tokens()` (`src/api/ContextStatus.h`).

### Prefix-stability rules the library enforces

A cache match ends at the **first differing byte**, so anything that
changes per iteration must live at the very end of the request:

- The composed system prompt (`SystemPromptBuilder::compose`) is
  **byte-static for the whole session** — no token counters, no clocks.
- The per-iteration *Context Status* block (token usage, iteration
  counter, current time, last-turn cache-hit count) is carried as a
  **transient trailing system message** (`context_status::
  upsert_context_status_message`), replaced every iteration. Legacy
  sessions with the block embedded in the system message are cleaned
  automatically on the next turn.
- Tool definitions and message history are append-only within a turn,
  so turn N+1's request shares its entire prefix with turn N.

### Best settings per provider

- **MiniMax**: nothing to configure. Keep ≥ 512 tokens of stable prefix
  (the tool schema alone usually qualifies). Explicit `cache_control`
  breakpoints exist only on MiniMax's Anthropic-compatible endpoint —
  deliberately unused (we speak the OpenAI-compatible protocol).
- **Moonshot / Kimi**: set a stable agent name or `session_id`; both
  flow into `prompt_cache_key`. The platform's explicit Context Cache
  API (`/v1/caching`, per-minute storage billing) targets static
  document sets and is deliberately unused.
- **LM Studio**: keep one model loaded (disable idle-evict / "auto
  unload"), avoid engine/model switching mid-session. Cache reuse shows
  up as a prompt-processing speedup in the server log from the second
  turn onward. Note: some hybrid-architecture models (e.g. Qwen3.5-35B
  on the MLX engine) have broken KV reuse upstream — verify with the
  server logs if prefill time never drops.

## CLI / TUI surface

- `/providers` shows each provider's effective plugin (`(auto: X)` when
  auto-detected).
- `/provider-add NAME HOST KEY [PLUGIN]` — `PLUGIN` optional; use `-` as
  the key for auth-less providers (e.g. LM Studio).
- The settings TUI provider editor has a `plugin` field (blank = auto)
  and validates explicit values against the registry.

## Tests

- `tests/test_provider_plugin_registry.cpp` — registration, lookup,
  host auto-detect, fallback.
- `tests/test_provider_minimax_plugin.cpp` — payload quirks, model table,
  parser.
- `tests/test_provider_moonshot_plugin.cpp` — temperature clamp,
  `prompt_cache_key`, `reasoning_content` round-trip.
- `tests/test_provider_lmstudio_plugin.cpp` — no-auth, payload stripping.
- `tests/test_provider_settings_migration.cpp` — schema v5 `plugin`
  column migration.
