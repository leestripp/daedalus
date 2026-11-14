import { spawn, spawnSync } from 'node:child_process';
import { constants } from 'node:fs';
import { accessSync, mkdtempSync, readFileSync, rmSync, statSync } from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import electron from 'electron';

const executable = process.env.MONKEYBYTE_ELECTRON_EXECUTABLE ?? electron;
const forwarded = process.argv.slice(2);
const smokeUserData = forwarded.includes('--smoke-test')
  ? mkdtempSync(path.join(os.tmpdir(), 'monkeybyte-electron-settings-'))
  : null;

function cleanupSmokeSettings() {
  if (smokeUserData) {
    rmSync(smokeUserData, { recursive: true, force: true });
  }
}

function fail(message) {
  console.error(message);
  process.exitCode = 1;
}

if (forwarded.includes('--no-sandbox') || forwarded.includes('--disable-setuid-sandbox')) {
  cleanupSmokeSettings();
  fail('Sandbox flags are selected by the launcher policy and cannot be overridden.');
} else {
  launch();
}

function helperWorks(helper) {
  try {
    accessSync(helper, constants.X_OK);
    const stat = statSync(helper);
    if (stat.uid !== 0 || stat.gid !== 0 || (stat.mode & 0o7777) !== 0o4755) {
      return false;
    }
    const mount = spawnSync('findmnt', ['-T', helper, '-o', 'OPTIONS', '-n', '-r'], {
      encoding: 'utf8',
    });
    return mount.status === 0 && !mount.stdout.split(',').includes('nosuid');
  } catch {
    return false;
  }
}

function readSysctl(name, fallback) {
  try {
    return readFileSync(name, 'utf8').trim();
  } catch {
    return fallback;
  }
}

function namespaceSandboxWorks() {
  return readSysctl('/proc/sys/kernel/unprivileged_userns_clone', '0') === '1'
    && Number(readSysctl('/proc/sys/user/max_user_namespaces', '0')) > 0
    && readSysctl('/proc/sys/kernel/apparmor_restrict_unprivileged_userns', '0') !== '1';
}

function repairInstructions(helper) {
  const runtime = path.join(os.homedir(), '.cache', 'monkeybyte-electron-runtime');
  return [
    `No usable Chromium sandbox for ${executable}.`,
    `The helper ${helper} must be root:root, mode 4755, on a setuid-capable filesystem.`,
    'On this AppArmor-restricted host, install a helper instead of disabling sandboxing:',
    `  cp -a node_modules/electron/dist "${runtime}"`,
    `  sudo chown root:root "${runtime}/chrome-sandbox"`,
    `  sudo chmod 4755 "${runtime}/chrome-sandbox"`,
    `  MONKEYBYTE_ELECTRON_EXECUTABLE="${runtime}/electron" npm start`,
  ].join('\n');
}

function launch() {
  const helper = path.join(path.dirname(executable), 'chrome-sandbox');
  const useSetuid = helperWorks(helper);
  if (!useSetuid && !namespaceSandboxWorks()) {
    cleanupSmokeSettings();
    fail(repairInstructions(helper));
    return;
  }
  const args = [
    ...(useSetuid ? [] : ['--disable-setuid-sandbox']),
    '.',
    ...forwarded,
  ];
  const child = spawn(executable, args, {
    cwd: process.cwd(),
    env: {
      ...process.env,
      ...(smokeUserData ? { MONKEYBYTE_SMOKE_USER_DATA: smokeUserData } : {}),
    },
    stdio: 'inherit',
  });
  child.once('error', (error) => {
    cleanupSmokeSettings();
    fail(`Could not launch Electron: ${error.message}`);
  });
  child.once('exit', (code, signal) => {
    cleanupSmokeSettings();
    if (signal) {
      fail(`Electron exited from signal ${signal}.`);
    } else {
      process.exitCode = code ?? 1;
    }
  });
}
