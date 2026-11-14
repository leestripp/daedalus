// SPDX-License-Identifier: BSL-1.0
import http from 'node:http';
import { promises as fs } from 'node:fs';
import path from 'node:path';

async function sendControlW(window) {
  window.webContents.sendInputEvent({ type: 'keyDown', keyCode: 'w', modifiers: ['control'] });
  window.webContents.sendInputEvent({ type: 'keyUp', keyCode: 'w', modifiers: ['control'] });
}

async function waitForBrowserCount(window, expected) {
  const deadline = Date.now() + 10000;
  while (Date.now() < deadline) {
    const count = await window.webContents.executeJavaScript(
      "window.monkeybyte.browserExecute('browser.list').then((value) => value.tabs.length)", true);
    if (count === expected) return;
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`Timed out waiting for ${expected} browser tabs after Ctrl+W.`);
}

export async function runBrowserSmokeTest(window, root) {
  const schemeFixture = '<meta name="color-scheme" content="light dark"><style>body{background:#fafafa;color:#222}@media(prefers-color-scheme:dark){body{background:#111111;color:#eeeeee}}</style>';
  const server = http.createServer((request, response) => {
    response.setHeader('Content-Type', 'text/html; charset=utf-8');
    if (request.url === '/light-only') {
      const scheme = ['dark', 'light'].includes(request.headers['sec-ch-prefers-color-scheme'])
        ? request.headers['sec-ch-prefers-color-scheme'] : 'none';
      response.end('<!DOCTYPE html><title>Light Only Fixture</title><style>body{background:#fafafa;color:#222}</style><main><h1>Light-only page</h1><p>Scheme header: ' + scheme + '</p></main>');
      return;
    }
    if (request.url === '/second') {
      response.end('<!DOCTYPE html><title>Second Page</title>' + schemeFixture + '<main><h1>Second rendered page</h1></main>');
      return;
    }
    response.end(`<!DOCTYPE html><title>Browser Fixture</title>${schemeFixture}<main>
      <h1>Rendered fixture</h1><a href="/second">Second page</a>
      <label>Name <input id="name"></label><input id="upload" type="file">
      <script>
        console.error('fixture-console-error');
        document.querySelector('#upload').addEventListener('change', (event) => {
          document.body.dataset.upload = event.target.files[0]?.name || '';
        });
      </script></main>`);
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const address = server.address();
  const url = `http://127.0.0.1:${address.port}/`;
  const upload = path.join(root, 'upload-fixture.txt');
  await fs.writeFile(upload, 'browser upload\n');
  await fs.writeFile(path.join(root, 'preview.md'), '# Preview Fixture\n\nSafe **Markdown**.\n');
  try {
    const result = await window.webContents.executeJavaScript(`(async () => {
      const checks = [];
      const check = (condition, name) => {
        checks.push({ name, passed: Boolean(condition) });
        if (!condition) throw new Error('Browser smoke assertion failed: ' + name);
      };
      const waitFor = async (callback, description) => {
        const deadline = Date.now() + 8000;
        while (Date.now() < deadline) {
          try { const value = await callback(); if (value) return value; } catch {}
          await new Promise((resolve) => setTimeout(resolve, 80));
        }
        throw new Error('Timed out: ' + description);
      };
      const api = window.monkeybyte;
      const home = document.querySelector('#browser-home-url');
      home.value = ${JSON.stringify(url)};
      home.dispatchEvent(new Event('change', { bubbles: true }));
      await waitFor(async () => (await api.getBootstrap()).settings.browserHomeUrl === ${JSON.stringify(url)},
        'browser home URL saved through Settings');
      document.querySelector('#open-browser').click();
      await waitFor(async () => (await api.browserExecute('browser.list')).tabs.length === 1,
        'Open Browser toolbar button');
      const opened = (await api.browserExecute('browser.list')).tabs[0];
      check(opened.id, 'Open Browser returns a real browser tab');
      await waitFor(() => !document.querySelector('#browser-address').disabled,
        'manual browser address bar is ready');
      check(document.activeElement === document.querySelector('#browser-address') &&
        document.querySelector('#browser-address').value === ${JSON.stringify(url)},
        'Open Browser focuses the address bar with the home URL');
      const read = await waitFor(async () => {
        const value = await api.browserExecute('browser.read', { tab: opened.id, selector: 'main' });
        return value.text.includes('Rendered fixture') ? value : null;
      }, 'rendered DOM text');
      check(read.title === 'Browser Fixture', 'browser.read returns live title');
      const address = document.querySelector('#browser-address');
      await waitFor(() => !address.disabled, 'address bar enables for the active browser tab');
      check(!document.querySelector('#browser-go').disabled, 'Go button enables for the active browser tab');
      const toolbarBounds = document.querySelector('.browser-toolbar').getBoundingClientRect();
      const addressBounds = address.getBoundingClientRect();
      const deviceBounds = document.querySelector('#browser-device').getBoundingClientRect();
      check(addressBounds.width >= 80 && addressBounds.right <= toolbarBounds.right + 1 &&
        deviceBounds.right <= toolbarBounds.right + 1, 'address bar and viewport selector stay inside the toolbar');
      const scheme = document.querySelector('#browser-scheme');
      check(scheme.getAttribute('role') === 'switch' && scheme.getAttribute('aria-checked') === 'true' &&
        scheme.textContent === 'Dark', 'browser color scheme defaults to Dark');
      const darkMedia = await api.browserExecute('browser.eval', {
        tab: opened.id, script: 'matchMedia("(prefers-color-scheme: dark)").matches',
      });
      check(darkMedia.result === true, 'Dark switch reaches prefers-color-scheme');
      const darkBackground = await api.browserExecute('browser.eval', {
        tab: opened.id, script: 'getComputedStyle(document.body).backgroundColor',
      });
      check(darkBackground.result === 'rgb(17, 17, 17)', 'Dark switch changes the rendered page colors: ' + JSON.stringify(darkBackground));
      scheme.click();
      await waitFor(async () => {
        const value = await api.browserExecute('browser.eval', {
          tab: opened.id, script: 'matchMedia("(prefers-color-scheme: light)").matches',
        });
        return value.result === true && scheme.textContent === 'Light' ? value : null;
      }, 'Light switch updates the page and toolbar');
      const lightBackground = await api.browserExecute('browser.eval', {
        tab: opened.id, script: 'getComputedStyle(document.body).backgroundColor',
      });
      check(lightBackground.result === 'rgb(250, 250, 250)', 'Light switch changes the rendered page colors: ' + JSON.stringify(lightBackground));
      scheme.click();
      await waitFor(async () => {
        const value = await api.browserExecute('browser.eval', {
          tab: opened.id, script: 'matchMedia("(prefers-color-scheme: dark)").matches',
        });
        return value.result === true && scheme.textContent === 'Dark' ? value : null;
      }, 'switch returns to Dark');
      address.value = ${JSON.stringify(`${url}second`)};
      address.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true, cancelable: true }));
      await waitFor(async () => {
        const value = await api.browserExecute('browser.read', { tab: opened.id });
        return value.title === 'Second Page' ? value : null;
      }, 'address bar Enter navigation');
      const secondDark = await api.browserExecute('browser.eval', {
        tab: opened.id, script: 'matchMedia("(prefers-color-scheme: dark)").matches',
      });
      check(secondDark.result === true, 'Dark emulation survives address navigation');
      await api.browserExecute('browser.navigate', { tab: opened.id, url: ${JSON.stringify(`${url}light-only`)} });
      await waitFor(async () => {
        const value = await api.browserExecute('browser.read', { tab: opened.id });
        return value.text.includes('Scheme header: dark') ? value : null;
      }, 'Dark switch sends the color-scheme client hint');
      scheme.click();
      await waitFor(() => scheme.textContent === 'Light', 'Light switch applies before reload');
      await api.browserExecute('browser.navigate', { tab: opened.id, url: ${JSON.stringify(`${url}light-only`)} });
      await waitFor(async () => {
        const value = await api.browserExecute('browser.read', { tab: opened.id });
        return value.text.includes('Scheme header: light') ? value : null;
      }, 'Light switch sends the color-scheme client hint');
      const emulationErrors = await api.browserExecute('browser.console', { tab: opened.id });
      check(!emulationErrors.entries.some((entry) => entry.text.includes('Color-scheme emulation failed')),
        'Chromium auto-darkening commands are accepted');
      scheme.click();
      await waitFor(async () => {
        const value = await api.browserExecute('browser.eval', {
          tab: opened.id, script: 'matchMedia("(prefers-color-scheme: dark)").matches',
        });
        return value.result === true && scheme.textContent === 'Dark' ? value : null;
      }, 'switch returns to Dark after light-only check');
      await api.browserExecute('browser.navigate', { tab: opened.id, url: ${JSON.stringify(url)} });
      await api.browserExecute('browser.navigate', { tab: opened.id, url: ${JSON.stringify(`${url}second`)} });
      document.querySelector('#browser-back').click();
      await waitFor(async () => {
        const value = await api.browserExecute('browser.read', { tab: opened.id });
        return value.title === 'Browser Fixture' ? value : null;
      }, 'browser Back toolbar navigation');
      address.value = ${JSON.stringify(`${url}second`)};
      document.querySelector('#browser-go').click();
      await waitFor(async () => {
        const value = await api.browserExecute('browser.read', { tab: opened.id });
        return value.title === 'Second Page' ? value : null;
      }, 'Go button navigation');
      document.querySelector('#browser-back').click();
      await waitFor(async () => {
        const value = await api.browserExecute('browser.read', { tab: opened.id });
        return value.title === 'Browser Fixture' ? value : null;
      }, 'return to form fixture after toolbar navigation');
      const evaluated = await api.browserExecute('browser.eval', {
        tab: opened.id, script: 'document.querySelector("h1").textContent',
      });
      check(evaluated.result === 'Rendered fixture', 'browser.eval reads DOM state');
      const statements = await api.browserExecute('browser.eval', {
        tab: opened.id,
        script: 'const heading = document.querySelector("h1").textContent; const labels = document.querySelectorAll("label").length; ({heading, labels})',
      });
      check(statements.result?.heading === 'Rendered fixture' && statements.result?.labels === 1,
        'browser.eval executes multi-statement scripts');
      const promised = await api.browserExecute('browser.eval', {
        tab: opened.id,
        script: 'new Promise((resolve) => setTimeout(() => resolve("async-browser-result"), 20))',
      });
      check(promised.result === 'async-browser-result', 'browser.eval awaits returned Promises');
      let exceptionSeen = false;
      try {
        await api.browserExecute('browser.eval', { tab: opened.id, script: 'document.querySelector("#missing").textContent = "bad"' });
      } catch (error) {
        exceptionSeen = error.message.includes('Cannot set properties of null');
      }
      check(exceptionSeen, 'browser.eval returns actual page exception details');
      await api.browserExecute('browser.eval', {
        tab: opened.id,
        script: '(() => { document.querySelector("#name").value = "filled"; return document.querySelector("#name").value; })()',
      });
      const links = await api.browserExecute('browser.links', { tab: opened.id });
      check(links.links.length === 1 && links.links[0].href.endsWith('/second'), 'browser.links lists real links');
      const consoleResult = await api.browserExecute('browser.console', { tab: opened.id });
      check(consoleResult.entries.some((entry) => entry.text.includes('fixture-console-error')), 'browser.console captures page errors');
      await api.browserExecute('browser.upload', { tab: opened.id, path: ${JSON.stringify(upload)}, selector: '#upload' });
      const uploaded = await api.browserExecute('browser.eval', {
        tab: opened.id, script: 'document.body.dataset.upload',
      });
      check(uploaded.result === 'upload-fixture.txt', 'browser.upload sets a native file input');
      await api.browserExecute('browser.eval', { tab: opened.id, script: 'window.open("/second")' });
      await waitFor(async () => (await api.browserExecute('browser.list')).tabs.length === 2, 'popup browser tab');
      const listed = await api.browserExecute('browser.list');
      check(listed.tabs.length === 2, 'browser.list includes popup tabs');
      for (const tab of listed.tabs) await api.browserExecute('browser.close', { tab: tab.id });
      const preview = await api.previewFile(${JSON.stringify(root)}, 'preview.md', 'markdown');
      const previewText = await waitFor(async () => {
        const value = await api.browserExecute('browser.read', { tab: preview.id });
        return value.text.includes('Preview Fixture') ? value : null;
      }, 'Markdown preview');
      check(previewText.text.includes('Safe Markdown.'), 'Markdown preview renders sanitized content');
      await api.browserExecute('browser.close', { tab: preview.id });
      check((await api.browserExecute('browser.list')).tabs.length === 0, 'browser.close removes every native view');
      return { checks };
    })()`, true);
    await window.webContents.executeJavaScript("document.querySelector('#open-browser').click()", true);
    await waitForBrowserCount(window, 1);
    await sendControlW(window);
    await waitForBrowserCount(window, 0);
    if (window.isDestroyed()) throw new Error('Ctrl+W destroyed the window from an active browser tab.');
    result.checks.push({ name: 'Ctrl+W closes only the active browser tab', passed: true });
    console.log(JSON.stringify({ status: 'success', browser: result }, null, 2));
    return 0;
  } finally {
    await new Promise((resolve) => server.close(resolve));
  }
}
