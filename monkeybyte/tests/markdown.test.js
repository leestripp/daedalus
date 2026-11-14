// SPDX-License-Identifier: BSL-1.0
import assert from 'node:assert/strict';
import test from 'node:test';
import { markdownToHtml, safeHref } from '../src/main/markdown.js';

test('Markdown renders common blocks and escapes active HTML', () => {
  const html = markdownToHtml('# Title\n\n**bold** [link](https://example.com)\n\n- one\n- two\n\n```\n<script>alert(1)</script>\n```', { baseUrl: 'file:///workspace/' });
  assert.match(html, /<h1>Title<\/h1>/);
  assert.match(html, /<strong>bold<\/strong>/);
  assert.match(html, /<ul>\s*<li>one<\/li>\s*<li>two<\/li>\s*<\/ul>/);
  assert.match(html, /&lt;script&gt;/);
  assert.doesNotMatch(html, /<script>alert/);
  assert.match(html, /script-src 'none'/);
});

test('Markdown rejects dangerous link schemes', () => {
  assert.equal(safeHref('javascript:alert(1)'), '#');
  assert.equal(safeHref('data:text/html,hello'), '#');
  assert.equal(safeHref('https://example.com'), 'https://example.com');
  assert.equal(safeHref('../image.png'), '../image.png');
});
