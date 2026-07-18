# BUG-003 + BUG-004 — Discovered 2026-07-18 by §FIX-CLI-DEMO run

## TL;DR

The library has **two related bugs** that make `Agent::execute_tool()`
unreliable for the most common case: a host that runs the CLI in a
CWD that isn't the project root, and a host that passes
`cfg.auto_approve=true` without setting up an LLMClient.

Both bugs were *worked around* in `tests/test_real_world_tools.cpp`
(commit history dropped — see "Note on rebase" below). This CLI run
is the first time we hit them **end-to-end through the public API
without any test scaffolding around it**.

---

## BUG-003 (probable) — `AgentImpl::execute_tool` ignores `cfg.project_root`

### Symptom (in daedalus-cli)

```
$ /workspace/daedalus/build/demos/daedalus-cli/daedalus-cli \
      --auto --model minimax-m2.7 \
      --home /home/ubuntu/.local/share/daedalus \
      /workspace/daedalus/Testing/test-cli-bare/project
```

User prompt sent via stdin: "Read sum.cpp and tell me in one
sentence what is wrong with the buggy_sum function."

Agent response (final message, from
`/workspace/daedalus/Testing/test-cli-bare/project/.daedalus/messages.json`):

> "I couldn't find `sum.cpp` in the project — no file with that
> name exists. Could you provide the correct path or create the file?"

`sum.cpp` **does** exist in the project root:

```
$ ls -la /workspace/daedalus/Testing/test-cli-bare/project/sum.cpp
-rw-r--r-- 1 ubuntu ubuntu 78 Jul 16 01:05 sum.cpp
```

### Root cause

`src/library/AgentImpl.cpp` line 795-796:

```cpp
// FileIO needs a base path — use the current working directory.
FileIO file_io(std::filesystem::current_path());
```

The agent has `cfg.project_root = /workspace/daedalus/Testing/test-cli-bare/project`,
but the FileIO instance is built from the **process** CWD (which
is `/workspace/daedalus` — wherever you ran `daedalus-cli` from).
`FileIO::read_file_content` does NOT prepend `project_path_` —
it uses the path verbatim. So `read_file: {path: "sum.cpp"}`
looks in `/workspace/daedalus/sum.cpp` (not found) instead of
`/workspace/daedalus/Testing/test-cli-bare/project/sum.cpp`.

### Same bug, second tool

After `read_file` failed, the agent called `execute_bash: find . -name "sum.cpp"`.
Output was empty. BashTool does `cd $project_path && cmd` where
`project_path = file_io.get_project_path() = current_path()`, so
the bash command also runs from `/workspace/daedalus` (or wherever
ctest was launched from), not the agent's configured project root.

### Why this matters to anyone

`Agent::execute_tool()` is the **documented public API** for hosts
that want to drive tool calls without an LLM in the loop (test
harnesses, REPL commands like `/tools`, hand-built agent flows).
Today any host that runs from a different CWD than the project
root silently gets "File does not exist" for any relative-path
tool call.

### Suggested fix

In `AgentImpl::execute_tool`, replace

```cpp
FileIO file_io(std::filesystem::current_path());
```

with

```cpp
FileIO file_io(cfg_.project_root.empty()
                 ? std::filesystem::current_path()
                 : cfg_.project_root);
```

Or even better: cache the FileIO at Agent construction time so
`AgentConfig::project_root` is the single source of truth for all
tool calls, including LLM-driven ones.

---

## BUG-004 (probable) — `AgentImpl::execute_tool` ignores `cfg.auto_approve`

### Symptom

Same run as BUG-003. If we modify the prompt to "compile sum.cpp
and tell me if it works", `execute_bash` returns:

```json
{
  "message": "Command denied by host.",
  "status": "denied"
}
```

…even with `--auto` passed on the CLI. The host has no way to
grant approval through the public API.

### Root cause

`src/tools/ToolHandler.cpp` line 166:

```cpp
bool request_command_approval(const std::string& command) {
    // Auto mode always approves.
    if (auto_mode_enabled) return true;
    ...
}
```

