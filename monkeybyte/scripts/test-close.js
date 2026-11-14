// SPDX-License-Identifier: BSL-1.0
import { spawn } from 'node:child_process';
import { promises as fs } from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { setTimeout as delay } from 'node:timers/promises';

const root = fileURLToPath(new URL('..', import.meta.url));
const executable = process.env.MONKEYBYTE_ELECTRON_EXECUTABLE
  ?? path.join(root, 'release', 'MonkeyByte-linux-x64', 'monkeybyte');
const port = process.env.MONKEYBYTE_CLOSE_DEBUG_PORT ?? '9346';
const configHome = await fs.mkdtemp(path.join(os.tmpdir(), 'monkeybyte-electron-close-'));
const started = Date.now();
const child = spawn(executable, [
  '--remote-debugging-address=127.0.0.1', `--remote-debugging-port=${port}`,
], {
  env: { ...process.env, XDG_CONFIG_HOME: configHome },
  stdio: ['ignore', 'pipe', 'pipe'],
  detached: true,
});
let diagnostics = '';
child.stderr.on('data', (chunk) => { diagnostics = `${diagnostics}${chunk}`.slice(-8000); });
const exited = new Promise((resolve) => {
  child.once('exit', (code, signal) => resolve({ code, signal }));
});

try {
  let page;
  for (let attempt = 0; attempt < 100 && !page; ++attempt) {
    try {
      const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
      page = targets.find((target) => target.type === 'page');
    } catch {
      // Chromium has not opened its loopback debugging socket yet.
    }
    if (!page) await delay(100);
  }
  if (!page) throw new Error('Electron renderer did not start.');
  await delay(2500);
  const socket = new WebSocket(page.webSocketDebuggerUrl);
  await new Promise((resolve, reject) => {
    socket.addEventListener('open', resolve, { once: true });
    socket.addEventListener('error', reject, { once: true });
  });
  socket.send(JSON.stringify({ id: 1, method: 'Browser.close' }));
  const result = await Promise.race([exited, delay(12000, null)]);
  if (!result) throw new Error('Normal close did not return within 12 seconds.');
  socket.close();
  if (result.code !== 0 || result.signal) {
    throw new Error(`Normal close exited abnormally: ${JSON.stringify(result)}`);
  }
  console.log(JSON.stringify({ closed: true, elapsedMs: Date.now() - started, ...result }));
} catch (error) {
  try { process.kill(-child.pid, 'SIGKILL'); } catch { child.kill('SIGKILL'); }
  console.error(error.stack ?? error.message);
  console.error(diagnostics);
  process.exitCode = 1;
} finally {
  await fs.rm(configHome, { recursive: true, force: true });
}
