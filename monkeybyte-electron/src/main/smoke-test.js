import { clipboard, Menu } from 'electron';
import { promises as fs } from 'node:fs';
import os from 'node:os';
import path from 'node:path';

export async function prepareSmokeWorkspace(store) {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'monkeybyte-electron-smoke-'));
  await fs.writeFile(path.join(root, 'hello.txt'), 'Hello from Electron.\n', 'utf8');
  await fs.mkdir(path.join(root, 'folder'));
  await fs.mkdir(path.join(root, 'bin'));
  const converter = path.join(root, 'bin', 'screenplain');
  await fs.writeFile(converter, `#!/bin/sh
if [ "$1" = "--format" ]; then shift 2; fi
input="$1"; output="$2"
case "$output" in
  *.html) printf '<!doctype html><title>Fixture Screenplay</title><h1>Screenplay Preview</h1><script>bad()</script>' > "$output" ;;
  *) printf '%%PDF-FAKE-FOUNTAIN\n' > "$output" ;;
esac
`, { mode: 0o755 });
  await fs.writeFile(path.join(root, '.hidden'), 'hidden\n', 'utf8');
  await store.update({ workspace: root, activePanel: 'workspace', theme: 'dark' });
  return root;
}

async function sendControlKey(window, key) {
  window.webContents.sendInputEvent({ type: 'keyDown', keyCode: key, modifiers: ['control'] });
  window.webContents.sendInputEvent({ type: 'keyUp', keyCode: key, modifiers: ['control'] });
  await new Promise((resolve) => setTimeout(resolve, 250));
}

async function verifyKeyboardShortcuts(window) {
  const roles = [];
  const visit = (items) => {
    for (const item of items) {
      if (item.role === 'quit' || item.role === 'close') roles.push({ role: item.role, accelerator: item.accelerator });
      if (item.submenu) visit(item.submenu.items);
    }
  };
  visit(Menu.getApplicationMenu()?.items ?? []);
  if (!roles.some((item) => item.role === 'quit')) throw new Error('Quit must remain available in the menu.');
  if (roles.some((item) => item.accelerator)) throw new Error('Quit/Close menu items must not retain window-closing accelerators: ' + JSON.stringify(roles));

  const id = window.webContents.id;
  await sendControlKey(window, 'q');
  if (window.isDestroyed() || window.webContents.isDestroyed() || window.webContents.id !== id) {
    throw new Error('Ctrl+Q destroyed the application window.');
  }
  const before = await window.webContents.executeJavaScript(
    "document.querySelectorAll('#tab-list .editor-tab').length", true);
  await sendControlKey(window, 'w');
  const afterOne = await window.webContents.executeJavaScript(
    "document.querySelectorAll('#tab-list .editor-tab').length", true);
  if (window.isDestroyed() || afterOne !== before - 1) {
    throw new Error('Ctrl+W must close exactly one active document tab: ' + JSON.stringify({ before, afterOne }));
  }
  await sendControlKey(window, 'w');
  const closed = await window.webContents.executeJavaScript(`(() => ({
    title: document.querySelector('#file-title').textContent,
    closeDisabled: document.querySelector('#close-file').disabled,
    tabs: document.querySelectorAll('#tab-list .editor-tab').length,
  }))()`, true);
  if (window.isDestroyed() || closed.title !== 'MonkeyByte' || !closed.closeDisabled || closed.tabs !== 0) {
    throw new Error('Ctrl+Q/W keyboard regression: ' + JSON.stringify(closed));
  }
  // No active tab: another Ctrl+W must remain a harmless no-op.
  await sendControlKey(window, 'w');
  if (window.isDestroyed()) throw new Error('Ctrl+Q/W with no tab destroyed the application window.');
  return { ctrlQIgnored: true, ctrlWClosedDocumentOnly: true, ctrlWWithoutTabIgnored: true,
    menuAcceleratorsRemoved: true };
}

