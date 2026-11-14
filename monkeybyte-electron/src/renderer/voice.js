// SPDX-License-Identifier: BSL-1.0

export function normalizationGain(samples, targetRms = 0.12, maximumGain = 4) {
  if (!samples.length) return 1;
  let energy = 0;
  for (const sample of samples) energy += sample * sample;
  const rms = Math.sqrt(energy / samples.length);
  return rms > 0 ? Math.min(maximumGain, targetRms / rms) : 1;
}

export function drainSpeechSegments(text) {
  let remainder = text;
  const segments = [];
  while (remainder.length) {
    const sentence = /[.!?](?:["'”’)\]]+)?\s+/.exec(remainder);
    const paragraph = /\r?\n\r?\n/.exec(remainder);
    let end = -1;
    if (sentence) end = sentence.index + sentence[0].length;
    if (paragraph && (end < 0 || paragraph.index + paragraph[0].length < end)) {
      end = paragraph.index + paragraph[0].length;
    }
    if (end < 0) break;
    const segment = remainder.slice(0, end).trim();
    if (segment) segments.push(segment);
    remainder = remainder.slice(end);
  }
  return { segments, remainder };
}

function wav16(buffer) {
  const channels = buffer.numberOfChannels;
  const frames = buffer.length;
  const bytes = new ArrayBuffer(44 + frames * channels * 2);
  const view = new DataView(bytes);
  const write = (offset, text) => { for (let i = 0; i < text.length; ++i) view.setUint8(offset + i, text.charCodeAt(i)); };
  write(0, 'RIFF'); view.setUint32(4, 36 + frames * channels * 2, true); write(8, 'WAVE');
  write(12, 'fmt '); view.setUint32(16, 16, true); view.setUint16(20, 1, true);
  view.setUint16(22, channels, true); view.setUint32(24, buffer.sampleRate, true);
  view.setUint32(28, buffer.sampleRate * channels * 2, true); view.setUint16(32, channels * 2, true);
  view.setUint16(34, 16, true); write(36, 'data'); view.setUint32(40, frames * channels * 2, true);
  let offset = 44;
  for (let frame = 0; frame < frames; ++frame) {
    for (let channel = 0; channel < channels; ++channel) {
      const sample = Math.max(-1, Math.min(1, buffer.getChannelData(channel)[frame]));
      view.setInt16(offset, sample < 0 ? sample * 0x8000 : sample * 0x7fff, true); offset += 2;
    }
  }
  return bytes;
}

async function normalizedWav(bytes, mime) {
  const context = new AudioContext();
  try {
    const source = await context.decodeAudioData(bytes.slice(0));
    const first = source.getChannelData(0);
    const gain = normalizationGain(first);
    const ceiling = 0.98;
    const lookahead = Math.max(1, Math.round(source.sampleRate * 0.005));
    const release = Math.pow(0.5, 1 / Math.max(1, Math.round(source.sampleRate * 0.08)));
    const decay = Math.pow(0.5, 1 / lookahead);
    const envelope = new Float32Array(source.length);
    const required = new Float32Array(source.length);
    for (let frame = 0; frame < source.length; ++frame) {
      let peak = 0;
      for (let channel = 0; channel < source.numberOfChannels; ++channel) {
        source.getChannelData(channel)[frame] *= gain;
        peak = Math.max(peak, Math.abs(source.getChannelData(channel)[frame]));
      }
      required[frame] = peak > ceiling ? ceiling / peak : 1;
    }
    let future = 1;
    for (let frame = source.length - 1; frame >= 0; --frame) {
      future = Math.max(required[frame], future * decay); envelope[frame] = future;
    }
    let smooth = 1;
    for (let frame = 0; frame < source.length; ++frame) {
      smooth = envelope[frame] < smooth ? envelope[frame] : release * smooth + (1 - release) * envelope[frame];
      for (let channel = 0; channel < source.numberOfChannels; ++channel) source.getChannelData(channel)[frame] *= smooth;
    }
    return new Blob([wav16(source)], { type: 'audio/wav' });
  } finally { await context.close(); }
}

export async function recordingWav(bytes, mime) {
  const context = new AudioContext();
  let decoded;
  try {
    decoded = await context.decodeAudioData(bytes.slice(0));
  } finally {
    await context.close();
  }
  const sampleRate = 16000;
  const frames = Math.max(1, Math.ceil(decoded.duration * sampleRate));
  const offline = new OfflineAudioContext(1, frames, sampleRate);
  const source = offline.createBufferSource();
  source.buffer = decoded;
  source.connect(offline.destination);
  source.start();
  const rendered = await offline.startRendering();
  return new Blob([wav16(rendered)], { type: 'audio/wav' });
}

export function createNarration(request, onError, onPlayed = () => {}, onDebug = () => {}) {
  let generation = 0;
  let context = null;   // §AUDIO-CLOCK 2026-11-14: one shared AudioContext;
  let master = null;    // rendering runs on Chromium's real-time audio thread,
  let cursor = 0;       // so scheduled audio plays gaplessly no matter how
  const live = new Set(); // busy the renderer main thread is with chat UI.
  let burst = null;     // §PREBUFFER: first segment after silence, held until
  let inflight = 0;     // §TTS-CAP: the chimpflow service serializes requests;
  const waiters = [];   // flooding it with 8 concurrent synths made every one
                        // take 6-12s. Two in flight, the rest queue FIFO here.
  async function acquireSynth() {
    if (inflight < 2) { inflight += 1; return; }
    await new Promise((resolve) => waiters.push(resolve));
    inflight += 1;
  }
  function releaseSynth() {
    inflight -= 1;
    const next = waiters.shift();
    if (next) next();
  }
                        // the second is ready (or cap/turn-end) so playback
                        // never opens with a thinking-gap hole.

  // §PIPELINE 2026-11-14: synthesis (TTS request + decode/normalize) starts
  // the moment speak() is called and runs in parallel; ordering is imposed
  // by the audio-clock cursor at schedule time, not by a promise queue.
  async function prepare(text, ticket, options) {
    if (!text.trim() || ticket !== generation) return null;
    onDebug(`tts synth start "${text.slice(0, 50)}"`);
    const started = performance.now();
    await acquireSynth();
    let result = null;
    try {
      result = await request(text, options);
    } finally {
      releaseSynth();
    }
    onDebug(`tts synth done "${text.slice(0, 50)}" in ${Math.round(performance.now() - started)}ms`);
    if (ticket !== generation) return null;
    const bytes = Uint8Array.from(atob(result.base64), (character) => character.charCodeAt(0));
    return normalizedWav(bytes.buffer, result.mime);
  }

  function ensureGraph() {
    if (!context) {
      context = new AudioContext();
      master = context.createGain();
      master.connect(context.destination);
      cursor = context.currentTime;
    }
    if (context.state === 'suspended') {
      // Chromium auto-suspends idle contexts; playback scheduled onto a
      // frozen clock is silent, so this must never go unnoticed.
      onDebug('tts WARNING: audio context suspended — resuming');
      context.resume().catch(() => {});
    }
    context.addEventListener?.('statechange', () => onDebug(`tts audio state → ${context.state}`));
    return context;
  }

  function startSource(item, ctx) {
    const startAt = Math.max(ctx.currentTime, cursor);
    cursor = startAt + item.buffer.duration;
    live.add(item.source);
    item.source.onended = () => live.delete(item.source);
    item.source.start(startAt);
    onDebug(`tts play start ${item.label} at=${startAt.toFixed(2)} dur=${item.buffer.duration.toFixed(2)}`);
  }

  async function play(prepared, ticket, label = '') {
    let clip = null;
    try {
      clip = await prepared;
    } catch (error) {
      // A cancelled segment can still fail in flight (abort, shutdown);
      // only fresh-segment failures are worth surfacing.
      if (ticket === generation) throw error;
      return false;
    }
    if (!clip || ticket !== generation) return false;
    const ctx = ensureGraph();
    const bytes = await clip.arrayBuffer();
    const buffer = await ctx.decodeAudioData(bytes);
    if (ticket !== generation) return false;
    const source = ctx.createBufferSource();
    source.buffer = buffer;
    source.connect(master);
    // §PREBUFFER: the first segment after silence holds briefly so the next
    // can catch up; released by the second segment's readiness, the cap
    // timer, turn end (release()), or cancel. Everything else chains on the
    // audio clock — each segment starts exactly when the previous ends, so
    // late 'ended' delivery under UI jank can never open playback gaps.
    const item = { source, buffer, ticket, label };
    if (burst?.first) {
      clearTimeout(burst.timer);
      startSource(burst.first, ctx);
      burst = null;
      startSource(item, ctx);
    } else if (!live.size && cursor <= ctx.currentTime) {
      const timer = setTimeout(() => {
        if (burst?.first) startSource(burst.first, ensureGraph());
        burst = null;
      }, 5000);
      timer.unref?.();
      burst = { first: item, timer };
      onDebug(`tts burst hold ${label}`);
    } else {
      startSource(item, ctx);
    }
    const playStarted = performance.now();
    await new Promise((resolve) => source.addEventListener('ended', resolve, { once: true }));
    onDebug(`tts play end ${label} after ${Math.round(performance.now() - playStarted)}ms`);
    return ticket === generation;
  }

  return {
    speak(text, options = undefined) {
      const ticket = generation;
      const prepared = prepare(text, ticket, options);
      onDebug(`tts speak queued "${text.slice(0, 50)}"`);
      const done = play(prepared, ticket, `"${text.slice(0, 50)}"`);
      done.then((played) => { if (played) onPlayed(); })
        .catch((error) => onError(error));
      return done;
    },
    release() {
      // Turn end: speak whatever is held immediately — a one-line reply
      // should not wait out the prebuffer cap for a sibling that will
      // never come.
      if (burst?.first) {
        clearTimeout(burst.timer);
        startSource(burst.first, ensureGraph());
        burst = null;
        onDebug('tts burst released at turn end');
      }
    },
    cancel() {
      generation += 1;
      for (const source of live) {
        try { source.stop(); } catch { /* already ended or never started */ }
      }
      live.clear();
      if (burst?.first) {
        clearTimeout(burst.timer);
        try { burst.first.source.start(); burst.first.source.stop(); } catch { /* resolve its waiter */ }
        burst = null;
      }
      if (context) cursor = context.currentTime;
      onDebug('tts cancel: clock reset');
    },
  };
}
