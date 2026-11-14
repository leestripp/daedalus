import { randomUUID } from 'node:crypto';
import { promises as fs } from 'node:fs';
import path from 'node:path';

const PANELS = new Set(['workspace', 'search', 'git', 'settings']);
const THEMES = new Set(['system', 'dark', 'light']);

export const DEFAULT_SETTINGS = Object.freeze({
  workspace: null,
  showHidden: false,
  showLineNumbers: true,
  highlightCurrentLine: true,
  searchExcludes: ['.git', 'node_modules', 'build', 'dist', 'release', '*.min.js'],
  browserHomeUrl: '',
  theme: 'system',
  fontSize: 14,
  tabSize: 4,
  wordWrap: true,
  sidebarWidth: 260,
  chatWidth: 340,
  activePanel: 'workspace',
  windowWidth: 1100,
  windowHeight: 720,
});

function integerInRange(value, fallback, minimum, maximum) {
  return Number.isInteger(value) && value >= minimum && value <= maximum ? value : fallback;
}

export function normalizeSettings(candidate = {}) {
  const source = candidate && typeof candidate === 'object' ? candidate : {};
  const workspace = typeof source.workspace === 'string' && path.isAbsolute(source.workspace)
    ? path.resolve(source.workspace)
    : null;
  const tabSize = [2, 4, 8].includes(source.tabSize) ? source.tabSize : DEFAULT_SETTINGS.tabSize;
  return {
    workspace,
    showHidden: typeof source.showHidden === 'boolean' ? source.showHidden : DEFAULT_SETTINGS.showHidden,
    showLineNumbers: typeof source.showLineNumbers === 'boolean' ? source.showLineNumbers : DEFAULT_SETTINGS.showLineNumbers,
    highlightCurrentLine: typeof source.highlightCurrentLine === 'boolean' ? source.highlightCurrentLine : DEFAULT_SETTINGS.highlightCurrentLine,
    searchExcludes: Array.isArray(source.searchExcludes) ? source.searchExcludes.filter((v) => typeof v === 'string').slice(0, 100) : [...DEFAULT_SETTINGS.searchExcludes],
    browserHomeUrl: typeof source.browserHomeUrl === 'string' ? source.browserHomeUrl.slice(0, 2000) : '',
    theme: THEMES.has(source.theme) ? source.theme : DEFAULT_SETTINGS.theme,
    fontSize: integerInRange(source.fontSize, DEFAULT_SETTINGS.fontSize, 10, 24),
    tabSize,
    wordWrap: typeof source.wordWrap === 'boolean' ? source.wordWrap : DEFAULT_SETTINGS.wordWrap,
    sidebarWidth: integerInRange(source.sidebarWidth, DEFAULT_SETTINGS.sidebarWidth, 150, 500),
    chatWidth: integerInRange(source.chatWidth, DEFAULT_SETTINGS.chatWidth, 250, 900),
    activePanel: PANELS.has(source.activePanel) ? source.activePanel : DEFAULT_SETTINGS.activePanel,
    windowWidth: integerInRange(source.windowWidth, DEFAULT_SETTINGS.windowWidth, 900, 7680),
    windowHeight: integerInRange(source.windowHeight, DEFAULT_SETTINGS.windowHeight, 600, 4320),
  };
}

export function createSettingsStore(filename) {
  let current = { ...DEFAULT_SETTINGS };
  let queue = Promise.resolve();

  async function load() {
    try {
      const parsed = JSON.parse(await fs.readFile(filename, 'utf8'));
      current = normalizeSettings(parsed);
    } catch (error) {
      if (error?.code !== 'ENOENT') {
        console.warn(`MonkeyByte settings cache was reset: ${error.message}`);
      }
      current = { ...DEFAULT_SETTINGS };
    }
    return { ...current };
  }

  function snapshot() {
    return { ...current };
  }

  function update(patch) {
    const operation = queue.then(async () => {
      current = normalizeSettings({ ...current, ...patch });
      await fs.mkdir(path.dirname(filename), { recursive: true });
      const temporary = `${filename}.${process.pid}-${randomUUID()}.tmp`;
      try {
        await fs.writeFile(temporary, `${JSON.stringify(current, null, 2)}\n`, {
          encoding: 'utf8',
          flag: 'wx',
        });
        await fs.rename(temporary, filename);
      } catch (error) {
        await fs.rm(temporary, { force: true }).catch(() => {});
        throw error;
      }
      return snapshot();
    });
    queue = operation.catch(() => {});
    return operation;
  }

  return { load, snapshot, update, filePath: filename };
}
