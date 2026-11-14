// SPDX-License-Identifier: BSL-1.0
import { byId, errorMessage, message } from './dom.js';

function fileIcon(entry) {
  if (entry.isDirectory) return '📁';
  const extension = entry.name.split('.').pop()?.toLowerCase();
  const icons = { cpp: 'C++', h: 'H', hpp: 'HPP', js: 'JS', mjs: 'JS', ts: 'TS', py: 'PY',
                  md: 'MD', json: '{}', html: '<>', css: '#', cmake: '⚙', txt: 'TXT' };
  return icons[extension] ?? '📄';
}

function templateContent(path, template) {
  const name = path.split('/').pop() ?? 'untitled';
  if (template === 'cpp') return `#include <iostream>\n\nint main() {\n    std::cout << "Hello, world!\\n";\n    return 0;\n}\n`;
  if (template === 'header') return '#pragma once\n\n';
  if (template === 'html') return '<!DOCTYPE html>\n<html lang="en">\n<head>\n  <meta charset="utf-8">\n  <title>Document</title>\n</head>\n<body>\n\n</body>\n</html>\n';
  if (template === 'markdown') return `# ${name.replace(/\.[^.]+$/, '')}\n\n`;
  if (template === 'python') return 'def main():\n    pass\n\n\nif __name__ == "__main__":\n    main()\n';
  if (template === 'javascript') return "'use strict';\n\nfunction main() {\n}\n\nmain();\n";
  return '';
}

