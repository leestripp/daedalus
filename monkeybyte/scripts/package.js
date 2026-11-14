// SPDX-License-Identifier: BSL-1.0
// Dependency-free Linux packaging around the already pinned Electron runtime.
import { spawnSync } from 'node:child_process';
import { createRequire } from 'node:module';
import { promises as fs } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const require = createRequire(import.meta.url);
const component = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const root = path.resolve(component, '..');
const args = process.argv.slice(2);
const outputIndex = args.indexOf('--output');
const output = path.resolve(outputIndex >= 0 ? args[outputIndex + 1] : path.join(component, 'release', 'MonkeyByte-linux-x64'));
const packageJson = JSON.parse(await fs.readFile(path.join(component, 'package.json'), 'utf8'));
const electron = process.env.MONKEYBYTE_ELECTRON_EXECUTABLE || require('electron');
const electronDir = path.dirname(electron);

if (!args.includes('--skip-build')) {
  const build = spawnSync('cmake', ['--build', path.join(root, 'build'), '--target', 'monkeybyte-electron-backend', '-j4'], { stdio: 'inherit' });
  if (build.status !== 0) process.exit(build.status ?? 1);
}

await fs.rm(output, { recursive: true, force: true });
await fs.mkdir(path.dirname(output), { recursive: true });
await fs.cp(electronDir, output, { recursive: true, preserveTimestamps: true });
await fs.rename(path.join(output, 'electron'), path.join(output, 'monkeybyte-bin'));
const resources = path.join(output, 'resources');
await fs.mkdir(path.join(resources, 'app'), { recursive: true });
for (const name of ['package.json', 'src', 'project-types']) {
  await fs.cp(path.join(component, name), path.join(resources, 'app', name), { recursive: true });
}
await fs.mkdir(path.join(resources, 'backend'), { recursive: true });
await fs.cp(path.join(root, 'build', 'monkeybyte', 'monkeybyte-electron-backend'),
  path.join(resources, 'backend', 'monkeybyte-electron-backend'));
await fs.cp(path.join(component, 'resources', 'monkeybyte.svg'), path.join(output, 'monkeybyte.svg'));

const launcher = `#!/usr/bin/env bash
set -euo pipefail
here="$(dirname "$(readlink -f "$0")")"
sandbox=()
helper=""
for candidate in "\${MONKEYBYTE_CHROME_SANDBOX:-}" "$here/chrome-sandbox" \
  /opt/google/chrome/chrome-sandbox /usr/lib/chromium/chrome-sandbox \
  "$HOME"/Applications/*/chrome-sandbox; do
  if [[ -n "$candidate" && -f "$candidate" && -u "$candidate" && "$(stat -c %u "$candidate")" == 0 ]]; then
    helper="$candidate"; break
  fi
done
if [[ -n "$helper" ]]; then
  export CHROME_DEVEL_SANDBOX="$helper"
  # Electron validates the helper beside its executable even when the
  # Chromium override is present. User-local packages are writable, so point
  # that helper at the already trusted root-owned installation.
  if [[ "$helper" != "$here/chrome-sandbox" && -w "$here" ]]; then
    ln -sfn "$helper" "$here/chrome-sandbox"
  fi
else
  # Try the kernel namespace sandbox where AppArmor permits it. Never use
  # --no-sandbox: restricted systems must install the Debian package or a
  # root-owned setuid helper instead.
  sandbox+=(--disable-setuid-sandbox)
fi
exec "$here/monkeybyte-bin" "\${sandbox[@]}" "$@"
`;
await fs.writeFile(path.join(output, 'monkeybyte'), launcher, { mode: 0o755 });
const desktop = await fs.readFile(path.join(component, 'resources', 'monkeybyte.desktop'), 'utf8');
await fs.writeFile(path.join(output, 'monkeybyte.desktop'), desktop);

const release = path.dirname(output);
const archive = path.join(release, `monkeybyte-electron-${packageJson.version}-linux-x64.tar.gz`);
const tar = spawnSync('tar', ['-czf', archive, '-C', release, path.basename(output)], { stdio: 'inherit' });
if (tar.status !== 0) process.exit(tar.status ?? 1);

let deb = null;
const dpkg = spawnSync('which', ['dpkg-deb'], { encoding: 'utf8' });
if (dpkg.status === 0) {
  const stage = path.join(release, 'deb-stage');
  await fs.rm(stage, { recursive: true, force: true });
  const library = path.join(stage, 'usr', 'lib', 'monkeybyte-electron');
  await fs.mkdir(library, { recursive: true });
  await fs.cp(output, library, { recursive: true });
  await fs.mkdir(path.join(stage, 'usr', 'bin'), { recursive: true });
  await fs.symlink('../lib/monkeybyte-electron/monkeybyte', path.join(stage, 'usr', 'bin', 'monkeybyte-electron'));
  const applications = path.join(stage, 'usr', 'share', 'applications');
  await fs.mkdir(applications, { recursive: true });
  await fs.writeFile(path.join(applications, 'monkeybyte.desktop'), desktop.replace(/^Exec=.*$/gm, 'Exec=monkeybyte-electron %U'));
  const icons = path.join(stage, 'usr', 'share', 'icons', 'hicolor', 'scalable', 'apps');
  await fs.mkdir(icons, { recursive: true });
  await fs.cp(path.join(output, 'monkeybyte.svg'), path.join(icons, 'monkeybyte.svg'));
  const debian = path.join(stage, 'DEBIAN');
  await fs.mkdir(debian, { recursive: true });
  await fs.writeFile(path.join(debian, 'control'), `Package: monkeybyte-electron\nVersion: ${packageJson.version}\nSection: editors\nPriority: optional\nArchitecture: amd64\nMaintainer: Lee Stripp\nDepends: libc6, libgtk-3-0, libnss3, libasound2t64 | libasound2\nDescription: MonkeyByte AI code editor\n Standalone Electron IDE with the Daedalus agent runtime, browser tools,\n persistent sessions, ChimpFlow voice, and project-type workflows.\n`);
  await fs.writeFile(path.join(debian, 'postinst'), '#!/bin/sh\nset -e\nchown root:root /usr/lib/monkeybyte-electron/chrome-sandbox\nchmod 4755 /usr/lib/monkeybyte-electron/chrome-sandbox\n', { mode: 0o755 });
  deb = path.join(release, `monkeybyte-electron_${packageJson.version}_amd64.deb`);
  const result = spawnSync('dpkg-deb', ['--build', '--root-owner-group', stage, deb], { stdio: 'inherit' });
  if (result.status !== 0) process.exit(result.status ?? 1);
  await fs.rm(stage, { recursive: true, force: true });
}

console.log(JSON.stringify({ status: 'success', output, archive, deb }, null, 2));
