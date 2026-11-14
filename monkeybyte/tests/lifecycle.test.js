// SPDX-License-Identifier: BSL-1.0
import assert from 'node:assert/strict';
import http from 'node:http';
import { test } from 'node:test';
import { createConnection } from 'node:net';

import { closeHttpServer, within } from '../src/main/lifecycle.js';

test('within returns the fallback when an operation never settles', async () => {
  const started = Date.now();
  const result = await within(new Promise(() => {}), 20, 'timed-out');
  assert.equal(result, 'timed-out');
  assert.ok(Date.now() - started < 500);
});

test('within preserves successful values and rejections', async () => {
  assert.equal(await within(Promise.resolve(42), 1000), 42);
  await assert.rejects(within(Promise.reject(new Error('expected')), 1000), /expected/);
});

test('closeHttpServer destroys a stuck keep-alive connection and returns', async () => {
  const server = http.createServer(() => {
    // Deliberately leave the response open.
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const socket = await new Promise((resolve, reject) => {
    const client = createConnection({ host: '127.0.0.1', port: server.address().port }, () => {
      client.write('GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n');
      resolve(client);
    });
    client.on('error', reject);
  });
  const started = Date.now();
  assert.equal(await closeHttpServer(server, 1000), true);
  assert.ok(Date.now() - started < 1000);
  socket.destroy();
});

