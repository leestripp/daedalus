import { readdirSync } from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

const roots = ['src', 'tests', 'scripts'];
const files = [];

function collect(directory) {
  for (const entry of readdirSync(directory, { withFileTypes: true })) {
    const filename = path.join(directory, entry.name);
    if (entry.isDirectory()) {
      collect(filename);
    } else if (/\.(?:js|cjs)$/.test(entry.name)) {
      files.push(filename);
    }
  }
}

for (const root of roots) {
  collect(root);
}

for (const filename of files) {
  const result = spawnSync(process.execPath, ['--check', filename], { stdio: 'inherit' });
  if (result.status !== 0) {
    process.exit(result.status ?? 1);
  }
}
console.log(`Syntax checked ${files.length} JavaScript files.`);
