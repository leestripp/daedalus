// Playwright test that drives the REAL daedalus-webui HTTP server
// (not a data: URL) — so we exercise the full stack:
//   - http://127.0.0.1:8088/  (the webui's index.html)
//   - POST /sessions          (creates a session)
//   - GET  /info              (server info)
//   - GET  /sessions/.../events  (SSE stream — we won't actually use it
//     since we don't have an LLM, but the EventSource should at least
//     connect and surface the initial ":connected" frame)
//
// Then we mock the EventSource and inject synthetic tool events
// (the same ones we used in the offline tests), and verify the
// rendered DOM matches.
//
// Run: node tests/webui/playwright_real_server_test.js

'use strict';

const { chromium } = require('playwright');
const fs   = require('fs');
const path = require('path');

const WEBUI_URL = process.argv[2] || 'http://127.0.0.1:8088';

function rgbToHex(rgb) {
    if (!rgb) return '';
    const m = rgb.match(/rgba?\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)/);
    if (!m) return rgb;
    return '#' + [m[1], m[2], m[3]].map(n => {
        const h = parseInt(n, 10).toString(16);
        return h.length === 1 ? '0' + h : h;
    }).join('');
}

function assert(cond, msg) {
    if (!cond) { console.log('FAIL:', msg); process.exit(1); }
    console.log('OK:  ', msg);
}

const cardSel = id => `[data-id="${id}"]`;

