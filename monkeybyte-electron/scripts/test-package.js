// SPDX-License-Identifier: BSL-1.0
import { spawn, spawnSync } from 'node:child_process';
import { promises as fs } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const component = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const release = path.join(component, 'release');
const packageJson = JSON.parse(await fs.readFile(path.join(component, 'package.json'), 'utf8'));
const desktop = await fs.readFile(path.join(release, 'MonkeyByte-linux-x64', 'monkeybyte.desktop'), 'utf8');
if (!desktop.includes('Name=MonkeyByte\n') || desktop.includes('MonkeyByte (Electron)') ||
    !desktop.includes('Exec=monkeybyte-electron')) {
  throw new Error('Portable desktop entry must be named MonkeyByte and launch Electron');
}
await fs.access(path.join(release, 'MonkeyByte-linux-x64', 'monkeybyte')).then(async () => {
  await fs.access(path.join(release, 'MonkeyByte-linux-x64', 'monkeybyte-electron.desktop')).then(() => {
    throw new Error('Portable package still contains a duplicate Electron desktop entry');
  }, () => {});
});

const whichDpkg = spawnSync('which', ['dpkg-deb']);
if (whichDpkg.status === 0) {
  const deb = path.join(release, `monkeybyte-electron_${packageJson.version}_amd64.deb`);
  const contents = spawnSync('dpkg-deb', ['--contents', deb], { encoding: 'utf8' });
  if (contents.status !== 0) throw new Error(contents.stderr || 'Could not inspect Electron Debian package');
  const paths = contents.stdout.split('\n');
  const includesPath = (entry) => paths.some((line) => line.includes(` ${entry} `) || line.endsWith(` ${entry}`));
  if (!includesPath('./usr/bin/monkeybyte-electron')) {
    throw new Error('Electron Debian package is missing monkeybyte-electron');
  }
  if (!includesPath('./usr/share/applications/monkeybyte.desktop')) {
    throw new Error('Debian package is missing the primary MonkeyByte launcher');
  }
  for (const forbidden of ['./usr/bin/monkeybyte', './usr/share/applications/monkeybyte-electron.desktop']) {
    if (includesPath(forbidden)) {
      throw new Error(`Electron Debian package must not install ${forbidden}`);
    }
  }
  const control = spawnSync('dpkg-deb', ['--field', deb, 'Package'], { encoding: 'utf8' });
  if (control.status !== 0 || control.stdout.trim() !== 'monkeybyte-electron') {
    throw new Error('Electron Debian package has the wrong package name');
  }
}

const executable = path.join(component, 'release', 'MonkeyByte-linux-x64', 'monkeybyte');
const child = spawn(executable, ['--smoke-test'], {
  cwd: component, env: { ...process.env }, stdio: 'inherit',
});
child.once('error', (error) => { console.error(error); process.exitCode = 1; });
child.once('exit', (code, signal) => {
  if (signal) { console.error(`Packaged smoke test terminated by ${signal}`); process.exitCode = 1; }
  else process.exitCode = code ?? 1;
});
