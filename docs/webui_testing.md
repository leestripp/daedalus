# Webui Testing with Firefox BiDi

This document captures the recipe we use to test `demos/daedalus-webui/`
end-to-end. The same pattern works for any browser-based host of
`libdaedalus.a` — it catches DOM bugs, JS errors, SSE misconfiguration,
and visual regressions in one shot.

## Why BiDi (not selenium / playwright)

- **Zero deps.** The Python client at
  `~/.daedalus/skills/firefox/scripts/bidi_client.py` is stdlib only
  (`socket`, `json`, `struct`, `base64`). No `pip install`.
- **Direct to Firefox.** No driver download, no Chrome-vs-Firefox
  parity concerns, no MCP tool registration needed.
- **Event subscription is first-class.** `log.entryAdded`,
  `browsingContext.load`, `network.responseCompleted` — one subscribe,
  many fires.
- **Screenshots + JS evaluate** in the same client.

## Setup (one-time)

```bash
# Install Firefox if missing (Ubuntu 26.04+):
sudo install -d -m 0755 /etc/apt/keyrings
sudo wget -q https://packages.mozilla.org/apt/repo-signing-key.gpg \
    -O /etc/apt/keyrings/packages.mozilla.org.asc
echo "deb [signed-by=/etc/apt/keyrings/packages.mozilla.org.asc] \
https://packages.mozilla.org/apt mozilla main" \
    | sudo tee /etc/apt/sources.list.d/mozilla.list
sudo apt update && sudo apt install -y firefox
```

The agent's Firefox profile is at `/tmp/fx-bidi-agent`. The
`start-firefox-debug.sh` script seeds the `user.js` prefs that
enable `devtools.debugger.remote-enabled` and friends — without
those, Firefox 151+ silently ignores `--remote-debugging-port`.

## Start the webui

```bash
cmake -S . -B build -DDAEDALUS_DEMO_WEBUI=ON
cmake --build build -j12 --target daedalus-webui
./build/demos/daedalus-webui/daedalus-webui 8088 &    # server on 8088
```

## Start Firefox with BiDi

```bash
FIREFOX_DEBUGGER_PORT=9222 ~/.daedalus/skills/firefox/scripts/start-firefox-debug.sh
```

Wait for the line `✓ Firefox BiDi is up at ws://localhost:9222/session`.

## Recipe: a "happy path" smoke test

Save this as `tests/test_webui_bidi.py` (or use it as a CI step):

```python
#!/usr/bin/env python3
"""Smoke-test demos/daedalus-webui via Firefox BiDi."""

import sys, time, json, os
sys.path.insert(0, os.path.expanduser("~/.daedalus/skills/firefox/scripts"))
from bidi_client import BidiClient, log_collector, console_messages, css_warnings

URL = "http://127.0.0.1:8088"

with BidiClient("localhost", 9222) as bc:
    bc.session_new()
    bc.session_subscribe(["log.entryAdded", "browsingContext.load"])
    ctx = bc.context()

    # ---- 1. Page loads cleanly ----
    bc.navigate(ctx, URL)
    time.sleep(2)
    events = log_collector(bc)
    cm = console_messages(events)
    cw = css_warnings(events)
    assert len(cm) == 0, f"console errors: {cm}"
    assert len(cw) == 0, f"CSS warnings: {cw}"

    # ---- 2. Layout sanity ----
    struct = bc.script_evaluate(ctx, """
        (() => {
            const panes = document.querySelectorAll('.pane').length;
            const buttons = document.querySelectorAll('button').length;
            return JSON.stringify({ panes, buttons });
        })()
    """)
    assert '"panes":3' in struct, f"expected 3 panes, got {struct}"

    # ---- 3. End-to-end: create session, send msg, observe SSE ----
    sid = json.loads(bc.script_evaluate(ctx, """
        (async () => {
            const r = await fetch('/sessions', {method:'POST'});
            return JSON.stringify(await r.json());
        })()
    """, await_promise=True))["session_id"]

    bc.script_evaluate(ctx, f"""
        (() => {{
            window.__sse = [];
            new EventSource('/sessions/{sid}/events').addEventListener('state_change',
                e => window.__sse.push(JSON.parse(e.data).to));
        }})()
    """)

    bc.script_evaluate(ctx, f"""
        (async () => {{
            await fetch('/sessions/{sid}/messages', {{
                method: 'POST',
                headers: {{'Content-Type':'application/json'}},
                body: JSON.stringify({{message:'Say hi in one short sentence.'}})
            }});
        }})()
    """, await_promise=True)

    # Wait for the agent to actually finish its first turn.
    time.sleep(45)

    transcript = bc.script_evaluate(ctx, """
        document.getElementById('transcript').innerText
    """)
    assert len(transcript) > 0, "transcript pane was empty after 45s"

    bc.take_screenshot(ctx, "/tmp/webui_smoke_pass.png")
    bc.session_end()
    print("PASS")
```

