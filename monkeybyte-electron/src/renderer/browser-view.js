// SPDX-License-Identifier: BSL-1.0
import { byId } from './dom.js';

export function createBrowserView({ api, editor, onError, getHomeUrl }) {
  let metadata = { activeId: null, tabs: [] };
  let viewport = null;
  const known = new Set();

  function active() {
    const id = editor.state().activeId;
    return metadata.tabs.find((tab) => tab.id === id) ?? null;
  }

  function renderChrome() {
    const tab = active();
    byId('browser-back').disabled = !tab?.canGoBack;
    byId('browser-forward').disabled = !tab?.canGoForward;
    byId('browser-reload').disabled = !tab;
    byId('browser-address').disabled = !tab;
    byId('browser-go').disabled = !tab;
    byId('browser-scheme').disabled = !tab;
    const dark = metadata.colorScheme !== 'light';
    byId('browser-scheme').setAttribute('aria-checked', String(dark));
    byId('browser-scheme').textContent = dark ? 'Dark' : 'Light';
    byId('browser-scheme').title = `Browser color scheme: ${dark ? 'Dark' : 'Light'} (click to switch)`;
    byId('browser-device').disabled = !tab;
    byId('browser-rotate').disabled = !tab;
    if (tab && document.activeElement !== byId('browser-address')) {
      byId('browser-address').value = tab.kind === 'markdown' ? '' : tab.url;
      byId('browser-address').placeholder = tab.kind === 'markdown' ? 'Rendered Markdown preview' : tab.url;
    }
  }

  function changed(next) {
    metadata = next;
    for (const tab of metadata.tabs) {
      if (!known.has(tab.id)) {
        known.add(tab.id);
        editor.openSpecial({ id: tab.id, kind: tab.kind, name: tab.title || 'Browser', url: tab.url });
      } else editor.updateSpecial(tab.id, { name: tab.title || 'Browser', url: tab.url });
    }
    for (const id of [...known]) {
      if (!metadata.tabs.some((tab) => tab.id === id)) {
        known.delete(id);
        if (editor.state().tabs.some((tab) => tab.id === id)) editor.closeTab(id, true).catch(onError);
      }
    }
    if (metadata.activeId && editor.state().activeId !== metadata.activeId &&
        metadata.tabs.some((tab) => tab.id === metadata.activeId)) {
      editor.activateTab(metadata.activeId);
    }
    renderChrome();
  }

  function command(name, args = {}) {
    const tab = active();
    return api.browserExecute(name, { tab: tab?.id, ...args }).catch(onError);
  }

  byId('browser-back').addEventListener('click', () => command('browser.back'));
  byId('browser-forward').addEventListener('click', () => command('browser.forward'));
  byId('browser-reload').addEventListener('click', () => command('browser.reload'));
  function navigate() {
    command('browser.navigate', { url: byId('browser-address').value });
  }
  byId('browser-address').addEventListener('keydown', (event) => {
    if (event.key !== 'Enter') return;
    event.preventDefault(); navigate();
  });
  byId('browser-go').addEventListener('click', navigate);
  byId('browser-scheme').addEventListener('click', () => {
    command('browser.scheme', { scheme: metadata.colorScheme === 'light' ? 'dark' : 'light' });
  });
  byId('browser-device').addEventListener('change', () => {
    const value = byId('browser-device').value;
    viewport = value === 'fit' ? null : { width: Number(value.split('x')[0]), height: Number(value.split('x')[1]) };
    api.browserViewport(viewport).catch(onError);
  });
  byId('browser-rotate').addEventListener('click', () => {
    if (!viewport) viewport = { width: byId('browser-host').clientHeight, height: byId('browser-host').clientWidth };
    else viewport = { width: viewport.height, height: viewport.width };
    byId('browser-device').value = 'fit';
    api.browserViewport(viewport).catch(onError);
  });

  byId('open-browser').addEventListener('click', async () => {
    try {
      const home = getHomeUrl?.().trim() || 'https://www.google.com';
      const opened = await api.browserExecute('browser.open', { url: home });
      const state = await api.browserExecute('browser.list', {});
      const tab = state.tabs.find((candidate) => candidate.id === opened.id);
      const address = byId('browser-address');
      address.value = tab?.url ?? home;
      address.focus(); address.select();
    } catch (error) { onError(error); }
  });

  const host = byId('browser-host');
  function updateBounds() {
    if (host.hidden || !host.clientWidth || !host.clientHeight) return;
    const rect = host.getBoundingClientRect();
    api.browserBounds({ x: rect.left, y: rect.top, width: rect.width, height: rect.height }).catch(onError);
  }
  const observer = new ResizeObserver(updateBounds);
  observer.observe(host);
  window.addEventListener('resize', updateBounds);
  api.onBrowserChanged(changed);
  renderChrome();
  return { changed, updateBounds, open: (url) => api.browserExecute('browser.open', { url }) };
}
