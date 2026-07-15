// SPDX-License-Identifier: BSL-1.0
// tests/webui/playwright_three_call_test.js
//
// §Phase E1: Playwright smoke test for the webui's tool-panel JS.
//
// Spins up a real headless Chromium, loads the actual webui HTML
// (demos/daedalus-webui/index_html.h), and fires synthetic SSE
// events through a real EventSource. Asserts the rendered DOM
// after each event — including computed CSS properties.
//
// The node-based test (three_call_test.js) verifies the data-flow
// invariants. THIS test verifies the same flow against a real
// browser, which exercises:
//   - Real EventSource over a real EventTarget dispatch
//   - Real CSS layout / paint (computed style assertions)
//   - The actual HTML/CSS that's served, with no copy-paste
//
// RUN:
//   node tests/webui/playwright_three_call_test.js
//
// Requires: `npm install playwright` + `npx playwright install chromium`
// (the latter is ~177 MiB; see tests/webui/README.md for details).

'use strict';

const fs   = require('fs');
const path = require('path');
const { chromium } = require('playwright');

// ------------------------------------------------------------------------
// Load the served HTML from index_html.h (no copy-paste — the test
// always exercises the actual served bytes).
// ------------------------------------------------------------------------
const htmlPath = process.argv[2] ||
    '/workspace/daedalus/demos/daedalus-webui/index_html.h';
const htmlSrc = fs.readFileSync(htmlPath, 'utf8');

// We serve the HTML from an in-memory data: URL — but the page
// references absolute paths like "/sessions" via fetch(). Those
// will be intercepted by our addInitScript mock, so the data: URL
// is fine.
const dataUrl = 'data:text/html;charset=utf-8,' +
    encodeURIComponent(htmlSrc);

function assert(cond, msg) {
    if (!cond) { console.log('FAIL:', msg); process.exit(1); }
    console.log('OK:  ', msg);
}

// CSS selector that matches `<div data-id="X">` — that's how the
// JS identifies each card. The HTML `id` attribute is unused.
const cardSel = id => `[data-id="${id}"]`;

// Chrome returns getComputedStyle colours as `rgb(R, G, B)` strings,
// not hex. Convert to lowercase `#rrggbb` so we can assert against
// the same hex values the CSS uses.
function rgbToHex(rgb) {
    if (!rgb) return '';
    const m = rgb.match(/rgba?\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)/);
    if (!m) return rgb;
    return '#' + [m[1], m[2], m[3]].map(n => {
        const h = parseInt(n, 10).toString(16);
        return h.length === 1 ? '0' + h : h;
    }).join('');
}

