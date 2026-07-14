# AGENT RULES — non-negotiable, do not rewrite history

This file is a permanent, project-scoped guardrail for any agent
(CodeMax, MCP tool, future session) working in this repo.

## The rules

1. **NEVER** `git push --force`, `git push -f`, `git push --force-with-lease`,
   `git push --force-if-includes` — under any circumstances, on any branch.
2. **NEVER** `git rebase -i` on a branch that exists on `origin`.
3. **NEVER** `git commit --amend` after the commit has been pushed.
4. **NEVER** `git filter-branch` or `git rebase --root` followed by a push.
5. **NEVER** `git push` from the agent — the user pushes. Always.
6. **NEVER** silently "fix" something that looks wrong with the remote. STOP
   and tell the user. They fix it manually.
7. If undoing a pushed commit is needed, propose **`git revert`** (adds a
   new commit, preserves history). Never `git reset --hard`.

## Why these rules exist

Past agents (including earlier CodeMax sessions on this project) have
broken this repo and others by:

- Force-pushing to overwrite remote history
- Doing interactive rebases the user didn't notice
- Committing 25+ times before the user spotted something wrong
- Leaving `local/main` and `origin/main` so far out of sync that the
  user had to nuke and re-create the remote repo

Each recovery cost the user real money (GitLab restore + project time)
and several hours of fixing. The constraint set above exists so that
pattern doesn't repeat.

## File location

The original version of this file was committed at `.claude/AGENT_RULES.md`,
which was wrong — this project is **CodeMax**, not Claude. It has been
moved to `docs/AGENT_RULES/AGENT_RULES.md`. Future agents working in
this repo should look here, not under `.claude/`.
