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
  let queue = Promise.resolve();
  let generation = 0;
  let audio = null;

  // §PIPELINE 2026-11-14: synthesis (TTS request + decode/normalize) starts
  // the moment speak() is called and runs in parallel with whatever is
  // playing; only playback is serialized. Synthesizing inside the queue made
  // every sentence pay full TTS+decode latency as a dead gap after the
  // previous segment ended.
  async function prepare(text, ticket, options) {
    if (!text.trim() || ticket !== generation) return null;
    onDebug(`tts synth start "${text.slice(0, 50)}"`);
    const started = performance.now();
    const result = await request(text, options);
    onDebug(`tts synth done "${text.slice(0, 50)}" in ${Math.round(performance.now() - started)}ms`);
    if (ticket !== generation) return null;
    const bytes = Uint8Array.from(atob(result.base64), (character) => character.charCodeAt(0));
    return normalizedWav(bytes.buffer, result.mime);
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
    onDebug(`tts play start ${label}`);
    const playStarted = performance.now();
    const url = URL.createObjectURL(clip);
    const player = new Audio(url);
    audio?.pause(); audio = player;
    player.addEventListener('ended', () => URL.revokeObjectURL(url), { once: true });
    // §ENDED-RACE 2026-11-14: attach completion listeners BEFORE play() —
    // a short clip can fire 'ended' during play()'s task-rounds, before a
    // post-await attach would see it, parking the queue forever (only
    // cancel() ever unstuck it: "next sentence plays when I hit Stop").
    await new Promise((resolve) => {
      player.addEventListener('ended', resolve, { once: true });
      player.addEventListener('error', resolve, { once: true });
      player.addEventListener('pause', resolve, { once: true });
      try { Promise.resolve(player.play()).catch(resolve); }
      catch { resolve(); }  // stubbed/legacy play() may throw synchronously
      if (player.ended) resolve();   // already finished while we were attaching
    });
    if (ticket !== generation) return false;
    // §PLAY-TO-END 2026-11-14: hold the queue until this segment actually
    // finishes. audio.play() resolves at playback START, so chaining on it
    // let the next sentence pause() this one mid-sentence. 'pause'/'error'
    // are the interruption exits (cancel() pauses), checked via the
    // generation ticket / current-player identity below.
    await new Promise((resolve) => {
      player.addEventListener('ended', resolve, { once: true });
      player.addEventListener('error', resolve, { once: true });
      player.addEventListener('pause', resolve, { once: true });
    });
    onDebug(`tts play end ${label} after ${Math.round(performance.now() - playStarted)}ms`);
    if (ticket !== generation || audio !== player) return false;
    return true;
  }

  return {
    speak(text, options = undefined) {
      const ticket = generation;
      const prepared = prepare(text, ticket, options);
      onDebug(`tts speak queued "${text.slice(0, 50)}"`);
      queue = queue.then(() => play(prepared, ticket, `"${text.slice(0, 50)}"`))
        .then((played) => { if (played) onPlayed(); })
        .catch((error) => onError(error));
      return queue;
    },
    cancel() {
      onDebug('tts cancel');
      generation += 1; audio?.pause(); audio = null;
      queue = Promise.resolve();
      onDebug('tts cancel: queue reset');
    },
  };
}
