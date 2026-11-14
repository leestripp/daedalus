export function createEditorState() {
  return { tabs: [], activeId: null };
}

export function editorTabId(root, relativePath) {
  return `${root}\n${relativePath}`;
}

export function activeEditorTab(state) {
  return state.tabs.find((tab) => tab.id === state.activeId) ?? null;
}

export function openEditorTab(state, { root, path, name, content }) {
  const id = editorTabId(root, path);
  const existing = state.tabs.find((tab) => tab.id === id);
  if (existing) {
    return { ...state, activeId: id };
  }
  const tab = {
    id,
    root,
    path,
    name,
    content,
    originalContent: content,
    isSaving: false,
    kind: 'file',
  };
  return { tabs: [...state.tabs, tab], activeId: id };
}

function replaceTab(state, id, replacement) {
  return {
    ...state,
    tabs: state.tabs.map((tab) => (tab.id === id ? { ...tab, ...replacement } : tab)),
  };
}

export function updateActiveContent(state, content) {
  if (!state.activeId) {
    return state;
  }
  return replaceTab(state, state.activeId, { content });
}

export function setTabSaving(state, id, isSaving) {
  return replaceTab(state, id, { isSaving });
}

export function markTabSaved(state, id, savedContent) {
  return replaceTab(state, id, {
    originalContent: savedContent,
    isSaving: false,
  });
}

export function closeEditorTab(state, id) {
  const index = state.tabs.findIndex((tab) => tab.id === id);
  if (index < 0) {
    return state;
  }
  const tabs = state.tabs.filter((tab) => tab.id !== id);
  let activeId = state.activeId;
  if (activeId === id) {
    activeId = tabs[Math.max(0, index - 1)]?.id ?? null;
  }
  return { tabs, activeId };
}

export function isEditorTabDirty(tab) {
  return Boolean(tab && tab.content !== tab.originalContent);
}

export function retargetEditorTabs(state, root, oldPath, newPath) {
  return {
    ...state,
    tabs: state.tabs.map((tab) => {
      if (tab.root !== root) return tab;
      let nextPath = tab.path;
      if (tab.path === oldPath) nextPath = newPath;
      else if (oldPath && tab.path.startsWith(`${oldPath}/`)) {
        nextPath = `${newPath}${tab.path.slice(oldPath.length)}`;
      }
      if (nextPath === tab.path) return tab;
      const nextName = nextPath.split('/').pop() || nextPath;
      return { ...tab, path: nextPath, name: nextName, id: `${root}\n${nextPath}` };
    }),
  };
}

export function openSpecialEditorTab(state, { id, kind, name, url }) {
  const existing = state.tabs.find((tab) => tab.id === id);
  if (existing) return { ...state, activeId: id,
    tabs: state.tabs.map((tab) => tab.id === id ? { ...tab, name, url, kind } : tab) };
  return { tabs: [...state.tabs, { id, kind, name, url, path: url, root: '',
    content: '', originalContent: '', isSaving: false }], activeId: id };
}

export function activateEditorTab(state, id) {
  return state.tabs.some((tab) => tab.id === id) ? { ...state, activeId: id } : state;
}

export function updateSpecialEditorTab(state, id, patch) {
  return { ...state, tabs: state.tabs.map((tab) => tab.id === id ? { ...tab, ...patch } : tab) };
}
