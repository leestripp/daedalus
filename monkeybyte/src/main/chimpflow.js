// SPDX-License-Identifier: BSL-1.0

function endpoint(config) {
  const base = new URL(config.serverUrl || 'http://127.0.0.1');
  if (!['http:', 'https:'].includes(base.protocol) || base.pathname !== '/' || base.search || base.hash) {
    throw new Error('ChimpFlow server URL must be an HTTP(S) origin without a path.');
  }
  base.port = String(config.port || 8090);
  return base;
}

export function createChimpFlowService(backend) {
  async function config(overrides = null) {
    const value = await backend.request('/v1/chimpflow/config');
    const allowed = {};
    for (const key of ['enabled', 'serverUrl', 'port', 'ttsModel', 'sttModel', 'voice']) {
      if (overrides && key in overrides) allowed[key] = overrides[key];
    }
    return { ...value, ...allowed,
      apiKey: overrides?.apiKey || value.apiKey || process.env.CHIMPFLOW_KEY || '' };
  }

  async function request(path, { method = 'GET', body, headers = {}, timeout = 120000,
    overrides = null, requireEnabled = true, query = null } = {}) {
    const settings = await config(overrides);
    if (requireEnabled && !settings.enabled) throw new Error('Enable ChimpFlow in Settings first.');
    const url = endpoint(settings); url.pathname = path;
    if (query) url.search = new URLSearchParams(query).toString();
    const response = await fetch(url, {
      method, body, signal: AbortSignal.timeout(timeout),
      headers: { ...headers, ...(settings.apiKey ? { Authorization: `Bearer ${settings.apiKey}` } : {}) },
    });
    if (!response.ok) throw new Error(`ChimpFlow HTTP ${response.status}: ${(await response.text()).slice(0, 1000)}`);
    return response;
  }

  return {
    async models(overrides = null) {
      const response = await request('/v1/models', { overrides, requireEnabled: false });
      const payload = await response.json();
      const models = Array.isArray(payload) ? payload : payload?.models ?? payload?.data;
      if (!Array.isArray(models)) throw new Error('ChimpFlow returned an invalid model catalog.');
      return { models };
    },
    async voices(overrides = null) {
      const settings = await config(overrides);
      const response = await request('/v1/voices', { overrides, requireEnabled: false,
        query: { model: settings.ttsModel || 'kokoro-82m' } });
      const payload = await response.json();
      const voices = Array.isArray(payload) ? payload : payload?.voices ?? payload?.data;
      if (!Array.isArray(voices)) throw new Error('ChimpFlow returned an invalid voice catalog.');
      return { voices };
    },
    async speak(text, overrides = null) {
      if (typeof text !== 'string' || !text.trim() || text.length > 100000) throw new Error('Invalid speech text.');
      const settings = await config(overrides);
      const response = await request('/v1/audio/speech', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        overrides, requireEnabled: !overrides,
        body: JSON.stringify({ model: settings.ttsModel || 'kokoro-82m', voice: settings.voice || undefined,
          input: text, response_format: 'mp3' }),
      });
      const bytes = Buffer.from(await response.arrayBuffer());
      return { mime: response.headers.get('content-type') || 'audio/mpeg', base64: bytes.toString('base64') };
    },
    async transcribe(base64, mime = 'audio/webm') {
      if (typeof base64 !== 'string' || base64.length > 40 * 1024 * 1024) throw new Error('Dictation is too large.');
      const settings = await config();
      const bytes = Buffer.from(base64, 'base64');
      const form = new FormData();
      const extension = mime.includes('wav') ? 'wav' : mime.includes('mpeg') ? 'mp3' : 'webm';
      form.append('file', new Blob([bytes], { type: mime }), `dictation.${extension}`);
      form.append('model', settings.sttModel || 'whisper');
      const response = await request('/v1/audio/transcriptions', { method: 'POST', body: form });
      return response.json();
    },
  };
}
