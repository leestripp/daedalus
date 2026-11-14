import { app, BrowserWindow, Menu, nativeTheme } from 'electron';
import { promises as fs } from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import { runAgentSmokeTest } from './agent-smoke-test.js';
import { runBrowserSmokeTest } from './browser-smoke-test.js';
import { registerIpc } from './ipc.js';
import { within } from './lifecycle.js';
import { createSettingsStore } from './settings-store.js';
import { prepareSmokeWorkspace, runSmokeTest } from './smoke-test.js';

const directory = path.dirname(fileURLToPath(import.meta.url));
// §AUDIO-CLOCK: AudioContext may start suspended without a user gesture.
app.commandLine.appendSwitch('autoplay-policy', 'no-user-gesture-required');
const smokeMode = process.argv.includes('--smoke-test');
let mainWindow = null;
let smokeWorkspace = null;
let smokeUserData = null;
let backendHandle = null;
let backendClosed = false;
let requestedExitCode = 0;
const smokeRendererErrors = [];
const closeDebug = (stage) => { if (process.env.MONKEYBYTE_DEBUG_CLOSE) console.error(`[close] ${stage}`); };

// Electron's default menu binds Ctrl+Q to Quit and Ctrl+W to the window.
// Keep Quit available by mouse, but never let either shortcut destroy the
// window. This listener covers the renderer and every embedded BrowserView.
function removeWindowClosingAccelerators() {
  const menu = Menu.getApplicationMenu();
  if (!menu) return;
  const template = (items) => items.map((item) => ({
    label: item.label,
    role: item.role,
    type: item.type,
    accelerator: item.role === 'quit' || item.role === 'close' ? '' : item.accelerator,
    submenu: item.submenu ? template(item.submenu.items) : undefined,
  }));
  Menu.setApplicationMenu(Menu.buildFromTemplate(template(menu.items)));
}

app.on('web-contents-created', (_event, contents) => {
  contents.on('before-input-event', (event, input) => {
    if (input.type !== 'keyDown' || !input.control || input.alt || input.meta || input.shift) return;
    const key = input.key.toLowerCase();
    if (key === 'q') {
      event.preventDefault();
      return;
    }
    if (key !== 'w') return;
    event.preventDefault();
    if (!mainWindow || mainWindow.isDestroyed() || mainWindow.webContents.isDestroyed()) return;
    // Use the existing tab action: it closes documents/browser previews and
    // retains the normal unsaved-document confirmation. A disabled button
    // means there is no active tab, so Ctrl+W is deliberately a no-op.
    mainWindow.webContents.executeJavaScript(
      "document.querySelector('#close-file')?.click()", true,
    ).catch((error) => console.warn(`Could not close active tab: ${error.message}`));
  });
});

async function saveWindowState(window, store) {
  closeDebug('geometry: begin');
  const { width, height } = window.getBounds();
  const patch = { windowWidth: width, windowHeight: height };
  if (!window.webContents.isDestroyed()) {
    try {
      const layout = await within(window.webContents.executeJavaScript(`(() => ({
        sidebarWidth: Math.round(document.querySelector('#sidebar')?.getBoundingClientRect().width ?? 0),
        chatWidth: Math.round(document.querySelector('#chat-panel')?.getBoundingClientRect().width ?? 0),
      }))()`, true), 2000, null);
      if (layout?.sidebarWidth >= 150) patch.sidebarWidth = layout.sidebarWidth;
      if (layout?.chatWidth >= 250) patch.chatWidth = layout.chatWidth;
    } catch (error) {
      console.warn(`Could not read panel geometry: ${error.message}`);
    }
  }
  await within(store.update(patch), 2000);
  closeDebug('geometry: saved');
}

