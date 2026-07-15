# WebUI tests

This directory holds browser-side smoke tests for the
`demos/daedalus-webui/` UI. They run with Node.js — no build step
required (the tests read `index_html.h` directly).

## How to set up (one time)

```bash
cd tests/webui
npm install                  # installs playwright
npx playwright install chromium   # ~290 MiB (chromium + ffmpeg + headless shell)
```

## How to run

```bash
cd tests/webui

# Run both tests in sequence
npm test

# Or individually
node three_call_test.js              # data-flow invariants (~30ms)
node playwright_three_call_test.js   # full browser test (~3s)
```

The tests read `demos/daedalus-webui/index_html.h` directly, so
they always exercise the actual served bytes — no copy-paste.

## Tests

### `three_call_test.js`

Node-based smoke test that loads the JS in `index_html.h` into a
`vm` context, mocks the DOM at the minimum level needed by the
tool-card code path, and fires synthetic SSE events directly into
the captured listeners. Asserts on the resulting DOM state.

**Coverage** (29 assertions, ~30ms):

- 3 distinct cards created in the `#tools` panel
- Each card's `.tool-card-name` matches the SSE payload
- Cross-pollination: progress for `call_002` does NOT touch `call_001`
- Counter accumulates correctly across multiple calls
- Card insertion order is preserved
- Orphan `tool_progress` events (unknown `id`) are silently dropped
- Duplicate `tool_call` refreshes args without creating a 4th card

**No external dependencies** (no browser binary). Good as a
"smoke" test that runs in <50ms.

### `playwright_three_call_test.js`

Playwright-driven test that loads the actual HTML into a real
headless Chromium, mocks `window.fetch` and `window.EventSource`
via `addInitScript` (runs before the page's own `<script>` body),
and fires synthetic SSE events through the real browser
EventSource dispatch.

**Coverage** (30 assertions, ~3s):

Everything `three_call_test.js` covers, plus:

- **Real EventSource dispatch**: events go through Chromium's
  actual EventTarget implementation, not a JS-only mock.
- **Computed CSS** (the new value vs the node test):
  - `.tool-card` border-radius is `6px`
  - `.tool-card.success` border-color is `#2ea043` (green)
  - `.tool-card.error`   border-color is `#da3633` (red)
  - `.tool-card.running` border-color is `#1f6feb` (blue)
  - `.tool-counter.has-error` color is `#f85149` (red)
  - `.tool-card-name` has `text-overflow: ellipsis`
- **Cross-card isolation**: creating pw_004 doesn't change the
  computed style of pw_001.
- **Class transitions**: pw_004 starts .running, transitions to
  .error; border colour follows.
- **Screenshot** (`playwright_three_call.png`) for visual
  reference.

**Requires** Playwright + chromium (see setup above).

## When to run which

| Scenario | Run |
|----------|-----|
| Pre-commit smoke check | `node three_call_test.js` (30ms, no infra) |
| Pre-merge browser check | `npm test` (3s, needs chromium) |
| Visual review of tool panel UI | `open playwright_three_call.png` |

## Future

When the project grows a CI pipeline that can install
Playwright + chromium, both tests can be wired into the build
as optional `add_test()` entries. For now they are run manually
or as part of pre-merge checks.