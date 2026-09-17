# Bug: discover_skills never rescans; project-local skills silently missed

**Reported:** 2026-09-17 (found from the HiveMind project while creating a
project-local skill)
**Severity:** medium — documented workflow is broken; project-local
discovery silently no-ops under GUI hosts
**Suggested fix:** `./skill-discovery-fix.patch` (compiles clean against
current main; was applied, build-verified, then reverted — apply with
`git apply skill-discovery-fix.patch`)

## Symptoms

1. Create a new skill (e.g. `.agents/skills/deploy-hivemind/SKILL.md`),
   call `discover_skills` — the new skill never appears, no matter how
   many times you call it, despite the tool description explicitly
   saying "then call discover_skills again to pick it up."
2. `skill_directories` in the response lists only the two user-global
   dirs — the project-local `.agents/skills/` is absent even as
   `exists: false`.
3. Passing the optional `directory` argument changes nothing.

## Root causes (three, compounding)

**A. The handler returns a session-start cache, forever.**
`DiscoverSkillsTool::execute` (src/tools/SkillTool.cpp) reads
`get_all_cached_skills()` and never triggers a rescan. The cache is
populated once at agent init (`discover_all_skills()` in
AgentImpl.cpp); `refresh_cache()` exists but is only called from
`add_skill_directory()`. So mid-session skill creation is invisible —
the documented create-then-rediscover workflow cannot work.

**B. The `directory` argument is silently ignored.**
The tool schema advertises an optional `directory` parameter, but the
signature is `execute(const json& /*args*/, ...)` — the arg is never
read.

**C. Project-local default is CWD-relative.**
`SkillManager::init_after_settings()` pushes literal
`.agents/skills/` — resolved against the **daemon process CWD**, not
the session's project root. MonkeyByte (and any GUI host) launches the
daemon from its own directory, so project-local discovery silently
matches nothing. `AgentConfig.project_root` is available at the
construction site (AgentImpl.cpp, `body_->skill_manager`) but is never
passed down.

## Suggested fix (in the patch)

1. `init_after_settings(const std::filesystem::path& project_root = {})` —
   anchors `<project_root>/.agents/skills` when provided; empty keeps
   the legacy CWD-relative behaviour for tests / daedalus-cli.
2. AgentImpl passes `cfg_.project_root` at SkillManager construction.
3. `DiscoverSkillsTool::execute`: honour `directory` (fresh scan of
   that dir, bypassing the cache); otherwise `refresh_cache()` before
   reading the cache so discovery is always current.

Note: the patch's directory-branch uses `discover_skills()` (full
`Skill` objects) because `SkillMetadata` has no `path` member.
