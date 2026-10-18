# Plan.md — daedalus Second Brain (Global Knowledge Base)

> **Status**: IMPLEMENTED (2026-10-18) — Phases 1–3 done: SecondBrain core,
> schema v10 (brain_notes + brain_fts), `brain` tool registered, 21 new tests
> (B1-B14, T1-T6), ctest 109/109 green, static_analysis clean. Design deviations
> from §2/§3 below: ONE action-dispatched `brain` tool instead of seven
> brain_* tools (VaultTool pattern — one compact schema riding every request);
> note id = clean slug with -2/-3 collision suffixes (no timestamp component);
> reindex diffs mtime+size (no sha256 — cheap and sufficient for a local cache);
> no SystemPromptBuilder prose section ("no prose" philosophy — usage guidance
> rides in the tool's own description, like the vault).
> **Supersedes**: the monorepo-consolidation plan previously in this file
> (all phases DONE; preserved in git history, last at commit 556b317).

---

## 1. Research — what a "Second Brain" is

The term was popularised by Tiago Forte's *Building a Second Brain* (BASB):
an external, trusted system that stores everything you learn so your
biological brain can forget it. Forte's workflow is **CODE** —
**C**apture, **O**rganize, **D**istill, **E**xpress — and his organization
scheme is **PARA**: **P**rojects, **A**reas, **R**esources, **A**rchives
(sorted by *actionability*, not by topic).

The other major school is **Zettelkasten** (Niklas Luhmann's slip-box):
many small *atomic notes* (one idea per note), each with a unique ID,
densely interlinked, so structure *emerges* from links instead of being
imposed by folders. Modern digital implementations (Obsidian, Logseq,
Foam, Zettlr) are all **plain-markdown file stores with wiki-links and
full-text search** — no database required as the source of truth.

A third, newer pattern matters most for daedalus: **agent memory
runtimes**. Current open-source practice (sqliteai/sqlite-memory,
tigerless-labs/agent-memory, and numerous agent-memory writeups) has
converged on exactly the same architecture, deliberately *without* a
vector database:

> **Plain Markdown files are the source of truth; a SQLite FTS5 index
> provides fast ranked search and is rebuildable from the files at any
> time.**

Semantic/vector search is consistently listed as an *optional later
layer*, not a prerequisite — FTS5 keyword search over well-titled atomic
notes is good enough, and keeps everything local, inspectable, and
git-friendly.

### Comparison of candidate approaches

| Approach | Pros | Cons | Verdict for daedalus |
|---|---|---|---|
| PARA folders | Simple, actionable, zero tooling | Shallow linking, notes can only live in one place | Good for top-level layout |
| Zettelkasten | Emergent knowledge web, atomic notes, links | Needs discipline; IDs/links need tooling | Good for note *format* |
| Digital garden | Notes evolve in public | Not goal-oriented | Not needed |
| Plain markdown + FTS5 | Local-first, human-readable, diffable, rebuildable index, no new deps | Keyword search only (for now) | **Chosen core** |
| Vector DB / embeddings | Semantic recall | New heavy dependency, opaque, overkill at this scale | Later, optional layer |

## 2. Decision — the daedalus Second Brain

**A global, markdown-file knowledge base indexed by SQLite FTS5, living
in the user's XDG data home next to the existing global database.**

Concretely:

```
~/.local/share/daedalus/            (or $XDG_DATA_HOME/daedalus)
├── daedalus.db                     ← existing global DB (vault, providers, …)
│                                     + NEW brain_fts FTS5 index table
└── brain/                          ← NEW: the Second Brain (source of truth)
    ├── projects/                   ← PARA: active work (per-effort knowledge)
    ├── areas/                      ← PARA: ongoing responsibilities (this codebase, fleet ops…)
    ├── resources/                  ← PARA: reference material (research, how-tos, snippets)
    ├── archives/                   ← PARA: inactive/completed material
    └── inbox/                      ← quick captures, unsorted (CODE: Capture first)
```

Design principles borrowed from the research:

1. **Markdown files are the source of truth** — human-readable,
   editable in any editor, diffable, never locked in. The SQLite index is
   a *derived cache*: `brain_reindex` rebuilds it from disk at any time.
2. **Atomic notes, Zettelkasten-style** — one idea per note, unique ID,
   `[[wiki-links]]` between notes so knowledge webs emerge.
3. **PARA-lite folders** — five top-level folders give actionability
   ordering without deep hierarchies. Notes can be *filed* (moved between
   folders) as they age from inbox → resources → archives.
4. **Global, not per-project** — exactly like the vault: one brain per
   user, reachable from every project and every agent, independent of
   `AgentConfig::daedalus_home`. Precedent: `Vault` + `SettingsStore`
   already do this via the XDG path logic in `src/library/paths.cpp`.

### Note format (YAML frontmatter + body)

```markdown
---
id: 2026-10-18-sqlite-fts5-unicode61
title: SQLite FTS5 unicode61 tokenizer quirks
tags: [sqlite, search, daedalus]
created: 2026-10-18
updated: 2026-10-18
links: []
---

FTS5's unicode61 tokenizer strips punctuation from terms, so C++ symbol
searches ("std::string_view") match poorly; quote the phrase or use
trigram tokenizer instead. See [[2026-10-18-brain-search-design]].
```

- `id` doubles as the filename: `<id>.md` inside a PARA folder.
- Frontmatter is parsed leniently (title/tags/dates only — no full YAML
  dependency; the existing code style avoids new deps).

### Search: FTS5 in the existing global DB

- New schema migration (next version after current) adding:
  - `brain_notes` — metadata table (id, path, folder, title, tags, created, updated, sha256, mtime) so reindexing is incremental.
  - `brain_fts` — FTS5 virtual table (`title`, `tags`, `body`) with `porter unicode61` tokenization, external-content or contentless mode keyed by note id.
- `snippet()`/`bm25()` give ranked results with context excerpts.
- Verified on this machine: system SQLite has FTS5 (`FTS5_OK`).

## 3. Implementation phases

### Phase 1 — Core library (`daedalus/src/core/`)
- [ ] `SecondBrain.h/.cpp` — owns the brain dir (creates the five PARA
      folders on first use), CRUD for notes, lenient frontmatter
      parse/emit, `[[link]]` extraction, move-between-folders ("file").
- [ ] Schema migration in `SettingsStore.cpp`: `brain_notes` + `brain_fts`;
      incremental reindex (compare mtime/sha256), full rebuild fallback.
- [ ] Unit tests: round-trip frontmatter, capture/list/read/update,
      FTS5 ranking, reindex-after-external-edit.

### Phase 2 — Agent tools (`daedalus/src/tools/BrainTool.h/.cpp`)
Modeled on `MemoryTool` / `Vault` wiring (static service pointer set by
ToolFactory):
- [ ] `brain_capture(folder?, title, tags?, body)` → creates note, indexes it. Default folder `inbox` (Capture first, Organize later — CODE).
- [ ] `brain_search(query, limit?)` → ranked results with snippets.
- [ ] `brain_read(id)` → full note.
- [ ] `brain_list(folder?, tag?)` → browse.
- [ ] `brain_update(id, body?/tags?/title?)` → edit + reindex (Distill).
- [ ] `brain_file(id, folder)` → move between PARA folders (Organize).
- [ ] `brain_reindex()` → rebuild index from disk (recovery path).

### Phase 3 — Integration
- [ ] Register tools in `ToolFactory` for icarus + monkeybyte.
- [ ] `SystemPromptBuilder`: one-paragraph hint that a global Second Brain
      exists (when to capture, when to search before answering).
- [ ] `AGENTS.md` / docs update; CHANGELOG entry; version bump.

### Phase 4 — Later / optional (explicitly out of scope now)
- [ ] Semantic search layer (local embeddings via llama.cpp) over the same
      notes, merged with bm25 ranking (hybrid retrieval).
- [ ] Backlink graph ("notes that link here") UI in monkeybyte.
- [ ] Auto-capture at session end (distill session into a note).

## 4. Why this and not the alternatives

- **Not a pure-DB design**: notes must survive outside daedalus — the user
  can open `~/…/brain/` in Obsidian/Logseq/VS Code and it *is* a valid
  vault. Markdown-as-truth is the single most agreed-upon principle in
  the research.
- **Not vector-first**: every current agent-memory project treats
  embeddings as an add-on. FTS5 is already in our SQLite, needs no new
  dependencies (per project conventions), and is rebuildable.
- **Not per-project**: the whole point is cross-project recall — same
  reasoning that made the vault global. Per-project notes already exist
  (`SessionMemory` in `.daedalus/`); the brain is the layer above them.
- **PARA+Zettelkasten hybrid**: PARA answers "where does this note live?"
  (actionability), Zettelkasten answers "how does knowledge compound?"
  (atomic, linked notes). Using both is the mainstream recommendation.
