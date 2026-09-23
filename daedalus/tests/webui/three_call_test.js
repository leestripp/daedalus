// SPDX-License-Identifier: BSL-1.0
// tests/webui/three_call_test.js
//
// §Phase F1: Three-call wiring check.
//
// Verifies the webui's tool-panel JS handles three sequential
// tool calls correctly when fed SSE events directly. Tests:
//
//   - toolCardsById Map keeps one entry per call_id
//   - tool_call event creates a card in "running" state
//   - tool_progress updates ONLY the matching card
//   - tool_result seals ONLY the matching card (success or error)
//   - toolCounter accumulates correctly across multiple calls
//   - Card insertion order is preserved
//   - Orphan progress events (unknown id) are dropped
//   - Idempotent tool_call refreshes args without duplicating the card
//
// The script extracts the actual JS from `index_html.h` (no
// copy-paste) so the test cannot drift from the served HTML. The
// DOM is mocked at the minimum level needed by the tool-card code
// path; we don't pretend to be a browser — we only verify the
// data-flow invariants.
//
// RUN:
//   node tests/webui/three_call_test.js
//   node tests/webui/three_call_test.js path/to/index_html.h
//
// Not registered with ctest: requires Node.js >= 18 (set up
// optionally — see CodeMax.md §C2 follow-ups for the Playwright
// vs node choice). CI runners without node will skip this test
// silently.

'use strict';

const fs   = require('fs');
const vm   = require('vm');
const path = require('path');

// ------------------------------------------------------------------------
// Minimal DOM mock — only what the tool-panel code path uses.
// ------------------------------------------------------------------------
class MockClassList {
  constructor() { this._set = new Set(); }
  add(c)    { this._set.add(c); }
  remove(c) { this._set.delete(c); }
  contains(c) { return this._set.has(c); }
  toggle(c, force) {
    const has = this._set.has(c);
    const want = force === undefined ? !has : !!force;
    if (want) this._set.add(c); else this._set.delete(c);
    return want;
  }
  toString() { return [...this._set].join(' '); }
  *[Symbol.iterator]() { yield* this._set; }
}

let _nextId = 0;
class MockElement {
  constructor(tag) {
    this.tag = tag;
    this.id = '';
    this.dataset = {};
    this.style = {};
    this.attrs = {};
    this.children = [];
    this.parent = null;
    this.classList = new MockClassList();
    this.eventListeners = {};
    this._textContent = '';
    this._innerHTML = '';
    this.scrollHeight = 0;
    this.scrollTop = 0;
    this._hidden = false;
  }
  get className() {
    return [...this.classList._set].join(' ');
  }
  set className(v) {
    this.classList._set.clear();
    for (const c of String(v).split(/\s+/).filter(Boolean)) {
      this.classList._set.add(c);
    }
  }
  get textContent() {
    if (this.children.length === 0) return this._textContent;
    return this.children.map(c => c.textContent).join('');
  }
  set textContent(v) {
    this._textContent = String(v);
    this.children = [];   // setting textContent replaces children
  }
  get innerHTML() { return this._innerHTML; }
  set innerHTML(v) {
    this._innerHTML = String(v);
    this.children = [];   // setting innerHTML clears children too
  }
  get hidden() { return this._hidden; }
  set hidden(v) { this._hidden = !!v; }
  appendChild(child) {
    if (child.parent) child.parent.removeChild(child);
    child.parent = this;
    this.children.push(child);
    return child;
  }
  removeChild(child) {
    const i = this.children.indexOf(child);
    if (i >= 0) this.children.splice(i, 1);
    child.parent = null;
    return child;
  }
  querySelector(sel) { return MockElement._findOne(this, sel); }
  querySelectorAll(sel) { return MockElement._findAll(this, sel); }
  addEventListener(ev, fn) {
    (this.eventListeners[ev] = this.eventListeners[ev] || []).push(fn);
  }
  // Simple selector matching: tag, .class, #id.
  static _matches(node, sel) {
    if (!sel) return false;
    if (sel.startsWith('.')) return node.classList.contains(sel.slice(1));
    if (sel.startsWith('#')) return node.id === sel.slice(1);
    return node.tag === sel;
  }
  static _findOne(root, sel) {
    if (MockElement._matches(root, sel)) return root;
    for (const c of root.children) {
      const f = MockElement._findOne(c, sel);
      if (f) return f;
    }
    return null;
  }
  static _findAll(root, sel) {
    const out = [];
    const visit = (n) => {
      if (MockElement._matches(n, sel)) out.push(n);
      for (const c of n.children) visit(c);
    };
    visit(root);
    return out;
  }
}