(async () => {
    const browser = await chromium.launch({ headless: true });
    const page    = await browser.newPage();

    // Expose rgbToHex to the page so we can use it inside
    // `page.evaluate(...)` blocks (the helper is defined in the
    // node-side test script, not the page).
    await page.exposeFunction('rgbToHex', rgbToHex);

    // Capture browser console for debugging if a test fails.
    page.on('console', msg => {
        if (msg.type() === 'error' || msg.type() === 'warning') {
            process.stderr.write('[browser ' + msg.type() + '] ' + msg.text() + '\n');
        }
    });
    page.on('pageerror', err => {
        process.stderr.write('[browser err] ' + err.message + '\n');
    });

    // Inject mocks BEFORE any page script runs. addInitScript runs in
    // every new document, before <script> bodies execute. This is
    // exactly the moment we need to replace window.fetch and
    // window.EventSource.
    await page.addInitScript(() => {
        // --- fetch mock ---
        const realFetch = window.fetch.bind(window);
        window.fetch = async (url, opts) => {
            const u = typeof url === 'string' ? url : url.url;
            if (u === '/sessions' && opts && opts.method === 'POST') {
                return { json: async () => ({
                    session_id: 'pw-test-session',
                    model:      { id: 'mock', name: 'mock' }
                }) };
            }
            if (u === '/info') {
                return { json: async () => ({ cwd: '/tmp' }) };
            }
            return realFetch(url, opts);
        };

        // --- EventSource mock ---
        // Captures every listener registered. window.__es_fire(name,
        // data) dispatches a synthetic event to all listeners for
        // that event name.
        const listeners = {};
        let instanceCount = 0;
        window.__es_listeners = listeners;
        window.__es_fire = (name, data) => {
            const ev = { data: JSON.stringify(data) };
            for (const fn of (listeners[name] || [])) fn(ev);
        };
        window.__es_instanceCount = () => instanceCount;
        window.__es_url = null;
        window.EventSource = class MockES {
            constructor(url) {
                ++instanceCount;
                window.__es_url = url;
                this.url = url;
                this.listeners = listeners;
            }
            addEventListener(name, fn) {
                (listeners[name] = listeners[name] || []).push(fn);
            }
            close() {}
        };
    });

    // Load the HTML.
    await page.goto(dataUrl, { waitUntil: 'load' });

    // Wait for the page to have called openEventStream() and our
    // mock EventSource to have been constructed. This is the
    // signal that the script ran to completion.
    await page.waitForFunction(() => window.__es_url !== null, null, {
        timeout: 5000,
    });

    // Disable CSS transitions so computed-style assertions don't
    // catch an in-flight animation frame. The .tool-card rule has
    // `transition: border-color 200ms`, so a test that fires a
    // tool_result and immediately checks getComputedStyle can see
    // an intermediate colour (e.g. between #1f6feb running and
    // #2ea043 success). Forcing transition: none everywhere makes
    // every computed style deterministic.
    await page.addStyleTag({
        content: `*, *::before, *::after { transition: none !important; ` +
                 `animation: none !important; }`,
    });

    // ---------- Sequence: 3 sequential tool calls ----------
    // Mirrors the node-based test (tests/webui/three_call_test.js)
    // so the two test suites can be compared 1:1.

    // Call 1: read_file (success, with progress)
    await page.evaluate(() => {
        window.__es_fire('tool_call', {
            id: 'pw_001', name: 'read_file',
            args: { path: '/tmp/foo.txt', line_numbers: true }
        });
    });
    assert(await page.locator(cardSel('pw_001')).count() === 1,
           'pw_001 card exists in DOM');
    const c1 = page.locator(cardSel('pw_001'));
    assert(await c1.evaluate(el => el.classList.contains('running')),
           'pw_001 has .running class');
    assert((await c1.locator('.tool-card-name').textContent()) === 'read_file',
           'pw_001 name = "read_file"');

    await page.evaluate(() => {
        window.__es_fire('tool_progress', {
            id: 'pw_001',
            progress: { stage: 'reading', percent: 50, message: 'halfway' }
        });
    });
    // Bar width: live computed style. CSS sets it via inline style
    // on the .tool-card-progress-bar element.
    const barWidth = await c1.locator('.tool-card-progress-bar')
        .evaluate(el => el.style.width);
    assert(barWidth === '50%', `pw_001 bar.style.width = "${barWidth}"`);
    const c1msg = await c1.locator('.tool-card-message').textContent();
    assert(c1msg === 'halfway', `pw_001 msg = "${c1msg}"`);

    await page.evaluate(() => {
        window.__es_fire('tool_result', {
            id: 'pw_001',
            result: { status: 'success', path: '/tmp/foo.txt', content: 'hi' }
        });
    });
    assert(await c1.evaluate(el => el.classList.contains('success')),
           'pw_001 has .success class');
    assert(!await c1.evaluate(el => el.classList.contains('running')),
           'pw_001 no longer has .running class');
    const c1res = await c1.locator('.tool-card-result').textContent();
    assert(c1res.includes('hi'),
           `pw_001 result rendered (text="${c1res.trim()}")`);

    // Call 2: edit_file_patch (success, no progress events)
    await page.evaluate(() => {
        window.__es_fire('tool_call', {
            id: 'pw_002', name: 'edit_file_patch',
            args: { path: '/tmp/foo.txt', old_str: 'hi', new_str: 'bye' }
        });
    });
    const c2 = page.locator(cardSel('pw_002'));
    assert(await c2.evaluate(el => el.classList.contains('running')),
           'pw_002 running');

    // Cross-pollination: progress for pw_002 must NOT touch pw_001.
    const c1barBefore = await c1.locator('.tool-card-progress-bar')
        .evaluate(el => el.style.width);
    await page.evaluate(() => {
        window.__es_fire('tool_progress', {
            id: 'pw_002', progress: { stage: 'editing', percent: 100 }
        });
    });
    const c1barAfter = await c1.locator('.tool-card-progress-bar')
        .evaluate(el => el.style.width);
    assert(c1barBefore === c1barAfter,
           `pw_001 bar unchanged when pw_002 fires progress ` +
           `(${c1barBefore} -> ${c1barAfter})`);

    await page.evaluate(() => {
        window.__es_fire('tool_result', {
            id: 'pw_002', result: { status: 'success', applied: true }
        });
    });
    assert(await c2.evaluate(el => el.classList.contains('success')),
           'pw_002 success');
    assert(await c1.evaluate(el => el.classList.contains('success')),
           'pw_001 still success (not overwritten)');

    // Call 3: read_file (error)
    await page.evaluate(() => {
        window.__es_fire('tool_call', {
            id: 'pw_003', name: 'read_file',
            args: { path: '/tmp/missing.txt' }
        });
    });
    const c3 = page.locator(cardSel('pw_003'));
    assert(await c3.evaluate(el => el.classList.contains('running')),
           'pw_003 running');

    await page.evaluate(() => {
        window.__es_fire('tool_result', {
            id: 'pw_003',
            result: { status: 'error',
                      message: 'File does not exist: /tmp/missing.txt' }
        });
    });
    assert(await c3.evaluate(el => el.classList.contains('error')),
           'pw_003 has .error class');
    assert(!await c3.evaluate(el => el.classList.contains('success')),
           'pw_003 not .success');
    const c3res = await c3.locator('.tool-card-result').textContent();
    assert(c3res.includes('File does not exist'),
           `pw_003 error message rendered (text="${c3res.trim()}")`);

    // ---------- Final state assertions ----------

    const cardCount = await page.locator('.tool-card').count();
    assert(cardCount === 3, `3 .tool-card elements in DOM (got ${cardCount})`);

    // Counter should show "2 ok · 1 err".
    const counterText = await page.locator('#tool-counter').textContent();
    assert(counterText.includes('2 ok'),
           `counter has "2 ok" — got "${counterText}"`);
    assert(counterText.includes('1 err'),
           `counter has "1 err" — got "${counterText}"`);

    // Order: pw_001, pw_002, pw_003 (insertion order).
    const ids = await page.locator('.tool-card')
        .evaluateAll(els => els.map(el => el.dataset.id));
    assert(JSON.stringify(ids) === JSON.stringify(['pw_001', 'pw_002', 'pw_003']),
           `card order is pw_001, pw_002, pw_003 (got ${JSON.stringify(ids)})`);

    // ---------- CSS computed-style sanity (the new value Playwright adds) ----------
    // Confirms the actual CSS rules from the <style> block are applied,
    // not just that the class names are set. This catches refactors
    // that rename a CSS class but leave the JS pointing at the old name.
    // The .tool-card has these state-dependent border-colors:
    //   default           : #2a2f36 (defined on .tool-card itself)
    //   .tool-card.running: #1f6feb (overrides during execution)
    //   .tool-card.success: #2ea043 (overrides on success)
    //   .tool-card.error  : #da3633 (overrides on error)
    // We verify all three state-colors via the respective cards.

    // pw_001 is .success → border should be #2ea043 (46, 160, 67).
    const successBorder = await c1.evaluate(el =>
        rgbToHex(getComputedStyle(el).borderColor));
    assert(successBorder === '#2ea043',
           `pw_001 (success) computed border-color = "#2ea043" — got "${successBorder}"`);

    // pw_002 is .success → same check.
    const c2Border = await c2.evaluate(el => rgbToHex(getComputedStyle(el).borderColor));
    assert(c2Border === '#2ea043',
           `pw_002 (success) computed border-color = "#2ea043" — got "${c2Border}"`);

    // pw_003 is .error → border should be #da3633 (218, 54, 51).
    const errorBorder = await c3.evaluate(el => rgbToHex(getComputedStyle(el).borderColor));
    assert(errorBorder === '#da3633',
           `pw_003 (error) computed border-color = "#da3633" — got "${errorBorder}"`);

    // Round-trip: fire a fresh tool_call for a 4th card, verify its
    // .tool-card.running border is #1f6feb (31, 111, 235). Then fire
    // a tool_result error to drive it through the .error state.
    await page.evaluate(() => {
        window.__es_fire('tool_call', {
            id: 'pw_004', name: 'execute_bash',
            args: { command: 'echo running-test' }
        });
    });
    const c4 = page.locator(cardSel('pw_004'));
    assert(await c4.evaluate(el => el.classList.contains('running')),
           'pw_004 has .running class');
    // pw_004 is .running → border should be #1f6feb (31, 111, 235).
    const runningBorder = await c4.evaluate(el =>
        rgbToHex(getComputedStyle(el).borderColor));
    assert(runningBorder === '#1f6feb',
           `pw_004 (running) computed border-color = "#1f6feb" — got "${runningBorder}"`);

    // pw_001 should still be the success color (running class addition
    // to a sibling must not affect the original card's computed style).
    const c1BorderAfter = await c1.evaluate(el =>
        rgbToHex(getComputedStyle(el).borderColor));
    assert(c1BorderAfter === '#2ea043',
           `pw_001 (success) border still "#2ea043" after pw_004 created — got "${c1BorderAfter}"`);

    // Round-trip pw_004 to error to verify the .error border applies
    // AFTER the class changes.
    await page.evaluate(() => {
        window.__es_fire('tool_result', {
            id: 'pw_004',
            result: { status: 'error', message: 'forced' }
        });
    });
    const c4BorderAfter = await c4.evaluate(el =>
        rgbToHex(getComputedStyle(el).borderColor));
    assert(c4BorderAfter === '#da3633',
           `pw_004 (now error) computed border-color = "#da3633" — got "${c4BorderAfter}"`);

    // ---------- Counter pill colour (state-aware styling) ----------
    // .tool-counter.has-error → color: #f85149 (248, 81, 73).
    const counterColor = await page.locator('#tool-counter')
        .evaluate(el => rgbToHex(getComputedStyle(el).color));
    assert(counterColor === '#f85149',
           `counter has-error computed color = "#f85149" — got "${counterColor}"`);

    // .tool-card-name font-style sanity: should be inline-display with
    // an ellipsis for long names. The default text is what the JS set.
    const c1NameFont = await c1.locator('.tool-card-name')
        .evaluate(el => getComputedStyle(el).textOverflow);
    assert(c1NameFont === 'ellipsis',
           `pw_001 .tool-card-name has text-overflow: ellipsis — got "${c1NameFont}"`);

    // ---------- Render a screenshot for visual confirmation ----------
    const screenshotPath = path.join(__dirname, 'playwright_three_call.png');
    await page.screenshot({ path: screenshotPath, fullPage: true });
    console.log(`OK:   screenshot saved: ${screenshotPath}`);

    await browser.close();
    console.log('\nAll Playwright three-call assertions passed.');
    process.exit(0);
})().catch(e => {
    console.error(e);
    process.exit(1);
});