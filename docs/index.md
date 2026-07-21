# daedalus Documentation Index

This folder contains living reference documentation for host developers
embedding `libdaedalus.a` and for core contributors.

---

## Library Embedding

| File | Audience | Description |
|------|----------|-------------|
| [`embedding.md`](embedding.md) | **Host developers** | Complete embedding guide: minimum viable host, `AgentSink` contract, config, lifecycle, cancellation, provider/model setup, composition patterns. |

### Quick Reference (in embedding.md)

| Section | What it covers |
|---------|---------------|
| [Callback Data Reference](embedding.md#callback-data-reference) | **What data each callback receives** — critical for understanding when to use `on_tool_call` vs `on_log` vs `on_assistant_delta` |
| [Provider / Model Configuration](embedding.md#provider--model-configuration) | How to create `ProviderConfig` and `ModelConfig` entries in `SettingsStore` |

## Agent Internals

| File | Audience | Description |
|------|----------|-------------|
| [`agent_system_prompts.md`](agent_system_prompts.md) | **Core contributors** | System prompt components: identity, tools section, phase descriptions, skill blocks. |
| [`how_compress_works.md`](how_compress_works.md) | **Core contributors** | The 70/30 context compression algorithm, safe split-point rules, tool-call-pair integrity. |
| [`crash_resilience.md`](crash_resilience.md) | **Host developers / users** | Atomic save, write-through auto-flush, async-signal-safe crash handler, drop-last-message recovery. Explains `messages.json`, `messages.json.crash`, and the SIGSEGV/SIGABRT/SIGFPE/SIGILL handler. |
| [`library_design.md`](library_design.md) | **Core contributors** | Permanent invariants for `libdaedalus.a`: the no-stdio rule, async-signal-safety, output channels the library may use. |
| [`agent_state_machine.md`](agent_state_machine.md) | **Core contributors** | Redirect stub — state enum + table now in `Agent.h` / `Agent.md §3.4`. |
| [`session_memory.md`](session_memory.md) | **Host developers** | Long-term session memory: what is stored, how it is compressed/filtered, and how tools interact with it. |

## Tool Reference

| File | Audience | Description |
|------|----------|-------------|
| [`tools.md`](tools.md) | **Host developers / agents** | All tools, JSON schema, message chain integrity, tool-call pairs. |
| [`skills.md`](skills.md) | **Host developers** | Skill discovery, frontmatter format, per-agent skill sets. |
| [`custom-tools.md`](custom-tools.md) | **Host developers** | How to register host-implemented tools via `Agent::register_tool()`. |

## Storage & Settings

| File | Audience | Description |
|------|----------|-------------|
| [`storage.md`](storage.md) | **Host developers / users** | Two-tier rule (XDG user vs project-per-agent), all env vars, migration recipes for laptop / systemd / Podman / Docker / nix, FAQ. Source of truth if other docs conflict. |
| [`settings.md`](settings.md) | **Host developers** | Complete reference for `daedalus::SettingsStore`: SQLite-backed API, provider/model config, kv_settings, model discovery, auto-migration from JSON, `SettingsManager` deprecation guide. |
| [`xdg_compliance.md`](xdg_compliance.md) | **Host developers / packagers** | XDG Base Directory compliance, path resolution, and env-var overrides. |

## Integrations

| File | Audience | Description |
|------|----------|-------------|
| [`a2a.md`](a2a.md) | **Host developers** | A2A (Agent2Agent Protocol) client and server: discovery, REST endpoints, canonical data model, compliance notes. |
| [`mcp_server.md`](mcp_server.md) | **Host developers** | MCP server integration via `SettingsStore` (legacy path support). Stdio + HTTP transports, JSON-RPC, MiniMax MCP tools (`understand_image`, `web_search`). |
| [`network-tools.md`](network-tools.md) | **Host developers** | OmniNet-backed HTTP/DNS/TCP/TLS/WebSocket tools available to the agent. |
| [`sse_streaming.md`](sse_streaming.md) | **Core contributors** | OpenAI-compatible SSE consumption from `LLMClient` to LLM providers: chunked-encoding gotchas, `usage`-trailer recovery, `reasoning_split` quirk. Must-read before touching `HttpSession::submit_cancellable` or `MiniMaxSseParser`. |
| [`token_counts.md`](token_counts.md) | **Core contributors** | Token-count tracking, graceful degradation when providers omit usage metadata, and the SSE canary tests. |

## Demo / Testing

| File | Audience | Description |
|------|----------|-------------|
| [`webui.md`](webui.md) | **Host developers / users** | `demos/daedalus-webui` reference: build, run, REST endpoints, SSE stream, and the Settings page (providers, models, discovery, general, MCP). |
| [`webui_testing.md`](webui_testing.md) | **Core contributors** | Firefox BiDi end-to-end testing recipe for the browser-based webui demo. |
| [`testing.md`](testing.md) | **Core contributors** | Two-layer test strategy: unit tests (`tests/test_*.cpp`, ctest) and integration tests (`/workspace/Testing/test_*/`, real LLM). How to run, how to add, what each layer verifies. |

## Development Notes

| File | Audience | Description |
|------|----------|-------------|
| [`BUGS_FOUND_2026-07-18.md`](BUGS_FOUND_2026-07-18.md) | **Core contributors** | Bug tracker / post-mortem notes from the 2026-07-18 audit. |

---

## Legacy (deleted or superseded)

These files have been removed or merged. Do not restore them; they described
designs that have been superseded.

| File | Reason |
|------|--------|
| `design.md` | Superseded by `embedding.md` |
| `Modes_Flags.md` | Superseded by Phase 2.7 (all modes collapsed) |
| `manager-mode.md` | Superseded by Phase 2.7 |
| `OpenAI_API_tool_call_standard.md` | Redundant with `tools.md` |
| `cpp23_async_refactor.md` | Historical; implementation is source of truth |
| `background-process-design.md` | Implementation is source of truth |
| `output_styles.md` | Library no longer owns terminal output |
| `minimax-mcp.md` | Merged into `mcp_server.md` |
| `lm-studio.md` | Local IP/port note, not a doc |
| `sink.md` | Merged into `embedding.md` |
