import { app, clipboard, dialog, ipcMain, nativeTheme, shell } from 'electron';
import { spawn } from 'node:child_process';
import path from 'node:path';
import { promises as fs } from 'node:fs';
import { randomUUID } from 'node:crypto';

import {
  createFolder,
  createTextFile,
  deleteWorkspacePath,
  importWorkspacePaths,
  listDirectory,
  readTextFile,
  renameWorkspacePath,
  resolveWorkspacePath,
  validateWorkspace,
  writeTextFile,
} from './file-service.js';
import { gitStatus, searchWorkspace } from './workspace-operations.js';
import { createBrowserManager } from './browser-manager.js';
import { createBackendClient } from './backend-client.js';
import { createChimpFlowService } from './chimpflow.js';
import { createProjectTypeService } from './project-types.js';
import { createFountainService } from './fountain.js';

const UI_SETTING_KEYS = new Set([
  'showHidden',
  'showLineNumbers',
  'highlightCurrentLine',
  'searchExcludes',
  'browserHomeUrl',
  'theme',
  'fontSize',
  'tabSize',
  'wordWrap',
  'sidebarWidth',
  'chatWidth',
  'activePanel',
]);

function publicError(error) {
  return new Error(error instanceof Error ? error.message : String(error));
}

export async function registerIpc({ store, getWindow, backendExecutable }) {
  const browser = createBrowserManager({ getWindow, onChanged: (metadata) => {
    getWindow()?.webContents.send('mb:browser:changed', metadata);
  } });
  const allowedRoots = new Set();

  async function allowWorkspace(root) {
    const canonical = await validateWorkspace(root);
    allowedRoots.add(canonical);
    return canonical;
  }

  async function currentWorkspace() {
    const workspace = store.snapshot().workspace;
    if (!workspace) {
      throw new Error('Select a workspace first.');
    }
    return allowWorkspace(workspace);
  }

  async function trustedRoot(root) {
    if (typeof root !== 'string' || !path.isAbsolute(root)) {
      throw new Error('Invalid workspace root.');
    }
    const canonical = await validateWorkspace(root);
    if (!allowedRoots.has(canonical)) {
      throw new Error('This workspace is no longer available to the window.');
    }
    return canonical;
  }

  function handle(channel, callback) {
    ipcMain.handle(channel, async (event, ...args) => {
      if (event.sender !== getWindow()?.webContents) {
        throw new Error('IPC sender is not the MonkeyByte window.');
      }
      try {
        return await callback(...args);
      } catch (error) {
        throw publicError(error);
      }
    });
  }

  const hostRequests = new Map();
  handle('mb:host:result', async (id, result) => {
    const pending = hostRequests.get(id);
    if (!pending) throw new Error('Unknown host command response.');
    hostRequests.delete(id);
    clearTimeout(pending.timeout);
    pending.resolve(result);
    return { ok: true };
  });

  function rendererCommand(command, args = {}) {
    const id = randomUUID();
    return new Promise((resolve, reject) => {
      const timeout = setTimeout(() => {
        hostRequests.delete(id);
        reject(new Error(`Renderer command timed out: ${command}`));
      }, 30000);
      hostRequests.set(id, { resolve, timeout });
      getWindow().webContents.send('mb:host:command', { id, command, args });
    });
  }

  async function executeHostCommand(command, args = {}) {
    if (command.startsWith('browser.')) return browser.execute(command, args);
    if (command === 'app.geometry') return { bounds: getWindow().getBounds(),
      contentSize: getWindow().getContentSize() };
    if (command === 'app.commands') return { commands: [
      'sizer.begin', 'sizer.delta', 'sizer.end', 'panel.width', 'panel.switch',
      'editor.open', 'editor.open_as', 'editor.selection', 'browser.open',
      'browser.navigate', 'browser.read', 'browser.eval', 'browser.links',
      'browser.console', 'browser.upload', 'browser.list', 'browser.close',
      'app.geometry', 'app.commands',
    ] };
    if (command === 'editor.open' || command === 'editor.open_as') {
      const workspace = await currentWorkspace();
      const supplied = typeof args.path === 'string' ? args.path : '';
      const relativeInput = path.isAbsolute(supplied) ? path.relative(workspace, supplied) : supplied;
      const absolute = await resolveWorkspacePath(workspace, relativeInput);
      const relative = path.relative(workspace, absolute);
      if (command === 'editor.open_as' && (args.mode === 'markdown' || args.mode === 'html')) {
        return args.mode === 'html' ? browser.openHtml(workspace, relative)
          : browser.openMarkdown(workspace, relative, store.snapshot().theme !== 'light');
      }
      return rendererCommand(command, { ...args, path: relative, root: workspace });
    }
    if (command === 'editor.selection' || command.startsWith('panel.') || command.startsWith('sizer.')) {
      return rendererCommand(command, args);
    }
    throw new Error(`Unknown host command: ${command}`);
  }

  const backend = createBackendClient({
    executable: backendExecutable,
    dataDir: path.join(path.dirname(store.filePath), 'backend'),
    onEvent: (type, payload) => getWindow()?.webContents.send('mb:agent:event', type, payload),
    executeUi: executeHostCommand,
  });
  const chimpflow = createChimpFlowService(backend);
  const fountain = createFountainService();
  const projectTypes = createProjectTypeService({
    builtinDir: path.resolve(path.dirname(new URL(import.meta.url).pathname), '../../project-types'),
    userDir: path.join(app.getPath('userData'), 'project-types'), backend,
  });
  const agentRoutes = new Map([
    ['GET:/v1/health', true], ['GET:/v1/sessions', true], ['GET:/v1/history', true],
    ['GET:/v1/settings', true], ['POST:/v1/settings', true],
    ['POST:/v1/sessions/ensure', true], ['POST:/v1/sessions/create', true],
    ['POST:/v1/sessions/select', true], ['POST:/v1/sessions/rename', true],
    ['POST:/v1/sessions/delete', true], ['POST:/v1/chat/send', true],
    ['POST:/v1/chat/steer', true], ['POST:/v1/chat/cancel', true],
    ['POST:/v1/chat/clear', true], ['POST:/v1/chat/approval', true],
    ['POST:/v1/models/select', true], ['POST:/v1/models/thinking', true],
    ['POST:/v1/debug/log', true],
  ]);
  handle('mb:agent:request', async (route, options = {}) => {
    const method = options.method === 'POST' ? 'POST' : 'GET';
    if (!agentRoutes.has(`${method}:${route}`)) throw new Error('Agent route is not allowed.');
    if (JSON.stringify(options.body ?? {}).length > 4 * 1024 * 1024) throw new Error('Agent request is too large.');
    return backend.request(route, { method, body: options.body ?? {}, timeout: options.timeout });
  });
  handle('mb:chimpflow:models', (settings) => chimpflow.models(settings));
  handle('mb:chimpflow:voices', (settings) => chimpflow.voices(settings));
  handle('mb:chimpflow:speak', (text, settings) => chimpflow.speak(text, settings));
  handle('mb:chimpflow:transcribe', (base64, mime) => chimpflow.transcribe(base64, mime));

  handle('mb:chat:attachments', async () => {
    const result = await dialog.showOpenDialog(getWindow(), {
      title: 'Attach Images', properties: ['openFile', 'multiSelections'],
      filters: [{ name: 'Images', extensions: ['png', 'jpg', 'jpeg', 'gif', 'webp'] }],
    });
    return result.canceled ? [] : result.filePaths;
  });

  handle('mb:bootstrap', async () => ({
    apiVersion: 1,
    settings: store.snapshot(),
    platform: process.platform,
    versions: {
      electron: process.versions.electron,
      chrome: process.versions.chrome,
      node: process.versions.node,
    },
  }));

  handle('mb:workspace:choose', async () => {
    const result = await dialog.showOpenDialog(getWindow(), {
      title: 'Open Workspace',
      defaultPath: store.snapshot().workspace ?? undefined,
      properties: ['openDirectory', 'createDirectory'],
    });
    if (result.canceled || result.filePaths.length === 0) {
      return store.snapshot();
    }
    const workspace = await allowWorkspace(result.filePaths[0]);
    return store.update({ workspace });
  });

  handle('mb:workspace:list', async (relativePath, showHidden) => {
    const workspace = await currentWorkspace();
    return listDirectory(workspace, relativePath, showHidden === true);
  });

  handle('mb:file:read', async (root, relativePath) => {
    const workspace = await trustedRoot(root);
    return readTextFile(workspace, relativePath);
  });

  handle('mb:file:write', async (root, relativePath, content) => {
    const workspace = await trustedRoot(root);
    await writeTextFile(workspace, relativePath, content);
    return { saved: true };
  });

  handle('mb:workspace:create-folder', async (root, relativePath) => {
    const workspace = await trustedRoot(root);
    await createFolder(workspace, relativePath);
    return { created: true };
  });

  handle('mb:workspace:create-file', async (root, relativePath, content) => {
    const workspace = await trustedRoot(root);
    await createTextFile(workspace, relativePath, content ?? '');
    return { created: true };
  });

  handle('mb:workspace:import', async (root, destination) => {
    const workspace = await trustedRoot(root);
    const result = await dialog.showOpenDialog(getWindow(), {
      title: 'Import Files', properties: ['openFile', 'multiSelections'],
    });
    if (result.canceled || result.filePaths.length === 0) return { imported: 0 };
    await importWorkspacePaths(workspace, destination, result.filePaths);
    return { imported: result.filePaths.length };
  });

  handle('mb:workspace:rename', async (root, oldPath, newPath) => {
    const workspace = await trustedRoot(root);
    await renameWorkspacePath(workspace, oldPath, newPath);
    return { renamed: true };
  });

  handle('mb:workspace:delete', async (root, relativePath) => {
    const workspace = await trustedRoot(root);
    const result = await dialog.showMessageBox(getWindow(), {
      type: 'warning', title: 'Delete Workspace Item',
      message: `Delete ${relativePath}?`,
      detail: 'This permanently deletes the item and any folder contents.',
      buttons: ['Cancel', 'Delete'], defaultId: 0, cancelId: 0, noLink: true,
    });
    if (result.response !== 1) return { deleted: false };
    await deleteWorkspacePath(workspace, relativePath);
    return { deleted: true };
  });

  handle('mb:workspace:copy-path', async (root, relativePath, absolute = true) => {
    const workspace = await trustedRoot(root);
    const resolved = await resolveWorkspacePath(workspace, relativePath);
    await clipboard.writeText(absolute === true ? resolved : relativePath);
    return { copied: true };
  });

  handle('mb:workspace:external', async (root, relativePath, reveal) => {
    const workspace = await trustedRoot(root);
    const absolute = await resolveWorkspacePath(workspace, relativePath);
    if (reveal) shell.showItemInFolder(absolute);
    else {
      const error = await shell.openPath(absolute);
      if (error) throw new Error(error);
    }
    return { opened: true };
  });

  handle('mb:workspace:terminal', async (root, relativePath) => {
    const workspace = await trustedRoot(root);
    const absolute = await resolveWorkspacePath(workspace, relativePath);
    const child = spawn('x-terminal-emulator', [], {
      cwd: absolute, detached: true, stdio: 'ignore',
    });
    child.on('error', () => {});
    child.unref();
    return { opened: true };
  });

  handle('mb:search', async (root, options) => {
    const workspace = await trustedRoot(root);
    return searchWorkspace(workspace, {
      ...(options && typeof options === 'object' ? options : {}),
      excludes: store.snapshot().searchExcludes,
    });
  });

  handle('mb:git:status', async (root) => gitStatus(await trustedRoot(root)));

  handle('mb:browser:execute', async (command, args) => browser.execute(command, args));
  handle('mb:browser:bounds', async (bounds) => {
    if (!bounds || ![bounds.x, bounds.y, bounds.width, bounds.height].every(Number.isFinite)) {
      throw new Error('Invalid browser bounds.');
    }
    browser.setBounds(bounds);
    return { applied: true };
  });
  handle('mb:browser:viewport', async (viewport) => {
    browser.setViewport(viewport && Number.isFinite(viewport.width) && Number.isFinite(viewport.height)
      ? { width: Math.max(200, Math.min(7680, viewport.width)),
          height: Math.max(200, Math.min(4320, viewport.height)) }
      : null);
    return { applied: true };
  });
  handle('mb:preview', async (root, relativePath, mode) => {
    const workspace = await trustedRoot(root);
    const dark = store.snapshot().theme === 'dark' ||
      (store.snapshot().theme === 'system' && nativeTheme.shouldUseDarkColors);
    if (mode === 'fountain') {
      const absolute = await resolveWorkspacePath(workspace, relativePath);
      const html = await fountain.html(absolute);
      return browser.create(`data:text/html;charset=utf-8,${encodeURIComponent(html)}`, 'fountain');
    }
    return mode === 'html' ? browser.openHtml(workspace, relativePath)
      : browser.openMarkdown(workspace, relativePath, dark);
  });

  handle('mb:project-types:list', () => projectTypes.list(store.snapshot().workspace));
  handle('mb:project-types:apply', async (id) => {
    const workspace = await currentWorkspace();
    return projectTypes.apply(workspace, id);
  });
  handle('mb:project-types:create', async (id) => {
    const result = await dialog.showOpenDialog(getWindow(), {
      title: 'Choose or Create the Workspace Folder', properties: ['openDirectory', 'createDirectory'],
    });
    if (result.canceled || !result.filePaths.length) return { workspace: null };
    const workspace = await allowWorkspace(result.filePaths[0]);
    await projectTypes.create(workspace, id);
    return store.update({ workspace });
  });
  handle('mb:project-types:save', (id, prompt) => projectTypes.savePrompt(id, prompt));

  handle('mb:fountain:export', async (root, relativePath, destinationRelative) => {
    const workspace = await trustedRoot(root);
    const input = await resolveWorkspacePath(workspace, relativePath);
    const bytes = await fountain.pdf(input);
    let output;
    if (destinationRelative) {
      if (path.isAbsolute(destinationRelative) || !destinationRelative.endsWith('.pdf')) throw new Error('PDF destination must be workspace-relative.');
      output = path.join(workspace, destinationRelative);
      await fs.mkdir(path.dirname(output), { recursive: true });
    } else {
      const result = await dialog.showSaveDialog(getWindow(), {
        title: 'Export Screenplay to PDF', defaultPath: path.join(workspace, relativePath.replace(/\.fountain$/i, '.pdf')),
        filters: [{ name: 'PDF', extensions: ['pdf'] }],
      });
      if (result.canceled || !result.filePath) return { exported: false };
      output = result.filePath;
    }
    await fs.writeFile(output, bytes);
    return { exported: true, path: output };
  });

  handle('mb:settings:update', async (patch) => {
    const source = patch && typeof patch === 'object' ? patch : {};
    const accepted = Object.fromEntries(
      Object.entries(source).filter(([key]) => UI_SETTING_KEYS.has(key)),
    );
    if (accepted.theme) nativeTheme.themeSource = accepted.theme;
    return store.update(accepted);
  });

  handle('mb:confirm-close', async (name) => {
    const result = await dialog.showMessageBox(getWindow(), {
      type: 'warning',
      title: 'Unsaved Changes',
      message: `${name} has unsaved changes.`,
      detail: 'Close without saving?',
      buttons: ['Cancel', 'Close Without Saving'],
      defaultId: 0,
      cancelId: 0,
      noLink: true,
    });
    return result.response === 1;
  });

  const initialWorkspace = store.snapshot().workspace;
  if (initialWorkspace) {
    const workspace = await allowWorkspace(initialWorkspace);
    if (workspace !== initialWorkspace) await store.update({ workspace });
  }
  await backend.start();
  return { close: () => backend.close(), diagnostics: () => backend.diagnostics(), request: backend.request };
}
