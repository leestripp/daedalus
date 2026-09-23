// Playwright FIREFOX test against the real daedalus-webui HTTP server.
// Identical to playwright_real_server_test.js but uses
// `firefox` instead of `chromium`. Catches any browser-specific
// rendering issues (CSS, JS) that Chromium would mask.
//
// Run: node tests/webui/playwright_firefox_test.js

'use strict';

const { firefox } = require('playwright');
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

    const infoResp = await fetch(`${WEBUI_URL}/info`);
    assert(infoResp.ok, `GET ${WEBUI_URL}/info returns ${infoResp.status}`);

    // Firefox can render headless without a display server. Playwright
    // bundles a fully-functional firefox binary.
    console.log('[test] launching firefox...');
    const browser = await firefox.launch({ headless: true });
    console.log('[test] firefox launched');
    const page = await browser.newPage();

    await page.exposeFunction('rgbToHex', rgbToHex);

    const consoleMessages = [];
    page.on('console', msg => {
        consoleMessages.push({ type: msg.type(), text: msg.text() });
        if (msg.type() === 'error') {
            process.stderr.write('[ff err] ' + msg.text() + '\n');
        }
    });
    page.on('pageerror', err => {
        consoleMessages.push({ type: 'pageerror', text: err.message });
        process.stderr.write('[ff pageerror] ' + err.message + '\n');
    });

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

    console.log('[test] navigating Firefox to', WEBUI_URL);
    await page.goto(WEBUI_URL, { waitUntil: 'load' });

    await page.waitForFunction(() => window.__es_url !== null, null, {
        timeout: 10000,
    });
    const esUrl = await page.evaluate(() => window.__es_url);
    console.log('[test] EventSource URL:', esUrl);

    // Disable CSS transitions for deterministic computed-style checks.
    await page.addStyleTag({
        content: `*, *::before, *::after { ` +
                 `transition: none !important; ` +
                 `animation: none !important; }`,
    });

    // Drive 3 sequential tool calls.
    await page.evaluate(() => {
        window.__es_fire('tool_call', {
            id: 'ff_001', name: 'read_file',
            args: { path: '/tmp/foo.txt' }
        });
    });
    const c1 = page.locator(cardSel('ff_001'));
    assert(await c1.count() === 1, 'ff_001 card exists in DOM (Firefox)');
    assert(await c1.evaluate(el => el.classList.contains('running')),
           'ff_001 .running class');

    await page.evaluate(() => {
        window.__es_fire('tool_progress', {
            id: 'ff_001',
            progress: { stage: 'reading', percent: 50, message: 'halfway' }
        });
    });
    const c1bar = await c1.locator('.tool-card-progress-bar')
        .evaluate(el => el.style.width);
    assert(c1bar === '50%', `ff_001 progress bar = "${c1bar}" (Firefox)`);

    await page.evaluate(() => {
        window.__es_fire('tool_result', {
            id: 'ff_001',
            result: { status: 'success', content: 'hi\n' }
        });
    });
    assert(await c1.evaluate(el => el.classList.contains('success')),
           'ff_001 .success class');

    await page.evaluate(() => {
        window.__es_fire('tool_call', {
            id: 'ff_002', name: 'edit_file_patch',
            args: { path: '/tmp/foo.txt' }
        });
    });
    await page.evaluate(() => {
        window.__es_fire('tool_result', {
            id: 'ff_002', result: { status: 'success' }
        });
    });
    const c2 = page.locator(cardSel('ff_002'));
    assert(await c2.evaluate(el => el.classList.contains('success')),
           'ff_002 .success class');

    await page.evaluate(() => {
        window.__es_fire('tool_call', {
            id: 'ff_003', name: 'read_file',
            args: { path: '/tmp/missing' }
        });
    });
    await page.evaluate(() => {
        window.__es_fire('tool_result', {
            id: 'ff_003',
            result: { status: 'error', message: 'File does not exist' }
        });
    });
    const c3 = page.locator(cardSel('ff_003'));
    assert(await c3.evaluate(el => el.classList.contains('error')),
           'ff_003 .error class');

    const counterText = await page.locator('#tool-counter').textContent();
    assert(counterText.includes('2 ok'),
           `Firefox counter shows 2 ok — got "${counterText}"`);
    assert(counterText.includes('1 err'),
           `Firefox counter shows 1 err — got "${counterText}"`);

    // Firefox-specific: check that a few CSS rules render correctly.
    const c1Border = await c1.evaluate(el =>
        rgbToHex(getComputedStyle(el).borderColor));
    assert(c1Border === '#2ea043',
           `Firefox computed success border-color = "#2ea043" — got "${c1Border}"`);

    const c3Border = await c3.evaluate(el =>
        rgbToHex(getComputedStyle(el).borderColor));
    assert(c3Border === '#da3633',
           `Firefox computed error border-color = "#da3633" — got "${c3Border}"`);

    const c2Border = await c2.evaluate(el =>
        rgbToHex(getComputedStyle(el).borderColor));
    assert(c2Border === '#2ea043',
           `Firefox computed success border-color = "#2ea043" — got "${c2Border}"`);

    // Screenshot.
    const screenshotPath = path.join(__dirname,
        'playwright_firefox_test.png');
    await page.screenshot({ path: screenshotPath, fullPage: true });
    console.log(`OK:   screenshot saved: ${screenshotPath}`);

    const errors = consoleMessages.filter(m => m.type === 'error');
    if (errors.length > 0) {
        console.log('[test] Firefox console errors:');
        for (const e of errors) console.log('   ', e.type, e.text);
    }
    assert(errors.length === 0,
           `Firefox: no console errors (got ${errors.length})`);

    await browser.close();
    console.log('\nFirefox real-server Playwright test passed.');
    process.exit(0);
})().catch(e => {
    console.error(e);
    process.exit(1);
});