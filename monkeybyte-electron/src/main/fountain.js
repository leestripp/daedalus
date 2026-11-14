// SPDX-License-Identifier: BSL-1.0
import { execFile } from 'node:child_process';
import { promises as fs } from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { promisify } from 'node:util';

const execute = promisify(execFile);

async function converter() {
  for (const name of ['wrap', 'screenplain']) {
    try { await execute('which', [name], { timeout: 3000 }); return name; }
    catch {}
  }
  throw new Error('Fountain preview requires wrap or screenplain on PATH.');
}

async function convert(input, format) {
  const tool = await converter();
  const temporary = await fs.mkdtemp(path.join(os.tmpdir(), 'monkeybyte-fountain-'));
  try {
    const source = path.join(temporary, `script.fountain`);
    const output = path.join(temporary, `script.${format}`);
    await fs.copyFile(input, source);
    if (tool === 'wrap') await execute('wrap', [format, source], { timeout: 30000 });
    else await execute('screenplain', [...(format === 'html' ? ['--format', 'html'] : []), source, output], { timeout: 30000 });
    return await fs.readFile(output);
  } finally { await fs.rm(temporary, { recursive: true, force: true }); }
}

export function createFountainService() {
  return {
    async html(input) {
      const html = (await convert(input, 'html')).toString('utf8');
      return html.replace(/<script\b[^>]*>[\s\S]*?<\/script>/gi, '')
        .replace(/\son[a-z-]+\s*=\s*(?:"[^"]*"|'[^']*')/gi, '');
    },
    async pdf(input) { return convert(input, 'pdf'); },
  };
}
