// SPDX-License-Identifier: BSL-1.0

const KEYWORDS = new Set((
  'alignas auto bool break case catch char class const constexpr consteval constinit continue ' +
  'default delete do double else enum explicit export extern false float for friend if inline int ' +
  'long namespace new noexcept nullptr operator private protected public return short signed sizeof ' +
  'static struct switch template this throw true try typedef typename union unsigned using virtual void ' +
  'async await function let var of in instanceof import from as yield async function ' +
  'def elif except finally global is lambda None nonlocal not or pass raise while with ' +
  'interface type implements extends super readonly string number undefined null'
).split(/\s+/));

export function languageForPath(path) {
  const extension = path.split('.').pop()?.toLowerCase();
  if (['py'].includes(extension)) return 'python';
  if (['js', 'mjs', 'cjs', 'ts', 'tsx', 'jsx', 'json'].includes(extension)) return 'javascript';
  if (['md', 'markdown'].includes(extension)) return 'markdown';
  if (['html', 'htm', 'xml', 'css'].includes(extension)) return 'markup';
  return 'cpp';
}

export function highlightText(text, path = '') {
  const language = languageForPath(path);
  const lines = [];
  let blockComment = false;
  for (const line of text.split('\n')) {
    if (language === 'markdown') {
      lines.push([{ text: line, className: /^\s{0,3}#{1,6}\s/.test(line) ? 'syntax-heading' : '' }]);
      continue;
    }
    const tokens = [];
    let rest = line;
    if (blockComment) {
      const end = rest.indexOf('*/');
      if (end < 0) {
        lines.push([{ text: rest, className: 'syntax-comment' }]);
        continue;
      }
      tokens.push({ text: rest.slice(0, end + 2), className: 'syntax-comment' });
      rest = rest.slice(end + 2);
      blockComment = false;
    }
    const pattern = language === 'python'
      ? /(#.*$)|("""[\s\S]*?"""|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*')|(\b\d+(?:\.\d+)?\b)|(\b[A-Za-z_][A-Za-z0-9_]*\b)/g
      : /(\/\*[\s\S]*?$|\/\/.*$)|("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|`(?:\\.|[^`\\])*`)|(\b\d+(?:\.\d+)?\b)|(\b[A-Za-z_][A-Za-z0-9_]*\b)/g;
    let last = 0;
    for (const match of rest.matchAll(pattern)) {
      const index = match.index ?? 0;
      if (index > last) tokens.push({ text: rest.slice(last, index), className: '' });
      let className = '';
      if (match[1]) className = 'syntax-comment';
      else if (match[2]) className = 'syntax-string';
      else if (match[3]) className = 'syntax-number';
      else if (match[4] && KEYWORDS.has(match[4])) className = 'syntax-keyword';
      if (className === 'syntax-comment' && match[0].startsWith('/*') && !match[0].endsWith('*/')) {
        blockComment = true;
      }
      tokens.push({ text: match[0], className });
      last = index + match[0].length;
    }
    if (last < rest.length) tokens.push({ text: rest.slice(last), className: '' });
    lines.push([...tokens.map((token) => ({ ...token })), ...tokens.length ? [] : [{ text: '', className: '' }]]);
    if (tokens.length && rest.startsWith('#include')) lines[lines.length - 1] = [{ text: rest, className: 'syntax-preprocessor' }];
  }
  return lines;
}
