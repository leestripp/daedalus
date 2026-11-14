// SPDX-License-Identifier: BSL-1.0
import assert from 'node:assert/strict';
import test from 'node:test';
import { createChimpFlowService } from '../src/main/chimpflow.js';
import { createNarration, drainSpeechSegments, normalizationGain } from '../src/renderer/voice.js';

function voiceServiceReturning(payload) {
  const backend = { request: async () => ({ enabled: true, serverUrl: 'http://127.0.0.1', port: 8090 }) };
  const originalFetch = globalThis.fetch;
  globalThis.fetch = async () => new Response(JSON.stringify(payload), {
    status: 200, headers: { 'Content-Type': 'application/json' },
  });
  return { service: createChimpFlowService(backend), restore: () => { globalThis.fetch = originalFetch; } };
}

test('ChimpFlow discovery normalizes the OpenAI-compatible data envelope', async () => {
  const { service, restore } = voiceServiceReturning({ object: 'list', data: [{ id: 'alba' }] });
  try {
    assert.deepEqual(await service.voices(), { voices: [{ id: 'alba' }] });
  } finally {
    restore();
  }
});

test('ChimpFlow discovery preserves legacy voice catalogs and rejects invalid payloads', async () => {
  const legacy = voiceServiceReturning({ voices: [{ id: 'fixture-voice' }] });
  try {
    assert.deepEqual(await legacy.service.voices(), { voices: [{ id: 'fixture-voice' }] });
  } finally {
    legacy.restore();
  }
  const invalid = voiceServiceReturning({ unexpected: [] });
  try {
    await assert.rejects(invalid.service.voices(), /invalid voice catalog/);
  } finally {
    invalid.restore();
  }
});

test('ChimpFlow discovery uses the current form key before settings are saved', async () => {
  const backend = { request: async () => ({ enabled: false, serverUrl: 'http://127.0.0.1', port: 8090, apiKey: 'old-key' }) };
  const originalFetch = globalThis.fetch;
  let authorization = '';
  let requestUrl = '';
  globalThis.fetch = async (url, options) => {
    requestUrl = String(url);
    authorization = options.headers.Authorization;
    return new Response(JSON.stringify({ data: [{ id: 'alba' }] }), { status: 200 });
  };
  try {
    const service = createChimpFlowService(backend);
    await service.voices({ enabled: false, apiKey: 'new-key', port: 9000 });
    assert.equal(authorization, 'Bearer new-key');
    assert.equal(requestUrl, 'http://127.0.0.1:9000/v1/voices?model=kokoro-82m');
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test('ChimpFlow model discovery returns runtime choices and voices are model-scoped', async () => {
  const backend = { request: async () => ({ enabled: true, serverUrl: 'http://127.0.0.1', port: 8090 }) };
  const originalFetch = globalThis.fetch;
  const urls = [];
  globalThis.fetch = async (url) => {
    urls.push(String(url));
    const payload = String(url).endsWith('/v1/models')
      ? { data: [{ id: 'kokoro-82m' }, { id: 'chatterbox' }, { id: 'pocket-tts' }] }
      : { data: [{ id: 'af_heart' }] };
    return new Response(JSON.stringify(payload), { status: 200 });
  };
  try {
    const service = createChimpFlowService(backend);
    const catalog = await service.models();
    assert.equal(catalog.models[0].id, 'kokoro-82m');
    assert.equal(catalog.models.length, 3);
    await service.voices({ ttsModel: 'chatterbox' });
    assert.equal(urls.at(-1), 'http://127.0.0.1:8090/v1/voices?model=chatterbox');
  } finally {
    globalThis.fetch = originalFetch;
  }
});


async function flush(times = 20) {
  for (let index = 0; index < times; ++index) await new Promise((resolve) => setImmediate(resolve));
}

function stubPlaybackEnvironment() {
  const players = [];
  class FakeAudio {
    constructor() { this.listeners = {}; players.push(this); }
    addEventListener(type, listener) { (this.listeners[type] ??= []).push(listener); }
    emit(type) { for (const listener of this.listeners[type] ?? []) listener(); }
    async play() {}
    pause() { this.emit('pause'); }
  }
  const sample = new Float32Array(64).fill(0.05);
  class FakeAudioContext {
    async decodeAudioData() {
      return { numberOfChannels: 1, length: sample.length, sampleRate: 22050, getChannelData: () => sample };
    }
    async close() {}
  }
  const originals = {
    audio: globalThis.Audio,
    context: globalThis.AudioContext,
    createObjectURL: URL.createObjectURL,
    revokeObjectURL: URL.revokeObjectURL,
  };
  globalThis.Audio = FakeAudio;
  globalThis.AudioContext = FakeAudioContext;
  URL.createObjectURL = () => 'blob:fake';
  URL.revokeObjectURL = () => {};
  return {
    players,
    restore() {
      globalThis.Audio = originals.audio;
      globalThis.AudioContext = originals.context;
      URL.createObjectURL = originals.createObjectURL;
      URL.revokeObjectURL = originals.revokeObjectURL;
    },
  };
}

test('narration holds the queue until each segment finishes playing', async () => {
  const environment = stubPlaybackEnvironment();
  try {
    const narration = createNarration(async () => ({ base64: btoa('fake-bytes'), mime: 'audio/wav' }));
    const first = narration.speak('First sentence.');
    const second = narration.speak('Second sentence.');
    await flush();
    // §PLAY-TO-END 2026-11-14: only one player may exist while the first
    // segment is still playing; the old start-chained queue created both.
    assert.equal(environment.players.length, 1);
    environment.players[0].emit('ended');
    await first;  // play() returned → queue advanced to segment 2
    await flush();
    assert.equal(environment.players.length, 2);
    environment.players[1].emit('ended');
    await second;
  } finally {
    environment.restore();
  }
});

test('cancel interrupts the current segment without counting it as played', async () => {
  const environment = stubPlaybackEnvironment();
  try {
    let played = 0;
    const narration = createNarration(
      async () => ({ base64: btoa('fake-bytes'), mime: 'audio/wav' }),
      () => {},
      () => { played += 1; },
    );
    const first = narration.speak('First sentence.');
    const second = narration.speak('Second sentence.');
    await flush();
    narration.cancel();
    await first;
    await second;
    assert.equal(played, 0);
  } finally {
    environment.restore();
  }
});

test('speech segments drain sentences and preserve the final remainder', () => {
  const result = drainSpeechSegments('First sentence. Second sentence!\n\nFinal incomplete');
  assert.deepEqual(result.segments, ['First sentence.', 'Second sentence!']);
  assert.equal(result.remainder, 'Final incomplete');
});

test('speech segmentation emits paragraph-only text and does not repeat it', () => {
  const first = drainSpeechSegments('A complete paragraph\n\nnext');
  assert.deepEqual(first.segments, ['A complete paragraph']);
  assert.equal(first.remainder, 'next');
  assert.deepEqual(drainSpeechSegments(first.remainder).segments, []);
});

test('RMS gain honors the 0.12 target and four-times cap', () => {
  assert.equal(normalizationGain([0.06, -0.06]), 2);
  assert.equal(normalizationGain([0.001, -0.001]), 4);
  assert.equal(normalizationGain([]), 1);
  assert.equal(normalizationGain([0, 0]), 1);
});
