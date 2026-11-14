// SPDX-License-Identifier: BSL-1.0
import assert from 'node:assert/strict';
import test from 'node:test';

import { compactTokens, formatAgentStatus } from '../src/renderer/chat-view.js';

test('status bar formats the same live statistics as GTK', () => {
  assert.equal(compactTokens(950), '950');
  assert.equal(compactTokens(9500), '9.5k');
  assert.equal(compactTokens(250000), '250k');
  assert.equal(formatAgentStatus('fixture-model', {
    iteration: 3, totalTokens: 12800, maxContextSize: 250000,
    promptTokens: 10000, cachedTokens: 5000,
    completedToolCalls: 2, failedToolCalls: 1,
  }), 'fixture-model · 13k/250k ctx (5%) · cached 5.0k/10k · iter 3 · tools 2 +1 failed');
});

test('status bar shows only the model before a session has stats', () => {
  assert.equal(formatAgentStatus('fixture-model', {}), 'fixture-model');
  assert.equal(formatAgentStatus('', {}), 'No model');
});

import { formatGitSummary } from '../src/renderer/git-view.js';

test('Git status bar includes changes and numeric push/pull counts', () => {
  assert.equal(formatGitSummary({ branch: 'main', upstream: 'origin/main', ahead: 2, behind: 1,
    staged: 1, unstaged: 1, untracked: 1, changes: 3, entries: [], notRepository: false }),
    '⑂ main · 3 changes · 1 staged · 1 unstaged · 1 untracked · 2 to push · 1 to pull');
  assert.equal(formatGitSummary({ branch: 'main', upstream: 'origin/main', ahead: 0, behind: 0,
    staged: 0, unstaged: 0, untracked: 0, changes: 0, entries: [], notRepository: false }),
    '⑂ main · clean · 0 to push · 0 to pull');
  assert.equal(formatGitSummary({ branch: 'main', upstream: '', ahead: 0, behind: 0,
    staged: 0, unstaged: 0, untracked: 0, changes: 0, entries: [], notRepository: false }),
    '⑂ main · clean · no upstream');
  assert.equal(formatGitSummary({ notRepository: true }), 'Not a Git repository');
});
