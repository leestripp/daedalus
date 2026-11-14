// SPDX-License-Identifier: BSL-1.0
import { byId, errorMessage, message } from './dom.js';

export function createSearchView({ api, getWorkspace, onOpenFile, onError }) {
  let latestGeneration = 0;
  async function search() {
    const generation = ++latestGeneration;
    const query = byId('search-input').value;
    const results = byId('search-results');
    if (!query) return;
    byId('search-submit').disabled = true;
    byId('search-status').textContent = 'Searching…';
    results.replaceChildren();
    try {
      const response = await api.searchWorkspace(getWorkspace(), {
        query, fileGlob: byId('search-files').value.trim(), caseSensitive: byId('search-case').checked,
      });
      if (generation !== latestGeneration) return;
      byId('search-status').textContent = `${response.matches.length} matches${response.truncated ? ' (result limit reached)' : ''}`;
      if (!response.matches.length) results.append(message('No matches found.'));
      for (const match of response.matches) {
        const button = document.createElement('button'); button.type = 'button'; button.className = 'search-result';
        const location = document.createElement('span'); location.className = 'search-location';
        location.textContent = `${match.path}:${match.line}:${match.column}`;
        const preview = document.createElement('span'); preview.textContent = match.preview;
        button.append(location, preview);
        button.addEventListener('click', () => onOpenFile({ root: getWorkspace(), path: match.path,
          name: match.path.split('/').pop() }, match.line).catch(onError));
        results.append(button);
      }
    } catch (error) { byId('search-status').textContent = errorMessage(error); }
    finally { byId('search-submit').disabled = false; }
  }
  byId('search-submit').addEventListener('click', () => search().catch(onError));
  byId('search-input').addEventListener('keydown', (event) => {
    if (event.key === 'Enter') { event.preventDefault(); search().catch(onError); }
  });
  return { search };
}
