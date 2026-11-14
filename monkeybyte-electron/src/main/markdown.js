// SPDX-License-Identifier: BSL-1.0

function escapeHtml(value) {
  return String(value).replaceAll('&', '&amp;').replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;').replaceAll('"', '&quot;').replaceAll("'", '&#39;');
}

function safeHref(value) {
  const href = value.trim();
  if (/^(?:https?:|file:)/i.test(href) || /^(?:\/|\.{0,2}\/|#|[^:]+\.(?:png|jpe?g|gif|webp|svg)$)/i.test(href)) {
    return href;
  }
  return '#';
}

function inline(value) {
  let text = escapeHtml(value);
  text = text.replace(/`([^`]+)`/g, '<code>$1</code>');
  text = text.replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>');
  text = text.replace(/\*([^*]+)\*/g, '<em>$1</em>');
  text = text.replace(/\[([^\]]+)\]\(([^)]+)\)/g, (_, label, href) =>
    `<a href="${escapeHtml(safeHref(href))}" target="_blank" rel="noreferrer">${label}</a>`);
  return text;
}

export function markdownToHtml(markdown, { title = 'Markdown Preview', baseUrl = '', dark = true } = {}) {
  const lines = String(markdown).split(/\r?\n/);
  const output = [];
  let inCode = false;
  let inList = false;
  let paragraph = [];
  const flushParagraph = () => {
    if (paragraph.length) output.push(`<p>${inline(paragraph.join(' '))}</p>`);
    paragraph = [];
  };
  const closeList = () => { if (inList) output.push('</ul>'); inList = false; };
  for (const line of lines) {
    if (line.startsWith('```')) {
      flushParagraph(); closeList();
      output.push(inCode ? '</code></pre>' : '<pre><code>');
      inCode = !inCode;
      continue;
    }
    if (inCode) { output.push(`${escapeHtml(line)}\n`); continue; }
    const heading = /^(#{1,6})\s+(.*)$/.exec(line);
    if (heading) {
      flushParagraph(); closeList();
      const level = heading[1].length;
      output.push(`<h${level}>${inline(heading[2])}</h${level}>`);
      continue;
    }
    const item = /^\s*[-*+]\s+(.*)$/.exec(line);
    if (item) {
      flushParagraph();
      if (!inList) { output.push('<ul>'); inList = true; }
      output.push(`<li>${inline(item[1])}</li>`);
      continue;
    }
    if (/^\s*([-*_]\s*){3,}$/.test(line)) { flushParagraph(); closeList(); output.push('<hr>'); continue; }
    if (/^\s*>\s?/.test(line)) { flushParagraph(); closeList(); output.push(`<blockquote>${inline(line.replace(/^\s*>\s?/, ''))}</blockquote>`); continue; }
    if (!line.trim()) { flushParagraph(); closeList(); continue; }
    paragraph.push(line.trim());
  }
  flushParagraph(); closeList();
  if (inCode) output.push('</code></pre>');
  const base = baseUrl ? `<base href="${escapeHtml(baseUrl)}">` : '';
  return `<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; img-src file: data: http: https:; style-src 'unsafe-inline'; font-src 'none'; script-src 'none'; object-src 'none'; form-action 'none'">
${base}<meta name="viewport" content="width=device-width,initial-scale=1"><title>${escapeHtml(title)}</title>
<style>
:root{color-scheme:${dark ? 'dark' : 'light'}}body{max-width:900px;margin:32px auto;padding:0 24px;background:${dark ? '#1e1e2e' : '#eff1f5'};color:${dark ? '#cdd6f4' : '#4c4f69'};font:16px/1.65 sans-serif}h1,h2,h3{color:${dark ? '#89b4fa' : '#1e66f5'}}a{color:#74c7ec}pre{overflow:auto;padding:16px;background:${dark ? '#181825' : '#e6e9ef'};border-radius:6px}code{font-family:monospace}blockquote{border-left:4px solid #74c7ec;margin-left:0;padding-left:16px;color:#a6adc8}img{max-width:100%}hr{border:0;border-top:1px solid #45475a}
</style></head><body>${output.join('\n')}</body></html>`;
}

export { escapeHtml, safeHref };