## Recipe: spot SSE reconnects during idle

Sometimes you want to **prove** that the SSE connection is stable
(or unstable) over a long idle period:

```python
import json, time, sys, os
sys.path.insert(0, os.path.expanduser("~/.daedalus/skills/firefox/scripts"))
from bidi_client import BidiClient

with BidiClient("localhost", 9222) as bc:
    bc.session_new()
    ctx = bc.context()
    bc.navigate(ctx, "http://127.0.0.1:8088")
    time.sleep(3)

    # Poll EventSource.readyState every second; record transitions.
    bc.script_evaluate(ctx, """
        (() => {
            window.__watch = { history: [], start: Date.now() };
            setInterval(() => {
                if (window.es) window.__watch.history.push({
                    t: Date.now() - window.__watch.start,
                    readyState: window.es.readyState,
                });
            }, 1000);
        })()
    """)

    time.sleep(60)

    history = json.loads(bc.script_evaluate(ctx,
        "JSON.stringify(window.__watch.history)"))
    transitions = sum(1 for i in range(1, len(history))
                       if history[i]["readyState"] != history[i-1]["readyState"])
    print(f"60 s idle: {len(history)} samples, {transitions} readyState transitions")
    # A stable SSE connection should have exactly 2 transitions:
    # CONNECTING → OPEN at the start, then nothing.
    assert transitions <= 3, f"too many reconnects: {transitions}"
```

## Recipe: visual diff (regression catch)

Two-phase test for catching visual regressions:

```python
# Phase 1: record the expected screenshot
expected = bc.take_screenshot(ctx, "/tmp/webui_baseline.png")

# Phase 2: compare against a future run
from PIL import Image, ImageChops
diff = ImageChops.difference(
    Image.open("/tmp/webui_baseline.png"),
    Image.open("/tmp/webui_candidate.png"),
)
bbox = diff.getbbox()
assert bbox is None, f"visual diff at {bbox}"
```

## Cleaning up

```bash
# Stop the webui (you know its PID):
kill <pid>

# Stop the agent's Firefox instance (NEVER use pkill -f firefox!):
pgrep -f "firefox.*--profile /tmp/fx-bidi-agent" | xargs -r kill
```

## When to use this

- Before merging any change to `demos/daedalus-webui/main.cpp`.
- After upgrading OmniNet (the SSE drop regression is exactly what
  this harness catches).
- As a manual smoke test before tagging a release.
- When debugging user reports of "the webui looks weird" or "SSE
  keeps disconnecting".

## Gotchas

- **`script_evaluate` returns a Promise handle** unless you pass
  `await_promise=True`. Always pass it for `fetch()`.
- **`let` bindings are not on `window`.** If the page declares
  `let es = null`, `window.es` is undefined. The webui demo now
  exposes `sessionId` and `es` via `Object.defineProperty` for
  exactly this reason.
- **`fetch` is blocked by CORS** if the server isn't configured
  with `corsEnabled = true`. The webui demo enables this.
- **`new EventSource("/...")` requires an absolute URL or one
  relative to the page origin.** If you navigate to `/`, an
  EventSource at `/sessions/<id>/events` resolves to the right
  thing; navigating to `http://localhost:8088/` works too.
- **Firefox's BiDi event log fires once per event.** If you
  navigate before subscribing, you miss them. Subscribe first.

## Reference

- `~/.daedalus/skills/firefox/scripts/bidi_client.py` — the client
- `~/.daedalus/skills/firefox/scripts/start-firefox-debug.sh` — the launcher
- `demos/daedalus-webui/main.cpp` — the demo under test