`auto_mode_enabled` is only flipped to `true` inside
`src/api/LLMClient.cpp` line 230, inside the LLMClient constructor:

```cpp
if (settings_store) {
    ...
    if (settings_store->get_auto_approve()) {
        ToolHandler::set_auto_mode(true);
    }
}
```

So auto-mode is set ONLY when an LLMClient is constructed AND ONLY
when the *settings_store*'s auto_approve is true. It is NOT set
when:

1. The host only constructs an Agent (no LLMClient), OR
2. The host passes `cfg.auto_approve=true` (different from the
   settings_store's auto_approve).

In our CLI run, the host DID construct an LLMClient (because
`minimax-m2.7` resolved and the agent had explicit creds), so the
settings_store's auto_approve SHOULD have been the gate. But
because execute_bash's CWD is wrong (BUG-003), the agent never
got far enough to call execute_bash in the same run. In a
follow-up test with bash CWD fixed, the same denied-by-host
result appears.

### Why this matters to anyone

Same surface as BUG-003. Hosts building on top of
`Agent::execute_tool()` can't get auto-approval to work without
also poking `ToolHandler::set_auto_mode(true)` themselves —
which requires `#include "tools/ToolHandler.h"` (internal header)
to call. The CLI demo bypasses this by happenstance because the
LLMClient happens to wire it from the settings store.

### Suggested fix

In `AgentImpl::initialize()` (the same place where we already
call `ToolHandler::init_memory_tools(...)` via LLMClient ctor),
add:

```cpp
if (cfg_.auto_approve) {
    ToolHandler::set_auto_mode(true);
}
```

…and also in `AgentImpl::execute_tool()` for the case where the
host calls execute_tool without ever constructing an LLMClient.

---

## Repro recipe (daedalus-cli, no LLMClient setup needed)

```bash
cd /workspace/daedalus
cmake --build build -j12 --target daedalus-cli
mkdir -p /tmp/cli-repro/project
cp Testing/test-cli-bare/project/sum.cpp /tmp/cli-repro/project/
cd /tmp/cli-repro/project
/workspace/daedalus/build/demos/daedalus-cli/daedalus-cli \
    --auto --model minimax-m2.7 \
    --home /home/ubuntu/.local/share/daedalus \
    . <<< "Read sum.cpp and tell me in one sentence what is wrong"
```

**Expected:** agent reads sum.cpp, returns "The function is buggy
because it returns `a - b` when it should return `a + b`."

**Actual:** agent returns "I couldn't find `sum.cpp` in the
project — no file with that name exists." (BUG-003)
And if the agent tries `compile via bash`: `"Command denied by
host."` (BUG-004).

## Evidence

Full transcript preserved at:
`/workspace/daedalus/Testing/test-cli-bare/CLI-DEMO-EVIDENCE-2026-07-18.log`
(`.gitignore`d — lives in the integration-test fixture dir).

Conversation history persisted at:
`/workspace/daedalus/Testing/test-cli-bare/project/.daedalus/messages.json`
(also `.gitignore`d).

## Note on rebase

The unit test that first surfaced both bugs (`tests/test_real_world_tools.cpp`,
plus `tests/test_demos_hello_world.cpp` and the matching
`tests/CMakeLists.txt` entries) was committed locally as
`0d4f09e FIX-REAL-WORLD: ...` and `de30cdf FIX-MCP-STORE-LOAD: ...`
on 2026-07-18 but later dropped by a `git pull --rebase origin main`
on the same day. The source files survived in the working tree
because they were already on origin/main via the rebase target.
This file is the journal entry for the bugs the tests were
written to document.

## Status

| ID   | Title                                                  | Status      | Filed |
|------|--------------------------------------------------------|-------------|-------|
| BUG-003 | `AgentImpl::execute_tool` ignores `cfg.project_root` | 🟡 OPEN (in this file) | no (here) |
| BUG-004 | `AgentImpl::execute_tool` ignores `cfg.auto_approve` | 🟡 OPEN (in this file) | no (here) |