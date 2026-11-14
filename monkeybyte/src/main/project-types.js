// SPDX-License-Identifier: BSL-1.0
import { promises as fs } from 'node:fs';
import path from 'node:path';

function parseType(filename, text) {
  const match = /^---\r?\n([\s\S]*?)\r?\n---\r?\n?/.exec(text);
  const fields = {};
  if (match) {
    for (const line of match[1].split(/\r?\n/)) {
      const colon = line.indexOf(':');
      if (colon > 0) fields[line.slice(0, colon).trim()] = line.slice(colon + 1).trim();
    }
  }
  const id = fields.id || path.basename(filename, '.md');
  return { id, name: fields.name || id, description: fields.description || '',
    detect: (fields.detect || '').split(',').map((value) => value.trim()).filter(Boolean),
    prompt: (match ? text.slice(match[0].length) : text).trim(), sourceFile: filename,
    templatesDir: path.join(path.dirname(filename), `${id}.templates`) };
}

function stamp(id) { return `<!-- monkeybyte:type: ${id} -->`; }

export function createProjectTypeService({ builtinDir, userDir, backend }) {
  async function list() {
    const types = new Map();
    for (const [directory, custom] of [[builtinDir, false], [userDir, true]]) {
      let names = [];
      try { names = await fs.readdir(directory); } catch (error) { if (error.code !== 'ENOENT') throw error; }
      for (const name of names.filter((name) => name.endsWith('.md')).sort()) {
        const filename = path.join(directory, name);
        const type = parseType(filename, await fs.readFile(filename, 'utf8'));
        types.set(type.id, { ...type, custom });
      }
    }
    return [...types.values()];
  }

  async function get(id) {
    for (const directory of [userDir, builtinDir]) {
      try {
        const filename = path.join(directory, `${id}.md`);
        return parseType(filename, await fs.readFile(filename, 'utf8'));
      } catch (error) { if (error.code !== 'ENOENT') throw error; }
    }
    throw new Error(`Unknown project type: ${id}`);
  }

  async function detect(workspace) {
    if (!workspace) return null;
    const types = [];
    for (const item of await list()) types.push(await get(item.id));
    try {
      const agents = await fs.readFile(path.join(workspace, 'AGENTS.md'), 'utf8');
      const match = /<!--\s*monkeybyte:type:\s*([A-Za-z0-9_.-]+)\s*-->/.exec(agents);
      if (match) return (await list()).find((type) => type.id === match[1]) ?? null;
    } catch (error) { if (error.code !== 'ENOENT') throw error; }
    const files = await fs.readdir(workspace);
    return types.find((type) => type.detect.some((pattern) => {
      if (pattern.startsWith('*.')) return files.some((file) => file.endsWith(pattern.slice(1)));
      return files.includes(pattern);
    })) ?? types.find((type) => type.id === 'generic') ?? null;
  }

  async function apply(workspace, id) {
    const type = await get(id);
    await fs.mkdir(workspace, { recursive: true });
    const agentsPath = path.join(workspace, 'AGENTS.md');
    let agents = '';
    try { agents = await fs.readFile(agentsPath, 'utf8'); } catch (error) { if (error.code !== 'ENOENT') throw error; }
    const line = stamp(id);
    if (/^.*monkeybyte:type:.*$/m.test(agents)) agents = agents.replace(/^.*monkeybyte:type:.*$/m, line);
    else agents = `${line}\n\n${agents || '# Project\n\n'}`;
    await fs.writeFile(agentsPath, agents, 'utf8');
    try {
      for (const name of await fs.readdir(type.templatesDir)) {
        const source = path.join(type.templatesDir, name);
        if (!(await fs.stat(source)).isFile()) continue;
        await fs.copyFile(source, path.join(workspace, name), fs.constants.COPYFILE_EXCL);
      }
    } catch (error) { if (!['ENOENT', 'EEXIST'].includes(error.code)) throw error; }
    await backend.request('/v1/settings', { method: 'POST', body: {
      workspacePrompt: { workspace, prompt: type.prompt },
    } });
    return detect(workspace);
  }

  async function savePrompt(id, prompt) {
    if (!/^[A-Za-z0-9_.-]+$/.test(id)) throw new Error('Invalid project type id.');
    let existing;
    try { existing = await get(id); }
    catch { existing = { id, name: id, description: 'Custom project type', detect: [], sourceFile: path.join(userDir, `${id}.md`) }; }
    await fs.mkdir(userDir, { recursive: true });
    const filename = path.join(userDir, `${id}.md`);
    let front = null;
    try {
      const source = await fs.readFile(existing.sourceFile, 'utf8');
      const boundary = source.indexOf('\n---', 4);
      front = source.startsWith('---') && boundary >= 0 ? source.slice(0, boundary + 5) : null;
    } catch (error) { if (error.code !== 'ENOENT') throw error; }
    const header = front || `---\nid: ${id}\nname: ${existing.name}\ndescription: ${existing.description}\ndetect: ${existing.detect.join(', ')}\n---\n\n`;
    await fs.writeFile(filename, `${header}${prompt.trim()}\n`, 'utf8');
    return list();
  }

  return {
    list: async (workspace) => ({ types: await list(), current: workspace ? await detect(workspace) : null }),
    apply,
    create: async (workspace, id) => { await apply(workspace, id); return workspace; },
    savePrompt,
  };
}
