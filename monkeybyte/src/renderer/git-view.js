// SPDX-License-Identifier: BSL-1.0
import { byId, errorMessage, message } from './dom.js';

export function formatGitSummary(status) {
  if (status.notRepository) return 'Not a Git repository';
  const parts = [`⑂ ${status.branch || 'Detached HEAD'}`];
  if (!status.changes) parts.push('clean');
  else {
    parts.push(`${status.changes} ${status.changes === 1 ? 'change' : 'changes'}`);
    if (status.staged) parts.push(`${status.staged} staged`);
    if (status.unstaged) parts.push(`${status.unstaged} unstaged`);
    if (status.untracked) parts.push(`${status.untracked} untracked`);
  }
  if (status.upstream) parts.push(`${status.ahead} to push`, `${status.behind} to pull`);
  else parts.push('no upstream');
  return parts.join(' · ');
}

export function createGitView({ api, getWorkspace, onOpenFile, onError }) {
  async function refresh() {
    if (!getWorkspace()) { byId('git-status-bar').textContent = ''; return; }
    byId('git-refresh').disabled = true;
    try {
      const status = await api.gitStatus(getWorkspace());
      const summary = formatGitSummary(status);
      byId('git-branch').textContent = summary;
      byId('git-status-bar').textContent = summary;
      byId('git-status-bar').title = status.notRepository ? summary
        : `${status.upstream || 'No upstream'} · ${status.ahead} commits ahead (push) · ${status.behind} commits behind (pull)`;
      if (status.notRepository) {
        byId('git-status').replaceChildren(message(summary));
        return;
      }
      const rows = status.entries.map((entry) => {
        const button = document.createElement('button'); button.type = 'button'; button.className = 'git-entry';
        const code = document.createElement('span'); code.className = 'git-code'; code.textContent = `${entry.index}${entry.worktree}`;
        const name = document.createElement('span'); name.textContent = entry.path; name.title = entry.oldPath ? `${entry.oldPath} → ${entry.path}` : entry.path;
        button.append(code, name);
        button.addEventListener('click', () => onOpenFile({ root: getWorkspace(), path: entry.path,
          name: entry.path.split('/').pop() }).catch(onError));
        return button;
      });
      byId('git-status').replaceChildren(...(rows.length ? rows : [message('Working tree clean.')]));
    } catch (error) {
      byId('git-branch').textContent = '';
      byId('git-status-bar').textContent = 'Git unavailable';
      byId('git-status').replaceChildren(message(/not a git repository/i.test(errorMessage(error)) ? 'Not a Git repository.' : errorMessage(error)));
    } finally { byId('git-refresh').disabled = false; }
  }
  byId('git-refresh').addEventListener('click', () => refresh().catch(onError));
  const timer = setInterval(() => { if (getWorkspace()) refresh().catch(() => {}); }, 3000);
  window.addEventListener('pagehide', () => clearInterval(timer), { once: true });
  return { refresh };
}
