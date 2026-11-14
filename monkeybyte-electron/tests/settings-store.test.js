import assert from 'node:assert/strict';
import { promises as fs } from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { afterEach, beforeEach, test } from 'node:test';

import {
  createSettingsStore,
  DEFAULT_SETTINGS,
  normalizeSettings,
} from '../src/main/settings-store.js';

let directory;
let filename;

beforeEach(async () => {
  directory = await fs.mkdtemp(path.join(os.tmpdir(), 'mb-settings-'));
  filename = path.join(directory, 'nested', 'settings.json');
});

afterEach(async () => {
  await fs.rm(directory, { recursive: true, force: true });
});

test('defaults and normalization enforce safe values', () => {
  assert.deepEqual(normalizeSettings(), DEFAULT_SETTINGS);
  const normalized = normalizeSettings({
    workspace: 'relative/path',
    fontSize: 900,
    tabSize: 3,
    sidebarWidth: 1,
    chatWidth: 5000,
    theme: 'blue',
    activePanel: 'invalid',
  });
  assert.equal(normalized.workspace, null);
  assert.equal(normalized.fontSize, DEFAULT_SETTINGS.fontSize);
  assert.equal(normalized.tabSize, DEFAULT_SETTINGS.tabSize);
  assert.equal(normalized.sidebarWidth, DEFAULT_SETTINGS.sidebarWidth);
  assert.equal(normalized.chatWidth, DEFAULT_SETTINGS.chatWidth);
  assert.equal(normalized.theme, DEFAULT_SETTINGS.theme);
  assert.equal(normalized.activePanel, DEFAULT_SETTINGS.activePanel);
});

test('settings persist atomically and reload', async () => {
  const store = createSettingsStore(filename);
  await store.load();
  await store.update({ theme: 'light', fontSize: 18, wordWrap: false, workspace: directory });
  const reloaded = createSettingsStore(filename);
  const settings = await reloaded.load();
  assert.equal(settings.theme, 'light');
  assert.equal(settings.fontSize, 18);
  assert.equal(settings.wordWrap, false);
  assert.equal(settings.workspace, directory);
  assert.equal(JSON.parse(await fs.readFile(filename, 'utf8')).theme, 'light');
});

test('malformed caches fail closed to defaults', async () => {
  await fs.mkdir(path.dirname(filename), { recursive: true });
  await fs.writeFile(filename, '{not json', 'utf8');
  const store = createSettingsStore(filename);
  assert.deepEqual(await store.load(), DEFAULT_SETTINGS);
});

test('concurrent updates are serialized without losing later writes', async () => {
  const store = createSettingsStore(filename);
  await store.load();
  await Promise.all([
    store.update({ theme: 'light' }),
    store.update({ fontSize: 20 }),
    store.update({ tabSize: 8 }),
  ]);
  const settings = store.snapshot();
  assert.equal(settings.theme, 'light');
  assert.equal(settings.fontSize, 20);
  assert.equal(settings.tabSize, 8);
});
