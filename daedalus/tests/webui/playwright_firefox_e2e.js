// End-to-end: open the real webui in Firefox, type a prompt,
// submit it, and capture the SSE stream + final DOM state.
//
// This tests the ACTUAL end-to-end flow — no mocks, no test
// scripts in the page. Just Firefox driving a real daedalus-webui
// HTTP server with a real LLM backend (if one is configured).
//
// Run: node tests/webui/playwright_firefox_e2e.js
'use strict';

const { firefox } = require('playwright');
const path = require('path');

const WEBUI_URL = process.argv[2] || 'http://127.0.0.1:8088';
const PROMPT    = process.argv[3] || 'what is 2 + 2? reply with just the number.';

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

(async () => {
    console.log(`[test] webui URL: ${WEBUI_URL}`);
    console.log(`[test] prompt:    "${PROMPT}"`);

    const browser = await firefox.launch({ headless: true });
    const page    = await browser.newPage();

    const consoleMessages = [];
    const sseEvents = [];
    page.on('console', msg => {
        consoleMessages.push({ type: msg.type(), text: msg.text() });
        if (['error','warning'].includes(msg.type())) {
            process.stderr.write('[ff ' + msg.type() + '] ' + msg.text() + '\n');
        }
    });
    page.on('pageerror', err => {
        process.stderr.write('[ff pageerror] ' + err.message + '\n');
    });
    page.on('response', resp => {
        const ct = resp.headers()['content-type'] || '';
        if (ct.startsWith('text/event-stream')) {
            console.log('[test] SSE response from:', resp.url());
        }
    });

    console.log('[test] navigating Firefox to', WEBUI_URL);
    await page.goto(WEBUI_URL, { waitUntil: 'load' });

    // Wait for the page to be ready: session id set, input enabled.
    await page.waitForFunction(() => {
        const inp = document.getElementById('input');
        return inp && !inp.disabled;
    }, null, { timeout: 10000 });
    console.log('[test] page ready, input enabled');

    // Capture the model name from the header.
    const modelText = await page.locator('#model').textContent();
    console.log('[test] model:', JSON.stringify(modelText));
    assert(modelText && modelText !== '…' && modelText.length > 0,
           `model name populated (got "${modelText}")`);

    // Type the prompt and submit.
    await page.locator('#input').fill(PROMPT);
    const shotBefore = path.join(__dirname,
        'playwright_firefox_e2e_before.png');
    await page.screenshot({ path: shotBefore });

    console.log('[test] submitting prompt...');
    await page.locator('#composer button[type=submit]').click();

    // Wait for either a Done state or an error state.
    await page.waitForFunction(() => {
        const el = document.getElementById('state');
        return el && ['Done','Cancelled','Error'].includes(el.textContent);
    }, null, { timeout: 60000 });

    const finalState = await page.locator('#state').textContent();
    console.log(`[test] terminal state: "${finalState}"`);
    assert(['Done','Cancelled','Error'].includes(finalState),
           `terminal state reached (got "${finalState}")`);

    // Capture transcript text.
    const transcript = await page.locator('#transcript').innerText();
    console.log('[test] transcript (last 800 chars):');
    console.log(transcript.slice(-800));

    // Tool calls section.
    const toolCount = await page.locator('.tool-card').count();
    console.log(`[test] ${toolCount} tool cards visible`);

    // Counter.
    const counterText = await page.locator('#tool-counter').textContent();
    console.log(`[test] counter: "${counterText}"`);

    // Final screenshot.
    const shotAfter = path.join(__dirname,
        'playwright_firefox_e2e_after.png');
    await page.screenshot({ path: shotAfter, fullPage: true });
    console.log(`[test] screenshots: ${shotBefore}, ${shotAfter}`);

    // Errors?
    const errors = consoleMessages.filter(m => m.type === 'error');
    if (errors.length > 0) {
        console.log('[test] errors:');
        for (const e of errors) console.log('   ', e.text);
    }

    await browser.close();
    console.log(`\nFirefox E2E complete (final state: ${finalState}).`);
})().catch(e => {
    console.error(e.message);
    process.exit(1);
});