export function createWorkspaceView({ api, onOpenFile, onWorkspaceChange, onError,
                                      onRenamePath, onDeletePath, onShowHiddenChange }) {
  const tree = byId('workspace-tree');
  const pathButton = byId('workspace-path');
  const hiddenButton = byId('toggle-hidden');
  const refreshButton = byId('refresh-workspace');
  const contextMenu = byId('workspace-context-menu');
  const expanded = new Set();
  let workspace = null;
  let showHidden = false;
  let generation = 0;

  function update(settings) {
    workspace = settings.workspace;
    showHidden = settings.showHidden;
    pathButton.textContent = workspace ?? 'No workspace selected';
    pathButton.title = workspace ?? 'Change workspace';
    hiddenButton.setAttribute('aria-pressed', String(showHidden));
    hiddenButton.setAttribute('aria-label', showHidden ? 'Hide hidden files' : 'Show hidden files');
    hiddenButton.title = showHidden ? 'Hide hidden files' : 'Show hidden files';
    hiddenButton.textContent = showHidden ? '●' : '○';
  }

  async function askName(title, initial = '', template = false) {
    const dialog = document.createElement('dialog');
    dialog.className = 'name-dialog';
    const form = document.createElement('form');
    form.method = 'dialog';
    const heading = document.createElement('h3');
    heading.textContent = title;
    const label = document.createElement('label');
    label.textContent = 'Workspace-relative path';
    const input = document.createElement('input');
    input.required = true;
    input.value = initial;
    label.append(input);
    const select = document.createElement('select');
    select.setAttribute('aria-label', 'File template');
    for (const [value, text] of [['plain', 'Empty file'], ['cpp', 'C++ source'],
      ['header', 'C/C++ header'], ['html', 'HTML document'], ['markdown', 'Markdown document'],
      ['python', 'Python script'], ['javascript', 'JavaScript script']]) {
      const option = document.createElement('option'); option.value = value; option.textContent = text; select.append(option);
    }
    select.hidden = !template;
    const actions = document.createElement('div'); actions.className = 'dialog-actions';
    const cancel = document.createElement('button'); cancel.value = 'cancel'; cancel.textContent = 'Cancel';
    const accept = document.createElement('button'); accept.value = 'ok'; accept.textContent = 'OK'; accept.className = 'primary';
    actions.append(cancel, accept);
    form.append(heading, label, select, actions); dialog.append(form); document.body.append(dialog);
    return new Promise((resolve) => {
      dialog.addEventListener('close', () => {
        const result = dialog.returnValue === 'ok' ? { path: input.value.trim(), template: select.value } : null;
        dialog.remove(); resolve(result);
      }, { once: true });
      dialog.showModal(); input.focus(); input.select();
    });
  }

  let menuReturnFocus = null;
  contextMenu.setAttribute('role', 'menu');
  contextMenu.setAttribute('aria-label', 'Workspace item actions');

  function hideMenu(restoreFocus = false) {
    if (contextMenu.hidden) return;
    contextMenu.hidden = true;
    contextMenu.replaceChildren();
    const target = menuReturnFocus;
    menuReturnFocus = null;
    if (restoreFocus && target?.isConnected) target.focus();
  }

  function showMenu(entry, event, position = null, returnFocus = null) {
    event.preventDefault(); event.stopPropagation();
    hideMenu();
    menuReturnFocus = returnFocus ?? document.activeElement;
    const actions = [];
    const add = (label, callback, danger = false) => actions.push({ label, callback, danger });
    const folderPath = entry.isDirectory ? entry.path : entry.path.split('/').slice(0, -1).join('/');
    if (!entry.isDirectory) {
      add('Open', () => onOpenFile({ root: workspace, path: entry.path, name: entry.name }));
      add('Open with System Application', () => api.openExternal(workspace, entry.path));
      if (/\.md(?:own)?$/i.test(entry.name)) {
        add('Open as Markdown Preview', () => api.previewFile(workspace, entry.path, 'markdown'));
      }
      if (/\.html?$/i.test(entry.name)) {
        add('Open as HTML Preview', () => api.previewFile(workspace, entry.path, 'html'));
      }
      if (/\.fountain$/i.test(entry.name)) {
        add('Open as Screenplay Preview', () => api.previewFile(workspace, entry.path, 'fountain'));
        add('Export to PDF…', () => api.exportFountain(workspace, entry.path));
      }
    }
    add('New File Here', () => createNewFile(folderPath));
    if (entry.isDirectory) {
      add('New Folder Here', () => createNewFolder(folderPath));
      add('Import Files…', () => importFiles(folderPath));
      add('Open in Terminal', () => api.openTerminal(workspace, entry.path));
    }
    add('Reveal in File Manager', () => api.openExternal(workspace, entry.path, true));
    add('Rename…', async () => {
      const answer = await askName(`Rename ${entry.name}`, entry.path);
      if (answer && answer.path !== entry.path) await onRenamePath(workspace, entry.path, answer.path);
    });
    add('Delete…', () => onDeletePath(workspace, entry.path), true);
    add('Copy Path', () => api.copyPath(workspace, entry.path, false));
    add('Copy Absolute Path', () => api.copyPath(workspace, entry.path, true));

    const buttons = actions.map((action) => {
      const button = document.createElement('button');
      button.type = 'button'; button.textContent = action.label;
      button.setAttribute('role', 'menuitem'); button.tabIndex = -1;
      if (action.danger) button.className = 'danger';
      button.addEventListener('click', () => {
        hideMenu(true);
        Promise.resolve(action.callback()).catch(onError);
      });
      return button;
    });
    contextMenu.replaceChildren(...buttons);
    contextMenu.style.visibility = 'hidden';
    contextMenu.hidden = false;
    const x = position?.x ?? event.clientX;
    const y = position?.y ?? event.clientY;
    contextMenu.style.left = `${Math.max(8, Math.min(x, window.innerWidth - contextMenu.offsetWidth - 8))}px`;
    contextMenu.style.top = `${Math.max(8, Math.min(y, window.innerHeight - contextMenu.offsetHeight - 8))}px`;
    contextMenu.style.visibility = '';
    buttons[0]?.focus();
  }

  contextMenu.addEventListener('keydown', (event) => {
    const items = [...contextMenu.querySelectorAll('[role="menuitem"]')];
    const current = items.indexOf(document.activeElement);
    let next = null;
    if (event.key === 'ArrowDown') next = (current + 1) % items.length;
    else if (event.key === 'ArrowUp') next = (current - 1 + items.length) % items.length;
    else if (event.key === 'Home') next = 0;
    else if (event.key === 'End') next = items.length - 1;
    else if (event.key === 'Escape') {
      event.preventDefault(); event.stopPropagation(); hideMenu(true); return;
    } else if (event.key === 'Tab') { hideMenu(); return; }
    if (next !== null && items.length) {
      event.preventDefault(); items[next].focus();
    }
  });

  function row(entry, depth) {
    const container = document.createElement('div');
    container.className = 'tree-row'; container.dataset.path = entry.path;
    container.style.setProperty('--depth', String(depth)); container.setAttribute('role', 'treeitem');
    const toggle = document.createElement(entry.isDirectory ? 'button' : 'span');
    toggle.className = 'tree-toggle'; toggle.textContent = entry.isDirectory ? '▸' : '';
    if (entry.isDirectory) {
      toggle.type = 'button'; toggle.title = `Expand ${entry.name}`;
      toggle.setAttribute('aria-label', `Expand ${entry.name}`); toggle.setAttribute('aria-expanded', 'false');
    } else toggle.setAttribute('aria-hidden', 'true');
    const open = document.createElement('button'); open.type = 'button'; open.className = 'tree-file';
    const icon = document.createElement('span'); icon.className = `file-icon ext-${entry.name.split('.').pop()?.toLowerCase()}`;
    icon.textContent = fileIcon(entry); icon.setAttribute('aria-hidden', 'true');
    open.append(icon, document.createTextNode(` ${entry.name}`)); open.title = entry.path;
    open.setAttribute('aria-haspopup', 'menu'); open.setAttribute('aria-controls', 'workspace-context-menu');
    if (!entry.isDirectory) open.dataset.filePath = entry.path;
    container.append(toggle, open);
    container.addEventListener('contextmenu', (event) => showMenu(entry, event, null, open));
    open.addEventListener('keydown', (event) => {
      if (event.key !== 'ContextMenu' && !(event.key === 'F10' && event.shiftKey)) return;
      const bounds = open.getBoundingClientRect();
      showMenu(entry, event, { x: bounds.left + 8, y: bounds.bottom }, open);
    });
    return { container, toggle, open };
  }

  async function fillChildren(folderPath, container, depth, currentGeneration) {
    const children = await api.listDirectory(folderPath, showHidden);
    if (currentGeneration !== generation) return;
    container.replaceChildren();
    for (const child of children) await renderEntry(child, container, depth, currentGeneration);
    if (children.length === 0) container.append(message('Empty folder'));
  }

  async function renderEntry(entry, parent, depth, currentGeneration) {
    const { container, toggle, open } = row(entry, depth); parent.append(container);
    if (!entry.isDirectory) {
      open.addEventListener('click', () => Promise.resolve(onOpenFile({ root: workspace, path: entry.path, name: entry.name })).catch(onError));
      return;
    }
    const children = document.createElement('div'); children.className = 'tree-children'; children.hidden = true; parent.append(children);
    let loaded = false;
    async function expand() {
      expanded.add(entry.path); children.hidden = false; toggle.textContent = '▾'; toggle.setAttribute('aria-expanded', 'true');
      if (!loaded) {
        loaded = true;
        try { await fillChildren(entry.path, children, depth + 1, generation); }
        catch (error) { children.replaceChildren(message(errorMessage(error))); }
      }
    }
    function collapse() { expanded.delete(entry.path); children.hidden = true; toggle.textContent = '▸'; toggle.setAttribute('aria-expanded', 'false'); }
    function onToggle() { if (children.hidden) expand().catch(onError); else collapse(); }
    toggle.addEventListener('click', onToggle); open.addEventListener('click', onToggle);
    if (expanded.has(entry.path)) await expand();
  }

  async function refresh() {
    hideMenu();
    const currentGeneration = ++generation;
    tree.replaceChildren(message(workspace ? 'Loading workspace…' : 'Choose a workspace to begin.'));
    refreshButton.disabled = true;
    try {
      if (!workspace) return;
      const rootEntries = await api.listDirectory('', showHidden);
      if (currentGeneration !== generation) return;
      tree.replaceChildren();
      for (const entry of rootEntries) await renderEntry(entry, tree, 0, currentGeneration);
      if (rootEntries.length === 0) tree.append(message('This workspace is empty.'));
    } catch (error) {
      if (currentGeneration === generation) tree.replaceChildren(message(`Error loading workspace: ${errorMessage(error)}`));
    } finally { if (currentGeneration === generation) refreshButton.disabled = false; }
  }

  async function createNewFile(folderPath = '') {
    if (!workspace) throw new Error('Select a workspace first.');
    const answer = await askName('New File', folderPath ? `${folderPath}/untitled.txt` : 'untitled.txt', true);
    if (!answer) return;
    await api.createFile(workspace, answer.path, templateContent(answer.path, answer.template));
    await refresh(); await onOpenFile({ root: workspace, path: answer.path, name: answer.path.split('/').pop() });
  }

  async function createNewFolder(folderPath = '') {
    if (!workspace) throw new Error('Select a workspace first.');
    const answer = await askName('New Folder', folderPath ? `${folderPath}/new-folder` : 'new-folder');
    if (!answer) return;
    await api.createFolder(workspace, answer.path); expanded.add(answer.path); await refresh();
  }

  async function importFiles(folderPath = '') {
    if (!workspace) throw new Error('Select a workspace first.');
    await api.importFiles(workspace, folderPath); await refresh();
  }

  async function choose() {
    const previous = workspace; const settings = await api.chooseWorkspace(); onWorkspaceChange(settings);
    if (settings.workspace !== previous) { expanded.clear(); await refresh(); }
  }

  async function reveal(relativePath) {
    const parts = relativePath.split('/'); parts.pop();
    let current = '';
    for (const part of parts) { current = current ? `${current}/${part}` : part; expanded.add(current); }
    await refresh();
    const row = [...tree.querySelectorAll('[data-path]')].find((element) => element.dataset.path === relativePath);
    row?.scrollIntoView({ block: 'center' });
  }

  byId('new-workspace').addEventListener('click', async () => {
    try {
      const types = await api.projectTypesList();
      const id = window.prompt(`Project type (${types.types.map((type) => type.id).join(', ')})`, 'generic');
      if (!id) return;
      const settings = await api.projectTypeCreate(id.trim());
      if (settings.workspace) { onWorkspaceChange(settings); await refresh(); }
    } catch (error) { onError(error); }
  });
  byId('set-workspace-type').addEventListener('click', async () => {
    try {
      const types = await api.projectTypesList();
      const id = window.prompt(`Project type (${types.types.map((type) => type.id).join(', ')})`, types.current?.id ?? 'generic');
      if (!id) return;
      await api.projectTypeApply(id.trim()); await refresh();
    } catch (error) { onError(error); }
  });
  byId('choose-workspace').addEventListener('click', () => choose().catch(onError));
  pathButton.addEventListener('click', () => choose().catch(onError));
  refreshButton.addEventListener('click', () => refresh().catch(onError));
  byId('new-folder').addEventListener('click', () => createNewFolder().catch(onError));
  byId('import-files').addEventListener('click', () => importFiles().catch(onError));
  byId('new-file-main').addEventListener('click', () => createNewFile().catch(onError));
  hiddenButton.addEventListener('click', () => onShowHiddenChange(!showHidden).catch(onError));
  document.addEventListener('click', (event) => { if (!contextMenu.contains(event.target)) hideMenu(); });
  document.addEventListener('keydown', (event) => { if (event.key === 'Escape') hideMenu(true); });

  return { update, refresh, reveal, createNewFile };
}