const document = {
  _byId: {},
  getElementById(id) {
    if (!this._byId[id]) {
      const el = new MockElement('div');
      el.id = id;
      this._byId[id] = el;
    }
    return this._byId[id];
  },
  createElement(tag) { return new MockElement(tag); },
  addEventListener() {},
  defineProperty() {},
  body: new MockElement('body'),
};

// ------------------------------------------------------------------------
// Mock EventSource — captures every listener registered so the test can
// fire synthetic events. The first instance is "the session's SSE" so
// we expose it via a captured global.
// ------------------------------------------------------------------------
const capturedEventSources = [];
class MockEventSource {
  constructor(url) {
    this.url = url;
    this.listeners = {};
    this._openListeners = [];
    capturedEventSources.push(this);
  }
  addEventListener(name, fn) {
    (this.listeners[name] = this.listeners[name] || []).push(fn);
  }
  removeEventListener(name, fn) {
    if (this.listeners[name]) {
      this.listeners[name] = this.listeners[name].filter(f => f !== fn);
    }
  }
  close() {}
}
function fire(es, name, payload) {
  const ev = { data: JSON.stringify(payload) };
  for (const fn of (es.listeners[name] || [])) fn(ev);
}

// ------------------------------------------------------------------------
// Mock fetch — POST /sessions returns a session id. Other calls no-op.
// ------------------------------------------------------------------------
const capturedFetches = [];
async function mockFetch(url, opts) {
  capturedFetches.push({ url, opts });
  if (url === '/sessions' && opts && opts.method === 'POST') {
    return { json: async () => ({ session_id: 'test-session',
                                  model: { id: 'mock' } }) };
  }
  if (url === '/info') {
    return { json: async () => ({ cwd: '/tmp' }) };
  }
  return { json: async () => ({}) };
}

// ------------------------------------------------------------------------
// Extract the <script>...</script> from the served HTML and run it in
// a vm context with all globals mocked.
// ------------------------------------------------------------------------
const htmlPath = process.argv[2] ||
    '/workspace/daedalus/demos/daedalus-webui/index_html.h';
const htmlSrc = fs.readFileSync(htmlPath, 'utf8');
const m = htmlSrc.match(/<script>([\s\S]+?)<\/script>/);
if (!m) { console.error('No <script> block found in', htmlPath); process.exit(2); }
const js = m[1];

const ctx = {
  console,
  setTimeout, clearTimeout,
  URL, FormData,
  document,
  EventSource: MockEventSource,
  fetch: mockFetch,
  // 'window' is referenced by the script but as a property in some places.
  // The script uses Object.defineProperty(window, ...) at the top — so
  // we need a real object there.
  window: {},
  location: { reload: () => {} },
  navigator: {},
};
// Add `window` and other globals that the script expects.
ctx.window.console = console;
ctx.window.document = document;
ctx.window.EventSource = MockEventSource;
ctx.window.fetch = mockFetch;
ctx.window.URL = URL;
ctx.window.setTimeout = setTimeout;
ctx.window.clearTimeout = clearTimeout;
ctx.window.location = ctx.location;
ctx.window.navigator = ctx.navigator;
vm.createContext(ctx);
vm.runInContext(js, ctx);

// ------------------------------------------------------------------------
// Wait one microtask tick so newSession()'s fetch().then() callback
// has a chance to call openEventStream() and create the EventSource.
// ------------------------------------------------------------------------
async function wait(ms) { return new Promise(r => setTimeout(r, ms)); }

function assert(cond, msg) {
  if (!cond) { console.log('FAIL:', msg); process.exit(1); }
  console.log('OK:  ', msg);
}