export async function runSmokeTest(window, root) {
  try {
    const result = await window.webContents.executeJavaScript(`(async () => {
      const pause = (ms = 120) => new Promise((resolve) => setTimeout(resolve, ms));
      const checks = [];
      const waitFor = async (callback, description) => {
        const deadline = Date.now() + 10000;
        while (Date.now() < deadline) {
          const value = callback(); if (value) return value;
          await pause(80);
        }
        throw new Error('Timed out: ' + description + '; workspace=' + document.querySelector('#workspace-tree')?.textContent + '; status=' + document.querySelector('#git-status-bar')?.textContent);
      };
      const check = (condition, name) => {
        checks.push({ name, passed: Boolean(condition) });
        if (!condition) throw new Error('Smoke assertion failed: ' + name);
      };
      await pause();
      const api = window.monkeybyte;
      check(Boolean(api), 'preload API is available');
      check(typeof require === 'undefined', 'renderer has no Node require');
      check(typeof process === 'undefined', 'renderer has no Node process');
      check(!('ipcRenderer' in window), 'renderer has no generic IPC handle');
      check(document.querySelectorAll('.activity-button').length === 4, 'four activity buttons');
      await waitFor(() => document.querySelector('#git-status-bar').textContent === 'Not a Git repository',
        'non-Git workspace has a clear bottom status instead of a renderer error');
      document.querySelector('#refresh-workspace').click();
      await waitFor(() => document.querySelector('[data-file-path="hello.txt"]'), 'workspace file listing');
      check(document.querySelector('[data-file-path="hello.txt"]'), 'workspace file is listed');
      check(!document.querySelector('[data-file-path=".hidden"]'), 'dotfiles are hidden');
      const hiddenToggle = document.querySelector('#toggle-hidden');
      check(hiddenToggle.textContent === '○' && hiddenToggle.getAttribute('aria-pressed') === 'false',
        'hidden-files control has a clear Off indicator');
      hiddenToggle.click();
      await waitFor(() => document.querySelector('[data-file-path=".hidden"]'), 'hidden-files toggle reveals dotfiles');
      check(hiddenToggle.textContent === '●' && hiddenToggle.getAttribute('aria-pressed') === 'true',
        'hidden-files control has a clear On indicator');
      check((await api.getBootstrap()).settings.showHidden === true, 'hidden-files toolbar persists the real setting');
      hiddenToggle.click();
      await waitFor(() => !document.querySelector('[data-file-path=".hidden"]'), 'hidden-files toggle hides dotfiles again');
      check(hiddenToggle.textContent === '○' && hiddenToggle.getAttribute('aria-pressed') === 'false',
        'hidden-files control returns to its Off indicator');
      document.querySelector('[data-file-path="hello.txt"]').click();
      await pause();
      check(document.querySelector('#file-title').textContent === 'hello.txt', 'file opens in a tab');
      const editor = document.querySelector('#editor');
      await waitFor(() => editor && editor.value.includes('Hello from Electron.'), 'editor content');
      check(editor && editor.value.includes('Hello from Electron.'), 'file content loads');

      const menu = document.querySelector('#workspace-context-menu');
      const openMenu = async (path) => {
        const button = document.querySelector('[data-path="' + path + '"] .tree-file');
        button.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, cancelable: true,
          clientX: window.innerWidth + 100, clientY: window.innerHeight + 100 }));
        await waitFor(() => !menu.hidden, 'context menu for ' + path);
        return [...menu.querySelectorAll('[role="menuitem"]')].map((item) => item.textContent);
      };
      const chooseMenuItem = async (label) => {
        const item = [...menu.querySelectorAll('[role="menuitem"]')].find((button) => button.textContent === label);
        check(Boolean(item), 'menu includes ' + label);
        item.click();
        await pause();
      };
      const fileButton = document.querySelector('[data-file-path="hello.txt"]');
      fileButton.focus();
      fileButton.dispatchEvent(new KeyboardEvent('keydown', { key: 'F10', shiftKey: true, bubbles: true, cancelable: true }));
      await waitFor(() => !menu.hidden, 'Shift+F10 opens context menu');
      check(menu.getAttribute('role') === 'menu', 'workspace popup has menu semantics');
      check(document.activeElement.textContent === 'Open', 'first menu action receives focus');
      document.activeElement.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true, cancelable: true }));
      check(document.activeElement.textContent === 'Open with System Application', 'arrow keys navigate menu actions');
      menu.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }));
      check(menu.hidden && document.activeElement === fileButton, 'Escape closes menu and restores tree focus');

      let labels = await openMenu('hello.txt');
      for (const label of ['Open', 'Open with System Application', 'New File Here', 'Rename…', 'Delete…', 'Copy Path']) {
        check(labels.includes(label), 'file menu includes ' + label);
      }
      const menuBounds = menu.getBoundingClientRect();
      check(menuBounds.left >= 0 && menuBounds.top >= 0 && menuBounds.right <= window.innerWidth &&
        menuBounds.bottom <= window.innerHeight, 'context menu stays inside the viewport');
      await chooseMenuItem('New File Here');
      await waitFor(() => document.querySelector('dialog.name-dialog'), 'new-file-here dialog');
      let dialog = document.querySelector('dialog.name-dialog');
      dialog.querySelector('input').value = 'context-note.md';
      dialog.querySelector('select').value = 'markdown';
      dialog.querySelector('button[value="ok"]').click();
      await waitFor(() => document.querySelector('[data-file-path="context-note.md"]'), 'file created from another file context');
      check(document.querySelector('#file-title').textContent === 'context-note.md', 'new context file opens in editor');
      await waitFor(() => editor.value === '# context-note\\n\\n', 'new context file content is ready');

      labels = await openMenu('context-note.md');
      check(labels.includes('Open as Markdown Preview'), 'Markdown preview is offered from context menu');
      await chooseMenuItem('Rename…');
      await waitFor(() => document.querySelector('dialog.name-dialog'), 'rename dialog');
      dialog = document.querySelector('dialog.name-dialog');
      check(dialog.querySelector('input').value === 'context-note.md', 'rename starts with the selected path');
      dialog.querySelector('input').value = 'renamed-context.md';
      dialog.querySelector('button[value="ok"]').click();
      await waitFor(() => document.querySelector('[data-file-path="renamed-context.md"]'), 'context rename refreshes tree');
      check(!document.querySelector('[data-file-path="context-note.md"]'), 'old context filename disappears');

      document.querySelector('[data-path="folder"] .tree-file').click();
      await waitFor(() => document.querySelector('[data-path="folder"] .tree-toggle').getAttribute('aria-expanded') === 'true', 'folder expands');
      labels = await openMenu('folder');
      for (const label of ['New File Here', 'New Folder Here', 'Import Files…', 'Open in Terminal', 'Copy Absolute Path']) {
        check(labels.includes(label), 'folder menu includes ' + label);
      }
      await chooseMenuItem('New Folder Here');
      await waitFor(() => document.querySelector('dialog.name-dialog'), 'new-folder-here dialog');
      dialog = document.querySelector('dialog.name-dialog');
      check(dialog.querySelector('input').value === 'folder/new-folder', 'folder action targets the selected directory');
      dialog.querySelector('input').value = 'folder/context-subfolder';
      dialog.querySelector('button[value="ok"]').click();
      await waitFor(() => document.querySelector('[data-path="folder/context-subfolder"]'), 'folder created from directory context');

      await openMenu('renamed-context.md');
      await chooseMenuItem('Copy Path');
      await openMenu('renamed-context.md');
      await chooseMenuItem('Copy Absolute Path');

      document.querySelector('[data-file-path="hello.txt"]').click();
      await waitFor(() => document.querySelector('#file-title').textContent === 'hello.txt' &&
        editor.value.includes('Hello from Electron.'), 'original editor is ready for save test');
      editor.value = 'Saved by the Electron smoke test.\\n';
      editor.dispatchEvent(new Event('input', { bubbles: true }));
      check(!document.querySelector('#save-file').disabled, 'dirty file enables Save');
      document.querySelector('#save-file').click();
      await pause(200);
      check(document.querySelector('#save-file').disabled, 'save clears dirty state');
      document.querySelector('[data-panel="settings"]').click();
      await pause();
      check(document.querySelector('#app').classList.contains('settings-active'), 'Settings hides editor/chat');
      const theme = document.querySelector('#theme-select');
      theme.value = 'light';
      theme.dispatchEvent(new Event('change', { bubbles: true }));
      await pause();
      check(document.documentElement.dataset.theme === 'light', 'light theme applies live');
      document.querySelector('[data-panel="workspace"]').click();
      await pause();
      check(!document.querySelector('#app').classList.contains('settings-active'), 'Workspace restores IDE layout');
      check(document.querySelector('[data-testid="chat-panel"]'), 'chat panel is rendered');
      return { checks };
    })()`, true);

    const contextNote = await fs.readFile(path.join(root, 'renamed-context.md'), 'utf8');
    if (contextNote !== '# context-note\n\n') {
      throw new Error('Context-menu create/rename did not produce the expected Markdown file.');
    }
    const folderStat = await fs.stat(path.join(root, 'folder', 'context-subfolder'));
    if (!folderStat.isDirectory()) throw new Error('Context-menu folder creation failed.');
    const copiedAbsolutePath = await clipboard.readText();
    if (copiedAbsolutePath !== path.join(root, 'renamed-context.md')) {
      throw new Error('Copy Absolute Path mismatch: ' + JSON.stringify({ actual: copiedAbsolutePath, expected: path.join(root, 'renamed-context.md') }));
    }
    const saved = await fs.readFile(path.join(root, 'hello.txt'), 'utf8');
    if (saved !== 'Saved by the Electron smoke test.\n') {
      throw new Error('Saved file content does not match the renderer edit.');
    }
    const keyboardShortcuts = await verifyKeyboardShortcuts(window);
    console.log(JSON.stringify({ status: 'success', ...result, fileWriteVerified: true,
      contextMenuFilesystemVerified: true, contextMenuClipboardVerified: true, keyboardShortcuts }, null, 2));
    return 0;
  } finally {
    await fs.rm(root, { recursive: true, force: true });
  }
}
