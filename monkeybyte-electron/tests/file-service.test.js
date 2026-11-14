import assert from 'node:assert/strict';
import { promises as fs } from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { afterEach, beforeEach, test } from 'node:test';

import {
  createFolder,
  createTextFile,
  deleteWorkspacePath,
  importWorkspacePaths,
  listDirectory,
  MAX_READ_BYTES,
  readTextFile,
  renameWorkspacePath,
  validateWorkspace,
  writeTextFile,
} from '../src/main/file-service.js';

let root;
let outside;

beforeEach(async () => {
  root = await fs.mkdtemp(path.join(os.tmpdir(), 'mb-files-root-'));
  outside = await fs.mkdtemp(path.join(os.tmpdir(), 'mb-files-outside-'));
  await fs.mkdir(path.join(root, 'folder'));
  await fs.writeFile(path.join(root, 'z.txt'), 'z\n');
  await fs.writeFile(path.join(root, 'A.txt'), 'alpha\n');
  await fs.writeFile(path.join(root, '.hidden'), 'secret\n');
});

afterEach(async () => {
  await Promise.all([
    fs.rm(root, { recursive: true, force: true }),
    fs.rm(outside, { recursive: true, force: true }),
  ]);
});

test('workspace validation resolves the canonical directory', async () => {
  assert.equal(await validateWorkspace(root), await fs.realpath(root));
  await assert.rejects(validateWorkspace(path.join(root, 'missing')), /ENOENT/);
});

test('directory listing is sorted and hides dotfiles by default', async () => {
  const visible = await listDirectory(root);
  assert.deepEqual(visible.map((entry) => entry.name), ['folder', 'A.txt', 'z.txt']);
  const withHidden = await listDirectory(root, '', true);
  assert(withHidden.some((entry) => entry.name === '.hidden'));
  assert.equal(withHidden[0].isDirectory, true);
});

test('text files read as UTF-8 and writes are atomic', async () => {
  assert.equal(await readTextFile(root, 'A.txt'), 'alpha\n');
  await writeTextFile(root, 'A.txt', 'updated π\n');
  assert.equal(await readTextFile(root, 'A.txt'), 'updated π\n');
  await writeTextFile(root, 'new.txt', 'new file\n');
  assert.equal(await fs.readFile(path.join(root, 'new.txt'), 'utf8'), 'new file\n');
  const names = await fs.readdir(root);
  assert.equal(names.some((name) => name.includes('.monkeybyte-') && name.endsWith('.tmp')), false);
});

test('binary, invalid UTF-8, and oversized files are rejected', async () => {
  await fs.writeFile(path.join(root, 'binary.dat'), Buffer.from([0x41, 0x00, 0x42]));
  await fs.writeFile(path.join(root, 'invalid.txt'), Buffer.from([0xff, 0xfe]));
  await fs.writeFile(path.join(root, 'large.txt'), Buffer.alloc(MAX_READ_BYTES + 1, 0x41));
  await assert.rejects(readTextFile(root, 'binary.dat'), /Binary files/);
  await assert.rejects(readTextFile(root, 'invalid.txt'), /valid UTF-8/);
  await assert.rejects(readTextFile(root, 'large.txt'), /5 MB/);
});

test('absolute paths and parent traversal are rejected', async () => {
  await assert.rejects(readTextFile(root, '../escape.txt'), /escapes|Absolute/);
  await assert.rejects(readTextFile(root, path.join(outside, 'escape.txt')), /Absolute/);
  await assert.rejects(writeTextFile(root, '../escape.txt', 'bad'), /escapes/);
});

test('symlink escapes are rejected for reads, folders, and writes', async () => {
  await fs.writeFile(path.join(outside, 'escape.txt'), 'outside\n');
  await fs.symlink(path.join(outside, 'escape.txt'), path.join(root, 'link.txt'));
  await fs.symlink(outside, path.join(root, 'linked-folder'));
  await assert.rejects(readTextFile(root, 'link.txt'), /escapes/);
  await assert.rejects(listDirectory(root, 'linked-folder'), /escapes/);
  await assert.rejects(writeTextFile(root, 'link.txt', 'bad'), /non-symlink|escapes/);
  assert.equal(await fs.readFile(path.join(outside, 'escape.txt'), 'utf8'), 'outside\n');
});

test('writes require an existing contained parent directory', async () => {
  await assert.rejects(writeTextFile(root, 'missing/new.txt', 'bad'), /ENOENT/);
});

test('workspace mutations create, import, rename, and delete safely', async () => {
  
  await createFolder(root, 'docs');
  await createTextFile(root, 'docs/a.txt', 'alpha');
  assert.equal(await readTextFile(root, 'docs/a.txt'), 'alpha');
  const external = path.join(outside, 'external.txt');
  await fs.writeFile(external, 'imported');
  await importWorkspacePaths(root, 'docs', [external]);
  assert.equal(await readTextFile(root, 'docs/external.txt'), 'imported');
  await renameWorkspacePath(root, 'docs', 'library');
  assert.equal(await readTextFile(root, 'library/a.txt'), 'alpha');
  await deleteWorkspacePath(root, 'library');
  await assert.rejects(() => readTextFile(root, 'library/a.txt'), /ENOENT/);
  await assert.rejects(() => createTextFile(root, '../escape.txt'), /escapes/);
});

test('workspace mutations refuse existing destinations and symlink escapes', async () => {
  
  
  await createTextFile(root, 'a.txt', 'a');
  await assert.rejects(() => createTextFile(root, 'a.txt'), /EEXIST/);
  await createTextFile(root, 'b.txt', 'b');
  await assert.rejects(() => renameWorkspacePath(root, 'a.txt', 'b.txt'), /already exists/);
  await fs.symlink(outside, path.join(root, 'escape'));
  await assert.rejects(() => createFolder(root, 'escape/new'), /workspace|directory/);
});
