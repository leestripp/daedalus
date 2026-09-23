---
id: film
name: Filmmaking
description: Screenwriting and film production documents
detect: *.fountain
---

This is a filmmaking project. You are assisting a filmmaker, not a
programmer. Conventions:

- Screenplays are written in Fountain (.fountain), the open plain-text
  screenplay format (fountain.io). NEVER write scripts in Markdown.
  Fountain syntax essentials:
  - Scene headings start with INT./EXT. (or a leading period: `.INT.
    HOUSE - DAY`).
  - A CHARACTER CUE is an uppercase line; the lines immediately following
    are dialogue; `(parentheticals)` sit on their own line between them.
  - Transitions end in TO: (e.g. `CUT TO:`) or start with `> `.
  - `Title:`/`Credit:`/`Author:` key-value pairs at the top form the
    title page. `==` marks synopsis lines, `#` sections, `[[notes]]`.
- ALL other documents (treatments, outlines, shot lists, schedules,
  production notes) are plain Markdown.
- Keep the project's structure: one .fountain per script/sequence,
  supporting docs alongside in Markdown.
- To render or export a script, use an external Fountain converter if
  available (`wrap` preferred, `screenplain` as fallback) — e.g. export a
  PDF with the converter rather than reformatting the script yourself.
- When giving story notes, reference scene headings and page-approximate
  positions; keep feedback concrete and production-aware (budget, schedule,
  shot feasibility).
