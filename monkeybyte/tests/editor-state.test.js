import assert from 'node:assert/strict';
import test from 'node:test';

import {
  activeEditorTab,
  closeEditorTab,
  createEditorState,
  editorTabId,
  isEditorTabDirty,
  markTabSaved,
  openEditorTab,
  setTabSaving,
  updateActiveContent,
  retargetEditorTabs,
} from '../src/renderer/editor-state.js';

function tab(path, content = path) {
  return { root: '/workspace', path, name: path, content };
}

test('opening a file creates or activates one tab', () => {
  let state = createEditorState();
  state = openEditorTab(state, tab('a.txt'));
  state = openEditorTab(state, tab('b.txt'));
  assert.equal(state.tabs.length, 2);
  assert.equal(state.activeId, editorTabId('/workspace', 'b.txt'));
  state = openEditorTab(state, tab('a.txt', 'ignored reload'));
  assert.equal(state.tabs.length, 2);
  assert.equal(activeEditorTab(state).content, 'a.txt');
});

test('editing, saving, and failed-save state preserve content', () => {
  let state = openEditorTab(createEditorState(), tab('a.txt', 'old'));
  state = updateActiveContent(state, 'new');
  const id = state.activeId;
  assert.equal(isEditorTabDirty(activeEditorTab(state)), true);
  state = setTabSaving(state, id, true);
  assert.equal(activeEditorTab(state).isSaving, true);
  state = setTabSaving(state, id, false);
  assert.equal(isEditorTabDirty(activeEditorTab(state)), true);
  state = markTabSaved(state, id, 'new');
  assert.equal(isEditorTabDirty(activeEditorTab(state)), false);
  assert.equal(activeEditorTab(state).isSaving, false);
});

test('closing the active middle tab selects the previous tab', () => {
  let state = createEditorState();
  for (const name of ['a.txt', 'b.txt', 'c.txt']) {
    state = openEditorTab(state, tab(name));
  }
  state = { ...state, activeId: editorTabId('/workspace', 'b.txt') };
  state = closeEditorTab(state, state.activeId);
  assert.equal(activeEditorTab(state).name, 'a.txt');
  state = closeEditorTab(state, state.activeId);
  assert.equal(activeEditorTab(state).name, 'c.txt');
  state = closeEditorTab(state, state.activeId);
  assert.equal(activeEditorTab(state), null);
});

test('closing a background tab does not change the active tab', () => {
  let state = createEditorState();
  state = openEditorTab(state, tab('a.txt'));
  state = openEditorTab(state, tab('b.txt'));
  const active = state.activeId;
  state = closeEditorTab(state, editorTabId('/workspace', 'a.txt'));
  assert.equal(state.activeId, active);
  assert.equal(state.tabs.length, 1);
});

test('renaming a folder retargets open descendants without losing dirty state', () => {
  let state = createEditorState();
  state = openEditorTab(state, { root: '/workspace', path: 'src/a.cpp', name: 'a.cpp', content: 'one' });
  state = openEditorTab(state, { root: '/workspace', path: 'other/a.cpp', name: 'a.cpp', content: 'two' });
  state = { ...state, tabs: [{ ...state.tabs[0], content: 'dirty' }, state.tabs[1]] };
  state = retargetEditorTabs(state, '/workspace', 'src', 'lib');
  assert.equal(state.tabs[0].path, 'lib/a.cpp');
  assert.equal(state.tabs[0].id, '/workspace\nlib/a.cpp');
  assert.equal(isEditorTabDirty(state.tabs[0]), true);
  assert.equal(state.tabs[1].path, 'other/a.cpp');
});
