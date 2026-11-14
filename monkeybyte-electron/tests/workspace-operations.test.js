// SPDX-License-Identifier: BSL-1.0
'use strict';

import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { excluded, globRegex, gitStatus, searchWorkspace } from '../src/main/workspace-operations.js';

test('workspace glob matching supports files, directories, and **', () => {
  assert.equal(globRegex('*.js').test('main.js'), true);
  assert.equal(globRegex('*.js').test('src/main.js'), false);
  assert.equal(globRegex('src/**/*.js').test('src/main/app.js'), true);
  assert.equal(excluded('build/debug/app.o', ['build']), true);
  assert.equal(excluded('src/node_modules/a.js', ['node_modules']), true);
  assert.equal(excluded('src/main.js', ['build', '*.map']), false);
});

test('workspace search is case aware, bounded, and ignores excluded/binary files', async (t) => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'mb-search-'));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  fs.mkdirSync(path.join(root, 'src'));
  fs.mkdirSync(path.join(root, 'build'));
  fs.writeFileSync(path.join(root, 'src', 'a.txt'), 'Alpha\nalpha alpha\n');
  fs.writeFileSync(path.join(root, 'build', 'ignored.txt'), 'alpha\n');
  fs.writeFileSync(path.join(root, 'src', 'binary.dat'), Buffer.from([0, 97]));
  const result = await searchWorkspace(root, { query: 'alpha', excludes: ['build'] });
  assert.deepEqual(result.matches.map((m) => [m.line, m.column]), [[1, 1], [2, 1], [2, 7]]);
  const sensitive = await searchWorkspace(root, { query: 'Alpha', caseSensitive: true });
  assert.deepEqual(sensitive.matches.map((m) => m.line), [1]);
  const bounded = await searchWorkspace(root, { query: 'alpha', maxResults: 1 });
  assert.equal(bounded.matches.length, 1);
  assert.equal(bounded.truncated, true);
});

test('git status parses branch, modifications, untracked files, and renames', async (t) => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'mb-git-'));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  execFileSync('git', ['init', '-q', '-b', 'main'], { cwd: root });
  execFileSync('git', ['config', 'user.email', 'test@example.invalid'], { cwd: root });
  execFileSync('git', ['config', 'user.name', 'Test'], { cwd: root });
  fs.writeFileSync(path.join(root, 'a.txt'), 'one\n');
  execFileSync('git', ['add', 'a.txt'], { cwd: root });
  execFileSync('git', ['commit', '-qm', 'initial'], { cwd: root });
  execFileSync('git', ['mv', 'a.txt', 'b.txt'], { cwd: root });
  fs.writeFileSync(path.join(root, 'b.txt'), 'two\n');
  fs.writeFileSync(path.join(root, 'new.txt'), 'new\n');
  const status = await gitStatus(root);
  assert.equal(status.branch, 'main');
  assert.ok(status.entries.some((e) => e.index === 'R' && e.path === 'b.txt' && e.oldPath === 'a.txt'));
  assert.ok(status.entries.some((e) => e.path === 'new.txt' && e.index === '?'));
});

test('git status reports staged/unstaged/untracked and upstream ahead/behind counts', async (t) => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'mb-git-counts-'));
  const remote = fs.mkdtempSync(path.join(os.tmpdir(), 'mb-git-remote-'));
  const peer = fs.mkdtempSync(path.join(os.tmpdir(), 'mb-git-peer-'));
  t.after(() => {
    fs.rmSync(root, { recursive: true, force: true });
    fs.rmSync(remote, { recursive: true, force: true });
    fs.rmSync(peer, { recursive: true, force: true });
  });
  const git = (cwd, ...args) => execFileSync('git', args, { cwd });
  git(root, 'init', '-q', '-b', 'main');
  git(root, 'config', 'user.email', 'test@example.invalid');
  git(root, 'config', 'user.name', 'Test');
  fs.writeFileSync(path.join(root, 'tracked.txt'), 'initial\n');
  git(root, 'add', 'tracked.txt');
  git(root, 'commit', '-qm', 'initial');
  git(remote, 'init', '-q', '--bare');
  git(root, 'remote', 'add', 'origin', remote);
  git(root, 'push', '-qu', 'origin', 'main');
  git(remote, 'symbolic-ref', 'HEAD', 'refs/heads/main');
  fs.writeFileSync(path.join(root, 'ahead.txt'), 'ahead\n');
  git(root, 'add', 'ahead.txt');
  git(root, 'commit', '-qm', 'ahead commit');

  execFileSync('git', ['clone', '-q', remote, peer]);
  git(peer, 'config', 'user.email', 'test@example.invalid');
  git(peer, 'config', 'user.name', 'Peer');
  fs.writeFileSync(path.join(peer, 'behind.txt'), 'behind\n');
  git(peer, 'add', 'behind.txt');
  git(peer, 'commit', '-qm', 'behind commit');
  git(peer, 'push', '-q', 'origin', 'main');
  git(root, 'fetch', '-q', 'origin');

  fs.writeFileSync(path.join(root, 'staged.txt'), 'staged\n');
  git(root, 'add', 'staged.txt');
  fs.writeFileSync(path.join(root, 'tracked.txt'), 'unstaged\n');
  fs.writeFileSync(path.join(root, 'untracked.txt'), 'untracked\n');
  const status = await gitStatus(root);
  assert.equal(status.branch, 'main');
  assert.equal(status.upstream, 'origin/main');
  assert.equal(status.ahead, 1);
  assert.equal(status.behind, 1);
  assert.equal(status.staged, 1);
  assert.equal(status.unstaged, 1);
  assert.equal(status.untracked, 1);
  assert.equal(status.changes, 3);
  assert.equal(status.entries.length, 3);
  assert.equal(status.notRepository, false);
});

test('non-Git workspace returns an empty status with no renderer-incompatible fields', async (t) => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'mb-not-git-'));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const status = await gitStatus(root);
  assert.equal(status.notRepository, true);
  assert.deepEqual(status.entries, []);
  assert.equal(status.changes, 0);
  assert.equal(status.ahead, 0);
  assert.equal(status.behind, 0);
});