(async () => {
  // Let fetch + openEventStream complete.
  await wait(20);

  assert(capturedEventSources.length >= 1,
         'one EventSource was created (openEventStream)');
  const es = capturedEventSources[capturedEventSources.length - 1];

  // Helper to find a card by its data-id.
  const tools = document.getElementById('tools');
  function cardById(id) {
    // Our MockElement doesn't fully implement CSS selectors —
    // fall back to dataset.id matching.
    return tools.querySelector('div') ?
        [...tools.children].find(c => c.dataset && c.dataset.id === id) :
        null;
  }

  // ---------- Sequence: 3 sequential tool calls ----------

  // Call 1: read_file (success, with progress)
  fire(es, 'tool_call', {
    id: 'call_001', name: 'read_file',
    args: { path: '/tmp/foo.txt', line_numbers: true }
  });
  assert(!!cardById('call_001'), 'call_001 created');
  let c1 = cardById('call_001');
  assert(c1.classList.contains('running'), 'call_001 is running');
  const c1name = c1.querySelector('.tool-card-name').textContent;
  assert(c1name === 'read_file', `call_001 name = "${c1name}"`);

  fire(es, 'tool_progress', {
    id: 'call_001', progress: { stage: 'reading', percent: 50, message: 'halfway' }
  });
  const c1bar = c1.querySelector('.tool-card-progress-bar');
  assert(c1bar.style.width === '50%', `call_001 bar = "${c1bar.style.width}"`);
  const c1msg = c1.querySelector('.tool-card-message');
  assert(c1msg.textContent === 'halfway', `call_001 msg = "${c1msg.textContent}"`);

  fire(es, 'tool_result', {
    id: 'call_001',
    result: { status: 'success', path: '/tmp/foo.txt', content: 'hello\n' }
  });
  assert(c1.classList.contains('success'), 'call_001 success');
  assert(!c1.classList.contains('running'), 'call_001 not running');
  const c1res = c1.querySelector('.tool-card-result');
  assert(c1res && c1res.textContent.includes('hello'),
         'call_001 result rendered');

  // Call 2: edit_file_patch (success, no progress events)
  fire(es, 'tool_call', {
    id: 'call_002', name: 'edit_file_patch',
    args: { path: '/tmp/foo.txt', old_str: 'hello', new_str: 'goodbye' }
  });
  assert(!!cardById('call_002'), 'call_002 created');
  let c2 = cardById('call_002');
  assert(c2.classList.contains('running'), 'call_002 is running');

  // Cross-pollination check: progress for call_002 must NOT touch call_001.
  const c1barBefore = c1bar.style.width;
  fire(es, 'tool_progress', {
    id: 'call_002', progress: { stage: 'editing', percent: 100 }
  });
  assert(c1bar.style.width === c1barBefore,
         'call_001 progress bar unchanged when call_002 fires progress');

  fire(es, 'tool_result', {
    id: 'call_002', result: { status: 'success', applied: true }
  });
  assert(c2.classList.contains('success'), 'call_002 success');
  assert(c1.classList.contains('success'),
         'call_001 still success (not overwritten by call_002 result)');

  // Call 3: read_file (error)
  fire(es, 'tool_call', {
    id: 'call_003', name: 'read_file',
    args: { path: '/tmp/missing.txt' }
  });
  let c3 = cardById('call_003');
  assert(!!c3, 'call_003 created');
  assert(c3.classList.contains('running'), 'call_003 is running');

  fire(es, 'tool_result', {
    id: 'call_003',
    result: { status: 'error', message: 'File does not exist: /tmp/missing.txt' }
  });
  assert(c3.classList.contains('error'), 'call_003 error');
  assert(!c3.classList.contains('success'), 'call_003 not success');
  const c3res = c3.querySelector('.tool-card-result');
  assert(c3res.textContent.includes('File does not exist'),
         'call_003 error message rendered');

  // ---------- Final state assertions ----------

  assert(tools.children.length === 3,
         `3 cards in #tools (got ${tools.children.length})`);

  // Counter should show 2 ok · 1 err (in some order).
  const counter = document.getElementById('tool-counter');
  assert(!counter.hidden, 'counter visible');
  assert(counter.textContent.includes('2 ok'),
         `counter has "2 ok" — got "${counter.textContent}"`);
  assert(counter.textContent.includes('1 err'),
         `counter has "1 err" — got "${counter.textContent}"`);

  // Card order: tools.children preserves insertion order in our mock.
  // call_001 was created first, so it should be index 0.
  assert(tools.children[0].dataset.id === 'call_001',
         'first card is call_001');
  assert(tools.children[1].dataset.id === 'call_002',
         'second card is call_002');
  assert(tools.children[2].dataset.id === 'call_003',
         'third card is call_003');

  // ---------- Out-of-order: progress for an unknown id is dropped ----------
  fire(es, 'tool_progress', {
    id: 'call_unknown', progress: { stage: 'lost' }
  });
  // Should not throw, should not affect any card.
  assert(tools.children.length === 3,
         'still 3 cards after orphan progress');

  // ---------- Idempotent tool_call: same id twice ----------
  fire(es, 'tool_call', {
    id: 'call_001', name: 'read_file',  // same id as before
    args: { path: '/tmp/different.txt' }
  });
  assert(tools.children.length === 3,
         'idempotent tool_call does not create a 4th card');
  // Card args updated to the new value (per the "refresh args on retried
  // calls" comment in the script).
  const newArgs = c1.querySelector('.tool-card-args').textContent;
  assert(newArgs.includes('different.txt'),
         `card args refreshed on duplicate call_id — got "${newArgs}"`);

  console.log('\nAll three-call wiring assertions passed.');
  process.exit(0);
})().catch(e => { console.error(e); process.exit(1); });