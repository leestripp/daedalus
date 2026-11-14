import { createChatView } from './chat-view.js';
import { createBrowserView } from './browser-view.js';
import { byId, errorMessage } from './dom.js';
import { createEditorView } from './editor-view.js';
import { createGitView } from './git-view.js';
import { createNavigation } from './navigation.js';
import { createResizers } from './resizers.js';
import { createSearchView } from './search-view.js';
import { createSettingsView } from './settings-view.js';
import { createWorkspaceView } from './workspace-view.js';

const systemTheme = window.matchMedia('(prefers-color-scheme: dark)');
let settings = null;
let workspaceView = null;
let editorView = null;
let navigation = null;
let settingsView = null;
let chatView = null;

function reportError(error) {
  const text = errorMessage(error);
  byId('agent-status').textContent = `Error: ${text}`;
  byId('settings-status').textContent = text;
  console.error(text);
}

function applyTheme() {
  if (!settings) {
    return;
  }
  const resolved = settings.theme === 'system'
    ? (systemTheme.matches ? 'dark' : 'light')
    : settings.theme;
  document.documentElement.dataset.theme = resolved;
}

function applySettings(next) {
  settings = next;
  applyTheme();
  const app = byId('app');
  app.style.setProperty('--sidebar-width', `${settings.sidebarWidth}px`);
  app.style.setProperty('--chat-width', `${settings.chatWidth}px`);
  navigation.setActive(settings.activePanel);
  settingsView.update(settings);
  workspaceView.update(settings);
  editorView.applySettings(settings);
  if (chatView && settings.workspace !== chatView.workspace()) {
    chatView.ensure(settings.workspace).catch(reportError);
  }
}

async function saveSettings(patch) {
  try {
    const refreshNeeded = 'showHidden' in patch && patch.showHidden !== settings.showHidden;
    applySettings({ ...settings, ...patch });
    const saved = await window.monkeybyte.updateSettings(patch);
    applySettings(saved);
    byId('settings-status').textContent = 'Settings saved.';
    if (refreshNeeded) {
      await workspaceView.refresh();
    }
  } catch (error) {
    reportError(error);
  }
}

async function start() {
  if (!window.monkeybyte) {
    throw new Error('The secure MonkeyByte preload bridge is unavailable.');
  }
  const api = window.monkeybyte;
  const bootstrap = await api.getBootstrap();

  navigation = createNavigation((activePanel) => saveSettings({ activePanel }).catch(reportError));
  settingsView = createSettingsView((patch) => saveSettings(patch).catch(reportError), api);
  settingsView.refreshAgent().catch(reportError);
  editorView = createEditorView({ api, onError: reportError,
    onWorkspaceRefresh: () => workspaceView?.refresh().catch(reportError),
    onTabActivated: (tab) => {
      if (tab && tab.kind !== 'file') api.browserExecute('browser.activate', { tab: tab.id }).catch(reportError);
      else api.browserExecute('browser.hide').catch(reportError);
    },
    onSpecialClosed: (tab) => api.browserExecute('browser.close', { tab: tab.id }).catch(reportError) });
  workspaceView = createWorkspaceView({
    api,
    onOpenFile: (file, line) => editorView.openFile(file, line).catch(reportError),
    onWorkspaceChange: applySettings,
    onError: reportError,
    onRenamePath: (root, oldPath, newPath) => editorView.renamePath(root, oldPath, newPath).catch(reportError),
    onDeletePath: (root, path) => editorView.deletePath(root, path).catch(reportError),
    onShowHiddenChange: (showHidden) => saveSettings({ showHidden }),
  });
  const browserView = createBrowserView({ api, editor: editorView, onError: reportError,
    getHomeUrl: () => settings?.browserHomeUrl ?? '' });
  void browserView;
  const searchView = createSearchView({ api, getWorkspace: () => settings.workspace,
    onOpenFile: (file, line) => editorView.openFile(file, line).catch(reportError), onError: reportError });
  const gitView = createGitView({ api, getWorkspace: () => settings.workspace,
    onOpenFile: (file) => editorView.openFile(file).catch(reportError), onError: reportError });
  void searchView;
  chatView = createChatView({ api, getWorkspace: () => settings.workspace, onError: reportError });
  createResizers({
    onPreview: (patch) => applySettings({ ...settings, ...patch }),
    onCommit: (patch) => saveSettings(patch).catch(reportError),
  });

  applySettings(bootstrap.settings);
  byId('agent-status').textContent = 'Agent Idle';
  if (settings.workspace) {
    await workspaceView.refresh();
    await gitView.refresh();
  }
}

function registerHostCommands() {
  window.monkeybyte.onHostCommand(async ({ id, command, args }) => {
    let result;
    try {
      if (command === 'editor.open' || command === 'editor.open_as') {
        const root = args.root.endsWith('/') ? args.root.slice(0, -1) : args.root;
        const path = args.path.startsWith(`${root}/`) ? args.path.slice(root.length + 1) : args.path;
        await editorView.openFile({ root, path,
          name: path.split('/').pop(), isDirectory: false }, args.line);
        result = { ok: true, path };
      } else if (command === 'editor.selection') result = editorView.getSelection();
      else if (command === 'panel.switch') {
        navigation.setActive(args.panel ?? args.target);
        await saveSettings({ activePanel: args.panel ?? args.target }); result = { ok: true };
      } else if (command === 'panel.width' || command === 'sizer.delta') {
        const target = args.target ?? args.panel;
        const key = target === 'chat' ? 'chatWidth' : 'sidebarWidth';
        const current = settings[key];
        const width = command === 'panel.width' ? Number(args.width) : current + Number(args.dx ?? 0);
        await saveSettings({ [key]: Math.max(180, Math.min(900, width)) }); result = { width: settings[key] };
      } else if (command === 'sizer.begin' || command === 'sizer.end') result = { ok: true, width: settings[args.target === 'chat' ? 'chatWidth' : 'sidebarWidth'] };
      else result = { error: `Unknown renderer command: ${command}` };
    } catch (error) { result = { error: errorMessage(error) }; }
    await window.monkeybyte.hostCommandResult(id, result);
  });
}

registerHostCommands();
systemTheme.addEventListener('change', applyTheme);
start().catch(reportError);
