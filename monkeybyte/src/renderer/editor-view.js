// SPDX-License-Identifier: BSL-1.0
import { byId, errorMessage } from './dom.js';
import {
  activeEditorTab,
  closeEditorTab,
  createEditorState,
  isEditorTabDirty,
  markTabSaved,
  openEditorTab,
  openSpecialEditorTab,
  activateEditorTab,
  updateSpecialEditorTab,
  retargetEditorTabs,
  setTabSaving,
  updateActiveContent,
} from './editor-state.js';
import { highlightText } from './syntax-highlight.js';

export function createEditorView({ api, onError, onWorkspaceRefresh, onTabActivated, onSpecialClosed }) {
  const tabsElement = byId('tab-list');
  const editor = byId('editor');
  const highlight = byId('editor-highlight');
  const lineNumbers = byId('line-numbers');
  const saveButton = byId('save-file');
  const closeButton = byId('close-file');
  const deleteButton = byId('delete-file');
  let state = createEditorState();
  let settings = { fontSize: 14, tabSize: 4, wordWrap: true, showLineNumbers: true };

  function renderTabs() {
    const tabs = state.tabs.map((tab, index) => {
      const wrapper = document.createElement('div');
      wrapper.className = `editor-tab${tab.id === state.activeId ? ' active' : ''}`;
      const open = document.createElement('button');
      open.type = 'button';
      open.className = 'tab-open';
      open.setAttribute('role', 'tab');
      open.setAttribute('aria-selected', String(tab.id === state.activeId));
      open.textContent = `${isEditorTabDirty(tab) ? '● ' : ''}${tab.name}`;
      open.title = `${tab.root}/${tab.path}`;
      open.addEventListener('click', () => {
        activateTab(tab.id);
      });
      const close = document.createElement('button');
      close.type = 'button';
      close.className = 'tab-close';
      close.textContent = '×';
      close.title = `Close ${tab.name}`;
      close.setAttribute('aria-label', `Close ${tab.name}`);
      close.addEventListener('click', (event) => {
        event.stopPropagation();
        closeTab(tab.id).catch(onError);
      });
      wrapper.dataset.tabIndex = String(index);
      wrapper.append(open, close);
      return wrapper;
    });
    tabsElement.replaceChildren(...tabs);
  }

  function renderHeader() {
    const active = activeEditorTab(state);
    const special = active && active.kind !== 'file';
    const dirty = !special && isEditorTabDirty(active);
    byId('file-title').textContent = active?.name ?? 'MonkeyByte';
    byId('dirty-indicator').hidden = !dirty;
    saveButton.disabled = !active || !dirty || active.isSaving;
    closeButton.disabled = !active;
    deleteButton.disabled = !active || active.isSaving;
    byId('editor-empty').hidden = Boolean(active);
    byId('editor-container').hidden = !active || special;
    byId('browser-container').hidden = !special;
    editor.disabled = !active || active?.isSaving;
    byId('save-status').textContent = active?.isSaving ? 'Saving…' : '';
    byId('find-toggle').disabled = !active;
  }

  function renderBuffer() {
    const active = activeEditorTab(state);
    if (active && active.kind !== 'file') { renderHighlight(); return; }
    editor.value = active?.content ?? '';
    editor.scrollTop = 0;
    editor.scrollLeft = 0;
    renderHighlight();
  }

  function renderHighlight() {
    const active = activeEditorTab(state);
    const lines = highlightText(editor.value, active?.path ?? '');
    const fragments = lines.map((tokens, index) => {
      const row = document.createElement('div');
      row.className = 'highlight-line';
      row.dataset.line = String(index + 1);
      for (const token of tokens) {
        const span = document.createElement('span');
        if (token.className) span.className = token.className;
        span.textContent = token.text;
        row.append(span);
      }
      row.append('\n');
      return row;
    });
    highlight.replaceChildren(...fragments);
    const numbers = lines.map((_, index) => {
      const row = document.createElement('div');
      row.textContent = String(index + 1);
      return row;
    });
    lineNumbers.replaceChildren(...numbers);
    lineNumbers.hidden = !settings.showLineNumbers;
    syncScroll();
  }

  function renderAll() {
    renderTabs();
    renderHeader();
    renderBuffer();
  }

  function renderChrome() {
    renderTabs();
    renderHeader();
  }

  async function openFile(file, line = 0) {
    try {
      const content = await api.readTextFile(file.root, file.path);
      state = openEditorTab(state, { ...file, content });
      renderAll();
      onTabActivated?.(activeEditorTab(state));
      if (line > 0) {
        const lines = editor.value.split('\n');
        const position = lines.slice(0, line - 1).reduce((sum, value) => sum + value.length + 1, 0);
        editor.focus();
        editor.setSelectionRange(position, position + (lines[line - 1]?.length ?? 0));
        const rowHeight = Number.parseFloat(getComputedStyle(editor).lineHeight) || settings.fontSize * 1.55;
        editor.scrollTop = Math.max(0, (line - 3) * rowHeight);
        syncScroll();
      }
    } catch (error) {
      if (/Binary files|valid UTF-8/.test(errorMessage(error))) {
        await api.openExternal(file.root, file.path);
        byId('save-status').textContent = `Opened ${file.name} externally`;
      } else onError(error);
    }
  }

  async function saveActive() {
    const active = activeEditorTab(state);
    if (!active || active.isSaving || !isEditorTabDirty(active)) return;
    const id = active.id;
    const savedContent = active.content;
    state = setTabSaving(state, id, true);
    renderHeader();
    try {
      await api.writeTextFile(active.root, active.path, active.content);
      state = markTabSaved(state, id, savedContent);
      renderChrome();
      if (state.activeId === id) {
        byId('save-status').textContent = 'Saved';
        setTimeout(() => {
          if (state.activeId === id && !activeEditorTab(state)?.isSaving) byId('save-status').textContent = '';
        }, 2000);
      }
      onWorkspaceRefresh?.();
    } catch (error) {
      state = setTabSaving(state, id, false);
      renderHeader();
      onError(error);
    }
  }

  async function closeTab(id, force = false) {
    const tab = state.tabs.find((candidate) => candidate.id === id);
    if (!tab) return;
    if (!force && isEditorTabDirty(tab) && !(await api.confirmClose(tab.name))) return;
    if (tab.kind !== 'file' && !force) await onSpecialClosed?.(tab);
    state = closeEditorTab(state, id);
    renderAll();
    onTabActivated?.(activeEditorTab(state));
  }

  async function deleteActive() {
    const active = activeEditorTab(state);
    if (!active) return;
    if (isEditorTabDirty(active) && !(await api.confirmClose(`${active.name} before deleting`))) return;
    const result = await api.deletePath(active.root, active.path);
    if (result.deleted) {
      await closeTab(active.id, true);
      onWorkspaceRefresh?.();
    }
  }

  async function renamePath(root, oldPath, newPath) {
    const affected = state.tabs.filter((tab) => tab.root === root &&
      (tab.path === oldPath || tab.path.startsWith(`${oldPath}/`)));
    if (affected.some(isEditorTabDirty)) throw new Error('Save affected files before renaming them.');
    await api.renamePath(root, oldPath, newPath);
    state = retargetEditorTabs(state, root, oldPath, newPath);
    renderAll();
    onWorkspaceRefresh?.();
  }

  async function deletePath(root, relativePath) {
    const affected = state.tabs.filter((tab) => tab.root === root &&
      (tab.path === relativePath || tab.path.startsWith(`${relativePath}/`)));
    if (affected.some(isEditorTabDirty)) throw new Error('Save or close dirty files before deleting them.');
    const result = await api.deletePath(root, relativePath);
    if (result.deleted) {
      for (const tab of affected) await closeTab(tab.id, true);
      onWorkspaceRefresh?.();
    }
    return result.deleted;
  }

  function replaceSelection(text, selectStart = null, selectEnd = null) {
    const start = editor.selectionStart;
    const end = editor.selectionEnd;
    editor.setRangeText(text, start, end, 'end');
    if (selectStart !== null) editor.setSelectionRange(start + selectStart, start + (selectEnd ?? selectStart));
    editor.dispatchEvent(new Event('input'));
  }

  editor.addEventListener('input', () => {
    state = updateActiveContent(state, editor.value);
    renderChrome();
    renderHighlight();
  });
  editor.addEventListener('scroll', syncScroll);
  editor.addEventListener('keydown', (event) => {
    if (event.key === 'Tab' && !event.ctrlKey && !event.metaKey) {
      event.preventDefault();
      replaceSelection(' '.repeat(settings.tabSize));
      return;
    }
    const pairs = { '(': ')', '[': ']', '{': '}', '"': '"', "'": "'", '`': '`' };
    if (pairs[event.key] && !event.ctrlKey && !event.metaKey && !event.altKey) {
      event.preventDefault();
      const selected = editor.value.slice(editor.selectionStart, editor.selectionEnd);
      replaceSelection(`${event.key}${selected}${pairs[event.key]}`, 1, 1 + selected.length);
    }
  });

  function syncScroll() {
    highlight.scrollTop = editor.scrollTop;
    highlight.scrollLeft = editor.scrollLeft;
    lineNumbers.scrollTop = editor.scrollTop;
  }

  function findNext(replace = false) {
    const query = byId('find-input').value;
    if (!query) return;
    const caseSensitive = byId('find-case').checked;
    const source = caseSensitive ? editor.value : editor.value.toLocaleLowerCase();
    const needle = caseSensitive ? query : query.toLocaleLowerCase();
    let index = source.indexOf(needle, editor.selectionEnd);
    if (index < 0) index = source.indexOf(needle);
    if (index < 0) {
      byId('find-status').textContent = 'No matches';
      return;
    }
    editor.focus();
    editor.setSelectionRange(index, index + query.length);
    byId('find-status').textContent = '';
    if (replace) replaceSelection(byId('replace-input').value);
  }

  function replaceAll() {
    const query = byId('find-input').value;
    if (!query) return;
    const caseSensitive = byId('find-case').checked;
    const source = editor.value;
    const needle = caseSensitive ? query : query.toLocaleLowerCase();
    const haystack = caseSensitive ? source : source.toLocaleLowerCase();
    const replacement = byId('replace-input').value;
    let output = '';
    let from = 0;
    let count = 0;
    while (from <= source.length) {
      const index = haystack.indexOf(needle, from);
      if (index < 0) break;
      output += source.slice(from, index) + replacement;
      from = index + query.length;
      ++count;
    }
    output += source.slice(from);
    if (count) {
      editor.value = output;
      editor.dispatchEvent(new Event('input'));
    }
    byId('find-status').textContent = `${count} replacements`;
  }

  saveButton.addEventListener('click', () => saveActive().catch(onError));
  deleteButton.addEventListener('click', () => deleteActive().catch(onError));
  closeButton.addEventListener('click', () => state.activeId && closeTab(state.activeId).catch(onError));
  byId('find-toggle').addEventListener('click', () => {
    byId('find-bar').hidden = !byId('find-bar').hidden;
    if (!byId('find-bar').hidden) byId('find-input').focus();
  });
  byId('find-next').addEventListener('click', () => findNext());
  byId('find-input').addEventListener('keydown', (event) => {
    if (event.key === 'Enter') { event.preventDefault(); findNext(); }
    if (event.key === 'Escape') byId('find-bar').hidden = true;
  });
  byId('replace-next').addEventListener('click', () => findNext(true));
  byId('replace-all').addEventListener('click', replaceAll);

  document.addEventListener('keydown', (event) => {
    if (!(event.ctrlKey || event.metaKey) || event.altKey) return;
    const key = event.key.toLowerCase();
    if (key === 's') { event.preventDefault(); saveActive().catch(onError); }
    else if (key === 'w' && state.activeId) { event.preventDefault(); closeTab(state.activeId).catch(onError); }
    else if (key === 'f' && state.activeId) {
      event.preventDefault(); byId('find-bar').hidden = false; byId('find-input').focus(); byId('find-input').select();
    } else if (key === 'h' && state.activeId) {
      event.preventDefault(); byId('find-bar').hidden = false; byId('replace-input').focus();
    }
  });

  function applySettings(next) {
    settings = { ...settings, ...next };
    for (const element of [editor, highlight, lineNumbers]) {
      element.style.fontSize = `${settings.fontSize}px`;
      element.style.tabSize = String(settings.tabSize);
      element.style.whiteSpace = settings.wordWrap ? 'pre-wrap' : 'pre';
    }
    editor.wrap = settings.wordWrap ? 'soft' : 'off';
    renderHighlight();
  }

  function getSelection() {
    return { text: editor.value.slice(editor.selectionStart, editor.selectionEnd),
             start: editor.selectionStart, end: editor.selectionEnd,
             path: activeEditorTab(state)?.path ?? '' };
  }

  function openSpecial(tab) {
    state = openSpecialEditorTab(state, tab);
    renderAll();
    onTabActivated?.(activeEditorTab(state));
  }

  function activateTab(id) {
    state = activateEditorTab(state, id);
    renderAll();
    onTabActivated?.(activeEditorTab(state));
  }

  function updateSpecial(id, patch) {
    state = updateSpecialEditorTab(state, id, patch);
    renderTabs();
    if (state.activeId === id) renderHeader();
  }

  renderAll();
  return { openFile, openSpecial, activateTab, updateSpecial, applySettings,
           renamePath, deletePath, saveActive, getSelection, closeTab,
           state: () => state };
}
