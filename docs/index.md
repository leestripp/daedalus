# daedalus Documentation Index

This folder contains living reference documentation for host developers
embedding `libdaedalus.a` and for core contributors.

---

## Library Embedding

| File | Audience | Description |
|------|----------|-------------|
| [`embedding.md`](embedding.md) | **Host developers** | Complete embedding guide: minimum viable host, AgentSink contract (all methods + threading), config, lifecycle, cancellation, composition patterns. |
| [`sink.md`](sink.md) | *(deleted — merged into `embedding.md`)* | |

## Agent Internals

| File | Audience | Description |
|------|----------|-------------|
| [`agent_system_prompts.md`](agent_system_prompts.md) | **Core contributors** | System prompt components: identity, tools section, phase descriptions, skill blocks. |
| [`how_compress_works.md`](how_compress_works.md) | **Core contributors** | The 70/30 context compression algorithm, safe split-point rules, tool-call-pair integrity. |
| [`crash_resilience.md`](crash_resilience.md) | **Host developers / users** | Atomic save, write-through auto-flush, async-signal-safe crash handler, drop-last-message recovery. Explains `messages.json`, `messages.json.crash`, and the SIGSEGV/SIGABRT/SIGFPE/SIGILL handler. |
| [`library_design.md`](library_design.md) | **Core contributors** | Permanent invariants for `libdaedalus.a`: the no-stdio rule, async-signal-safety, output channels the library may use. Promoted from `CodeMax.md`. |
| [`agent_state_machine.md`](agent_state_machine.md) | **Core contributors** | Redirect stub — state enum + table now in `Agent.md §3.4`. |

## Tool Reference

| File | Audience | Description |
|------|----------|-------------|
| [`tools.md`](tools.md) | **Host developers / agents** | All tools, JSON schema, message chain integrity, tool-call pairs. |
| [`skills.md`](skills.md) | **Host developers** | Skill discovery, frontmatter format, per-agent skill sets. |

## Storage Layout

| File | Audience | Description |
|------|----------|-------------|
| [`storage.md`](storage.md) | **Host developers / users** | Two-tier rule (XDG user vs project-per-agent), all 11 env vars, migration recipes for laptop / systemd / Podman / Docker / nix, FAQ. Source of truth if other docs conflict. |
| [`settings.md`](settings.md) | **Host developers** | Complete reference for `daedalus::SettingsStore` (v0.4.0+): SQLite-backed API, model config, kv_settings, auto-migration from JSON, SettingsManager deprecation guide. |

## Integrations

| File | Audience | Description |
|------|----------|-------------|
| [`mcp_server.md`](mcp_server.md) | **Host developers** | MCP server integration via `$XDG_CONFIG_HOME/daedalus/mcp/servers.json` (legacy: `~/.daedalus/mcp/servers.json`, see [`storage.md`](storage.md) for migration). Stdio + HTTP transports, JSON-RPC, MiniMax MCP tools (`understand_image`, `web_search`). |
| [`network-tools.md`](network-tools.md) | **Host developers** | OmniNet-backed HTTP/DNS/TCP/TLS/WebSocket tools available to the agent. |

## Demo / Testing

| File | Audience | Description |
|------|----------|-------------|
| [`../containers/README.md`](../containers/README.md) | **DevOps / packagers** | Reference Dockerfile + docker-compose.yml implementing the XDG-clean deployment from `docs/storage.md` §4.4. |
| [`webui_testing.md`](webui_testing.md) | **Core contributors** | Firefox BiDi end-to-end testing recipe for browser-based hosts. |

## Legacy (deleted in Phase 10)

These files have been removed. Do not restore them; they described
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
| `session_memory.md` | **Rewritten** to reflect per-Agent `daedalus_home` design — see the new version |