function createWindow(settings, store) {
  const window = new BrowserWindow({
    title: 'MonkeyByte',
    width: settings.windowWidth,
    height: settings.windowHeight,
    minWidth: 900,
    minHeight: 600,
    show: !smokeMode,
    backgroundColor: '#1e1e2e',
    webPreferences: {
      preload: path.join(directory, '..', 'preload', 'index.cjs'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
    },
  });

  window.webContents.session.setPermissionRequestHandler((webContents, permission, callback) => {
    callback(webContents === window.webContents && permission === 'media');
  });
  window.webContents.session.setPermissionCheckHandler((webContents, permission) => (
    webContents === window.webContents && permission === 'media'
  ));
  let stateSaved = smokeMode;
  let saveInFlight = null;
  window.on('close', (event) => {
    closeDebug(`window.close stateSaved=${stateSaved}`);
    if (stateSaved) return;
    event.preventDefault();
    if (!saveInFlight) {
      saveInFlight = saveWindowState(window, store).catch((error) => {
        console.warn(`Could not save window geometry: ${error.message}`);
      }).finally(() => {
        closeDebug('window.close: retry without prevention');
        stateSaved = true;
        if (!window.isDestroyed()) window.close();
      });
    }
  });

  window.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
  window.webContents.on('will-navigate', (event) => {
    event.preventDefault();
  });
  return window;
}

async function start() {
  if (smokeMode) {
    smokeUserData = process.env.MONKEYBYTE_SMOKE_USER_DATA
      ?? await fs.mkdtemp(path.join(os.tmpdir(), 'monkeybyte-electron-settings-'));
    app.setPath('userData', smokeUserData);
  }
  removeWindowClosingAccelerators();
  const store = createSettingsStore(path.join(app.getPath('userData'), 'electron-settings.json'));
  await store.load();
  if (smokeMode) {
    smokeWorkspace = await prepareSmokeWorkspace(store);
    process.env.PATH = `${smokeWorkspace}/bin:${process.env.PATH}`;
    process.env.DAEDALUS_HOME = path.join(smokeUserData, 'daedalus-home');
    process.env.MONKEYBYTE_BACKEND_TEST_MODE = '1';
  }

  nativeTheme.themeSource = store.snapshot().theme;
  mainWindow = createWindow(store.snapshot(), store);

  const backendExecutable = process.env.MONKEYBYTE_BACKEND_EXECUTABLE ??
    (app.isPackaged ? path.join(process.resourcesPath, 'backend', 'monkeybyte-electron-backend')
      : path.resolve(directory, '..', '..', '..', 'build', 'monkeybyte', 'monkeybyte-electron-backend'));
  backendHandle = await registerIpc({ store, getWindow: () => mainWindow, backendExecutable });
  if (smokeMode) mainWindow.webContents.on('console-message', (_event, level, message) => { if (level >= 2) { smokeRendererErrors.push(message); console.error(`Renderer: ${message}`); } });
  await mainWindow.loadFile(path.join(directory, '..', 'renderer', 'index.html'));

  if (smokeMode) {
    try {
      await runBrowserSmokeTest(mainWindow, smokeWorkspace);
      await runAgentSmokeTest(mainWindow, smokeWorkspace, backendHandle);
      const exitCode = await runSmokeTest(mainWindow, smokeWorkspace);
      if (smokeRendererErrors.length) throw new Error(`Renderer errors during smoke: ${smokeRendererErrors.join(' | ')}`);
      await backendHandle.close(); backendClosed = true;
      await fs.rm(smokeUserData, { recursive: true, force: true });
      requestedExitCode = exitCode;
      app.exit(exitCode);
    } catch (error) {
      console.error(`Backend diagnostics:
${backendHandle.diagnostics()}`);
      await backendHandle.close(); backendClosed = true;
      console.error(`MonkeyByte smoke test failed: ${error.stack ?? error.message}`);
      await Promise.all([
        fs.rm(smokeWorkspace, { recursive: true, force: true }),
        fs.rm(smokeUserData, { recursive: true, force: true }),
      ]);
      requestedExitCode = 1;
      app.exit(1);
    }
  }
}

app.whenReady().then(start).catch((error) => {
  console.error(`MonkeyByte failed to start: ${error.stack ?? error.message}`);
  requestedExitCode = 1;
  app.exit(1);
});

app.on('window-all-closed', () => {
  closeDebug('window-all-closed');
  if (backendClosed || !backendHandle) {
    closeDebug('app.exit: all resources closed');
    app.exit(requestedExitCode);
  } else {
    app.quit();
  }
});

async function closeBackendAndQuit() {
  closeDebug('backend: close begin');
  if (backendHandle && !backendClosed) {
    backendClosed = true;
    await within(backendHandle.close(), 5000);
    closeDebug('backend: close settled');
  }
  if (BrowserWindow.getAllWindows().length === 0) {
    closeDebug('app.exit: backend and windows closed');
    app.exit(requestedExitCode);
  } else {
    closeDebug('app.quit: finish window close');
    app.quit();
  }
}

app.on('before-quit', (event) => {
  closeDebug(`before-quit backendClosed=${backendClosed} backendHandle=${Boolean(backendHandle)}`);
  if (!backendClosed && backendHandle) {
    event.preventDefault();
    closeBackendAndQuit().catch((error) => {
      console.error(`Backend shutdown failed: ${error.message}`);
      requestedExitCode = 1;
      app.exit(1);
    });
  }
});
