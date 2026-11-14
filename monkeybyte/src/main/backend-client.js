// SPDX-License-Identifier: BSL-1.0
import { spawn } from 'node:child_process';
import crypto from 'node:crypto';
import http from 'node:http';

import { closeHttpServer, within } from './lifecycle.js';

function readBody(request, maximum = 4 * 1024 * 1024) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let size = 0;
    request.on('data', (chunk) => {
      size += chunk.length;
      if (size > maximum) { reject(new Error('UI request is too large.')); request.destroy(); return; }
      chunks.push(chunk);
    });
    request.on('end', () => resolve(Buffer.concat(chunks).toString('utf8')));
    request.on('error', reject);
  });
}

export function createBackendClient({ executable, dataDir, onEvent, executeUi }) {
  const bridgeToken = crypto.randomBytes(32).toString('hex');
  const uiToken = crypto.randomBytes(32).toString('hex');
  let child = null;
  let port = 0;
  let abortEvents = null;
  let closing = false;
  let eventTask = null;
  let uiServer = null;
  let stderr = '';

  async function startUiServer() {
    function sendJson(response, status, value) {
      const body = JSON.stringify(value);
      response.writeHead(status, { 'Content-Type': 'application/json',
        'Content-Length': Buffer.byteLength(body), Connection: 'close' });
      response.end(body);
    }
    uiServer = http.createServer(async (request, response) => {
      try {
        if (request.headers['x-monkeybyte-token'] !== uiToken) {
          sendJson(response, 401, { error: 'Unauthorized' });
          return;
        }
        const body = JSON.parse(await readBody(request) || '{}');
        const result = await executeUi(body.command, body.args ?? {});
        sendJson(response, 200, result ?? { ok: true });
      } catch (error) {
        sendJson(response, 200, { error: error.message });
      }
    });
    await new Promise((resolve, reject) => {
      uiServer.once('error', reject);
      uiServer.listen(0, '127.0.0.1', resolve);
    });
    return uiServer.address().port;
  }

  // §SSE-RECONNECT 2026-11-14: the event stream must survive backend
  // restarts and transient drops — previously one failure ended events()
  // permanently and the UI went silently deaf (chat froze, narration never
  // fired) until the app was restarted.
  async function events() {
    let failures = 0;
    while (!closing) {
      abortEvents = new AbortController();
      try {
        const response = await fetch(`http://127.0.0.1:${port}/v1/events`, {
          headers: { 'X-MonkeyByte-Token': bridgeToken }, signal: abortEvents.signal,
        });
        if (!response.ok) throw new Error(`Event stream failed: ${response.status}`);
        failures = 0;
        const decoder = new TextDecoder();
        let buffer = '';
        for await (const chunk of response.body) {
          buffer += decoder.decode(chunk, { stream: true });
          let boundary;
          while ((boundary = buffer.indexOf('\n\n')) >= 0) {
            const block = buffer.slice(0, boundary); buffer = buffer.slice(boundary + 2);
            let type = 'message'; const data = [];
            for (const line of block.split('\n')) {
              if (line.startsWith('event:')) type = line.slice(6).trim();
              if (line.startsWith('data:')) data.push(line.slice(5).trimStart());
            }
            if (!data.length || type === 'ready') continue;
            // A malformed block must not kill the whole stream.
            try { onEvent(type, JSON.parse(data.join('\n'))); }
            catch (parseError) {
              if (!closing) onEvent('backend_error', { message: `Skipped malformed backend event: ${parseError.message}` });
            }
          }
        }
      } catch (error) {
        if (closing || error.name === 'AbortError') return;
        failures += 1;
        if (failures <= 2 || failures % 10 === 0) {
          onEvent('backend_error', { message: `Event stream lost (${error.message}); reconnecting…` });
        }
      }
      await new Promise((resolve) => setTimeout(resolve, failures <= 2 ? 500 : 3000));
    }
  }

  async function start() {
    const uiPort = await startUiServer();
    child = spawn(executable, ['--data-dir', dataDir, '--ui-port', String(uiPort)], {
      env: { ...process.env, MONKEYBYTE_BRIDGE_TOKEN: bridgeToken, MONKEYBYTE_UI_TOKEN: uiToken },
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    child.stderr.on('data', (chunk) => { stderr = `${stderr}${chunk}`.slice(-16000); });
    child.on('exit', (code) => {
      if (!closing) onEvent('backend_error', { message: `Backend exited (${code}): ${stderr}` });
    });
    const ready = await new Promise((resolve, reject) => {
      const timeout = setTimeout(() => reject(new Error(`Backend startup timed out: ${stderr}`)), 15000);
      let stdout = '';
      child.stdout.on('data', (chunk) => {
        stdout += chunk;
        const end = stdout.indexOf('\n');
        if (end < 0) return;
        clearTimeout(timeout);
        try { resolve(JSON.parse(stdout.slice(0, end))); }
        catch (error) { reject(error); }
      });
      child.once('exit', (code) => { clearTimeout(timeout); reject(new Error(`Backend exited (${code}): ${stderr}`)); });
    });
    port = ready.port;
    eventTask = events();
    return { port };
  }

  async function request(path, { method = 'GET', body = {}, timeout = 30000 } = {}) {
    if (!port) throw new Error('Backend is not running.');
    const response = await fetch(`http://127.0.0.1:${port}${path}`, {
      method, headers: { 'X-MonkeyByte-Token': bridgeToken, 'Content-Type': 'application/json' },
      body: method === 'GET' ? undefined : JSON.stringify(body),
      signal: AbortSignal.timeout(timeout),
    });
    const result = await response.json();
    if (!response.ok || result.error) throw new Error(result.error || `Backend HTTP ${response.status}`);
    return result;
  }

  async function close() {
    const debug = (stage) => { if (process.env.MONKEYBYTE_DEBUG_CLOSE) console.error(`[backend close] ${stage}`); };
    debug('begin');
    closing = true;
    abortEvents?.abort();
    if (child && child.exitCode === null) {
      const exited = new Promise((resolve) => child.once('exit', resolve));
      debug('SIGTERM');
      child.kill('SIGTERM');
      await Promise.race([exited, new Promise((resolve) => setTimeout(resolve, 3000))]);
      if (child.exitCode === null) {
        debug('SIGKILL');
        child.kill('SIGKILL');
        await within(exited, 1000);
      }
    }
    if (eventTask) await within(eventTask, 1000);
    debug('event stream settled');
    await closeHttpServer(uiServer, 1000);
    debug('UI server settled');
    uiServer = null;
  }

  return { start, request, close, diagnostics: () => stderr };
}
