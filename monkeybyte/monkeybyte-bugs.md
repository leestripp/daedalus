# MonkeyByte — known bugs

## BUG-001: Git status poller leaks one zombie `git` process every ~60 seconds

**Severity:** high (system-wide). The container this runs in caps total PIDs at
2048 (`pids.max`) and its PID 1 (`sleep infinity`) never reaps, so every leaked
process is a *permanent* zombie. At ~1,440 zombies/day from this bug alone, the
machine stops being able to fork in ~1.4 days and everything degrades.

**Symptom:** `[git] <defunct>` zombies accumulating at a steady ~1-per-minute
cadence, each parented to PID 1 (i.e. some intermediate parent exits without
calling `wait()`). Also observed: an `npm exec obs-mc` zombie from the OBS MCP
spawn — possibly the same class of bug (child spawned, parent exits, no reap).

**Evidence (2026-11-20, dev-pc container):**
- `/proc/*/stat` scan: 19 `git` zombies spaced almost exactly 60 s apart
  (start-times 6411, 12399, 18396, 24403, 30398, 36409 ticks …), ppid = 1,
  from immediately after container boot.
- `ps -e -o comm` showed matching live `git` processes mid-flight at the same
  60 s cadence while MonkeyByte (PID 2) was running.

**Suspected code:**
- `src/monkeybyte/ui/MainWindow.cpp:256` — `git_poll_conn_ = Glib::signal_timeout().connect(...)`
  (the ~60 s poll timer; interval to be confirmed there).
- `src/monkeybyte/backend/GitService.cpp:18` — `popen(cmd.c_str(), "r")` with
  `pclose` on the unique_ptr. If any path exits the helper/thread without
  `pclose`, or the child is spawned through an intermediate that exits first,
  the exiting `git` is never reaped. The ppid=1 signature means an intermediate
  between the poller and `git` is dying without waiting — trace the fork/exec
  chain from the poll callback.

**Reproduce / verify:**
1. Start MonkeyByte in a container whose PID 1 does not reap (current setup
   qualifies), or under an init that reaps but count orphans via `ps`.
2. Wait 5 minutes; run a pure-builtin zombie scan (forks may fail under
   exhaustion; builtins don't):
   `for f in /proc/[0-9]*/stat; do read line < "$f"; rest=${line##*) }; set -- $rest; [ "$1" = "Z" ] && echo "$line"; done | grep git`
3. Expect one new `git` zombie per poll interval.

**Fix direction:** ensure the poller reaps its child on every path
(popen/pclose lifetime strictly inside the calling scope, no early return
between popen and pclose, no fire-and-forget async spawn), or spawn via a
helper that always `waitpid`s. Also audit the `npm exec obs-mc` MCP spawn for
the same pattern. Note the general environment hazard: anything orphaned here
zombifies permanently until a container restart — relaunching the container
with `--init` (tini) would make future leaks self-heal instead of piling up.

## Resolution (2026-11-20)

`GIT_TRACE` identified the one-per-minute process as Git's automatic post-fetch
maintenance:

```text
git maintenance run --auto --quiet --detach
```

This is a deliberately detached grandchild, not a fork of MonkeyByte and not a
failure of the existing `popen()`/`pclose()` pair to reap its direct child. The
3-second status worker threading is unchanged.

`GitService::fetch_upstream()` now runs fetch with
`-c maintenance.auto=false -c gc.auto=0`, preventing background maintenance only
for this unattended UI fetch. A real-fetch regression in
`monkeybyte/tests/test_main.cpp` failed before the fix and passes after it; the
full 115-test CTest suite (including the separate tmpfs concurrency run) and
umbrella static analysis pass.

Restart MonkeyByte after installing the fixed binary. Zombies already parented
to PID 1 remain until the container restarts; `--init` is still recommended as
a defence against future orphaned processes from any source. The separately
observed `npm exec obs-mc` zombie belongs to the MCP process lifecycle and is
not changed by this Git fix.
