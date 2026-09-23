# Library Design Rules

> **Audience:** Core contributors writing code that links into
> `libdaedalus.a`. Hosts that *use* the library don't need to read
> this — these are the rules we follow internally so the library
> stays composable.

This document collects the permanent invariants that `libdaedalus.a`
must satisfy. They were previously scattered across `CodeMax.md`
(working memory) and review comments; they now live here as the
canonical spec.

---

## Rule 1 — NO stdio inside libdaedalus

**The library MUST NOT call `std::cout`, `std::cerr`, `std::cin`,
`printf`, `fprintf(..., stderr)`, or any function that writes directly
to stdout/stderr/stdin.** Applies to all of `src/library/`, `src/api/`,
`src/core/`, `src/mcp/`, `src/tools/` — anywhere linked into
`libdaedalus.a`.

### Why

A host that builds a TUI (ncurses, blessed, ratatui, raw terminal
mode, vim-style status line, …) needs total control of stdio. Library
code that reaches for `std::cout` or `std::cerr`:

- Corrupts the terminal (random escape sequences / cursor moves).
- Blocks the host's input-handling thread.
- Eats escape sequences the host is parsing.
- Breaks mouse / keystroke routing.

Once any library call leaks a single `\n` into a raw-mode terminal,
the host is dead in the water until the user runs `stty sane` or
restarts it.

### Output channels the library MAY use

1. **`AgentSink`** (host-controlled callback) — primary I/O path for
   user-visible text. Hosts wire this to their own stdout, a log
   file, an in-memory buffer, whatever fits the host's I/O model.
2. **`core/Logger.h`** — `LOG_DEBUG` / `LOG_INFO` / `LOG_WARNING` /
   `LOG_ERROR` macros for internal diagnostics. Writes to a per-project
   log file under `<project>/.daedalus/daedalus[-<name>]/logs/`. Does NOT
   touch stdio. The logger self-initializes on first use: `AgentImpl`
   initializes it with the project-scoped per-agent directory during
   `Agent::initialize()`; a bare `LOG_*` call before that falls back to
   `<cwd>/.daedalus/daedalus/logs/` (marked auto-initialized, and upgraded
   to the project path when the agent initializes). All filesystem
   failures are swallowed — logging never throws into library code.

### Output channels the library MUST NOT use

- `std::cout`, `std::cerr`, `std::clog`, `std::cin`
- `printf`, `fprintf(stderr, ...)`, `fputs(..., stderr)`, `puts`
- `write(1, ...)`, `write(2, ...)` to stdout / stderr fds

### Edge cases

- **File I/O is fine.** `fopen(path, "rb")` reads/writes *files*, not
  the host's stdio. `<cstdio>` for that is OK.
- **POSIX constants are fine.** `STDOUT_FILENO`, `STDERR_FILENO`,
  `STDIN_FILENO` are used in `Subprocess.cpp` and
  `BackgroundProcessManager.cpp` to re-route child-process stdio
  through pipes. The library never writes to those fds itself —
  they exist only as targets for the *child's* dup2.
- **Demos (`demos/daedalus-cli/`) are explicitly OUT OF SCOPE.**
  Demos own their terminal and use `std::cout` directly. Hosts can
  use whatever they want; this rule only applies to what gets
  linked into the static library that hosts consume.

### Verification

**Static check:** `grep -rn "std::cout\|std::cerr\|std::cin"
src/library src/api src/mcp` should return no matches outside
comments.

**Runtime check:** override `std::cout` and `std::cerr` rdbuf with
a counting `stringbuf`, run a full agent lifecycle (initialize →
run a turn → shutdown), and assert the counter is **0**.

Both checks were green as of 2026-07-16.

---

## Adding a new rule

1. Add a section above with the rule, the rationale, and an example.
2. Add a verification step that's automatable (a grep, a static
   assertion, a unit test, …).
3. Update `docs/embedding.md` if host developers need to know.
4. Reference the rule in code review checklists.