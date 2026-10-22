# Changelog

## [Unreleased]

### Added

- §SECOND-BRAIN (2026-10-18): the Global Second Brain — a plain-markdown
  knowledge base, GLOBAL across all projects and shared by every agent
  on this machine (the vault pattern). Design follows Tiago Forte's BASB
  (CODE workflow, PARA folders) + Zettelkasten atomic notes with
  [[wiki-links]], per Plan.md. Source of truth is the markdown tree at
  `<user_data_root>/brain/{inbox,projects,areas,resources,archives}/` —
  human-readable and editable with any editor. YAML frontmatter
  (id/title/tags/created/updated) parsed leniently; filename = note id =
  slug unique across folders. Search runs on SQLite FTS5 (new
  `brain_notes` metadata + `brain_fts` index tables, schema v10,
  porter+unicode61 tokenizer, bm25 weighted title>tags>body, snippet()
  excerpts) held in the existing global daedalus.db as a REBUILDABLE
  CACHE — `reindex` diffs mtime+size and drops vanished files. New core:
  `daedalus::SecondBrain` (src/core/SecondBrain.{h,cpp}) with capture /
  read / update / append / remove / move / backlinks / list / search /
  reindex; `SecondBrain::global()` targets the process-wide store like
  `SecretsVault::global()`. New `brain` tool (one action-dispatched
  tool, the VaultTool pattern): capture, search, read, list, update,
  append, file, backlinks, delete, reindex. Path-traversal-safe ids
  (slug chars only); FTS queries sanitised to alnum prefix tokens so
  MATCH metacharacters can never inject. Session memory (`remember`)
  stays for short session-scoped facts; the brain is for long-term
  knowledge. Tests: test_second_brain.cpp (B1-B14: CRUD, FTS5 stemming,
  reindex, traversal guard, lenient frontmatter, v9→v10 migration) and
  test_brain_tool.cpp (T1-T6: schema shape, JSON happy path, error
  contract); roster pin in test_list_tools.cpp.

### Changed

- §WEB-SEARCH-HYBRID (2026-10-11): the key-less `duckduckgo` provider is
  replaced by `hybrid` — an ordered chain of key-less HTML engines
  (Bing → DuckDuckGo) fetched with a browser user-agent, first non-empty
  result wins (same idea as web_fetch on a crafted search URL).
  DuckDuckGo's HTML endpoint 202-blocks most datacenter traffic and
  Mojeek serves Captcha pages (both verified live), so Bing is the
  primary with DDG kept as the last resort for networks where it works.
  The Bing parser decodes /ck/a redirect URLs (u= param, "a1" +
  base64url). Schema v9 migrates the duckduckgo row to hybrid,
  preserving enabled/priority. Settings UIs and tool messages updated.
  A relevance guard (mentions_query) rejects shadow-blocked SERPs:
  Bing answers automated traffic with 200 + generic query-unrelated
  results (verified: q=gtkmm → phone-plan ads, q=wikipedia → real
  hits), so a result page is only trusted when at least one result
  mentions a query word; otherwise the chain falls through to the
  next engine instead of returning confident garbage.

### Changed

- §WEB-SEARCH-CORE (2026-10-10): the per-provider search tools
  (`serper_search`, `searxng_search`) are replaced by ONE unified core
  `web_search` tool with a provider failover chain. Providers live in a
  new `web_search_providers` SettingsStore table (schema v8): `brave`,
  `tavily`, `exa`, `firecrawl`, `serper`, `serpapi` (API-key services),
  `searxng` (self-hosted; base_url + optional HTTP Basic auth OR
  bearer/API-key header for token-gated instances), and `duckduckgo`
  (key-less HTML endpoint, enabled by default). Enabled providers are
  tried in `priority` order; a provider error, block, or empty result
  falls through to the next. Args: `query`, optional `max_results`
  (default 10, cap 25), optional `provider` to force one provider and
  bypass the chain. The tool description also points browser-equipped
  agents at using a search-engine page directly and notes `web_fetch`
  on a search URL as a fallback. `migrate_to_v8` copies a legacy
  `serper_config` API key / `searxng_config` row into the matching
  provider rows and enables them (legacy tables left in place;
  `SerperConfig` / `SearxngConfig` helpers removed). Settings UI: new
  "Web Search" tab in the settings TUI (per-provider enable, key, URL,
  priority) and a Web Search section in monkeybyte's settings panel;
  the serper row left the Agent tab. monkeybyte's host-side
  DuckDuckGo-only `web_search` tool was removed (the core tool now
  covers it; `web_fetch` untouched).


- §QUEUE-JOIN (2026-10-04): `Agent::run_one_turn()` now consumes the
  ENTIRE pending user-message queue as ONE joined message instead of
  draining one message per turn. Everything submitted while a turn was
  busy (follow-ups, corrections to earlier queued items) becomes the
  user's single intent for the next turn: texts blank-line-joined in
  FIFO order, image attachments concatenated, `source` merged (distinct
  origins comma-joined, so the §MSG-HEADERS wire header stays truthful).
  Queue management while queued (`pending_messages()` / `edit` / `remove`
  / `clear`) is unchanged; monkeybyte's host-side drain loop joins the
  same way. The old off-by-one hazard (submitting without a matching
  turn strands a message) narrows accordingly — a turn now always
  consumes everything queued.

### Build / Infrastructure

- §Monorepo (2026-09-24): the `daedalus`, `icarus`, and `monkeybyte`
  repositories were consolidated into a single monorepo rooted at this
  repository (library history/tags preserved; hosts imported as
  `icarus/` and `monkeybyte/` subtrees).
  - New superbuild root `CMakeLists.txt`: one configure builds library +
    demos + tests + both hosts (`DAEDALUS_BUILD_ICARUS` /
    `DAEDALUS_BUILD_MONKEYBYTE`, both default OFF when this repo is
    consumed via FetchContent — library-only embedders unaffected).
  - Host CMake is dual-mode: in-tree they link the `daedalus` target;
    standalone they FetchContent this repo (library only).
  - Versioning is a single unified stream: bare `x.y.z` tags at the
    repo root, with the library and both hosts deriving their version
    from the same `git describe` (a brief prefixed-tag experiment was
    reverted the same day).
  - All tests labelled `lib` / `cli` / `app` for `ctest -L` slices.


All notable changes to CodeMax are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- **§CTX-ZERO-RESET (2026-10-22, lee: "context shows 0% on the first
  message after every Agent Loop end"): `LLMClient` hard-zeroed
  `last_total_tokens_` at every turn start,** and the per-iteration
  context-status block is built BEFORE the turn's first API response —
  so every new loop reported `0/max (0.0%)` on iteration 1 and the true
  value from iteration 2 on. The model read the zero as "context was
  reset" and narrated bogus percentages. The turn-start (and
  post-compaction) reset now seeds from the live history estimate —
  which reflects the actual persisted conversation — until the first
  real response replaces it with the provider's count. Side benefit:
  the 90% auto-compression gate at iteration 1 now sees a real number
  instead of always-safe zero.