(async () => {
    console.log(`[test] webui URL: ${WEBUI_URL}`);

    // Verify the server is up before launching the browser.
    const infoResp = await fetch(`${WEBUI_URL}/info`);
    assert(infoResp.ok, `GET ${WEBUI_URL}/info returns ${infoResp.status}`);
    const info = await infoResp.json();
    console.log('[test] /info:', JSON.stringify(info));

    const browser = await chromium.launch({ headless: true });
    const page    = await browser.newPage();

    await page.exposeFunction('rgbToHex', rgbToHex);

    const consoleMessages = [];
    page.on('console', msg => {
        consoleMessages.push({ type: msg.type(), text: msg.text() });
        if (msg.type() === 'error') {
            process.stderr.write('[browser err] ' + msg.text() + '\n');
        }
    });
    page.on('pageerror', err => {
        consoleMessages.push({ type: 'pageerror', text: err.message });
        process.stderr.write('[browser pageerror] ' + err.message + '\n');
    });

    // Override EventSource so we can drive it without a real LLM.
    // The webui's real /sessions/<id>/events endpoint will stay
    // open (consumed by the test) but we don't read from it.
    await page.addInitScript(() => {
        const listeners = {};
        let instanceCount = 0;
        window.__es_listeners = listeners;
        window.__es_fire = (name, data) => {
            const ev = { data: JSON.stringify(data) };
            for (const fn of (listeners[name] || [])) fn(ev);
        };
        window.__es_url = null;
        window.__es_instanceCount = () => instanceCount;
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

    console.log('[test] navigating to', WEBUI_URL);
    await page.goto(WEBUI_URL, { waitUntil: 'load' });

    // Wait for the page to have called openEventStream().
    await page.waitForFunction(() => window.__es_url !== null, null, {
        timeout: 5000,
    });
    const esUrl = await page.evaluate(() => window.__es_url);
    console.log('[test] EventSource URL:', esUrl);
    assert(esUrl.startsWith('/sessions/') && esUrl.endsWith('/events'),
           'EventSource URL is /sessions/<id>/events');

    // Verify the page's DOM hooks from index_html.h are present.
    const hasTools = await page.locator('#tools').count();
    assert(hasTools === 1, '#tools section is present');
    const hasCounter = await page.locator('#tool-counter').count();
    assert(hasCounter === 1, '#tool-counter badge is in the header');

    // The page should have populated model + cwd pills (from /info
    // and the POST /sessions response).
    const modelText = await page.locator('#model').textContent();
    console.log('[test] #model text:', JSON.stringify(modelText));
    const cwdText = await page.locator('#cwd').textContent();
    console.log('[test] #cwd text:', JSON.stringify(cwdText));
    assert(cwdText.length > 0 && cwdText !== '…',
           `#cwd populated (got "${cwdText}")`);

    // Disable CSS transitions for deterministic computed-style checks.
    await page.addStyleTag({
        content: `*, *::before, *::after { ` +
                 `transition: none !important; ` +
                 `animation: none !important; }`,
    });

    // ---------- Drive a 3-call sequence ----------
    console.log('[test] firing 3 sequential tool events...');

    // Call 1: read_file (success, with progress)
    await page.evaluate(() => {
        window.__es_fire('tool_call', {
            id: 'live_001', name: 'read_file',
            args: { path: '/tmp/foo.txt' }
        });
    });
    const c1 = page.locator(cardSel('live_001'));
    assert(await c1.count() === 1, 'live_001 card exists in DOM');
    assert(await c1.evaluate(el => el.classList.contains('running')),
           'live_001 .running class');

    await page.evaluate(() => {
        window.__es_fire('tool_progress', {
            id: 'live_001',
            progress: { stage: 'reading', percent: 50, message: 'halfway' }
        });
    });
    const c1bar = await c1.locator('.tool-card-progress-bar')
        .evaluate(el => el.style.width);
    assert(c1bar === '50%', `live_001 progress bar = "${c1bar}"`);

    await page.evaluate(() => {
        window.__es_fire('tool_result', {
            id: 'live_001',
            result: { status: 'success', content: 'hello\n' }
        });
    });
    assert(await c1.evaluate(el => el.classList.contains('success')),
           'live_001 .success class');

    // Call 2: edit_file_patch
    await page.evaluate(() => {
        window.__es_fire('tool_call', {
            id: 'live_002', name: 'edit_file_patch',
            args: { path: '/tmp/foo.txt', old_str: 'hello', new_str: 'bye' }
        });
    });
    const c2 = page.locator(cardSel('live_002'));
    assert(await c2.count() === 1, 'live_002 card exists');
    await page.evaluate(() => {
        window.__es_fire('tool_result', {
            id: 'live_002', result: { status: 'success', applied: true }
        });
    });
    assert(await c2.evaluate(el => el.classList.contains('success')),
           'live_002 .success class');

    // Call 3: read_file (error)
    await page.evaluate(() => {
        window.__es_fire('tool_call', {
            id: 'live_003', name: 'read_file',
            args: { path: '/tmp/missing' }
        });
    });
    const c3 = page.locator(cardSel('live_003'));
    await page.evaluate(() => {
        window.__es_fire('tool_result', {
            id: 'live_003',
            result: { status: 'error', message: 'File does not exist' }
        });
    });
    assert(await c3.evaluate(el => el.classList.contains('error')),
           'live_003 .error class');

    // ---------- Final state ----------
    const counterText = await page.locator('#tool-counter').textContent();
    assert(counterText.includes('2 ok'),
           `counter shows 2 ok — got "${counterText}"`);
    assert(counterText.includes('1 err'),
           `counter shows 1 err — got "${counterText}"`);

    // CSS sanity (computed).
    const c1Border = await c1.evaluate(el =>
        rgbToHex(getComputedStyle(el).borderColor));
    assert(c1Border === '#2ea043',
           `live_001 (success) computed border-color = "#2ea043" — got "${c1Border}"`);

    // ---------- Screenshot ----------
    const screenshotPath = path.join(__dirname,
        'playwright_real_server.png');
    await page.screenshot({ path: screenshotPath, fullPage: true });
    console.log(`OK:   screenshot saved: ${screenshotPath}`);

    // ---------- Console messages ----------
    console.log(`[test] ${consoleMessages.length} console messages`);
    const errors = consoleMessages.filter(m => m.type === 'error');
    if (errors.length > 0) {
        console.log('[test] errors:');
        for (const e of errors) console.log('   ', e.type, e.text);
    }
    assert(errors.length === 0,
           `no console errors (got ${errors.length})`);

    await browser.close();
    console.log('\nAll real-server Playwright assertions passed.');
    process.exit(0);
})().catch(e => {
    console.error(e);
    process.exit(1);
});