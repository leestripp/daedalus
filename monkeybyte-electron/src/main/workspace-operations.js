// SPDX-License-Identifier: BSL-1.0
'use strict';

import { spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

export function globRegex(pattern) {
  let out = '^';
  for (let i = 0; i < pattern.length; ++i) {
    const c = pattern[i];
    if (c === '*') {
      if (pattern[i + 1] === '*') {
        out += '.*';
        ++i;
      } else out += '[^/]*';
    } else if (c === '?') out += '[^/]';
    else out += /[\\^$.*+?()[\]{}|]/.test(c) ? `\\${c}` : c;
  }
  return new RegExp(`${out}$`);
}

export function excluded(relative, patterns) {
  const normalized = relative.split(path.sep).join('/');
  const base = path.basename(normalized);
  return patterns.some((pattern) => {
    const p = pattern.trim().replace(/^\.\//, '').replace(/\/$/, '');
    if (!p) return false;
    if (!p.includes('/') && (globRegex(p).test(base) ||
        normalized.split('/').some((segment) => globRegex(p).test(segment)))) return true;
    return globRegex(p).test(normalized) || normalized.startsWith(`${p}/`);
  });
}

export async function searchWorkspace(root, options = {}) {
  const query = String(options.query || '');
  if (!query) return { matches: [], truncated: false };
  const maxResults = Math.max(1, Math.min(1000, Number(options.maxResults) || 300));
  const patterns = Array.isArray(options.excludes) ? options.excludes : [];
  const caseSensitive = options.caseSensitive === true;
  const needle = caseSensitive ? query : query.toLocaleLowerCase();
  const matches = [];
  let truncated = false;
  const pending = [''];
  while (pending.length && !truncated) {
    const relativeDir = pending.shift();
    const absoluteDir = path.join(root, relativeDir);
    let entries;
    try {
      entries = await fs.promises.readdir(absoluteDir, { withFileTypes: true });
    } catch {
      continue;
    }
    entries.sort((a, b) => a.name.localeCompare(b.name));
    for (const entry of entries) {
      const relative = relativeDir ? path.join(relativeDir, entry.name) : entry.name;
      if (excluded(relative, patterns)) continue;
      if (entry.isSymbolicLink()) continue;
      if (entry.isDirectory()) {
        pending.push(relative);
        continue;
      }
      if (!entry.isFile()) continue;
      const normalized = relative.split(path.sep).join('/');
      if (options.fileGlob && !globRegex(options.fileGlob).test(normalized) &&
          !globRegex(options.fileGlob).test(entry.name)) continue;
      let data;
      try {
        const stat = await fs.promises.stat(path.join(root, relative));
        if (stat.size > 2 * 1024 * 1024) continue;
        data = await fs.promises.readFile(path.join(root, relative));
      } catch {
        continue;
      }
      if (data.includes(0)) continue;
      let text;
      try {
        text = new TextDecoder('utf-8', { fatal: true }).decode(data);
      } catch {
        continue;
      }
      const lines = text.split(/\r?\n/);
      for (let line = 0; line < lines.length; ++line) {
        const haystack = caseSensitive ? lines[line] : lines[line].toLocaleLowerCase();
        let from = 0;
        while (from <= haystack.length) {
          const column = haystack.indexOf(needle, from);
          if (column < 0) break;
          matches.push({ path: normalized, line: line + 1, column: column + 1,
                         preview: lines[line].trim().slice(0, 500) });
          if (matches.length >= maxResults) {
            truncated = true;
            break;
          }
          from = column + Math.max(1, needle.length);
        }
        if (truncated) break;
      }
      if (truncated) break;
    }
  }
  return { matches, truncated };
}

export function runGit(root, args) {
  return new Promise((resolve, reject) => {
    const child = spawn('git', args, { cwd: root, stdio: ['ignore', 'pipe', 'pipe'] });
    const stdout = [];
    const stderr = [];
    let bytes = 0;
    const timer = setTimeout(() => child.kill('SIGTERM'), 5000);
    child.stdout.on('data', (chunk) => {
      bytes += chunk.length;
      if (bytes <= 4 * 1024 * 1024) stdout.push(chunk);
      else child.kill('SIGTERM');
    });
    child.stderr.on('data', (chunk) => stderr.push(chunk));
    child.once('error', (error) => {
      clearTimeout(timer);
      reject(error);
    });
    child.once('close', (code) => {
      clearTimeout(timer);
      if (code === 0) resolve(Buffer.concat(stdout).toString('utf8'));
      else reject(new Error(Buffer.concat(stderr).toString('utf8').trim() ||
                            `git exited with status ${code}`));
    });
  });
}

async function readGitStatus(root) {
  const raw = await runGit(root, ['status', '--porcelain=v1', '-z', '--branch']);
  const fields = raw.split('\0');
  const entries = [];
  let branch = '';
  let upstream = '';
  let ahead = 0;
  let behind = 0;
  for (let i = 0; i < fields.length; ++i) {
    const field = fields[i];
    if (!field) continue;
    if (field.startsWith('## ')) {
      const header = field.slice(3);
      branch = header.split('...')[0];
      if (branch.startsWith('No commits yet on ')) branch = branch.slice('No commits yet on '.length);
      const upstreamMatch = header.match(/\.\.\.([^\s\]]+)/);
      upstream = upstreamMatch?.[1] ?? '';
      ahead = Number(header.match(/ahead (\d+)/)?.[1] ?? 0);
      behind = Number(header.match(/behind (\d+)/)?.[1] ?? 0);
      continue;
    }
    if (field.length < 4) continue;
    const index = field[0];
    const worktree = field[1];
    const filePath = field.slice(3);
    let oldPath = '';
    if (index === 'R' || index === 'C' || worktree === 'R' || worktree === 'C') {
      oldPath = fields[++i] || '';
    }
    entries.push({ index, worktree, path: filePath, oldPath });
  }
  const staged = entries.filter((entry) => entry.index !== ' ' && entry.index !== '?').length;
  const unstaged = entries.filter((entry) => entry.worktree !== ' ' && entry.worktree !== '?').length;
  const untracked = entries.filter((entry) => entry.index === '?' || entry.worktree === '?').length;
  return { branch, upstream, ahead, behind, staged, unstaged, untracked,
    changes: entries.length, entries, notRepository: false };
}

function emptyGitStatus(notRepository) {
  return { branch: '', upstream: '', ahead: 0, behind: 0, staged: 0, unstaged: 0,
    untracked: 0, changes: 0, entries: [], notRepository };
}


export async function gitStatus(root) {
  try { return await readGitStatus(root); }
  catch (error) {
    if (String(error.stderr ?? error.message).includes('not a git repository')) {
      return emptyGitStatus(true);
    }
    throw error;
  }
}
