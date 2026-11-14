// SPDX-License-Identifier: BSL-1.0
import { WebContentsView } from 'electron';
import { promises as fs } from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

import { markdownToHtml } from './markdown.js';
import { readTextFile, resolveWorkspacePath } from './file-service.js';

const MAX_CONSOLE = 200;

export function normalizeAddress(input) {
  const value = String(input ?? '').trim();
  if (!value) return '';
  if (/^https?:\/\//i.test(value) || /^file:\/\//i.test(value)) return value;
  if (/^[a-z][a-z0-9+.-]*:/i.test(value)) throw new Error('Only HTTP, HTTPS, and file URLs are allowed.');
  if (path.isAbsolute(value)) return pathToFileURL(value).href;
  return `http://${value}`;
}

function bounded(value, maximum = 8000) {
  const text = String(value ?? '');
  return text.length > maximum ? `${text.slice(0, maximum)}…` : text;
}

function clamp(value, minimum, maximum, fallback) {
  const number = Number(value);
  return Number.isFinite(number) ? Math.max(minimum, Math.min(maximum, Math.trunc(number))) : fallback;
}

export function createBrowserManager({ getWindow, onChanged }) {
  const tabs = new Map();
  let activeId = null;
  let lastId = null;
  let nextId = 1;
  let hostBounds = null;
  let viewport = null;
  let colorScheme = 'dark';

  const background = () => colorScheme === 'dark' ? '#1e1e2e' : '#eff1f5';
  const updateBackgrounds = () => {
    for (const tab of tabs.values()) tab.view.setBackgroundColor(background());
  };

  async function applyColorScheme(tab) {
    const debuggerApi = tab.view.webContents.debugger;
    if (!debuggerApi.isAttached()) debuggerApi.attach('1.3');
    await debuggerApi.sendCommand('Emulation.setEmulatedMedia', {
      features: [{ name: 'prefers-color-scheme', value: colorScheme }],
    });
    await debuggerApi.sendCommand('Emulation.setAutoDarkModeOverride', {
      enabled: colorScheme === 'dark',
    });
    await debuggerApi.sendCommand('Network.enable');
    await debuggerApi.sendCommand('Network.setExtraHTTPHeaders', {
      headers: { 'Sec-CH-Prefers-Color-Scheme': colorScheme },
    });
  }

  function metadata() {
    return { activeId, colorScheme, tabs: [...tabs.values()].map((tab) => ({
      id: tab.id, url: tab.view.webContents.getURL() || tab.initialUrl,
      initialUrl: tab.initialUrl, title: tab.view.webContents.getTitle() || tab.initialUrl,
      kind: tab.kind, canGoBack: tab.view.webContents.navigationHistory.canGoBack(),
      canGoForward: tab.view.webContents.navigationHistory.canGoForward(),
    })) };
  }

  function changed() { onChanged?.(metadata()); }

  function target(idOrUrl) {
    if (idOrUrl && tabs.has(idOrUrl)) return tabs.get(idOrUrl);
    if (idOrUrl) {
      const match = [...tabs.values()].find((tab) => tab.id === String(idOrUrl) ||
        tab.initialUrl === idOrUrl || tab.view.webContents.getURL() === idOrUrl);
      if (match) return match;
    }
    return tabs.get(activeId) ?? tabs.get(lastId) ?? null;
  }

  function layout() {
    const tab = tabs.get(activeId);
    if (!tab || !hostBounds) return;
    const width = viewport?.width ? Math.min(viewport.width, hostBounds.width) : hostBounds.width;
    const height = viewport?.height ? Math.min(viewport.height, hostBounds.height) : hostBounds.height;
    tab.view.setBounds({ x: Math.round(hostBounds.x + (hostBounds.width - width) / 2),
      y: Math.round(hostBounds.y), width: Math.round(width), height: Math.round(height) });
  }

  function activate(id) {
    if (!tabs.has(id)) throw new Error('No matching browser tab.');
    if (activeId === id) return metadata();
    activeId = id; lastId = id;
    for (const tab of tabs.values()) tab.view.setVisible(tab.id === id);
    layout(); changed();
    return metadata();
  }

  function create(url, kind = 'browser') {
    const normalized = ['markdown', 'fountain'].includes(kind) && String(url).startsWith('data:text/html')
      ? String(url) : normalizeAddress(url);
    if (!normalized) throw new Error('Missing URL.');
    const id = `browser-${nextId++}`;
    const view = new WebContentsView({ webPreferences: {
      sandbox: true, contextIsolation: true, nodeIntegration: false, javascript: true,
      safeDialogs: true, navigateOnDragDrop: false,
    } });
    view.setBackgroundColor(background());
    const tab = { id, view, initialUrl: normalized, kind, console: [] };
    tabs.set(id, tab);
    getWindow().contentView.addChildView(view);
    view.setVisible(false);
    const contents = view.webContents;
    contents.setWindowOpenHandler(({ url: popup }) => {
      try { create(popup); } catch { return { action: 'deny' }; }
      return { action: 'deny' };
    });
    contents.on('will-navigate', (event, destination) => {
      if (!/^(?:https?|file):/i.test(destination)) event.preventDefault();
    });
    contents.on('did-start-navigation', () => { tab.console = []; changed(); });
    contents.on('did-navigate', () => {
      applyColorScheme(tab).catch((error) => {
        tab.console.push({ level: 'error', text: `Color-scheme emulation failed: ${bounded(error.message)}` });
      });
      changed();
    });
    contents.on('did-navigate-in-page', changed);
    contents.on('page-title-updated', changed);
    contents.on('console-message', (event) => {
      const level = event.level === 'warning' ? 'warn' : (event.level === 'debug' ? 'verbose' : event.level);
      tab.console.push({ level: level ?? 'log', text: bounded(event.message, 4000) });
      if (tab.console.length > MAX_CONSOLE) tab.console.splice(0, tab.console.length - MAX_CONSOLE);
    });
    contents.on('render-process-gone', (_event, details) => {
      tab.console.push({ level: 'error', text: `Renderer gone: ${details.reason}` });
      changed();
    });
    contents.on('did-fail-load', (_event, _code, description, validatedURL, isMainFrame) => {
      if (isMainFrame) tab.console.push({ level: 'error', text: `${description}: ${validatedURL}` });
    });
    applyColorScheme(tab).catch((error) => {
      tab.console.push({ level: 'error', text: `Color-scheme emulation failed: ${bounded(error.message)}` });
    });
    activate(id);
    if (kind === 'markdown') contents.loadURL(normalized).catch(() => {});
    else contents.loadURL(normalized).catch((error) => {
      tab.console.push({ level: 'error', text: bounded(error.message) }); changed();
    });
    return { id, url: normalized };
  }

  function close(idOrUrl) {
    const tab = target(idOrUrl);
    if (!tab) return { closed: false };
    getWindow().contentView.removeChildView(tab.view);
    tab.view.webContents.destroy();
    tabs.delete(tab.id);
    if (activeId === tab.id) activeId = null;
    if (lastId === tab.id) lastId = [...tabs.keys()].at(-1) ?? null;
    changed();
    return { closed: true, id: tab.id };
  }

  function hide() {
    activeId = null;
    for (const tab of tabs.values()) tab.view.setVisible(false);
    changed();
  }

  async function read(args = {}) {
    const tab = target(args.tab);
    if (!tab) throw new Error('No matching browser tab.');
    const maximum = clamp(args.max_chars, 256, 20000, 8000);
    const selector = JSON.stringify(args.selector || '');
    const result = await tab.view.webContents.executeJavaScript(`(() => {
      const selector=${selector}; const root=selector?document.querySelector(selector):document.body;
      return {url:location.href,title:document.title,text:(root?.innerText||'').slice(0,${maximum})};
    })()`, true);
    return { ok: true, ...result, truncated: String(result.text).length >= maximum };
  }

  async function evaluate(args = {}) {
    const tab = target(args.tab);
    if (!tab) throw new Error('No matching browser tab.');
    if (!args.script) throw new Error('Missing script.');
    const maximum = clamp(args.max_chars, 256, 20000, 8000);
    const debuggerApi = tab.view.webContents.debugger;
    if (!debuggerApi.isAttached()) debuggerApi.attach('1.3');
    const response = await debuggerApi.sendCommand('Runtime.evaluate', {
      expression: String(args.script), awaitPromise: true, returnByValue: true, userGesture: true,
    });
    if (response.exceptionDetails) {
      const detail = response.exceptionDetails.exception?.description ?? response.exceptionDetails.text;
      throw new Error(detail || 'Browser script failed.');
    }
    const remote = response.result ?? {};
    const value = remote.type === 'undefined' ? null
      : (remote.value ?? remote.unserializableValue ?? remote.description ?? null);
    const raw = JSON.stringify(value) ?? 'null';
    return { ok: true, result: raw.length > maximum ? raw.slice(0, maximum) : value,
      truncated: raw.length > maximum };
  }

  async function links(args = {}) {
    const tab = target(args.tab);
    if (!tab) throw new Error('No matching browser tab.');
    const selector = JSON.stringify(args.selector || '');
    const maximum = clamp(args.max, 1, 500, 100);
    const result = await tab.view.webContents.executeJavaScript(`(() => {
      const selector=${selector},seen=new Set(),links=[];
      for(const node of document.querySelectorAll(selector?selector+' a':'a')) {
        if(!node.href||seen.has(node.href))continue; seen.add(node.href);
        links.push({text:(node.innerText||'').trim(),href:node.href}); if(links.length>=${maximum})break;
      } return {links};
    })()`, true);
    return { ok: true, ...result };
  }

  function consoleEntries(args = {}) {
    const tab = target(args.tab);
    if (!tab) throw new Error('No matching browser tab.');
    const maximum = clamp(args.max_entries, 1, MAX_CONSOLE, 50);
    const entries = tab.console.slice(-maximum);
    if (args.clear) tab.console = [];
    return { ok: true, entries };
  }

  async function upload(args = {}) {
    const tab = target(args.tab);
    if (!tab) throw new Error('No matching browser tab.');
    const paths = Array.isArray(args.paths) ? args.paths : [args.path];
    if (!paths.length || paths.some((value) => typeof value !== 'string' || !path.isAbsolute(value))) {
      throw new Error('Upload paths must be absolute.');
    }
    for (const filename of paths) await fs.stat(filename);
    const debuggerApi = tab.view.webContents.debugger;
    const attachedForUpload = !debuggerApi.isAttached();
    if (attachedForUpload) debuggerApi.attach('1.3');
    try {
      const { root } = await debuggerApi.sendCommand('DOM.getDocument');
      const selector = args.selector || 'input[type="file"]';
      const { nodeId } = await debuggerApi.sendCommand('DOM.querySelector', { nodeId: root.nodeId, selector });
      if (!nodeId) throw new Error(`No file input matches ${selector}.`);
      await debuggerApi.sendCommand('DOM.setFileInputFiles', { files: paths, nodeId });
    } finally { if (attachedForUpload) debuggerApi.detach(); }
    return { ok: true, paths };
  }

  async function openMarkdown(root, relativePath, dark) {
    const markdown = await readTextFile(root, relativePath);
    const baseUrl = `${pathToFileURL(path.dirname(path.join(root, relativePath)) + path.sep).href}`;
    const html = markdownToHtml(markdown, { title: path.basename(relativePath), baseUrl, dark });
    return create(`data:text/html;charset=utf-8,${encodeURIComponent(html)}`, 'markdown');
  }

  async function openHtml(root, relativePath) {
    const absolute = await resolveWorkspacePath(root, relativePath);
    await fs.stat(absolute);
    return create(pathToFileURL(absolute).href, 'html');
  }

  async function execute(command, args = {}) {
    if (command === 'browser.open') return create(args.url);
    if (command === 'browser.navigate') {
      const tab = target(args.tab); if (!tab) throw new Error('No matching browser tab.');
      const url = normalizeAddress(args.url); await tab.view.webContents.loadURL(url); changed(); return { ok: true, url };
    }
    if (command === 'browser.scheme') {
      if (!['dark', 'light'].includes(args.scheme)) throw new Error('Color scheme must be dark or light.');
      colorScheme = args.scheme;
      updateBackgrounds();
      await Promise.all([...tabs.values()].map(applyColorScheme));
      changed();
      return { ok: true, colorScheme };
    }
    if (command === 'browser.read') return read(args);
    if (command === 'browser.eval') return evaluate(args);
    if (command === 'browser.links') return links(args);
    if (command === 'browser.console') return consoleEntries(args);
    if (command === 'browser.upload') return upload(args);
    if (command === 'browser.list') return { ok: true, ...metadata() };
    if (command === 'browser.close') return close(args.tab ?? args.url);
    if (command === 'browser.activate') return activate(args.tab);
    if (command === 'browser.hide') { hide(); return { ok: true }; }
    if (command === 'browser.back') { const tab = target(args.tab); tab?.view.webContents.navigationHistory.goBack(); return { ok: true }; }
    if (command === 'browser.forward') { const tab = target(args.tab); tab?.view.webContents.navigationHistory.goForward(); return { ok: true }; }
    if (command === 'browser.reload') { const tab = target(args.tab); tab?.view.webContents.reload(); return { ok: true }; }
    throw new Error(`Unknown browser command: ${command}`);
  }

  return {
    execute, create, close, activate, openMarkdown, openHtml, metadata,
    setBounds(bounds) { hostBounds = bounds; layout(); },
    setViewport(next) { viewport = next; layout(); },
    hide,
    dispose() { for (const id of [...tabs.keys()]) close(id); },
  };
}