- **§RESTORE-RETRY + §RESTORE-SCROLL (2026-10-22, MonkeyByte "no old
  messages load on a fresh project" + 100-message smoke test): the host
  restored the persisted chat exactly once, in the ChatPanel
  constructor.** Any readiness the constructor predated — workspace bound
  after startup (fresh project via the folder picker), model configured
  in Settings, or the "Model not configured" agent-build race — left the
  welcome screen up forever, and switching workspaces cleared the view
  without loading the target workspace's history. The restore now re-runs
  (idempotently: teardown_messages rebuilds the view without touching the
  persisted session) on panel map when nothing rendered yet, and on
  workspace switch. Separately, a large restored history (100 bubbles)
  rendered pinned to the TOP: the constructor-time pin computed
  upper()≈0 before the scroller had an allocation, so the pin now fires
  once the scroller maps, with the existing 150 ms second pass and
  signal_changed re-pin covering late layout growth. Verified live:
  100-message history restores fully and lands at the latest message;
  a 3-turn conversation in a brand-new project survives restart.

- **§HISTORY-SNAPSHOT-FIX (2026-10-22, MonkeyByte "first load is a mess"
  report): assistant messages that carry BOTH text and `tool_calls`
  vanished from `history_snapshot()`.** The snapshot skipped every
  message containing `tool_calls` ("elide pure tool-call assistant
  messages"), but daedalus attaches the terminating `stop` tool call to
  the SAME assistant message that holds the final answer text — so a
  restarted host restored user bubbles only and the conversation looked
  broken. The snapshot now extracts the text first and elides only when
  there is no renderable text (true tool plumbing). Regression test in
  test_history_snapshot.cpp ("assistant text+tool_calls keeps the text").

- **§MSG-HEADERS-ANTI-IMITATION (2026-10-04): models imitated the wire
  header and typed `[from: assistant · received …]` as the first line of
  their own replies.** Because assistant output is stored verbatim,
  every chat bubble then rendered the raw header as message text —
  MonkeyByte's pretty `.message-origin` label could not remove text
  baked into the message body. Four layers now stop it:
  1. `wire_render()` renders **no header on assistant messages** — the
     model's own voice needs no origin, and seeing its prior replies
     wear the format on the wire was the imitation cue. `_meta` is
     still stripped from the wire copy (strict providers). User /
     system / tool / external messages keep their headers unchanged.
  2. `Messages::strip_leading_wire_header()` strips a leading
     `[from: …]` line from assistant content **at intake**, before the
     reply enters the stored chain. Shape-defensive (a `role: null`
     entry must not throw inside `sanitize_message_chain`) and
     conservative: only a well-formed header on the first line is
     removed; mid-text mentions, unclosed `[from:`, non-assistant
     roles, and header-only replies are left untouched.
  3. `sanitize_message_chain()` applies the strip on every load path
     (SQLite mirror, JSON file, crash file), migrating sessions
     poisoned before the intake strip existed — no `/clear` needed.
  4. The `# Message Origin Headers` prompt section hardened: "Your own
     replies carry NO header — yours is the only message without one."
  Also: the two unstamped tool-result error paths (malformed call,
  invalid arguments) now stamp `tool:unknown` / `tool:<name>` like the
  main path. New §ANTI-IMITATION cases in `message_headers_tests`.

### Added

- **§MSG-HEADERS (2026-10-04): every message now carries an origin
  envelope — `_meta.source` + `_meta.received_at`.** Messages were bare
  `{role, content}` JSON, so the model could not tell an old,
  already-addressed message from a fresh one: a context-compression
  summary read like a repeating event, a re-fired trigger or an inbound
  A2A message was indistinguishable from the human user, and a
  crash-restored prompt looked new.
  - The library stamps `received_at` (always **UTC**, `YYYY-MM-DD
    HH:MM:SS UTC` — §MSG-HEADERS-UTC: the user and the agent may run in
    different timezones, so all message timestamps share one standard
    reference) at message creation; the caller chooses only the source. All
    creation paths are covered: `Messages::add_*`, the turn entry in
    `LLMClient::run_one_turn` (system + user), assistant replies,
    tool results (`tool:<name>`), API-error recovery injections, the
    compression summary (`system:context-compression`), and the
    transient context-status block (`system:context-status`). Messages
    replayed from disk (`add_message`) keep their original `_meta`.
  - At payload-build time `Messages::wire_render()` expands `_meta`
    into a visible one-line wire header —
    `[from: trigger:nightly-backup · received 2026-10-04 07:27:44 UTC]`
    — and strips the `_meta` key from the wire copy, so strict
    providers never see an unknown message key while the model gains
    the origin/time context. The stored chain is never mutated, keeping
    the rendered header byte-stable across turns (prompt prefix-cache
    reuse unaffected). Legacy histories without `_meta` pass through
    unchanged.
  - New public API: `Agent::submit_user_message(text, imgs, source)` —
    hosts with non-interactive ingress (A2A peers, triggers, email
    bridges, devices) tag the origin with a free-form `kind:id` string
    (`user:<name>`, `a2a:<agent>`, `trigger:<name>`, `email:<addr>`,
    ...). New source kinds need no schema or API change. The existing
    overloads default to `user`. The icarus A2A daemon now tags inbound
    peer traffic `a2a:peer`.
  - §MSG-HEADERS-ANTI-IMITATION (2026-10-04): new always-appended
    `# Message Origin Headers` system-prompt section explains the wire
    header to the model and forbids writing/imitating it. Without this
    the model pattern-matched the `[from: ... · received ...]` format
    on every message and began its own replies with a fake header line,
    polluting user-visible answers and feeding the fake line back into
    the stored chain. Section is byte-static (§ContextCache-safe).
  - §MSG-HEADERS-UTC: **every datetime string the agent reads now names
    its zone** — memory and todo timestamps (already UTC via `gmtime`)
    gain an explicit ` UTC` suffix, matching the message envelope and
    the Context Status `Date/Time (UTC)` row. UTC is the single
    standard reference regardless of where in the world the agent or
    user runs (Sydney vs New York).
  - `PendingUserMessage` gains `source`; `HistoryEntry` gains `source`
    + `received_at` so host UIs can render the origin. New doctest
    suite `message_headers_tests` (10 cases).
  - MonkeyByte host UI: `ChatMessage::set_origin(source, received_at)`
    renders a `.message-origin` header line above bubbles whose message
    came from outside the conversation — "You · <time>" for the
    interactive user, "From <peer> (A2A)", "Trigger: <name>", "Context
    status" for the compression summary — so an old or external message
    can't masquerade as a fresh interactive prompt; the assistant's own
    voice and legacy unstamped messages get no header. ChatController
    stamps interactive sends `user:<login>` (GLib `get_user_name()`,
    not `$USER` — a sudo shell can leave that stale) via the new
    `submit_user_message` source overload; the panel labels the echo
    bubble with the same source so live and restored bubbles read
    identically.

### Changed

- **§K2.8 (2026-09-30): `kimi-for-coding` is now Kimi K2.8 Preview —
  thinking effort levels and 1M context.** Moonshot silently re-pointed
  the `kimi-for-coding` / `kimi-for-coding-highspeed` model IDs (Kimi
  Code subscription endpoint, `api.kimi.com/coding/v1`) at the new K2.8
  Preview model; the IDs are unchanged.
  - `MoonshotPlugin::supported_thinking_levels()` now reports
    **Low / High / Max** for the `kimi-for-coding` IDs (same structure
    as K3; server default is Max, `None` remains ignored), so both hosts
    offer the effort selector in model settings.
  - `MoonshotPlugin::mutate_chat_payload()` sends the effort for
    `kimi-for-coding` exactly like K3: `thinking.effort` on the Kimi
    Code endpoint, top-level `reasoning_effort` on the platform.
  - Static model table: `kimi-for-coding(-highspeed)` context size
    256K -> 1M (K2.8 Preview ships 1M-token context on all tiers);
    vision support was already flagged and remains.
  - `kimi-k2.7-code` keeps its no-control behaviour (payload never
    touched); the fixed-temperature clamp for the Kimi Code IDs is
    unchanged.

### Added

- **§MCP-CONTROL-CORE: `mcp_control` tool — agent-driven MCP server
  list/enable/disable** (2026-09-22). MCP tool schemas can dominate the
  context window (sent with every request); the agent can now shed a
  finished server's context itself instead of asking the user.
  - **Tool** (`src/tools/McpControlTool.*`): `action='list'` shows
    configured servers with transport, connected/enabled state and tool
    counts; `'enable'`/`'disable'` toggle one server by name. Unknown
    names are rejected; repeated toggles are no-op successes.
  - **Runtime-only semantics**: the SettingsStore `mcp_servers.enabled`
    column is never written — it stays the host/user's persistent
    control; a runtime-disabled server returns next session.
  - **Wiring**: enable/disable delegate through a new
    `ToolFactory::set_mcp_control_handler()` hook to
    `Agent::set_mcp_server_enabled()` (the existing public host API), so
    the per-agent `ToolFilter` and sink logging stay consistent; the
    tool is re-registered per refresh alongside `mcp_status`. Without a
    handler (direct ToolHandler users) a manager-only fallback handles
    disable and config-known enable.
  - **Effect**: `disable_server()` marks the tool surface dirty; the
    next request drops the server's proxied tools and the Context
    Status roster row (§LIST-TOOLS-CORE) reflects the new count.
  - **Tests**: `tests/test_mcp_control.cpp` — 10 hermetic cases over a
    standalone McpServerManager (no connections, no spawns).

- **§LIST-TOOLS-CORE: `list_tools` tool + live tool roster in the
  Context Status block** (2026-09-21). Fixes "agent doesn't know what
  tools it has" on resumed sessions: the tool-schema block in the system
  prompt is composed once (pinned as `messages[0]`) and never rebuilt,
  so it goes stale when tools/MCP servers appear later.
  - **`list_tools` core tool** (`src/tools/ListToolsTool.*`): returns
    the LIVE registry (core + host-registered + MCP-proxied) as a
    compact markdown roster of name + one-line description; optional
    `filter` (case-insensitive substring on name/description) and
    `verbose` (full JSON schemas). Descriptions are shortened to their
    first line so multi-paragraph MCP tool text can't bloat the table.
  - **Context Status roster row**: `LLMClient::inject_context_stats()`
    now emits `| Tools | N (call `list_tools` for the roster) |` plus
    core tool names and per-server MCP counts in the transient
    per-iteration status message — refreshed every cycle at the
    recency-favoured tail, cache-safe (§ContextCache), and respectful
    of the per-agent `ToolFilter`.
  - **Tests**: `tests/test_list_tools.cpp` — registration, roster
    contents/counts, filter semantics, verbose schemas, description
    shortening. Hermetic.

- **§SEARXNG-CORE: `searxng_search` — web search via a self-hosted
  SearXNG instance** (2026-09-21). Keyless, account-free metasearch
  counterpart to `serper_search`, for agents whose DuckDuckGo-scraping
  search path is bot-blocked:
  - **Storage**: new `searxng_config` table in the SettingsStore
    (schema v7, singleton row): `base_url`, `username`/`password`
    (HTTP Basic auth; empty username = unauthenticated instance),
    `enabled` master switch. Accessor pair
    `SettingsStore::get_searxng_settings()` /
    `set_searxng_settings()` with a `SearxngSettings` struct.
  - **Config helper** (`core/SearxngConfig.h`): `load_searxng_config()`
    is the single source of truth, returning a `SearxngConfigResult`
    with explicit statuses (`EMPTY_BASE_URL`, `INVALID_BASE_URL`,
    `DISABLED`, …) — mirrors `SerperConfig`'s contract.
  - **Agent tool**: built-in `searxng_search(query, max_results?)`
    registered by default; GETs `{base_url}/search?q=…&format=json`
    with an `Authorization: Basic` header when credentials are set.
    Unconfigured/disabled, it returns a descriptive error instead of
    executing (same contract as `serper_search`).
  - **Tests**: `tests/test_searxng_settings.cpp` — store round-trip /
    upsert / disabled persistence, URL validation + normalization,
    v6→v7 migration. Hermetic (temp dirs, no network).

- **§TASK-2026-09-21-01: SecretsVault — global encrypted password
  manager** (2026-09-21). daedalus now has a user-global,
  encrypted-at-rest store for logins, secure notes and API keys:
  - **Storage**: new `vault_entries` table in the existing global
    SettingsStore SQLite DB (schema v6) — one vault per user, shared by
    every project regardless of per-agent `daedalus_home`.
  - **Crypto**: AES-256-GCM via OpenSSL EVP (no new dependencies).
    Random nonce per entry, AAD = entry name (no ciphertext
    transplanting); only the secret fields blob is encrypted — metadata
    stays plaintext so listing needs no decryption. Master key is a
    0600-perm `vault.key` under the user config root (generated on
    first use; ~/.ssh-style filesystem-permission boundary,
    non-interactive unlock; passphrase wrap is future work).
  - **Public API** (`include/daedalus/vault.h`):
    `daedalus::SecretsVault::global()` plus injectable
    store+key construction for tests; `store` / `get` / `list`
    (metadata only) / `remove` / `exists`.
  - **Agent tool**: built-in `vault` tool with `action` ∈
    `list | get | store | edit | delete` — the agent can create, read,
    edit and delete entries autonomously. `store` replaces a whole
    entry; `edit` merges fields (rotate one secret without reading the
    others into context). `list` never returns secret values; write
    confirmations echo field NAMES only; `get` decryption is documented
    as entering conversation history (hosts can disable via
    `tools_disabled`). Tool handlers are statics with an injected vault
    so the full action contract is covered hermetically in tests.
  - Tests: `vault_tests` (round-trip, tamper GCM rejection, wrong-key
    rejection, no-plaintext-in-DB check, v5→v6 migration). Docs:
    `docs/vault.md`.

- **MonkeyByte upstream report 2026-09-10 — new public API** (reported
  against d73d1ba):

  - **§TURN-ERROR: terminal turn failures are now queryable** (BUG-4).
    Terminal LLM failures (fatal HTTP 4xx, retries exhausted, no model
    configured) still end the turn with public `AgentState::Done` — but
    hosts no longer have to track `on_error(will_retry=false)` across the
    turn boundary to find out. New: `Agent::turn_had_terminal_error()` and
    `Agent::last_error()` (returning the new `daedalus::AgentError{kind,
    message}`), valid after `run_one_turn()` returns and cleared when the
    next turn starts.
  - **§HISTORY-SNAPSHOT: read-only conversation history view** (REQ-2).
    `Agent::history_snapshot()` returns
    `std::vector<daedalus::HistoryEntry>{role, content}` — the
    conversation the agent actually sees (post-restore, post-compression,
    post-sanitization), with tool calls and tool results elided and
    multipart content flattened to its text parts. GUI hosts can drop
    their parallel chat-history stores.
  - **§COMPRESS-RESULT: `compress_context()` outcome feedback** (REQ-3).
    `Agent::compress_context()` now returns `daedalus::CompressResult`
    (`Compressed` / `Deferred` / `NothingToCompress` / `NoModel` /
    `Failed`) instead of `void`, so hosts can tell the user what happened
    instead of duplicating internal thresholds. Carry-over semantics are
    unchanged: NothingToCompress/Failed still arm the next-turn retry.
    Source-compatible for callers ignoring the return value.
  - **§AUTO-FLUSH-CFG: public kill switch for the flush worker**
    (REQ-1). `AgentConfig::session_auto_flush` (default true) controls
    the session-persistence background flush worker, previously reachable
    only via the internal `MessageCompressionConfig::auto_flush_enabled`.
    When disabled, saves still happen synchronously at quiescent points.
  - **§STATECHANGE-TYPED: type-safe `on_state_change`** (REQ-5).
    `AgentSink` gains an `on_state_change(daedalus::AgentState from,
    daedalus::AgentState to)` overload; its default forwards to the
    legacy int overload, so existing hosts keep working unchanged and new
    hosts can skip the cast.

- **Pending queue management API** (2026-09-10). Hosts can now list,
  edit, remove and clear the user messages queued by
  `Agent::submit_user_message()` before `run_one_turn()` consumes
  them — previously only `pending_count()` was exposed, so a host UI
  could not show or fix the user's queued follow-ups. New public API:
  `Agent::pending_messages()` (snapshot as the new
  `daedalus::PendingUserMessage` type: `text` + `images`),
  `Agent::edit_pending_message(index, new_text, new_images)` with
  keep/replace/clear attachment semantics, `Agent::remove_pending_message(index)`
  and `Agent::clear_pending_messages()`. Index-based edits return
  `false` on out-of-range indices and leave the queue untouched.

- **SKILL-DIRS: the agent now knows where skills live** (2026-09-04).
  `discover_skills` returns a new `skill_directories` array — every
  configured scan directory as `{path, exists}` — and its tool
  description explains how to create a skill (write a `<name>/SKILL.md`
  directory into one of them, preferring project-local `.agents/skills/`
  or user-global `~/.agents/skills/`). Previously only existing skills'
  paths were returned, so empty directories were invisible and the agent
  had no idea where to create a skill. Backed by the new public
  `SkillManager::skill_directories()`.

### Fixed

- **§MCP-SCHEMA-SANITIZE + vault schema: Moonshot HTTP-400'd EVERY turn
  after the recent tool additions** (2026-09-21). Two tool-schema
  incompatibilities with Moonshot's "moonshot flavored json schema"
  validator, each fatal to every chat request (the tool schema rides on
  all of them):
  - **vault tool (§TASK-2026-09-21-01 follow-up)**: the `fields`
    parameter shipped with `"additionalProperties": {"type": "string"}`
    — Moonshot rejects schema-valued `additionalProperties`
    ("must be a boolean or an object"). Removed; the string-values
    contract now lives in the parameter description. Hermetic
    regression coverage: `test_tool_definitions` now walks every
    default tool schema and fails on any non-boolean
    `additionalProperties`.
  - **MCP proxied tools**: `McpProxiedTool` piped server `inputSchema`s
    through verbatim, so servers emitting pydantic-style schemas
    (`anyOf: [T, {"type":"null"}]` Optionals, `anyOf` with `$ref` into
    `$defs`) were rejected ("detected infinite recursion without
    termination condition") — observed live with a 39-tool ComfyUI
    server. `get_definition()` now normalises schemas into the
    strict-provider subset: `anyOf`/`oneOf` null-branches dropped
    (optionality is already expressed by `required`), multi-branch
    combinators collapse to the primary branch, local
    `$ref` (`#/$defs/…`, `#/definitions/…`) inlined, schema-valued
    `additionalProperties` flattened to `true`, top-level `$defs`
    dropped. The rewrite only steers the model — wire calls are never
    validated against the schema — and is byte-identical for schemas
    that were already clean. Tests: `test_mcp_proxied_tool` gains 4
    sanitizer cases (and its assertions are now actually live in
    Release builds — the file previously missed the project's
    `#undef NDEBUG` convention). Docs: `docs/mcp_server.md`.

- **§BUG-001: MCP tools never injected — `servers.json` fallback
  restored, `mcp_status` introspection tool added** (2026-09-17,
  MonkeyByte report). The v0.5.0 migration moved MCP config into the
  SettingsStore `mcp_servers` table but never implemented the
  documented `mcp/servers.json` fallback (Readme and code comments both
  claimed it existed) — hosts writing the documented standard
  `mcpServers` file got zero servers, zero tools, and zero diagnostics.
  Fixes:
  - **`McpServerManager::load_servers_json_fallback()`**: parses the
    standard `mcpServers` JSON shape (`command`/`args`/`env`,
    `transport` or Claude-style `type`, `url`, `bearerToken`,
    `description`, `enabled`/`disabled`) and merges entries not already
    in the store — **the SQLite store stays authoritative on name
    conflicts**. Host-disabled and invalid entries are skipped with a
    logged reason. `AgentImpl` reads `<daedalus_home>/mcp/servers.json`
    for per-agent homes, else `~/.config/daedalus/mcp/servers.json`.
  - **New built-in `mcp_status` tool**: agent-visible introspection —
    per-server transport/config-source/connected/healthy/tool-count,
    the exact `servers.json` path + exists + parse error, host-disabled
    list, `.env` path, last error, and a config-locations hint when
    zero servers are configured. Registered whenever MCP is enabled,
    deliberately including the zero-server case (that is the one that
    needs diagnosing).
  - **Sink-visible load logging**: agent init now logs the configured
    server count and sources via the host sink, and warns when a
    `servers.json` exists but fails to parse or yields no usable
    servers — previously all diagnostics went to stderr, invisible to
    GUI hosts.
  - Reload semantics documented (config read at agent init; mid-session
    file edits take effect next session; hosts toggle live via
    `Agent::set_mcp_server_enabled()`). Regression test:
    `mcp_servers_json_fallback_tests`.

- **§SKILL-RESCAN: `discover_skills` rescans, honours `directory`, and
  finds project-local skills** (2026-09-17, HiveMind/MonkeyByte bug
  report). Three compounding bugs broke the documented
  create-a-skill-then-rediscover workflow: (1) the tool returned the
  session-start cache forever — `refresh_cache()` was never called, so
  skills created mid-session were invisible; (2) the advertised
  `directory` argument was silently ignored (`/*args*/`); (3) the
  project-local `.agents/skills/` default was resolved against the
  daemon process CWD instead of `AgentConfig.project_root`, so GUI
  hosts (which launch the daemon from their own directory) never saw
  project-local skills. Fixes: `DiscoverSkillsTool` now honours
  `directory` with a fresh cache-bypassing scan and otherwise calls
  `refresh_cache()` before reading the cache;
  `SkillManager::init_after_settings()` takes an optional
  `project_root` anchor (empty keeps the legacy CWD-relative behaviour
  for tests and daedalus-cli) which `AgentImpl` populates from
  `cfg_.project_root`. Regression test:
  `skill_discovery_refresh_tests`.

- **§QUEUE-MUTEX: `Agent::cancel()` no longer races the message
  queue** (2026-09-10, MonkeyByte BUG-3 + REQ-4). `cancel()` read
  `pending_user_messages_.empty()` unguarded while the driving thread
  could be inside `submit_user_message()` / `run_one_turn()` on the same
  vector — a data race (UB) through the one API entry point hosts with a
  UI thread are most likely to call cross-thread. All queue accesses
  (submit, dequeue, `pending_count`, the management API and the
  `cancel()` check) are now guarded by an internal mutex, which also
  upgrades the whole queue API to callable-from-any-thread.
  `run_one_turn()` remains driving-thread-only.

- **BASH-CANCEL: user-facing stop now interrupts a synchronous
  `execute_bash` instantly** (2026-09-10, MonkeyByte). `Agent::cancel()`
  fired `LLMClient::cancel_source_`, but the token was only checked
  *between* agent iterations — a blocking `Subprocess::execute` inside a
  tool call kept running until its full tool timeout (up to an hour),
  leaving the host UI hung on stop. The per-turn cancel token is now
  threaded through the tool layer (`ToolHandler::set_cancel_token()` →
  `BashTool::run_sync` → `BashCommandExecutor::execute` →
  `Subprocess::execute`), and the subprocess poll loops kill the whole
  process group the moment stop is requested (10ms granularity, same
  bounded-drain/reap as the timeout path). The tool result reports
  `status: "cancelled"` with any partial output; the turn then ends as
  `Cancelled` at the next loop checkpoint, as before. Background
  (`action=start`) processes are unaffected — they are deliberately
  detached. Regression tests cover both the normal and the
  closed-pipe (§SUBPROC-EOF-HANG) branches.
- **PERSIST-RACE: flush worker SIGSEGV racing raw chain mutations**
  (2026-09-02, daedalus_bugs.md BUG 1, MonkeyByte). The LLMClient turn
  path mutates the chain through the raw `get_messages()` reference
  without holding `messages_mutex_`; once PERSIST-DIRTY started calling
  `mark_dirty()` mid-turn, the flush worker's deep copy of `messages_`
  in `save_impl()` raced those mutations and tore a json object header
  (SIGSEGV inside `json::get_ptr`, host process died). `Messages` now
  uses a published-snapshot design: `mark_dirty()`/every internal
  mutator deep-copies the chain into `persist_snapshot_` under the
  mutex at quiescent points, and the worker only ever serializes that
  published copy — it never reads `messages_` again. Public `save()`
  refreshes the snapshot first, preserving legacy
  push-via-reference-then-save() semantics. Reproduced 5/8 runs pre-fix
  (SIGSEGV), 10/10 clean post-fix, ThreadSanitizer-clean.
- **SANITIZE-NOTHROW completion** (2026-09-02). The remaining
  `.value()` throw paths were hardened with `chain_safe_str`
  (`cleanup_dangling_tool_results`, `sanitize_assistant_message`,
  `truncate_if_needed`, LLMClient's dangling-tool `back()` check), and
  `save_impl()`'s `dump()` now uses `error_handler_t::replace` so
  invalid UTF-8 in a tool result can no longer throw `type_error.316`
  on the worker thread (std::terminate — SIGABRT).
  `compress()`/`replace_messages()`/`truncate_if_needed()` also mark
  the chain dirty now — their rewrites previously never reached
  disk.
- **SUBPROC-EOF-HANG / SUBPROC-DRAIN-HANG: `execute_bash` infinite
  hangs** (2026-09-02, daedalus_bugs.md BUG 2, MonkeyByte).
  (A) `Subprocess::execute_combined`'s EOF branch did a blocking
  `waitpid()` that ignored the configured timeout when the child closed
  its inherited pipe fds but kept running (`exec sleep 300 >&- 2>&-`);
  it now polls with WNOHANG against the remaining deadline and kills
  the process group on expiry. (B) The timeout-path drain loop spun
  forever when a `setsid` grandchild survived `killpg()` and held the
  pipe write ends open; it now uses the normal path's bounded drain
  (100 iterations x 10ms) with the loop condition fixed from `&&` to
  `||`. Regression tests: `tests/test_subprocess_timeout.cpp`.


## [1.0.2] — 2026-08-31

### Changed

- **PDF-CORE: `read_pdf` is a built-in agent tool again** (2026-08-31).
  `ReadPDFTool` was moved to `demos/example-tools/` during the TOOL-TRIM
  work even though poppler-cpp remained a REQUIRED dependency of the
  library build. The tool now lives in `src/tools/PDFTool.cpp`, is
  registered by default in `ToolFactory::initialize_tools()`, and
  poppler-cpp links PRIVATE into `libdaedalus.a`. Hosts no longer need
  to register `ReadPDFTool` themselves (the CLI and webui demos'
  explicit registrations were removed; re-registering remains harmless
  thanks to replace-by-name).

### Fixed

- **SANITIZE-NOTHROW: malformed chain entries crashed the flush worker**
  (2026-08-31, MonkeyByte coredump 2026-08-30). `sanitize_message_chain()`
  used `entry.value("role", ...)` / `.value("tool_call_id", ...)` /
  `.value("id", ...)`, which throw `json::type_error` when a chain entry
  is not an object, or when `role`/`tool_call_id`/`id` is present but
  null or non-string. The exception escaped `save()` onto the background
  flush worker thread → `std::terminate` → SIGABRT (intermittent: fired
  only when a malformed/partially-written chain entry met a flush tick).
  Fixes (`src/core/Messages.cpp`): sanitize now extracts strings
  defensively (`chain_safe_str()`) and drops non-object chain entries,
  tool results with missing `tool_call_id`, non-object `tool_call`
  entries, and `tool_call`s with non-object `function` instead of
  throwing; `save_impl()` re-marks `dirty_` and returns if sanitize ever
  throws; the flush worker wraps `flush_if_dirty()` in a last-resort
  catch so no persistence error can ride the thread into `terminate`.
  Regression tests: `daedalus_message_chain_validity_tests`
  (SANITIZE-NOTHROW suite).

## [1.0.1] — 2026-08-31

### Fixed

- **PERSIST-DIRTY: session history was never persisted during turns**
  (2026-08-31, reported via MonkeyByte: "the last message from the
  previous session is sent to the LLM again on startup"). The
  `LLMClient` turn path mutates the message chain through the raw
  `get_messages()` reference, bypassing `add_*()` - so `dirty_` was
  never set, the CRASH-2 write-through flush worker stayed idle for
  entire sessions, and the ONLY save happened in `cleanup()` (reachable
  solely via `Agent::shutdown()`). Hosts that close without
  `shutdown()` (MonkeyByte's `~ChatController`) reloaded a stale chain
  on the next startup - typically ending in an unanswered user message,
  which the model then re-answered alongside the user's fresh prompt
  ("both sent at once"), while exchanges newer than the last save were
  silently lost. Fixes: new `Messages::mark_dirty()` public API;
  `set_messages()` now marks dirty (sanitize/truncate/compress
  rewrites); the turn path marks dirty after the user push, after each
  assistant reply, after tool-result commits, and at end of turn;
  `cleanup_dangling_tool_results()` now wakes the worker immediately.
  With write-through restored, the `~Messages()` final
  `flush_if_dirty()` persists the live chain even when a host skips
  `shutdown()`. Regression tests: `daedalus_messages_tests`
  (reload-after-clean-shutdown fidelity + set_messages dirty
  write-through).

- **§IdleExitBreaker: set_status-only circuit breaker ends runaway
  turns** (2026-08-20). Follow-up to §IdleLoopFix: rejecting
  runtime-owned states stopped `set_status("Idle")`, but a finished
  model could still loop forever on *accepted* states — calling
  `set_status("Done")`, getting `status: success`, and answering every
  re-invocation with another `set_status` (observed in icarus: the host
  re-sent only the auto context-status block and the model replied
  `set_status("Done")` every iteration). `set_status` never exits the
  agent loop — only the `stop` tool does. `LLMClient` now counts
  consecutive iterations whose ONLY tool call is `set_status`; at 2 in
  a row the turn ends exactly as if `stop` had been called. The counter
  resets at turn start, on any other tool call, and on plain-text
  responses. Regression tests: `status_only_breaker_tests` (in-process
  HTTP server, mirrors `fatal_breaker_tests`).

## [1.0.0] — 2026-08-20

### Fixed

- **§IdleLoopFix: `set_status` no longer accepts runtime-owned states**
  (2026-08-20). `Idle`, `AwaitingApproval`, and `Compressing` are set by
  the library itself, but `SetStatusTool` used to accept all three from
  the model and return `status: success`. In the field this produced an
  infinite loop: a model that had finished its work would read the
  system prompt's "Idle = no work in flight" row and call
  `set_status("Idle")` expecting the agent loop to exit — but
  `set_status` only *reports* state to the host sink; the loop ends
  exclusively via the `stop` tool (or host cancel). The success result
  reinforced the behaviour, so the model repeated it forever.
  `SetStatusTool` now recognises the runtime-owned names (so the error
  is precise) but rejects them: the `Idle` message explicitly explains
  that no `set_status` call exits the loop and points at the `stop`
  tool. The tool description, the parameter schema description, and the
  system-prompt state-machine section were updated to match — `Idle`
  moved to the "Runtime-owned states" table, plus an explicit rule:
  "`set_status` never exits the agent loop — not even `Done`." The
  `AgentState::Idle` enum value itself is unchanged (still the
  host-facing initial/resting state). Regression coverage in
  `tests/test_set_status_tool.cpp` (rejection of all three states,
  case/whitespace variants, JSON-string form, and a contract check that
  error payloads never carry `status == "set_state"`, so no
  `on_state_change` event can fire).


## [0.10.3] — 2026-08-14

### Added

- **§COMPRESS-TOOL: model-facing `compress_context` tool restored**
  (2026-08-14). Removed on 2026-08-01 ("the agent never called it"), but
  in practice the agent panics as context pressure climbs toward the 90%
  auto-compress threshold with no way to shed stale context itself. The
  tool is back with the safer deferred semantics introduced by
  §AutoCompress: calling it does NOT compress inline — the tool result
  (`{"status":"compress_context","reason":...}`) is picked up by the new
  `LLMClient::handle_compress_context_result()` in `run_agent_cycle()`,
  which sets the same `compress_requested_` flag as the host-facing
  `Agent::compress_context()`. Compression runs at the cycle-boundary
  checkpoint, never interrupts in-flight tool calls, and reports via
  `AgentSink::on_compaction()` as usual. The context-status block now
  nudges the agent toward the tool above 70% pressure (instead of the
  old "Do NOT call any compression tool" warning at 90%). Registered by
  default in `ToolFactory`. Covered by a new e2e case (model emits the
  tool call below threshold → exactly one compaction at the cycle
  boundary), a `handle_compress_context_result` unit suite, and schema
  assertions in `tests/test_tool_definitions.cpp`.

### Fixed

- **§BUG-UTF8-TRUNC: invalid UTF-8 no longer aborts the agent turn.**
  Byte-level truncation (`resize`/`substr`) could split a multi-byte
  UTF-8 codepoint — the §BUG-MEMORY-BLOAT entry cap (2048 bytes) did
  exactly that to build commands; the corrupted string later escaped
  into the conversation (memory recall tool result), and the next
  request payload `dump()` threw `type_error.316 invalid UTF-8 byte at
  index 2048`, escaping `run_one_turn` and killing the turn with a raw
  exception. Fixes: new `utf8_truncate_safe()` helper (cuts at codepoint
  boundaries) applied to every truncation (SessionMemory entry/prompt/
  backstop caps, BashCommandExecutor output cuts, tool-result middle
  truncation); ALL request payload dumps now use
  `error_handler_t::replace` so a bad byte can never abort the loop —
  the request goes out with U+FFFD and the agent keeps working (with a
  host-visible warning); the strict-dump fallback that used to end the
  turn as DONE now reserializes with replacements and continues; memory
  recall sanitizes legacy corrupted entries on output.

## [0.10.2] — 2026-08-10

### Fixed

- **§BUG-MEMORY-BLOAT: session-memory dump no longer inflates the system
  prompt to ~250k tokens (the real "compressing over and over" trigger).**
  `SessionMemory::get_system_prompt_addition()` dumped EVERY successful
  build command ever recorded in full — heredoc scripts included — and
  `builds_` was the only memory category never pruned (`consolidate()`
  trimmed patterns/sessions only). On a long-lived project the store grew
  to ~3 MB / 570+ builds, the system prompt reached ~246k tokens (94% of
  a 256k window), and the 90% auto-compress fired every cycle while
  `Messages::compress()` — which never touches the system message — could
  not reduce it: an empty-looking chat looped "Compressing" indefinitely
  and every request re-sent ~250k cached tokens. Fixes: the prompt dump
  now renders only the top 20 builds by use_count/recency with commands
  collapsed to a capped single line, plus a 16 KB backstop cap on the
  whole addition; `add_build()` enforces the configured
  `max_entry_size_bytes` (2 KB) at store time and prunes the category to
  `max_entries_per_category` (50); `consolidate()` prunes builds too, so
  existing bloated stores shrink on session end; and the run loop skips
  auto-compress when fewer than 10 non-system messages exist (context
  dominated by the system prompt can never be compressed — keep working
  instead of shredding a tiny history for zero gain).

- **§BUG-COMPRESS-LOOP: auto-compression no longer loops forever.** The
  90% auto-compress branch in the agent run loop retried compression
  indefinitely whenever compression could not bring the context back
  below the threshold — the summary call failing repeatedly, a history
  too small to safely reduce, a single tool result bigger than the
  window, or a `max_context_size` configured far smaller than the real
  model window. Every attempt reset `iteration_count_` (so the
  max-iterations guard could never fire) and `continue`d the loop, so
  the agent sat in the "Compressing" state until the user cancelled —
  each failed attempt also blocking up to ~210 s in
  `Messages::compress()`'s internal retries (30 s/60 s/120 s). A
  consecutive-compression circuit breaker (mirroring the fatal-API-error
  breaker) now allows 2 attempts that fail to drop below the threshold,
  then emits a terminal `on_error(ErrorKind::Internal, …,
  will_retry=false)` and ends the turn as DONE. The counter resets at
  turn start and on any cycle that completes below the threshold, so
  legitimate back-to-back partial compressions still converge.

## [0.10.1] — 2026-08-10

### Fixed

- **Host image attachments now reach the model** (0.10.0 regression):
  `![alt](path)` Markdown in user messages was sent as literal text
  (`build_multimodal_message()` was dead code with zero call sites) and
  the public `Agent::submit_user_message(text, ImageAttachments)`
  overload silently discarded its images. Both paths now feed the
  read_image pipeline: refs are extracted at message time
  (`src/api/ImageRefs.h`), loaded via `ImageProcessor` /
  `download_image_as_base64` / raw bytes, and injected into the next
  request payload only — history keeps plain Markdown text, and the
  `model_supports_vision()` gate drops images on text-only models with
  a warning instead of risking a provider 400. The dead
  `build_multimodal_message()` / `find_image_references()` code was
  removed. Covered by `tests/image_attachment_tests` (extraction units
  plus end-to-end payload capture against an in-process HTTP server,
  including the vision-gate drop case).

## [0.10.0] — 2026-08-10

### Added

- **Core `read_image` tool — vision input for Kimi K3 and other
  vision-capable models** (§READ-IMAGE-CORE, 2026-08-10): a new default
  tool loads a local image and returns a base64 data URI; `LLMClient`
  attaches it to the *next* chat request as an `image_url` content part
  (the multipart `content` array shape documented at
  platform.kimi.ai/docs/guide/use-kimi-vision-model — base64 only, no
  public URLs) and strips the payload from stored history so the tokens
  are paid exactly once. PNGs are decoded via libpng and downscaled to
  ≤512px longest edge (Kimi accepts up to 4K; 512px keeps vision-token
  cost low) and JPEGs get the same treatment via libjpeg-turbo
  (§JPEG-SCALE: decode → bilinear scale → re-encode q85, so a 2000×1000
  photo goes on the wire as 512×256); `gif`/`bmp`/`webp` pass through as
  base64 with a 2MB cap; SVG is rejected with guidance (Kimi refuses SVG
  image parts — send the XML as text). The shared
  `ImageProcessor::load_local_image()` dispatcher routes by sniffed
  magic bytes, so the `![alt](path)` attachment path in `LLMClient`
  also benefits from JPEG scaling. Injection is gated on the active provider
  plugin's `supports_vision` table entry — text-only models (e.g.
  MiniMax-M2.x) drop the image with a warning instead of risking a
  provider 400. `ImageProcessor` learned WebP magic-byte / extension
  detection. Covered by `tests/test_read_image_tool_tests` (12 cases, incl.
  libjpeg-generated fixtures verified for exact scaled dimensions).

### Fixed

- **Compression request now uses the provider-clamped temperature**
  (§BUG-COMPRESS-TEMP, 2026-08-08): `Messages::call_llm_for_summary()`
  builds its own payload and bypasses `LLMClient::run_agent_cycle()`, so
  the provider plugin's `clamp_temperature()` was never applied — the
  summary request went out with the model row's raw temperature.
  Fixed-temperature models (Kimi `kimi-for-coding*` / `k3` /
  `kimi-k2.7-code` = 1.0) hard-reject any other value, so all 3 summary
  attempts failed and the last-resort fallback silently deleted ~70% of
  the context. `LLMClient` now clamps the temperature through the active
  plugin in all three places it populates `MessageCompressionConfig`
  (constructor with/without settings, and `set_config()` on model
  switch). Covered by a new end-to-end regression case in
  `tests/test_auto_compress.cpp` ("compression request carries
  provider-clamped temperature"); invariant documented in
  `docs/how_compress_works.md` and `docs/provider-plugins.md`.

## [0.9.8] — 2026-08-07

### Fixed

- **Compression fallback delete is now reported** (§COMPRESS-REPORT,
  2026-08-07): when all 3 summary attempts failed, `Messages::compress()`
  silently deleted the oldest ~70% of messages and returned the same
  `nullopt` as a genuine failure — so `on_compaction()` never fired,
  hosts keying success off it misreported the run as failed even though
  the context had really shrunk, and `compress_context()` re-armed
  `compress_requested_`, triggering a pointless second compress on the
  next turn. `compress()` now returns a `CompressResult` with a
  `CompressOutcome` (`Compressed` / `NothingToCompress` /
  `FallbackDeleted` / `Failed`); both `LLMClient` compress paths fire
  `on_compaction(before, after)` for `FallbackDeleted`, and the
  host-request flag is only carried over when the context was left
  untouched. Covered by a new fallback end-to-end case in
  `tests/test_auto_compress.cpp`.

- **`compression_timeout` config is honored**: the 3 compression
  attempts now use `compression_timeout`, 2x, 4x (default 30 →
  30s/60s/120s) instead of a hardcoded `{30, 60, 120}` array that
  ignored the configured value.

## [0.9.7] — 2026-08-05

Crash-resilient message persistence + libcurl removal + Agent
Communication Protocol (ACP) sub-library + runtime skill/MCP/tool toggles.

### Added

- **`Agent::compress_context()`** (§AutoCompress, 2026-08-01):
  host-facing compression trigger. Until now compression could only start
  via the automatic 90%-of-context threshold — hosts could observe
  (`on_compaction`, `AgentStats`, `Compressing` state) but not steer.
  When no turn is running the call compresses immediately on the
  caller's thread; when a turn is running it sets a thread-safe flag
  consumed at the end of the current/next agent cycle (the same code
  path as the threshold trigger), so it is safe mid-turn and never
  interrupts in-flight tool calls. The result is reported via
  `AgentSink::on_compaction()` as usual. Covered by below-threshold and
  idle-immediate end-to-end cases in `tests/test_auto_compress.cpp`;
  documented in `docs/embedding.md`.

- **User-selectable thinking levels** (§ThinkingLevel, 2026-08-01):
  - New `daedalus::ThinkingLevel` enum (`Default`/`None`/`Low`/`High`/
    `Max`) with string helpers (`include/daedalus/types/ThinkingLevel.h`)
    and `AgentConfig::thinking_level` (default `Default` = send nothing).
  - Plumbed through `AppConfig` → `ProviderChatContext` (which also gains
    `base_url`) into each plugin's `mutate_chat_payload()`. Mapping:
    Kimi K3 ids get `reasoning_effort` on the platform endpoint or
    `thinking: {"type":"enabled","effort":...}` on the Kimi Code
    subscription endpoint (`api.kimi.com`); `kimi-k2.5`/`k2.6` and
    `MiniMax-M3` get `thinking: {"type":"disabled"}` for `None`. All
    other models/levels are silently ignored — nothing is ever sent that
    the provider would reject. LM Studio strips both fields.
  - New `ProviderPlugin::supported_thinking_levels(model_id)` virtual and
    `Agent::supported_thinking_levels()` so hosts can build per-model
    pickers showing only valid levels. Runtime switching is possible via
    `Agent::set_thinking_level()` — the payload hook reads the level per
    request, so a mid-session change applies from the very next turn
    (no agent rebuild needed).
  - Covered by `tests/test_thinking_levels.cpp`; documented in
    `docs/provider-plugins.md` and `docs/embedding.md`.
  - The `daedalus-settings-tui` demo Agent tab now edits a persisted
    `thinking_level` kv row (`default`/`none`/`low`/`high`/`max`),
    demonstrating the documented host-side persistence pattern — the
    library deliberately persists nothing itself, so hosts own the
    SettingsStore row and feed it to `AgentConfig::thinking_level` /
    `Agent::set_thinking_level()`.

- **`Agent::pending_count()`** (`include/daedalus/Agent.h`): number of
  submitted user messages not yet consumed by `run_one_turn()`. Makes the
  submit/run pairing visible to hosts — submitting without a matching
  `run_one_turn()` leaves the queue off-by-one (every later turn processes
  the *previous* message). The `submit_user_message()` doc comment now
  carries an explicit warning about the 1:1 contract.

- **Structured error channel: `AgentSink::on_error()`**
  (`include/daedalus/AgentSink.h`, new `include/daedalus/types/ErrorKind.h`):
  - New public callback
    `on_error(daedalus::ErrorKind kind, std::string_view message, bool will_retry)`
    fired on every LLM/API error path: HTTP 401/429/4xx/5xx, transport
    failures, timeouts, malformed provider responses, structured
    `{"error": ...}` payloads, and library-side request failures.
  - `ErrorKind` gives hosts a machine-readable classification (`Network`,
    `Timeout`, `RateLimited`, `Auth`, `ClientError`, `ServerError`,
    `Parse`, `ApiError`, `Internal`); `will_retry` distinguishes transient
    failures (backoff in progress) from terminal ones. Provider error
    messages are delivered untruncated.
  - The default implementation forwards to `on_log(LogLevel::Error, ...)`,
    so existing hosts see identical output and opt in only when they want
    to act on errors (banner, backoff, model switch, abort).
  - Covered by `tests/test_on_error_callback.cpp`; documented in
    `docs/embedding.md`.

- **Host-controlled tool approval** (§BUG-006/007/008):
  - `AgentConfig::auto_approve` is now `std::optional<bool>`: set = the
    host is authoritative (the SettingsStore row is ignored, so hosts can
    force approval prompts ON); unset = legacy store fallback.
    (`include/daedalus/types/AgentConfig.h`)
  - New runtime controls on `Agent`: `set_auto_approve(bool)`,
    `clear_session_approvals()`, `clear_persisted_approvals()`,
    `set_remember_approvals(bool)` (opt out of auto-recording approved
    commands — "ask every time" semantics).
    (`include/daedalus/Agent.h`)
  - New sink callback
    `AgentSink::on_command_auto_approved(command, source)` with the new
    `daedalus::ApprovalSource` enum (`AutoApprove` / `SessionCache` /
    `PersistedCache`): hosts are notified whenever a command is approved
    by auto-approve mode or an approval cache instead of by
    `request_command_approval()`. Default no-op; existing hosts unaffected.
    (`include/daedalus/AgentSink.h`, `include/daedalus/types/ApprovalSource.h`)
  - Covered by `tests/test_approval_control.cpp`; documented in
    `docs/embedding.md`.

- **`Agent::delete_model()`** (`include/daedalus/Agent.h`,
  `src/library/Agent.{cpp,h}`): public-API passthrough to
  `SettingsStore::delete_model()`, symmetric with the existing
  `Agent::delete_provider()`. Returns false for an empty name or a
  missing SettingsStore; deleting an unknown name is idempotent.
  Covered by `tests/test_delete_model.cpp`.

- **Provider plugin system** (§ProviderPlugins): providers and their
  models are now described by `ProviderPlugin` hook implementations
  instead of hardcoded quirks in `LLMClient`/`SettingsStore`.
  - New public interface `include/daedalus/ProviderPlugin.h` (request
    shaping via `mutate_chat_payload()`, `clamp_temperature()`,
    `sanitize_history()`, `create_stream_parser()`,
    `parse_models_response()`, `classify_api_error_type()`, model
    knowledge via `known_models()`/`context_size_for()`, endpoint and
    auth metadata) plus `include/daedalus/ProviderRegistry.h` for
    registration and name/host resolution. Hosts can register custom
    plugins without forking the library.
  - Built-in plugins under `src/api/providers/`: `openai-compatible`
    (default, classic behavior), `minimax` (reasoning_split,
    continuous_usage_stats, M3/M2.x table), `moonshot`
    (kimi-k2.7-code temperature forced to 1.0, `prompt_cache_key`,
    kimi-k3/k2.x/moonshot-v1 table), `lmstudio` (auth-less, loopback
    hosts, minimal payloads, discovery-driven).
  - Settings schema v5: `providers.plugin` column (empty =
    auto-detect from name/host); `SettingsStore::
    get_max_context_size_for_model()` consults the plugin's model table
    before the legacy built-in map.
  - The shared SSE parser now accumulates `reasoning_content` fragments
    into the synthesized assistant message — Kimi rejects tool-loop
    histories that drop it.
  - New `Agent::active_provider_plugin()` accessor; CLI `/providers`
    shows the effective plugin and `/provider-add` takes an optional
    plugin argument (`-` key for auth-less providers); settings TUI
    provider editor gains a validated plugin field.
  - Covered by `tests/test_provider_plugin_registry.cpp`,
    `test_provider_minimax_plugin.cpp`,
    `test_provider_moonshot_plugin.cpp`,
    `test_provider_lmstudio_plugin.cpp`, and
    `test_provider_settings_migration.cpp`; documented in
    `docs/provider-plugins.md`.

- **Provider context-cache support** (§ContextCache, 2026-07-31):
  - Cache-hit usage is now captured for every provider:
    `LLMClient::update_token_usage()` folds Moonshot/Kimi's top-level
    `usage.cached_tokens` and MiniMax/OpenAI's nested
    `usage.prompt_tokens_details.cached_tokens` into one counter,
    exposed via `LLMClient::get_cached_tokens()` and the new
    `AgentStats::cached_tokens` field (CLI `/stats` and `/usage`
    displays show it when > 0).
  - Moonshot's `prompt_cache_key` (required by the Kimi Code Plan for
    good hit rates) now falls back to `AgentConfig::session_id` when no
    agent name is set, so the default agent no longer silently opts out
    (`AppConfig::session_id`).
  - The per-iteration *Context Status* block moved out of the system
    prompt into a **transient trailing system message**
    (`src/api/ContextStatus.h`,
    `LLMClient::inject_context_stats`). MiniMax and Kimi match
    server-side prompt caches on a stable tools → system → messages
    prefix; a stats block that changes every iteration near the top of
    the system message invalidated the entire conversation history for
    caching. The composed system prompt is now byte-static for the
    whole session (per-session Date/Time moved into the trailing
    message; `SystemPromptOptions`' context-stats fields removed), and
    legacy sessions with an embedded stats block are cleaned
    automatically on the next turn.
  - Covered by `tests/test_context_cache.cpp`; documented in
    `docs/provider-plugins.md` (per-provider caching matrix + best
    settings) and `docs/agent_system_prompts.md` §2.6.
  - Demo surface: webui's stats JSON + usage HUD show `cached_tokens`
    and the resolved provider plugin (`daedalus-webui`);
    daedalus-manager and daedalus-subagent print a final cached-token
    line for cache-capable providers.

- **`AgentConfig::memory_in_prompt`** (§ContextCache follow-up,
  2026-08-02): new behaviour flag (default `true`). When `false`, the
  Session Memory section is omitted from the composed system prompt
  while memory collection continues — hosts that surface memory through
  their own trailing message can keep the entire system prompt
  byte-static for server-side prompt caches. Previously the only switch
  was `memory_enabled = false`, which disabled the memory feature
  entirely. Wired through `AgentImpl` → `LLMClient` →
  `SystemPromptOptions::include_session_memory`.

### Changed

- **Session Memory moved to the END of the composed system prompt**
  (§ContextCache follow-up, 2026-08-02, from the icarus maintainers'
  prompt-cache review): `SystemPromptBuilder::compose()` now emits
  sections as host prompt → system_info note → MCP → tool definitions →
  skills → `# State Machine` → `# Reserved Agent Directories` →
  `# Session Memory`. MiniMax / Kimi server-side prompt caches stop at
  the first differing byte, and the memory dump is the most volatile
  section — previously any memory change invalidated the cache for the
  ~2.4k tokens of byte-static mandatory text that followed it. Only the
  trailing memory section itself misses now. Hosts that diff prompts or
  snapshot-test composed output will see the reordering. The volatile
  stats line (`Sessions: N | Builds: M | Errors resolved: X/Y`) also
  moved from the top to the bottom of the memory dump
  (`SessionMemory::get_system_prompt_addition()`) so unchanged memory
  content keeps a byte-stable prefix.

### Fixed

- **Crash-handler hardening + cleared/corrupt session persistence**
  (§CRASH-6, 2026-08-05, from the monkeybyte crash report in
  `daedalus_bugs.md`): seven defects in the `Messages` crash-handling
  and persistence machinery (`src/core/Messages.cpp`):
  - The signal handler snapshotted the registry into a `std::vector` —
    `malloc` inside a SIGSEGV handler deadlocks or re-corrupts the heap
    when the fault originated in the allocator. The registry is now a
    fixed-size array of `std::atomic<MessagesCrashState*>` with CAS
    registration: no allocation, no lock anywhere on the signal path
    (also removes the spinlock self-deadlock when a thread faults while
    holding the registry lock).
  - `save()` mutated the `std::string crash_payload_` the handler read —
    a dangling-`data()` race. The payload is now double-buffered:
    `save()` writes the inactive slot and flips an atomic index, so the
    handler never reads a buffer being written.
  - The handler dereferenced `Messages*` that a concurrent destructor
    could free (use-after-free). Per-instance crash state (fd + payload)
    now lives in a heap-allocated `MessagesCrashState` that is
    intentionally leaked for process lifetime, and the `.crash` fd is
    never closed (a closed fd number could be recycled mid-write).
  - A failed `ftruncate(fd, 0)` left a stale tail in the `.crash`
    mirror, producing corrupt recovery JSON exactly when the mirror was
    needed. Both `save()` and the handler now write first and
    `ftruncate` to the exact payload length, and the recovery parser
    tolerates trailing garbage by walking backwards over `]` candidates.
  - `Messages::clear()` never persisted: it set neither `dirty_` nor
    touched the SQLite mirror, so a corrupt chain survived "clear chat"
    until the whole `.daedalus/` folder was deleted. `clear()` now
    synchronously removes `messages.json`, the `.tmp`/`.crash` mirrors,
    the SQLite mirror row, and clears the crash payload.
  - `load()` ran only `sanitize_message_chain()` — a message with a
    missing/empty `role` (e.g. from an interrupted save) was loaded
    forever after and broke every LLM request. Both load paths (SQLite
    mirror and `messages.json`) now validate every message with
    `validate_message_for_recovery()` and drop invalid entries with a
    warning; an all-invalid mirror is deleted.
  Covered by new CRASH-6 cases in `tests/test_messages.cpp`; documented
  in `docs/crash_resilience.md`.

- **Streaming retry/fallback re-posts asked for SSE and crashed on
  `json::parse("data: ...")`** (§SYNC-FALLBACK-STREAM, 2026-08-01, found
  by live LM Studio verification): all four non-streaming fallback sites
  in `run_agent_cycle` (`src/api/LLMClient.cpp`) re-posted the original
  `request_payload` unchanged — in streaming mode that payload still
  carried `"stream": true` + `stream_options`, so the server answered
  with SSE and the sync path's `json::parse(response_body)` threw
  `parse error ... last read: 'd'` on every retry until the turn died.
  Triggered by any transient mid-turn error while streaming (the
  post-compression first request timing out on a slow local model was
  the live repro). Each fallback now posts a copy with `"stream": false`
  and no `stream_options`. Covered by the `TransientErrorServer` case in
  `tests/test_auto_compress.cpp`.

- **Compressed history started with an assistant tool call → strict chat
  templates hard-error every later cycle** (§COMPRESS-USER-MSG,
  2026-08-01, found by live LM Studio verification): `Messages::compress()`
  rebuilt history as `system + assistant(compress_context tool call) +
  tool(summary) + suffix`. qwen3's jinja template on LM Studio requires
  the FIRST message after system to be a user message (verified
  empirically — assistant/tool-first histories are rejected with `No user
  query found in messages` even when a user message exists later), so
  every post-compression request failed and the turn died on the
  fatal-error breaker. The rebuild now always inserts a synthetic user
  message right after the system message, carrying the summary
  (`src/core/Messages.cpp`). Covered by `tests/test_auto_compress.cpp`.

- **Stray `</think>` tags leaked into host UIs** (§ThinkTagFilter,
  2026-08-01): the shared SSE parser only handled reasoning sent as the
  `reasoning_content` *field* (Kimi/Moonshot). Models that embed
  `<think>...</think>` inline in `content` (qwen3-family, MiniMax when
  `reasoning_split` is not honored, deepseek-style) had every delta —
  tags included — passed verbatim to `on_assistant_delta` and persisted
  into history, and chunk-split tags defeat host-side string replacement.
  New stream-safe `ThinkTagFilter` (`src/api/ThinkTagFilter.h`) strips
  inline think blocks at the parser level: tags split across SSE chunks
  are held until disambiguated, stray `</think>` closers strip just the
  tag, unterminated blocks drop their tail at `finish()`, and
  never-confirmed tag prefixes are re-emitted as literal text. The
  buffered (non-streaming) path gets a one-shot `strip()` before the
  message enters history. Reasoning sent via the `reasoning_content`
  field is untouched and still round-trips for Kimi's tool loop.
  Covered by `tests/test_think_tag_filter.cpp` and new cases in
  `tests/test_minimax_sse_parser.cpp`.

- **LM Studio: token counts were always 0, so auto-compression never
  triggered** (2026-08-01): `LmStudioPlugin::mutate_chat_payload()` stripped
  `stream_options` outright, so LM Studio never sent the usage-only final
  SSE chunk; `last_total_tokens_` stayed 0 and
  `Messages::should_compress()` could never reach the 90%-of-context
  threshold. The plugin now forces
  `stream_options: {"include_usage": true}` in streaming mode (still
  stripped in non-streaming mode) while continuing to strip the provider
  extensions LM Studio rejects (`reasoning_split`, `prompt_cache_key`,
  `thinking`, `reasoning_effort`). Verified live against LM Studio
  (`tokens=16175 | ctx=16175/128000` after one turn).
  Updated `tests/test_provider_lmstudio_plugin.cpp` and
  `tests/test_thinking_levels.cpp`.

- **Duplicate tool definitions → provider HTTP 400 "function name X is
  duplicated"** (2026-08-01, reported via MonkeyByte):
  `ToolFactory::register_tool()` was a bare `push_back` on the
  process-global factory, so hosts that rebuild their Agent (model switch,
  workspace change, session reset) and re-register host tools accumulated a
  duplicate function definition per build; strict providers rejected every
  request. Registration is now **replace-by-name** (the existing slot is
  updated in place), the `AgentImpl::initialize()` host-tools loop no longer
  skips already-registered names (it refreshes stale adapters from previous
  agent instances), and a new `ToolFactory::unregister_tool(name)` supports
  hosts with dynamic tool surfaces (`src/tools/ToolFactory.{h,cpp}`,
  `src/library/AgentImpl.cpp`). Covered by
  `tests/test_tool_factory_dedup.cpp`.

- **Fatal API errors looped forever and corrupted history with hundreds of
  synthetic `[SYSTEM ERROR]` user messages** (2026-08-01, reported via
  MonkeyByte):
  - The `consecutive_fatal_count_` circuit breaker only tripped on an
    *identical* error signature on consecutive cycles. Deterministic 400s
    that alternate shape between cycles (each injected recovery message
    corrupts the history into the next shape, e.g. "function name
    duplicated" → "messages with role 'tool' must be...") reset the counter
    every cycle, so the breaker never fired. It now counts ANY consecutive
    fatal-classified cycle (threshold unchanged: one recovery attempt, then
    the turn ends) (`LLMClient::fatal_breaker_should_end_turn()`,
    `src/api/LLMClient.cpp`).
  - Four ACT-returning error paths bypassed the breaker entirely
    (transient→fatal during retry, retries exhausted, no-choices+fatal,
    empty response); all now route through it.
  - When the breaker trips the turn ends WITHOUT appending another
    `[SYSTEM ERROR]` user message, capping recovery injection at one per
    fatal sequence and ending the history-corruption burst.
  - Covered by `tests/test_fatal_breaker.cpp` (alternating-400 in-process
    HTTP server; asserts 2 requests, DONE, single injected message).

- **MCP stdio servers died silently on shell metacharacters + dead
  servers reported as connected** (§MCP-QUOTE, §MCP-EOF, 2026-07-30):
  - `McpClient::connect()` rebuilds the spawn command for `bash -c` but
    only re-quoted args containing spaces, `*`, or `?`. Any other shell
    metacharacter went through bare — e.g. a version spec like `mcp<2`
    was parsed by bash as a stdin redirection from a file named `2`,
    killing the server instantly. Every argument is now single-quoted
    (embedded single quotes escaped as `'\''`)
    (`src/mcp/McpClient.cpp`).
  - `send_request()`/`read_response()` returned `{}` on every failure
    path (EOF, timeout, write/poll/read error) while all three callers
    (`initialize()`, `list_tools()`, `call_tool()`) test `is_null()` for
    failure. A server that crashed during startup therefore produced a
    phantom success: `initialize()` returned true, `connect()` reported
    the server healthy with 0 tools, and no circuit-breaker entry or log
    line was ever emitted. Failure paths now return null json, so a dead
    server fails `connect()` with a real reason (e.g. "Server closed
    connection") (`src/mcp/McpClient.cpp`).
  - The circuit-breaker log line now includes the failure reason:
    `[MCP] server 'x' failed (count=N), retrying in Ys — <reason>`
    (`src/mcp/McpServerManager.cpp`).

- **Infinite HTTP 400 loop from misplaced tool results** (§fix 2026-07-30):
  the agent could get stuck re-sending a chain the provider rejects with
  "messages with role 'tool' must be a response to a preceeding message
  with 'tool_calls'", growing the history by two messages per cycle.
  - Root cause 1: the FATAL-error recovery in `run_agent_cycle()` appended
    a synthetic tool result for the last tool_call found *anywhere* in
    history — including already-answered mid-history calls — producing a
    tool message that does not follow its parent assistant. All four
    recovery sites now use the guarded
    `Messages::append_tool_results_if_pending()`, which injects results
    only for a genuinely pending tail block (multi-tool safe, never
    duplicates) (`src/api/LLMClient.cpp`, `src/core/Messages.cpp`).
  - Root cause 2: `Messages::sanitize_message_chain()` only checked that a
    tool result's `tool_call_id` existed somewhere in history. It is now
    **position-aware**: tool results are kept only in the contiguous block
    directly after their parent assistant, duplicates are dropped, and
    tool_calls without an adjacent result are pruned.
  - Root cause 3: `truncate_if_needed()` ran after sanitization and its
    `stopped_early` path could keep tool results while dropping their
    parent assistant (plus a duplicate-push bug). Block retention is now
    all-or-nothing, orphan results are dropped during truncation, and the
    chain is re-sanitized before the payload is rebuilt.
  - Safety net: a circuit breaker ends the turn when the identical FATAL
    API error repeats on consecutive cycles instead of returning `ACT`
    forever (`LLMClient::consecutive_fatal_count_`).
  - Covered by `tests/test_message_chain_validity.cpp` (15 cases).

- **Auto-approve could not be forced OFF by hosts** (§BUG-006/007/008,
  reported via MonkeyByte):
  - `AgentImpl::auto_approve()` returned
    `cfg.auto_approve || store.get_auto_approve()`, so a stale
    `auto_approve=true` row in the shared SettingsStore silently overrode
    a host explicitly asking for approval prompts — the config field could
    only widen auto-approve, never restrict it. The config is now
    authoritative when set (`src/library/AgentImpl.cpp`).
  - `ToolHandler`'s process-global auto-mode flag was only ever set ON —
    `AgentImpl::initialize()`, `AgentImpl::execute_tool()`, and the
    `LLMClient` ctor each called `set_auto_mode(true)` conditionally and
    nothing ever reset it, so toggling auto-approve ON→OFF within one
    process left approvals bypassed. The flag is now applied
    **unconditionally** from the single authority
    (`AgentImpl::auto_approve()`), and the `LLMClient` ctor honours the
    previously unread `AppConfig::auto_mode` field instead of reading the
    store directly (`src/api/LLMClient.cpp`).
  - Approved commands bypassed the sink via the session/persisted caches
    with no host control or visibility; see the new host-control API in
    **Added** above.
  - `demos/daedalus-cli` now only sets `cfg.auto_approve` when `--auto` is
    actually passed, so the persisted store setting still applies
    otherwise.

- **LLM errors that never reached the host** (`src/api/LLMClient.cpp`):
  - `run_agent_cycle()` never called `api_future.get()`: the async
    request's terminal exception ("API request failed after N attempts")
    and the HTTP error body were silently discarded, and non-streaming
    turns fell into the parse-failure fallback and issued a duplicate API
    request. The future is now consumed; terminal failures fire
    `on_error(ErrorKind::Network, ..., will_retry=false)`.
  - Several failure exits were `LOG_ERROR`-only (file log) — payload
    serialization/validation failures, invalid `tool_call_id`s, fatal
    errors during retry, and the terminal empty-response ("no choices")
    path. All now fire `on_error` (previously the host saw nothing and the
    turn ended as `AgentState::Done`).

- **Robust HTTP-error handling in the LLM client** (`src/api/LLMClient.{h,cpp}`,
  `src/api/HttpSession.cpp`, `src/api/ErrorMapping.h`):
  - HTTP status codes are no longer dropped between the transport layer and
    response handling. `make_api_request_async()` /
    `make_http_request_with_retry()` now return an internal `ApiResponse`
    (status + body) instead of a bare body string, and `run_agent_cycle()`
    handles error statuses first, keyed on the status code.
  - Streaming mode no longer feeds HTTP error bodies (plain JSON, not SSE)
    into the SSE parser. `HttpSession` accumulates the body on status >= 400
    and skips the stream callback, so the provider's structured error
    message survives. Previously a streaming 401 produced an empty buffer,
    a misleading `JSON parse failed (attempt 1/3)` warning, a 1s sleep and
    a duplicate fallback request before the real error surfaced.
  - Auth failures (401/403, and provider error types mapping to
    `ErrorKind::Auth` — including Kimi/Moonshot's
    `invalid_authentication_error`, now covered by
    `error_kind_from_api_type()`) are terminal for the turn: one
    `on_error(Auth, ..., will_retry=false)` and `AgentState::Done`.
    Previously the FATAL path injected "try a different approach" messages
    and returned `ACT`, looping the 401 until user interrupt or the
    iteration cap. Request-shape fatals (bad request, invalid message role,
    tool-call desync) keep the inject-and-recover behaviour.
  - Covered by `tests/test_on_error_callback.cpp`; verified live against
    the Kimi provider (happy path + forced 401).

- **TTY-safe live bash output streaming** (`BashTool`, `BashCommandExecutor`):
  - `BashCommandExecutor::sanitize_tty_sequences()` strips terminal-control
    sequences (cursor movement, screen erase, alternate buffer, OSC, DCS,
    etc.) from captured shell output while preserving UTF-8 text and ANSI
    SGR colour/style codes.
  - Sanitisation is applied to synchronous bash output and to background
    process `stdout`/`stderr` before they are returned to the API or host.
  - `Subprocess::execute()` and `BashCommandExecutor::execute()` gained
    streaming overloads that emit output chunks as they arrive.
  - `BashTool::run_sync()` forwards sanitised chunks via the existing
    `ToolProgressFn` callback as `{"stage":"output","output_chunk":"..."}`
    events, so hosts can display live command output during long-running
    commands while the LLM still receives the complete result at the end.
  - Documented in `BaseTool.h` and `include/daedalus/AgentSink.h`; covered by
    `tests/test_bash_output_sanitize.cpp`.
  - Streamed chunks are now sanitised **incrementally** by the new reusable
    `SanitizedOutputStream` class (`src/tools/SanitizedOutputStream.h`), which
    replaces the per-slice `OutputChunkBuffer`. Parser state (escape
    sequences, UTF-8 validation, CR/LF normalisation) persists across feeds,
    so a chunk boundary can never split an ANSI escape sequence, a
    multi-byte UTF-8 character, or a CR/LF pair — concatenating all emitted
    chunks is byte-identical to one-shot `sanitize_tty_sequences()` of the
    whole input for any split pattern. `sanitize_tty_sequences()` is now a
    one-shot wrapper over the same core, so the two paths cannot diverge.
    Pinned by split-invariance tests (every two-part split, byte-at-a-time,
    random pieces) in `tests/test_bash_output_sanitize.cpp`.
  - The CLI demo's `ConsoleAgentSink` now prints `output_chunk` progress
    events live (previously it dropped the payload and showed only a stage
    marker), and elides the duplicated `output` field from the final
    `[TOOL RESULT]` dump when the output already streamed. Tool calls and
    results are displayed as a simple header with readable `key: value`
    field lines (multi-line strings shown as indented raw blocks) instead
    of raw JSON dumps.

- **LLM lifecycle callbacks** (`AgentSink`):
  - `on_llm_wait_started()`, `on_llm_response_started()`, and
    `on_llm_request_finished()` give hosts precise hooks for driving a
    "thinking" spinner. The spinner now runs only while the agent is
    actually waiting for the LLM, instead of during prompt-building
    work that precedes the network request.
  - Wired through `LLMClient::run_agent_cycle` for both streaming and
    non-streaming paths, including retry fallbacks.
  - Updated the reference `ThinkingSpinner` and the CLI demo's
    `ConsoleAgentSink` to use the new hooks.
  - Documented in `docs/embedding.md`; covered by
    `tests/test_llm_lifecycle_callbacks.cpp`.

- **Runtime enable/disable API** (`Agent`):
  - `Agent::set_skill_enabled()`, `Agent::set_mcp_server_enabled()`,
    `Agent::set_tool_enabled()` and matching `is_*_enabled()` /
    `disabled_*()` query helpers.
  - Toggle state is scoped to each `Agent` instance: skills use a per-agent
    `SkillManager`, MCP servers use the existing per-agent
    `McpServerManager`, and tools are filtered through a per-agent
    `ToolFilter`.
  - Construction-time defaults via new `AgentConfig::tools_disabled` plus the
    existing `skills_disabled` and `mcp_servers_disabled` fields.

- **Phased todo list** (`TodoListTool`, `TodoListService`, `Agent`):
  - Todos now carry a `status` (`pending`/`in_progress`/`done`/`blocked`) and
    an optional `group` (phase/category, e.g. `Understand`, `Plan`,
    `Implement`, `Verify`).
  - `todo_add`, `todo_edit`, and `todo_list` expose the new fields; the
    listing is grouped by phase and includes a per-status progress summary.
  - `todo_count` returns per-status totals plus a `by_group` breakdown.
  - The CLI system prompt teaches the agent to phase-group work and keep
    exactly one task `in_progress` at a time.
  - Public API updated: `Agent::add_todo(text, status?, group?)`,
    `Agent::edit_todo(id, text?, status?, group?)`, and
    `Agent::list_todos(filter, group?)`.
  - Legacy persisted todos with boolean `done` are migrated to `status: done`
    on load.
  - Covered by `tests/test_todo_list_tool.cpp` and
    `tests/test_todo_public_api.cpp`.

- **Agent state machine — verification gate + Documenting state**:
  - Added public `AgentState::Documenting` for README, CHANGELOG, comment,
    and docs updates after verification.
  - `set_status` accepts `Documenting`; hosts receive `state_change` events
    for the new phase.
  - Rewrote the library state-machine prompt section and the CLI workflow
    section to teach the agent that `Verifying` is a decision gate: inspect
    build/test output, and on failure loop back to `Understanding` or
    `Planning` with new todos. Only transition to `Done` after verification
    passes and any needed documentation is complete.

- **Crash-resilient message persistence** (`Messages`):
  - **Atomic save** (CRASH-1): every `save()` writes to `messages.json.tmp`,
    `fsync()`s, then `rename()`s over `messages.json`. POSIX `rename()`
    on the same filesystem is atomic — readers never see a half-written
    file. `kill -9` mid-write now leaves the previous fully-written
    `messages.json` intact.
  - **Write-through auto-flush** (CRASH-2): every `add_*()` marks the
    message chain as dirty; a background `std::thread` flushes within
    ~100 ms (configurable via `auto_flush_interval_ms`). Destructor
    stops the worker cleanly and does one final flush.
  - **Async-signal-safe crash handler** (CRASH-4): `Messages::install_crash_handlers()`
    installs `SIGSEGV`/`SIGABRT`/`SIGFPE`/`SIGILL` handlers that write
    `messages.json.crash` from a pre-opened fd using only
    `open`/`write`/`close`/`fsync`, then re-raise so the user still
    gets a core dump / stack trace. Handler iterates a process-global
    registry of `Messages*` instances under a tiny `std::atomic<int>`
    spinlock (no `pthread_mutex` — not async-signal-safe).
  - **Crash recovery** (CRASH-5): `load()` falls back to
    `messages.json.crash` when the main file is missing or corrupt;
    defensively drops the last message if it looks like the cause of
    the crash (via `validate_message_for_recovery()` — checks role,
    tool_call_id, assistant tool_calls arguments JSON), then re-saves
    the recovered chain to `messages.json` atomically.
  - **Production wiring**: `Agent::install_crash_handlers()` /
    `uninstall_crash_handlers()` forward through `AgentImpl` and
    `LLMClient` to `Messages`. `daedalus-cli/main.cpp` calls
    `agent.install_crash_handlers()` after `agent.initialize()`.
  - Full architecture + trade-offs + end-to-end behaviour per crash
    scenario: **`docs/crash_resilience.md`**.

- **Agent Communication Protocol (ACP) sub-library** at
  `demos/daedalus-cli/acp/`. Sub-library, demos-only (uses `std::cerr`
  for diagnostics which would corrupt a TUI host). Enabled via
  `-DDAEDALUS_CLI_ACP=ON` (default OFF).
  - `AcpClient` with structured `AcpResult<T>` error propagation
    (replaces silent default-constructed return values).
  - SSE structured dispatch via `IRunEventHandler` callbacks
    (`on_run_created`, `on_message_part`, `on_run_completed`,
    `on_run_failed`, `on_run_cancelled`, `on_error`, …).
  - `OmniNet`-backed transport: `create_omni_transport()` (was
    `create_curl_transport` — the old name was a lie).
  - `stream_run_events()` was a leftover stub returning
    `AcpError{501,"not implemented"}`. Now correctly delegates to
    `transport_->stream_events()` (send_streaming + SseParser +
    dispatch_sse_event).
  - End-to-end test (`test_acp_end_to_end`, 4 cases / 42 assertions,
    ~2 s) spins up an in-process OmniNet HTTP server with a
    chunked-SSE endpoint, verifies all 11 spec event types route to
    the correct callbacks.

- **Documentation:**
  - `docs/crash_resilience.md` (new) — the conversation-history
    lifecycle story.
  - `docs/library_design.md` (new) — the no-stdio invariant for
    `libdaedalus.a` (formerly in `CodeMax.md`; promoted to a
    permanent spec).
  - `daedalus_cli` body shape for webui POST
    (`{"message":"..."}`, not `{"content":"..."}`) explicitly
    documented in `Readme.md`.

### Changed

- **`Agent::compress_context()` semantics: immediate when idle, honored
  on cancel** (§AutoCompress, 2026-08-01): previously the call only ever
  set a flag consumed after a *completed* agent cycle — an idle call did
  nothing visible until some future turn, and a turn cancelled before
  its first cycle (the loop-top interrupt break skips the checkpoint)
  stranded the request entirely: "just Cancelled, didn't even try to
  compress". Now, when no turn is running, compression happens
  immediately and synchronously on the caller's thread (`on_compaction`
  fires from that thread); a turn guard (`turn_mutex_` +
  `turn_active_`) makes this race-free against turn startup and safe
  from sink callbacks on the agent's own thread. When a turn is running
  the request still defers to the end-of-cycle checkpoint, and the
  cancelled-turn path now consumes a pending request before returning
  `CANCELLED`. If there is nothing to compress yet, the request carries
  over to the next turn's first cycle (unchanged). Covered by a new
  idle-immediate end-to-end case in `tests/test_auto_compress.cpp`.

- **`CurlMultiSession` → `HttpSession`** rename (12 files, ~172
  references). The class still wraps an HTTP client; the new name
  reflects the actual implementation (OmniNet HTTP, not curl). The
  public API is unchanged.
- **`make_curl_request_with_retry` → `make_http_request_with_retry`**
  in `LLMClient` (sync path). Same behaviour; libcurl no longer in
  the build.
- **`AcpClient::create_curl_transport` → `create_omni_transport`**.
- **`daedalus-webui` CORS**: default `cfg.corsAllowedOrigins`
  tightened from `"*"` to loopback-only
  (`http://localhost`, `http://localhost:8080`, `http://127.0.0.1`,
  `http://127.0.0.1:8080`); new `--cors-origins` CLI flag for
  override.
- **Phase 15 typo-warning tests**: case 5 re-enabled (was disabled
  pending `SkillManager::delete_default()` lifecycle fix B31).
  All 5 cases now pass in 0.03 s.
- **ACP error message handling**: `err.message` and `err.cause` in
  SSE error events truncated to 1024 chars + `…[truncated]` to
  prevent unbounded payloads from a malicious or buggy server
  consuming receiver memory.

### Removed

- **Model-facing `compress_context` tool removed** (2026-08-01): the
  agent never called it — compression effectively only ever ran via the
  automatic 90%-of-context threshold. `CompressContextTool`
  (`src/tools/UserTool.h/.cpp`), its `ToolFactory` registration, the
  `LLMClient` result handler, and the now-dead `compress_after_done`
  plumbing (`run_agent_cycle` by-ref param) are all gone. Compression is
  triggered by the 90% threshold or by the host via
  `Agent::compress_context()`. As part of the removal the
  compressed-history rebuild no longer emits a synthetic
  assistant(`compress_context`) + tool(summary) pair — the summary now
  rides inside the leading synthetic user message, so the rebuilt chain
  is valid for every provider (`src/core/Messages.cpp`).

- **libcurl dependency completely removed** from the build system.
  `find_package(CURL)` and `CURL::libcurl` links gone from
  `CMakeLists.txt`. Zero `curl_easy_*`, `curl_multi_*` calls
  anywhere in `src/`, `demos/`, `tests/`.
- **Deleted** `include/daedalus/net/CurlHandle.h`. File no longer
  referenced after the libcurl removal.
- **Deleted** `tests/test_curl_handle.cpp`. The curl handle unit
  tests are no longer applicable.
- **Deleted** `tests/test_curl_multi_session.cpp`; replaced by
  `tests/test_http_session.cpp` (covers the renamed HttpSession).
- **Legacy `.codemax[-<name>]/` runtime-state paths** completely
  removed. `.codemax` was a leftover from a previous tool with the
  same name as a user; never a previous name for `daedalus`. User
  explicitly banned `.codemax` references in the library. Fallback
  branch and constants removed from `src/library/project_paths.cpp`;
  test cases removed from `tests/test_project_paths.cpp`; doc
  sections removed from `docs/storage.md` and `docs/migration.md`.
  `grep -rn codemax src/ include/ tests/` → 0 hits
  (`demos/daedalus-cli/system_prompt.txt` retains one teaching
  example for the LLM about `pkill -f codemax`).

### Fixed

- **stdio MCP servers never received requests** (`McpClient`,
  `StdioMcpTransport`): the 2026-07-18 SIGPIPE fix switched the stdin
  pipe write from `write()` to `send(fd, ..., MSG_NOSIGNAL)` — but
  `send()` on a pipe fd fails with `ENOTSOCK` on Linux, so every write
  to a stdio MCP subprocess failed and no `mcp__*` tools were ever
  registered. `write_all()` now uses `write()` with `SIGPIPE` blocked
  in the calling thread for the duration of the call (mask restored
  afterwards, pending `SIGPIPE` drained), preserving the EPIPE-instead-
  of-signal behaviour without changing the process-wide signal
  disposition. Regression-pinned by a real JSON-RPC round trip over a
  pipe in `tests/test_mcp_transport.cpp`
  (`stdio_transport_jsonrpc_round_trip_over_pipe` — the old `/bin/cat`
  echo test accepted `WriteFailed` as a pass, which is how this slipped
  in) and `tests/test_mcp_hub_hardening.cpp`
  (`test_client_round_trip_over_stdio_pipe`).
- **State machine stuck in `Understanding`**: LLM-driven state
  transitions (via the `set_status` tool) were handled inside
  `LLMClient::run_agent_cycle`, which updated only LLMClient-private
  bookkeeping and fired `AgentSink::on_state_change` directly —
  `AgentImpl::state_` never saw them. As a result `Agent::state()`
  returned `Understanding` for the entire turn, the first transition
  event carried a bogus `Idle` prev, and the turn always ended with
  `Understanding → Done` regardless of intermediate states.
  `LLMClient` now exposes `set_state_change_callback()` +
  `handle_set_state_result()`; `AgentImpl` wires the callback to its
  own `set_state()`, making it the single source of truth for the
  public state (live `Agent::state()`, correct `from`/`to` on every
  `on_state_change`). Without a callback the old direct-sink
  behaviour is preserved for direct `LLMClient` users. Covered by
  `tests/test_state_change_routing.cpp`.
- **`# State Machine` system-prompt section mis-described the
  mechanism**: it claimed "each tool call carries a `state` field"
  (no code ever read such a field) and said the library fires
  "`state_change` SSE events" (SSE is a webui-host concept; the
  library fires the `on_state_change` sink callback). The section now
  instructs the agent to call the `set_status` tool on every phase
  change, documents the full state table including `AwaitingApproval`,
  and marks `AwaitingApproval`/`Compressing` as runtime-owned.
  `SetStatusTool`'s schema description was aligned accordingly.
- **Crash-safe message persistence**: every crash scenario
  (SIGSEGV, SIGABRT, SIGFPE, SIGILL, `kill -9`, power loss, OOM kill)
  now recovers up to the last successfully-`fsync()`'d save().
  Worst-case data-loss window is bounded by the 100 ms auto-flush
  interval.
- **ACP-16**: `RunCreateRequest::to_json()` had an empty `if (req.session_id)`
  block — the `session_id` field was silently dropped from every
  `POST /v1/runs` request body. Now correctly writes
  `j["session_id"] = *req.session_id;`.
- **daedalus-subagent / daedalus-manager `--help` crash** (F2,
  was open in v0.4.6-prep notes). Both demos now short-circuit
  `--help`/`-h` BEFORE positional argument parsing, so `--help`
  prints help text and exits 0 instead of passing `"--help"` as a
  non-existent `project_root` to `Agent::initialize()`.
- **Webui body shape documented** (F3, was open in v0.4.6-prep
  notes). `Readme.md` line 525 now explicitly states
  `/sessions/<id>/messages` expects `{"message": "..."}`.
- **Messages destructor no longer auto-saves** when background flush
  is disabled (per-phase test race fix: tests that wiped the data
  directory between Messages construction and destruction no longer
  get phantom `messages.json` files re-created by the destructor).
- **ACO transport / Message / Run `from_json` / `to_json`
  completeness**: previously `Run.created_at` / `Message.created_at`
  were parsed but not written out, and `from_json(Run&)` discarded
  most fields. Both now round-trip cleanly.
- **`is_connected` check**: previously returned true for any status
  `<500`; now requires `2xx` (the original semantics were a typo).
- **SSE streaming drops the usage-only trailer on slow providers**
  (e.g. MiniMax/M2.7). The streaming loop in
  `HttpSession::submit_cancellable` treated any `nullopt` from
  `OmniNet::HTTP::ClientResponseStream::read_some()` as end-of-stream.
  `read_some()` also returns `nullopt` when `socket->waitForRead(100ms)`
  times out without data — providers whose chunks are spaced
  >100 ms apart hit this and the loop exited after only the first
  2 reads, dropping the `{"choices":[],"usage":{...}}` trailer that
  carries real token counts. Fixed by checking `stream.eof()` before
  breaking on `nullopt`; on transient timeout, sleep 5 ms and
  continue. Symptom was `'usage' is null in SSE stream — token counts
  unavailable` and a zero-token-count UI bar on every turn. Full
  write-up of the SSE consumption pipeline and its gotchas:
  **[`docs/sse_streaming.md`](docs/sse_streaming.md)**.
- **`Authorization` header was malformed** in both
  `LLMClient::make_api_request_async` (streaming path) and
  `make_http_request_with_retry` (sync fallback). Earlier code did
  `req.header("Authorization", std::string("Authorization: Bearer ") + key)`,
  producing the malformed wire header `Authorization: Authorization: Bearer XXX`
  that OmniNet then sent to the API. MiniMax rejected every
  request with `authorized_error` (401). Fixed by passing only the
  value (`req.header("Authorization", "Bearer " + key)`). Visible
  symptom: every fresh agent session failed with a 401 on the
  first API call.
- **MiniMax-M2 streamed chain-of-thought leaked into `content`**
  (BUG-MINIMAX-105). MiniMax's HTTP stream inserts thinking into
  the `content` field as a `(think)\n...` prefix, which the agent
  UI double-shows as if it were the final answer. The official
  MiniMax-blessed fix is to send `reasoning_split: true` in the
  request body — documented at
  [MiniMax-M2 issue #105](https://github.com/MiniMax-AI/MiniMax-M2/issues/105)
  and in
  [MiniMax API docs](https://platform.minimax.io/docs/api-reference/text-openai-api).
  Now wired in `LLMClient::run_agent_cycle` so thinking goes to
  `reasoning_content` / `reasoning_details` and `content` stays
  clean.

- **ExternalProject layout broke `serper_settings_tests`** (BUG-002).
  `find_project_root()` walked up looking for the first directory
  containing `CMakeLists.txt`, which is the wrong sentinel in an
  `ExternalProject`/`FetchContent` layout: the test was running with
  CWD = the external project's build dir (which has a generated
  `CMakeLists.txt` but no `src/`), so the heuristic stopped one
  level too high and slurped the WRONG file. Three tests then
  failed at `test_serper_settings.cpp:97`, `:112`, `:126` with
  `CHECK(!content.empty())` blowing up on the empty content of
  the (wrong) `src/` directory. Fixed by changing the sentinel to
  `src/tools/SearchTool.cpp` (a file that only exists in the real
  source tree, never in a generated build tree). Also added an
  explicit `if (!std::filesystem::exists(path)) return 0;` guard
  to the three previously-unguarded tests so a future heuristic
  refactor degrades gracefully (SKIP) instead of FAIL when the
  source tree is unreachable.

### Tests

- `messages_tests` 73/73 + 1 skipped (203/203 assertions).
- `test_acp_basic` 40/40 (131/131 assertions).
- `test_acp_end_to_end` 4/4 (42/42 assertions).
- Full `ctest` → 65/65 pass, 2 live-LLM skipped, 0 failures
  (was 64/64 with `phase15_typo_warning_tests` excluded; that
  test is now passing again and excluded flag is no longer needed).
- **`real_world_tools_tests`** (NEW, §FIX-REAL-WORLD 2026-07-18):
  end-to-end test that drives the public `Agent::execute_tool`
  API against the canonical `Testing/test-cli-bare/project/sum.cpp`
  fixture. 4 phases: filesystem setup, agent+tools discovery,
  XDG override, real `read_file`+`edit_file_patch`+`execute_bash`
  with clang++ compile round-trip. Catches contract drift
  between documented tool names and actual argument keys
  (`old_str`/`new_str`, not `old_string`/`new_string`). Total
  `ctest` now 67/67 pass serially (2 live-LLM skipped).

### Migration notes (v0.4.x → v0.5.0)

- **libcurl users**: no API change. `LLMClient`'s sync HTTP path now
  uses OmniNet under the hood. Behaviour is identical.
- **`CurlMultiSession` users**: rename to `HttpSession`. Source
  drops & include paths are unchanged; only the class name moved.
- **`cfg.corsAllowedOrigins = "*"` (webui)**: now defaults to
  loopback-only. Hosts running on a non-loopback origin must pass
  `--cors-origins https://myhost.example` (comma-separated).
- **`.codemax[-<name>]/` users**: those paths no longer work.
  Migrate to `.daedalus[-<name>]/` (the existing canonical namespacing)
  via `daedalus-cli --migrate-data`. If data still exists in the
  legacy path the migrator handles it.
- **Crash recovery**: hosts that previously saw empty sessions after
  a crash now get the recovered conversation back automatically on
  next launch. No host code change required; `Messages::load()` is
  backward-compatible.

---

## [Unreleased]

### Fixed (2026-07-16, v0.4.2 prep — physical test pass)

- **`print_banner` crash on long paths** (HIGH — F0).
  `demos/daedalus-cli/banner.h` padded each field with
  `std::string(WIDTH - s.size(), ' ')`. Whenever `s.size() > WIDTH`
  (e.g. a 59-char `data_dir` on a long project path), the
  subtraction underflowed and the constructor threw
  `std::length_error`, `std::terminate`'d the demo, and aborted
  with `[FATAL] basic_string::_M_create` before the LLM was
  even contacted. Replaced the inline pad with a `pad_to(s, width)`
  helper that clamps to 0 and truncates with ` …` for long values.
  Found during the PT-1 physical test pass; all 4 demos now run
  end-to-end with the live `minimax-m2.7` model.

- **`daedalus-subagent --help` / `daedalus-manager --help` crash**
  (HIGH — F2). Both demos treat `argv[1]` as `project_root` and
  pass it to `Agent::initialize()` without a `--help` short-circuit;
  the demo throws `std::invalid_argument("Project path does not
  exist: --help")` and aborts. Same root cause in both; same
  fix pattern as `daedalus-cli/main.cpp:52-60`. **Fixed in v0.5.0**
  (see above). Tracked in CodeMax.md F2.

### Fixed (2026-07-15, v0.4.2 prep)

- **Test fix:** `legacy_deprecation_warning_tests` case 2
  (`$DAEDALUS_HOME set → no warning`) used `/opt/daedalus` as the
  override path. That fails with `Permission denied` on systems
  where the test runner is unprivileged (CI, podman container,
  any non-root user). Replaced with a temp-dir path so the test
  exercises the override-acknowledgement logic without depending
  on `/opt` being writable. The behaviour assertion — "when
  `$DAEDALUS_HOME` is set, no deprecation warning fires" — is
  unchanged.

- **Dead-code removal:** `matches_pattern()` in `src/core/FileIO.cpp`
  (the simple-glob helper used by the old SettingsManager
  load-ignore path) was never called — `FileIO` no longer takes
  a `SettingsManager*`, the settings pipeline went through SQLite
  in v0.4.0, and no SettingsManager/JSON-driven code reads the
  result. Compiler `-Wunneeded-internal-declaration` flagged it
  on every rebuild. Deleted.

- **DEBUG block:** `LLMClient::run_agent_cycle` had a 35-line
  "DEBUG: Log tool_calls" block gated on a specific `tc_id` and
  `has_invalid_chars` check. Both branches were empty — only
  local-variable assignments, no `LOG_*` call. Deleted.

- **Constructor dead code:** `AgentImpl::AgentImpl` had a 17-line
  block that defaulted `cfg_.settings_path`, `cfg_.mcp_servers_path`,
  `cfg_.mcp_env_path`, `cfg_.memory_dir` from `cfg_.daedalus_home`
  (or `default_*_path()`). Since v0.5.0 the SettingsStore reads
  directly from `cfg_.daedalus_home`; those four `AgentConfig`
  fields are now `[[deprecated]]` and only used as explicit
  per-field overrides (the override read at line 320–324 still
  works). The default-population block was pure write-only. Deleted.

- **Deprecation warnings:** `default_settings_path()`,
  `default_mcp_servers_path()`, `default_mcp_env_path()` now
  carry `[[deprecated("…")]]` annotations pointing at
  `SettingsStore::instance()` and `list_mcp_servers()`. Three
  call sites inside the library still use them (the `migrate.cpp`
  one-shot CLI, plus the daedalus-subagent / daedalus-manager
  demos) — the migration is left for a future release. CMake
  emits the warning cleanly; no errors.

- **Documentation sweep (Readme.md, docs/embedding.md,
  docs/mcp_server.md, docs/session_memory.md, docs/storage.md,
  docs/tools.md, docs/how_compress_works.md,
  docs/migration.md):**
  - Readme.md `StdoutSink` example no longer references the
    removed `AgentSink::request_user_input(std::string_view)`.
  - Readme.md "Configuration" section replaced the hand-edited
    `config.json` walkthrough with a programmatic example using
    `SettingsStore::upsert_model()`, plus a `sqlite3` one-liner
    for packagers.
  - Readme.md "File Locations" table now shows the v0.4.0+ DB
    path (`~/.local/share/daedalus/daedalus.db`).
  - Readme.md "Current tag" bumped from 0.2.5 → 0.4.1.
  - docs/embedding.md `Settings, memory, MCP paths` section
    rewritten to describe the v0.4.0 SQLite model and explicitly
    call out the removed `cfg.settings_path` / `cfg.mcp_*_path` /
    `cfg.memory_dir` fields.
  - docs/embedding.md state-machine table no longer lists
    `AwaitingUser` (removed in v0.4.0 with the `ask_user` tool).
  - docs/embedding.md `Wiring your own input source` section
    no longer claims the agent calls `request_user_input` mid-turn.
  - docs/mcp_server.md: added a "Configuration (v0.5.0+,
    recommended)" section using `SettingsStore::upsert_mcp_server`,
    kept the legacy JSON section as a fallback.
  - docs/session_memory.md "Storage location" tree no longer
    shows `settings.json` — settings live in `daedalus.db` now.
  - docs/storage.md §2.2: removed the `agent_dir/config.json`
    row (config moved to the user-tier SQLite DB).
  - docs/tools.md §14: replaced the removed `ask_user` reference
    with `set_status` (the actual current tool).
  - docs/how_compress_works.md "Per-Model Context Size" now uses
    `SettingsStore::upsert_model()` instead of a JSON snippet.
  - docs/migration.md §5: notes that `settings.json` was moved
    to SQLite in v0.4.0 (the migration guide previously claimed
    "unchanged").

### Added (2026-07-15)

- **Webui "Tool Calls" panel — structured cards** (§Phase D1).
  Each tool invocation now renders as its own card with a header
  (tool name + status icon + stage pill), a collapsed args block,
  a progress bar that updates in real time from `tool_progress`
  events, and a final result block (or error message). Replaces
  the previous flat "→ name ← result" line stream that mixed
  unrelated tool calls together.
  - **Correlation key:** OpenAI-style `call_id` (the same id the
    LLM uses to match tool calls to tool results). Threaded
    through to all three SSE event types via new 3-arg
    `AgentSink` overloads (`on_tool_call(name, call_id, args)`,
    `on_tool_result(name, call_id, result)`,
    `on_tool_progress(name, call_id, progress)`). The 2-arg
    overloads are kept as back-compat; sinks that don't care
    about correlation (ConsoleAgentSink, test stubs) keep
    overriding only those — the 3-arg default delegates.
  - **Card states:** `running` (blue border, indeterminate
    shimmer bar until percent arrives, spinner icon),
    `success` (green border, ✓ icon, result block),
    `error` / `denied` (red border, ✗ icon, error result).
  - **Idempotency:** if a `tool_call` arrives for an id that
    already has a card (e.g. a retried invocation), the args
    are refreshed in place — no duplicate card is created.
  - **Out-of-order tolerance:** `tool_progress` for an unknown
    id is silently dropped (LLMClient emits `tool_call` before
    the tool runs, so this should never happen, but defensive).
  - **SSE event payload changes:**
    `tool_call`, `tool_progress`, `tool_result` now include
    `"id": "<call_id>"` at the top level. Hosts that ignore the
    id (or use a different correlation scheme) are unaffected.

- **Unit test** (`daedalus_sink_call_id_tests`, 5 cases, 17
  assertions, all pass) for the new `AgentSink` 3-arg
  overloads:
  - D1.1 — 3-arg sink receives `call_id` for all 3 events
  - D1.2 — empty `call_id` still produces an event
  - D1.3 — back-compat: 2-arg-only sink still fires when called
    through a base pointer (which is how LLMClient does it)
  - D1.4 — default no-op sink swallows everything
  - D1.5 — same id across the 3 events enables correlation

- **CSS for the new panel:** `.tool-card` and its variants
  (`running` / `success` / `error` / `indeterminate`),
  `.tool-card-head`, `.tool-card-name`, `.tool-card-icon`,
  `.tool-card-stage`, `.tool-card-args`, `.tool-card-progress`,
  `.tool-card-progress-bar` (with `shimmer` keyframes for
  indeterminate state), `.tool-card-message`, `.tool-card-result`.
  Dark theme, monospace, ~80 lines.

### Added (2026-07-15)

- **Streaming tool-call progress events** (§Phase C1).
  Long-running tools (`execute_bash` with big builds, network ops,
  MCP calls) now report progress between `on_tool_call` and
  `on_tool_result` so hosts can show live feedback instead of
  freezing until the result lands.
  - **Public API:** `AgentSink::on_tool_progress(name, progress_json)`
    — virtual method on the sink interface. Default: no-op.
  - **Tool opt-in:** `BaseTool::execute(args, file_io, ToolProgressFn)`
    — new 3-arg overload. Default 2-arg `execute()` still works
    unchanged for the 18 tools that don't need progress.
  - **JSON shape:** `{ "stage": "...", "percent": 0..100, "message": "..." }`
    with `stage ∈ { "starting" | "executing" | "finalizing" | "done" }`.
  - **BashTool** is the first tool to emit progress (two events:
    `"starting"` at 0% and `"executing"` at 50% before the
    `BashCommandExecutor::execute` call returns).
  - **Demo sinks wired:**
    - `ConsoleAgentSink` (CLI) — compact one-liner per event:
      `  ↳ execute_bash: executing (50%)`
    - `WebSseSink` (webui) — broadcasts as a new SSE event type
      `tool_progress` with `{ "name", "progress" }` payload. The
      webui JS can render a live status bar from it.
  - **Test added:** `daedalus_tool_progress_tests` (4 cases, 14
    assertions, all pass):
    - `P1+P2`: BashTool emits `starting` + `executing` with the
      right JSON shape
    - `P3`: pre-execution rejection (privilege escalation) fires
      **zero** progress events
    - `P4`: legacy 2-arg `execute()` still works (back-compat)
    - `P5`: `ToolHandler::execute_tool_call` wires the closure
      through to the sink the same way `LLMClient` does

### Changed (2026-07-13)

- **XDG-compliant storage layout implemented in full.**
  Per-user state (`settings.json`, `mcp/servers.json`, `mcp/.env`,
  `skills/`) now resolves under XDG-compliant locations
  (`$XDG_CONFIG_HOME/daedalus/`, `$XDG_DATA_HOME/daedalus/`) instead
  of the flat `~/.daedalus/` layout. The legacy layout is honoured
  as a READ fallback for one release cycle with a one-shot
  `[DEPRECATION]` warning via `AgentSink`.

  **Env-var changes:**
  - **`$DAEDALUS_HOME`** is now the single-root override (was a
    no-op; the legacy `$DAEDALUSLIB_HOME` / `$CODEMAXLIB_HOME` vars
    are no longer consulted at the library layer).
  - **`$DAEDALUS_CONFIG_HOME` / `$DAEDALUS_DATA_HOME` /
    `$DAEDALUS_STATE_HOME` / `$DAEDALUS_CACHE_HOME`** override
    individual XDG roots.
  - All 4 standard `$XDG_*_HOME` vars honoured as before.

  **Public API additions:**
  - `daedalus::Paths` abstract interface + `Paths::global()` singleton
    + `Paths::set_override()` for testing.
  - `daedalus::ProjectPaths` per-project resolver (namespaced by
    `agent_name`).
  - `Paths::ensure_directories()` — auto-creates the four XDG roots
    with `0750` (config/data) / `0700` (state/cache) — secure
    default permissions for the four XDG dirs.
    Called from `AgentImpl::initialize()` so subsystems downstream
    don't have to think about mkdir + chmod.

  **CLI additions:**
  - `daedalus-cli --migrate-data`            — dry-run (prints plan)
  - `daedalus-cli --migrate-data-apply`      — actually copy files
  - `daedalus-cli --migrate-data-from DIR`   — non-default legacy root
  - `daedalus-cli --migrate-data-verbose`    — print every file copy

  The legacy `~/.daedalus/` directory is **never** removed by the
  library — the user does that manually after verifying the new layout.

  **New docs:** `docs/storage.md` (379 lines, source of truth for
  storage questions). `containers/Dockerfile` +
  `containers/docker-compose.yml` + `containers/README.md` (XDG-clean
  reference deployment).

  **Migration recipe:** `daedalus-cli --migrate-data-apply` → verify →
  `rm -rf ~/.daedalus`. See `docs/storage.md` §3 for details.

### Removed (effective 2026-07-13)

- **`default_memory_dir()` from `<daedalus/default_paths.h>`**.
  This used to return `~/.daedalus/memory/` and was the API that
  *enabled* the bug the XDG migration fixes — global memory shared
  every agent and every project. It was first deprecated in Phase 1
  (shim returning the legacy path) and is now **REMOVED**.
  All callers must use `daedalus::ProjectPaths::memory_dir()` instead,
  which returns the per-project, per-agent namespaced path.

### Removed (effective next release)

- **`$DAEDALUSLIB_HOME`** env var: silently no longer consulted at
  the library layer. Use `$DAEDALUS_HOME` instead.
- **`$CODEMAXLIB_HOME`** env var: same — replaced by `$DAEDALUS_HOME`.
  These vars were documented in earlier versions; they no longer have
  any effect.

### Changed (2026-07-12)

- **`$DAEDALUS_HOME` env var is no longer consulted by codemax-lib.**
  It belongs to the original codemax agent (different project,
  different conventions). The new env var is **`$CODEMAXLIB_HOME`**,
  with identical semantics (tier-3 fallback for the config home).
  Hosts that explicitly set `AgentConfig::settings_path` or
  `::daedalus_home` are unaffected. If `$DAEDALUS_HOME` is set in the
  shell, codemax-lib deliberately ignores it and falls through to
  `$HOME/.codemax`.

### Added (Phase 8.7, 2026-07-11)

- **Per-model `temperature` config.** Each model entry in
  `settings.json` may declare `"temperature": 0.7` (or any value in
  `0.0–2.0`). Hosts can override per-session via
  `cfg.temperature = 0.7`. The library sends `{"temperature": X}` on
  both the main chat request (`run_agent_cycle`) and the
  context-compression summary request (`call_llm_for_summary`).
  Resolution priority (highest first):
    1. `cfg.temperature >= 0.0` — explicit session override
    2. Model's value from `settings.json::models.<name>.temperature`
    3. `0.0` — library default (deterministic)
  Default `cfg.temperature = -1.0` is the sentinel meaning "use the
  model's value". See `docs/embedding.md` for the priority chain and
  code examples.

### Changed (Phase 9.9, 2026-07-11)

- **Default compiler switched from gcc → clang++.** CMake now pins
  `clang++` as the default C++ compiler. The pin runs before
  `project()` so the choice gets into the `CMAKE_CXX_COMPILER` cache
  before CMake latches onto system gcc. Users who prefer gcc can
  still override with `CC=gcc CXX=g++ cmake -S . -B build`. Verified
  clean build under clang 21.1.8.
- **clang-tidy infrastructure added.** New targets
  `codemax_clang_tidy_lib`, `codemax_clang_tidy_tests`,
  `codemax_clang_tidy_demos`, plus master aliases `codemax_clang_tidy`
  and `codemax_static_analysis`. Conservative rule set
  (`clang-analyzer-*`, `cppcoreguidelines-*` with noise disabled,
  `modernize-*`, `performance-*`, `readability-*`) — see `.clang-tidy`.
  HeaderFilterRegex restricts analysis to the project's own sources so
  OmniNet warnings are not flagged.
- **`.clang-tidy` and `.clang-format` are not committed.** Each
  developer generates them at configure time. Both are listed in
  `.gitignore`.

### Added (Phase 9.8, 2026-07-11)

- **SSE streaming wired end-to-end.** `LLMClient::run_agent_cycle`
  now sends `{"stream": true}` and forwards each `data: {...}`
  chunk to a `streaming_callback`. The `MiniMaxSseParser` (file-scope
  helper in `src/api/LLMClient.cpp`) parses chunks line-by-line and
  fires `sink_->on_assistant_delta(text)` per token. Replaces the
  previous "all at once at end of turn" delivery, giving chat hosts
  (webui, CLI REPL) real-time progressive text.
- **`AgentConfig::stream`** knob (default `true`). Set to `false`
  for batch / deterministic-replay hosts (test fixtures, embedded
  batching). Both modes produce identical final text.
- **Two critical streaming bugs fixed** (uncovered by the lava-lamp
  smoke test in `/workspace/Testing/test01/`):
    1. SSE parser assumed array-shaped chunks; MiniMax sends single
       objects per line. Fixed by normalising parsed→list-of-objects.
    2. Tool calls dropped on streaming path (chunks carry `delta`,
       but `run_agent_cycle` reads `message.tool_calls`). Fixed by
       `MiniMaxSseParser` accumulating `role`, `tool_calls[]`, and
       `finish_reason` alongside text content, and exposing
       `synthesized_message()` which returns the reconstructed
       message in the standard non-streaming shape.

### Added (Phase 9.7, 2026-07-11)

- **`AgentConfig::skills_dirs` + `AgentConfig::skills`.** Developer-
  controlled Skill loading. Both default to empty (no skills
  loaded). Hosts opt in by listing directories to scan and/or
  explicit skill names to activate. Pattern copied from
  `demos/codemax-webui/main.cpp`.

### Added (Phase 9.6, 2026-07-11)

- **Sink observability gaps closed.** `AgentSink` now fires
  `on_state_change` on every state transition so hosts can render
  real-time status (busy indicator, progress bar, cancel button).
  Host-side wiring demonstrated in `demos/codemax-webui/main.cpp`.

### Added (Phase 9.5, 2026-07-11)

- **codemax-webui polish.** Brought up to date with Phase 9.6
  observability hooks and Phase 9.7 skill loading. Demonstrates the
  full Sink pattern (BashTool ask_user, file logger, SSE delivery).

### Changed (2026-07-10)

- **Phase 7 demo set reduced from 5 to 4.** The `codemax-tui` (FTXUI)
  demo is cancelled. The remaining demo set is `codemax-cli`,
  `codemax-manager`, `codemax-subagent`, `codemax-webui`. TUI was
  redundant with `webui` (both stream `on_assistant_delta` events) and
  the web version is what developers actually want to reference. Hosts
  that want a TUI write their own ~200-line embed using the patterns
  from `demos/codemax-webui/` as the template. The CMake option
  `CODEMAX_DEMO_TUI` has been removed. Plan updated in `Agent.md`.

- **`codemax-webui` uses OmniNet, not cpp-httplib.**
  [OmniNet](https://gitlab.com/leestripp/omninet) is already a codemax
  dependency (FetchContent from GitLab, MIT, same author), providing
  HTTP server, SSE streaming, WebSocket, TLS, and static file serving.
  Reusing it means zero new external libraries for the demo, one
  license to review, and built-in SSE thread-safety. Patterns to copy
  in the demo already exist in this repo as
  `demos/http_server_demo` + `tests/test_sse_streaming`. Plan updated
  in `Agent.md`.

### Fixed (Phase 8.6, 2026-07-10)

- **`LLMClient` no longer uses the wrong model's `max_context_size` in
  the in-session token-usage stats.** Previously, running with
  `--model minimax-m2.7` (200K context) showed
  `Token Usage: N/500000` because the code asked settings for
  `default_model`'s context — which is `minimax-m3` (500K). Now
  `LLMClient` resolves the actually-selected model's context via
  `SettingsManager::get_max_context_size_for_model(config_.model_name)`.
  In-session progress bar shows the right number for the running model.
- **codemax-cli banner no longer hides the `auto_approve` setting from
  settings.json.** Previously the banner showed only `cfg.auto_approve`
  (false unless `--auto` was passed), missing the value from
  `settings.json::auto_approve` even when the library's tool
  dispatcher was actually using it. Now the banner calls
  `Agent::auto_approve()` which returns the *effective* value
  (`cfg.auto_approve || settings.json::auto_approve`).

### Added (Phase 8.6, 2026-07-10)

- **`Agent::active_model()`** — returns the `ModelInfo` (name, model_id,
  host, chat_endpoint, max_context_size) for the model that subsequent
  turns will hit. Resolution priority matches `LLMClient`:
  `cfg.model_name` → settings.json `default_model` → first model.
- **`Agent::library_version()`** — static, returns the git hash of the
  linked `libcodemax.a` (compiled in as `CODEMAX_GIT_VERSION` PUBLIC
  definition in `CMakeLists.txt`).
- **`AppConfig::model_name`** — settings.json key for the selected model
  (e.g. `"minimax-m2.7"`), populated by `make_app_config()` from
  `cfg.model_name`. Used by `LLMClient` to resolve per-model context.
- **codemax-cli startup banner** — now shows library version, active
  model + model_id + endpoint + max context, plus the runtime
  `daedalus_home` / `project_root` / `session_id` / `memory_enabled` /
  `auto_approve` / `max_iterations`. Hosts can verify config at a glance.

### Added (Phase 8.5, 2026-07-10)

- **`AgentConfig::daedalus_home` field** — per-Agent config directory.
  When set, empty derived fields (`settings_path`, `mcp_servers_path`,
  `mcp_env_path`, `memory_dir`) are resolved relative to this directory.
  Two `Agent` instances in one process can now have entirely separate
  settings, MCP servers, and memory without env vars or separate
  filesystem paths. See `docs/embedding.md` for the priority table.

### Changed (Phase 2.7, 2026-07-10)

- **`--manager-mode` flag removed from the CLI demo.** The orchestration
  pattern is preserved as a demo composition (`demos/codemax-manager/`,
  Phase 7).
- **`--sub-agent` and `--sub-agent-looping` flags removed from the CLI
  demo.** The sub-agent pattern is preserved as a demo composition
  (`demos/codemax-subagent/`, Phase 7).
- **`LLMClient::run_manager_mode()` deleted.** The Agent has exactly
  one loop (`run_one_turn`).
- **`LLMClient::get_manager_system_prompt()` deleted.** Hosts supply
  their own role prompt via `AgentConfig::system_prompt`.
- **CLI help text updated** to reflect the new flag set.

### Added (Phase 6, 2026-07-10)

- **`daedalus::Agent` public facade** (`include/codemax/Agent.h`). Hosts
  no longer include internal subsystem headers (`LLMClient`,
  `SettingsManager`, ...). The class is PIMPL over
  `std::unique_ptr<detail::AgentImpl>`.
- **`daedalus::AgentSink`** (`include/codemax/AgentSink.h`). The single
  callback interface the library uses to talk to the host.
- **`daedalus::AgentConfig`** (`include/codemax/types/AgentConfig.h`).
  Consolidated config struct passed to the Agent's constructor.
- **`daedalus::AgentState`** (`include/codemax/types/AgentState.h`).
  Public state enum (Idle, Understanding, Planning, Acting, Verifying,
  AwaitingUser, AwaitingApproval, Compressing, Done, Cancelled, Error).
- **`daedalus::AgentStats`** (`include/codemax/types/AgentStats.h`).
  Read-only stats snapshot (token counts, iteration, message counts).
- **`daedalus::default_paths.h`** — helpers that compute
  `~/.codemax/...` defaults honouring `$HOME` (the env var that
  controls the codemax home directory; no other env vars are read).
- **`docs/embedding.md`** — embedding guide for new host authors
  (includes the full `AgentSink` contract; `docs/sink.md` merged here in v0.3.9).

### Changed (Phase 3, 2026-07-09)

- **SSE streaming** replaces the thinking-spinner. `LLMClient` now
  parses `data: {...}` chunks line-by-line and calls
  `sink_->on_assistant_delta(text)` per token.
- **`execute_bash` approval** routes through
  `sink_->request_command_approval(cmd)`.
- **`ask_user` tool** routes through `sink_->request_user_input(prompt)`.
- **HTTP error / retry / warning messages** route through
  `sink_->on_log(level, msg)` instead of `std::cerr`.
- **`ConsoleAgentSink`** in `demos/codemax-cli/` — the CLI demo's
  concrete sink (cout for tokens, cerr for logs, linenoise for
  user input, y/n prompt for command approval).

### Changed (Phase 2.5, 2026-07-08)

- **`LLMClient::run_interaction()` deleted.** The REPL loop is now
  owned by `demos/codemax-cli/main.cpp`. The library exposes
  `run_one_turn(user_message)` instead.

### Changed (Phase 2, 2026-07-08)

- **`SettingsManager`** is no longer a singleton. Constructed with
  `(settings_path)` and owned by `Agent`.
- **`SkillManager`** is no longer a singleton. Same pattern.
- **`McpServerManager`** is no longer a singleton. Same pattern.

### Changed (Phase 1+, 2026-07-07)

- **`src/main.cpp` moved to `demos/codemax-cli/main.cpp`.** The CLI
  is now a demo that links against `libcodemax.a`.
- **`libcodemax.a`** is the primary build artifact. The library is
  built first; demos are built second against it.
- **`codemax.h`** is the umbrella header (one `#include` for the
  entire public API surface).

### Added (Phase 1, 2026-07-07)

- **`include/codemax/`** directory created.
- **`include/codemax/types/`** directory created.
- **`include/codemax/Agent.h`** (stub) created.
- **CMake static-library target** `codemax` (`add_library(... STATIC)`).
- **`install(TARGETS codemax ARCHIVE DESTINATION lib)`** for
  downstream `find_package` / raw link.

## [v0.4.0] — 2026-07-15

Settings refactor cutover + OmniNet bug resolutions.

### Added (v0.4.0)

- **`daedalus::SettingsStore` public API** in `include/daedalus/settings.h`:
  - `SettingsStore::instance()` — global default DB handle
  - `SettingsStore::for_home(path)` — per-agent isolated handle
  - CRUD for models, kv_settings, mcp_servers, serper_config
  - `transaction()` for atomic batches
  - `db_path()`, `has_data()` introspection

  SQLite-backed (`$XDG_DATA_HOME/daedalus/daedalus.db`), replaces the
  scattered JSON files. Hosts that use libdaedalus no longer touch
  `config.json` / `mcp/servers.json` / `mcp/.env` directly.

- **`tests/test_settings_store.cpp`** — 9 unit tests for the public
  API (CRUD, transaction, for_home isolation).
- **`tests/test_settings_store_e2e.cpp`** — end-to-end wire-in test
  (write via `SettingsStore` → read via `Agent.list_models()`).
- **`tests/test_settings_v040_cutover.cpp`** — 5 cutover tests pinning
  the SQLite-only semantics.
- **`tests/test_settings_migrator.cpp`** — 10 tests for the JSON→SQLite
  auto-migrator.
- **`omninet_bugs.md`** — tracks OmniNet bugs found via daedalus.

### Changed (v0.4.0)

- **`src/core/SettingsManager::load()` is now SQLite-only when the
  store has data.** JSON is consulted only when the store is empty.
  Stale scalars in `config.json` can no longer leak into a populated
  store.
- **Validation requires both `default_model_name` AND a non-empty
  `models` map** (was: either one threw spuriously).
- **Per-project state (session memory + messages)** moved into the
  `daedalus.db` store under `mem.<sid>` / `msg.<sid>` keys, with JSON
  files retained as a read fallback (v0.3.5 → v0.4.0 step).
- **`SettingsStore::list_models()` uses range-based iteration** again.
  The OmniNet callback-overload workaround has been removed — see
  "Fixed (v0.4.0)" below.

### Removed (v0.4.0)

- **`AskUserTool`** — broken and unused. Removed from
  `src/tools/UserTool.{h,cpp}`, plus the related plumbing:
  - `request_user_input()` from `AgentSink` (and both demo sinks)
  - `AgentState::AwaitingUser` enum value
  - CHAT → AwaitingUser mapping in `AgentImpl`
  - ask_user handling block in `LLMClient::run_agent_cycle()`
- **Stale "ask_user" / "AwaitingUser" references** throughout the
  library, demos, and tests.

### Fixed (v0.4.0)

- **OMNINET-1 (Bug 1) — `QueryResult` lost the first row's data on
  range-based iteration.** The constructor stepped to the first row
  but did not populate `m_currentRow`, so `for (const auto& row :
  db.query("SELECT ..."))` yielded N rows with the first empty and
  the real first row missing. Fixed upstream in OmniNet **0.8.7**
  (`Database.cpp` lines 335-344: populate `m_currentRow` after
  `step()`). daedalus workaround (callback overload) was retained
  until Bug 2 below was also confirmed fixed.
- **OMNINET-2 (Bug 2) — `QueryResult` returned a phantom empty row
  on the first range-based query after a DELETE or DB-file reopen.**
  The `Iterator` constructor blindly copied `m_currentRow` without
  honouring `m_atEnd`, so `begin() != end()` on empty result sets.
  Fixed upstream in OmniNet **0.8.8-1-gfbecf55**
  (`Database.cpp:392-404`: check `result->m_atEnd` and mirror it on
  the iterator). Upstream added 4 regression tests
  (`database_query_range_empty_after_delete`,
  `database_query_range_empty_after_reopen`,
  `database_query_range_empty_table`,
  `database_query_range_after_delete_then_insert`). OmniNet's own
  suite went 121/121 → 125/125. **Workaround removed** from
  `src/core/SettingsStore.cpp::list_models()`. Closed in daedalus.

### Migration notes (v0.4.0)

- First run on a system with existing JSON config: auto-migrator
  reads `config.json` / `mcp/servers.json` / `mcp/.env`, writes
  rows to `daedalus.db`, and renames the originals to
  `*.migrated`. No user action required.
- Placeholder API keys (`YOUR_API_KEY_HERE`) are skipped by the
  migrator — they'd fail validation later.
- `SettingsManager` itself is still present (would require touching
  many call sites to delete) but is now a thin SQLite shim. Future
  cleanup: route `AgentImpl::initialize()` straight through
  `SettingsStore::instance()` and delete `SettingsManager`.

## [0.x] — Pre-refactor

Single-executable CLI agent. See git history. Features retained in
the refactor: multi-provider LLM support, agent skills (open standard),
40+ tools, MCP integration, session memory, context compression
("70/30 split"), background processes, manager mode, sub-agent
looping, sub-agent mode.

[Unreleased]: https://gitlab.com/leestripp/daedalus/-/compare/1.0.0...main
[1.0.0]: https://gitlab.com/leestripp/daedalus/-/compare/0.10.3...1.0.0