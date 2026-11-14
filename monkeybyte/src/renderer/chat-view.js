// SPDX-License-Identifier: BSL-1.0
import { byId, errorMessage } from './dom.js';
import { createNarration, drainSpeechSegments, recordingWav } from './voice.js';

function textElement(tag, className, text) {
  const element = document.createElement(tag);
  if (className) element.className = className;
  element.textContent = text;
  return element;
}

function inlineMarkdown(container, text) {
  const pattern = /(`[^`]+`|\*\*[^*]+\*\*)/g;
  let cursor = 0;
  for (const match of text.matchAll(pattern)) {
    if (match.index > cursor) container.append(document.createTextNode(text.slice(cursor, match.index)));
    const token = match[0];
    const element = document.createElement(token.startsWith('`') ? 'code' : 'strong');
    element.textContent = token.startsWith('`') ? token.slice(1, -1) : token.slice(2, -2);
    container.append(element);
    cursor = match.index + token.length;
  }
  if (cursor < text.length) container.append(document.createTextNode(text.slice(cursor)));
}

function appendMarkdown(container, markdown) {
  container.replaceChildren();
  let paragraph = [];
  let code = null;
  const flush = () => {
    if (!paragraph.length) return;
    const p = document.createElement('p'); inlineMarkdown(p, paragraph.join(' ')); container.append(p); paragraph = [];
  };
  for (const line of String(markdown).split(/\r?\n/)) {
    if (line.startsWith('```')) {
      flush();
      if (code !== null) { const pre = textElement('pre', 'chat-code', code); container.append(pre); code = null; }
      else code = '';
      continue;
    }
    if (code !== null) { code += `${line}\n`; continue; }
    const heading = /^(#{1,4})\s+(.+)/.exec(line);
    if (heading) { flush(); container.append(textElement(`h${heading[1].length + 2}`, '', heading[2])); continue; }
    const item = /^\s*[-*]\s+(.+)/.exec(line);
    if (item) { flush(); const p = textElement('p', 'chat-list-item', `• ${item[1]}`); container.append(p); continue; }
    if (!line.trim()) { flush(); continue; }
    paragraph.push(line.trim());
  }
  flush();
  if (code !== null) container.append(textElement('pre', 'chat-code', code));
}

function messageElement(role, content) {
  const section = document.createElement('section');
  const text = String(content);
  // §COMPRESSION-NOTICE 2026-11-14: context-compression summaries are markdown
  // prose that can quote glyphs like ✗/❌ and render giant headings — the
  // "big cross on the compression" report. Show them as a compact, muted,
  // collapsed notice instead of a full message bubble.
  if (text.startsWith('[Context compressed')) {
    section.className = 'message-bubble notice';
    section.dataset.role = 'system';
    const details = document.createElement('details');
    details.className = 'compression-notice';
    details.append(textElement('summary', '',
      `🗜 Context compressed — summary of ${compactTokens(text.length)} chars`));
    details.append(textElement('pre', 'chat-code', text));
    section.append(details);
    return section;
  }
  section.className = `message-bubble ${role === 'user' ? 'user' : 'assistant'}`;
  section.dataset.role = role;
  const body = document.createElement('div');
  body.className = 'message-content';
  appendMarkdown(body, text);
  section.append(body);
  return section;
}

export function compactTokens(value) {
  const number = Number(value) || 0;
  if (number < 1000) return String(number);
  if (number < 10000) return `${(number / 1000).toFixed(1)}k`;
  return `${Math.round(number / 1000)}k`;
}

export function formatAgentStatus(model, stats = {}) {
  let text = model || 'No model';
  if ((stats.iteration ?? 0) > 0 || (stats.totalTokens ?? 0) > 0) {
    text += ` · ${compactTokens(stats.totalTokens)}`;
    if (stats.maxContextSize > 0) {
      const percent = Math.round(100 * stats.totalTokens / stats.maxContextSize);
      text += `/${compactTokens(stats.maxContextSize)} ctx (${percent}%)`;
    } else text += ' tok';
    if (stats.cachedTokens > 0) {
      text += ` · cached ${compactTokens(stats.cachedTokens)}/${compactTokens(stats.promptTokens)}`;
    }
    text += ` · iter ${stats.iteration ?? 0}`;
    if ((stats.completedToolCalls ?? 0) > 0 || (stats.failedToolCalls ?? 0) > 0) {
      text += ` · tools ${stats.completedToolCalls ?? 0}`;
      if (stats.failedToolCalls > 0) text += ` +${stats.failedToolCalls} failed`;
    }
  }
  return text;
}

export function createChatView({ api, getWorkspace, onError }) {
  const elements = {
    messages: byId('chat-messages'), draft: byId('chat-text'), send: byId('send-chat'),
    stop: byId('stop-chat'), steer: byId('steer-chat'), model: byId('model-select'),
    dictate: byId('dictate-chat'),
    thinking: byId('thinking-select'), activity: byId('chat-activity-text'),
    activityRow: byId('chat-activity'),
    error: byId('chat-error'), sessions: byId('session-select'), attachments: byId('chat-attachments'),
  };
  let busy = false;
  let streaming = null;
  let attachments = [];
  let currentWorkspace;
  let currentModel = '';
  let latestStats = {};
  let settings = { models: [] };
  let sessionData = { sessions: [] };
  let loggedBusy = null;
  let loggedState = '';
  let pendingNotice = null;  // §NO-DOUBLE-SPEAK: tell_user likely to stream
  let agentBusy = false;  // §MULTI-BLOCK: latest status busy flag
  let canonicalHistory = [];
  let liveTurn = null;
  const approvals = new Map();
  const toolCards = new Map();
  let speakOnComplete = false;
  let speechBuffer = '';
  let recorder = null;
  let microphone = null;

  function report(error) {
    const text = errorMessage(error);
    elements.error.textContent = text;
    elements.error.hidden = false;
    onError(error);
  }

  function updateVoiceSettings() {
    const voice = settings.chimpflow ?? {};
    const ready = voice.enabled && voice.dictationEnabled && voice.hasApiKey && voice.sttModel;
    elements.dictate.disabled = !ready;
    elements.dictate.title = ready ? 'Start dictation'
      : 'Enable ChimpFlow, dictation, a scoped key and STT model in Settings';
  }

  const narration = createNarration(api.chimpflowSpeak, report, () => {}, debugLog);

  function resetNarration() {
    speechBuffer = ''; pendingNotice = null; narration.cancel();
  }

  // §NO-TTS-WHILE-DICTATING 2026-11-14: TTS playing into an open mic is
  // transcribed by whisper as repeated phrases / [BLANK_AUDIO] junk.
  const dictating = () => recorder?.state === 'recording';

  function narrateDelta(text) {
    const voice = settings.chimpflow ?? {};
    // §MULTI-BLOCK 2026-11-14: narrate whenever the agent is live (armed at
    // send, or any busy period) — turns can carry several assistant text
    // blocks; gating only on the send-armed flag left later blocks silent
    // until a dump at the next turn's flush ("only heard part of it").
    if ((!speakOnComplete && !agentBusy) || !voice.enabled || !voice.readAssistant || dictating()) return;
    speechBuffer += text;
    const drained = drainSpeechSegments(speechBuffer);
    speechBuffer = drained.remainder;
    for (const segment of drained.segments) {
      const plain = messageElement('assistant', segment).textContent;
      narration.speak(plain);
    }
  }

  function flushNarration() {
    const remainder = speechBuffer; speechBuffer = '';
    if (remainder.trim() && !dictating()) narration.speak(messageElement('assistant', remainder).textContent);
    narration.release();  // §PREBUFFER: turn end frees a held first segment
  }

  function scroll() { elements.messages.scrollTop = elements.messages.scrollHeight; }

  function clearMessages() {
    streaming = null;
    elements.messages.replaceChildren();
    approvals.clear();
    toolCards.clear();
  }

  function setHistory(history) {
    clearMessages();
    canonicalHistory = [...history];
    liveTurn = null;
    if (!history.length) {
      const welcome = textElement('section', 'welcome-card', '');
      welcome.append(textElement('h3', '', '👋 Welcome to MonkeyByte!'),
        textElement('p', '', 'Ask the agent to inspect, edit, build, test, or control this IDE.'));
      elements.messages.append(welcome);
    }
    for (const entry of history) elements.messages.append(messageElement(entry.role, entry.content));
    scroll();
  }

  function appendDelta(text) {
    if (busy) setActivity('Streaming…', true);
    if (!streaming) {
      streaming = messageElement('assistant', '');
      streaming.classList.add('streaming');
      elements.messages.append(streaming);
      if (liveTurn) liveTurn.streamed = { element: streaming, text: '' };
    }
    if (liveTurn?.streamed) liveTurn.streamed.text += text;
    const body = streaming.querySelector('.message-content');
    body.textContent += text;
    scroll();
  }

  function toolCard(type, payload) {
    const id = payload.callId ?? '';
    let card = id ? toolCards.get(id) : null;
    if (!card) {
      const element = document.createElement('details');
      element.className = 'tool-card';
      if (id) element.dataset.callId = id;
      element.append(textElement('summary', '', ''), textElement('pre', '', ''));
      elements.messages.append(element);
      card = { element, name: payload.name ?? 'Tool', args: null, result: null, progress: null, done: false };
      if (id) toolCards.set(id, card);
    }
    if (type === 'tool_call') card.args = payload.args ?? {};
    else if (type === 'tool_result') {
      card.result = payload.result ?? {};
      card.done = true;
      card.failed = card.result?.success === false || Boolean(card.result?.error) ||
        (typeof card.result?.status === 'string' && !['success', 'ok'].includes(card.result.status.toLowerCase()));
    }
    else card.progress = payload.progress ?? {};
    const content = { arguments: card.args ?? {} };
    if (card.progress !== null) content.progress = card.progress;
    if (card.done) content.result = card.result;
    card.element.dataset.status = card.done ? (card.failed ? 'error' : 'success') : 'running';
    card.element.querySelector('summary').textContent = `${card.done ? (card.failed ? '✕' : '✓') : '⚙'} ${card.name}`;
    card.element.querySelector('pre').textContent = JSON.stringify(content, null, 2);
    scroll();
  }

  function mergeLiveHistory(history) {
    const turn = liveTurn;
    liveTurn = null;
    canonicalHistory = [...history];
    if (!turn) { setHistory(history); return; }
    const fresh = history.slice(turn.depth);
    const notices = [...turn.notices];
    let streamConsumed = false;
    for (const entry of fresh) {
      if (entry.role !== 'assistant') continue; // user_message events already rendered every prompt
      if (turn.streamed && !streamConsumed && turn.streamed.text === entry.content) {
        appendMarkdown(turn.streamed.element.querySelector('.message-content'), entry.content);
        streamConsumed = true;
        continue;
      }
      const notice = notices.indexOf(entry.content);
      if (notice >= 0) { notices.splice(notice, 1); continue; }
      elements.messages.append(messageElement(entry.role, entry.content));
    }
    if (turn.streamed) turn.streamed.element.classList.remove('streaming');
    streaming = null;
    scroll();
  }

  function approvalCard(payload) {
    const card = document.createElement('section');
    card.className = 'approval-card'; card.dataset.approvalId = payload.id;
    card.append(textElement('strong', '', 'Command approval required'),
      textElement('pre', '', payload.command));
    const row = document.createElement('div'); row.className = 'approval-actions';
    const allow = textElement('button', 'primary', 'Allow'); allow.type = 'button';
    const deny = textElement('button', '', 'Deny'); deny.type = 'button';
    const respond = async (approved) => {
      allow.disabled = deny.disabled = true;
      try { await api.agentRequest('/v1/chat/approval', { method: 'POST', body: { id: payload.id, approved } }); }
      catch (error) { report(error); allow.disabled = deny.disabled = false; }
    };
    allow.addEventListener('click', () => respond(true));
    deny.addEventListener('click', () => respond(false));
    row.append(allow, deny); card.append(row); elements.messages.append(card);
    approvals.set(payload.id, card); scroll();
  }

  function setActivity(text, working) {
    elements.activity.textContent = text;
    elements.activityRow.dataset.busy = String(working);
  }

  function renderAgentStatus() {
    byId('agent-status').textContent = formatAgentStatus(currentModel, latestStats);
  }

  function updateStatus(status) {
    busy = Boolean(status.busy);
    if (busy !== loggedBusy || String(status.state ?? '') !== loggedState) {
      loggedBusy = busy;
      loggedState = String(status.state ?? '');
      debugLog(`status busy=${busy} state=${loggedState || '?'} pending=${status.pending ?? 0}`);
    }
    if (status.stats) latestStats = status.stats;
    if (status.model !== undefined) currentModel = status.model || '';
    renderAgentStatus();
    const activity = busy ? (status.state === 'Compressing' ? 'Compacting…'
      : status.state === 'Cancelling' ? 'Cancelling…' : 'Thinking…')
      : status.state === 'Error' ? 'Error' : 'Ready';
    setActivity(`${activity}${status.pending ? ` · ${status.pending} queued` : ''}`, busy);
    elements.send.textContent = 'Send';
    elements.send.disabled = !currentWorkspace;
    elements.steer.disabled = !currentWorkspace;
    elements.stop.disabled = !busy || !currentWorkspace;
    elements.model.disabled = busy || !settings.models.length;
    elements.thinking.disabled = busy;
    if (status.model) elements.model.value = status.model;
    if (status.thinking) elements.thinking.value = status.thinking;
  }

  function updateSessions(data) {
    sessionData = data;
    const active = data.activeId;
    elements.sessions.replaceChildren();
    for (const session of data.sessions) {
      const option = textElement('option', '', session.name);
      option.value = session.id; option.title = session.workspace;
      elements.sessions.append(option);
    }
    elements.sessions.value = active;
  }

  async function ensure(workspace) {
    currentWorkspace = workspace;
    if (!workspace) {
      settings = { models: [] };
      clearMessages(); setHistory([]);
      elements.model.replaceChildren(textElement('option', '', 'Select a workspace'));
      elements.model.value = 'Select a workspace';
      updateStatus({ busy: false, state: 'Idle', pending: 0 });
      elements.thinking.disabled = true;
      elements.error.hidden = true;
      return;
    }
    try {
      const result = await api.agentRequest('/v1/sessions/ensure', { method: 'POST', body: { workspace } });
      updateSessions(result.sessions); setHistory(result.history); updateStatus(result.status);
      settings = await api.agentRequest('/v1/settings');
      elements.model.replaceChildren();
      for (const model of settings.models) {
        const option = textElement('option', '', model.name); option.value = model.name;
        elements.model.append(option);
      }
      if (!elements.model.value) elements.model.value = settings.defaultModel;
      elements.model.disabled = !settings.models.length;
      elements.thinking.disabled = false;
      updateVoiceSettings();
      elements.error.hidden = true;
    } catch (error) { report(error); }
  }

  // §DEBUG-LOG: fire-and-forget line to the shared agent/app debug log.
  function debugLog(message) {
    api.agentRequest('/v1/debug/log', { method: 'POST', body: { entry: message } }).catch(() => {});
  }

  async function request(path, body = {}) {
    try { return await api.agentRequest(path, { method: 'POST', body }); }
    catch (error) { report(error); throw error; }
  }

  async function newSession() {
    if (!currentWorkspace) return;
    const result = await request('/v1/sessions/create', { workspace: currentWorkspace, name: 'New Chat' });
    updateSessions(result.sessions); setHistory(result.history);
  }

  async function command(text) {
    const [name, ...rest] = text.slice(1).split(/\s+/);
    const argument = rest.join(' ');
    if (name === 'help') {
      elements.messages.append(messageElement('assistant', 'Commands: `/new`, `/rename NAME`, `/clear`, `/model NAME`, `/thinking LEVEL`, `/stop`, `/sessions`, `/help`')); scroll(); return true;
    }
    if (name === 'new') { await newSession(); return true; }
    if (name === 'stop') { await request('/v1/chat/cancel'); return true; }
    if (name === 'clear') { await request('/v1/chat/clear'); setHistory([]); return true; }
    if (name === 'rename' && argument) { const id = sessionData.activeId;
      const result = await request('/v1/sessions/rename', { id, name: argument }); updateSessions(result.sessions); return true; }
    if (name === 'model' && argument) { await request('/v1/models/select', { model: argument }); return true; }
    if (name === 'thinking' && argument) { await request('/v1/models/thinking', { level: argument }); return true; }
    if (name === 'sessions') { byId('session-select').focus(); return true; }
    return false;
  }

  async function send(steer = false) {
    const text = elements.draft.value.trim();
    if (!text && !attachments.length) return;
    elements.draft.value = '';
    if (text.startsWith('/') && !steer && await command(text)) return;
    debugLog(`send workspace=${currentWorkspace ?? '?'} chars=${text.length}`);
    const sentAttachments = attachments;
    attachments = []; renderAttachments();
    if (!liveTurn) liveTurn = { depth: canonicalHistory.length, notices: [], streamed: null };
    // §FIRST-WORDS 2026-11-14: arm narration BEFORE the request roundtrip.
    // The backend starts the turn on arrival, so streamed deltas can begin
    // before this POST resolves; arming after the await dropped the opening
    // words of the reply (everything before the first tool call).
    resetNarration();
    speakOnComplete = true;
    try {
      // §STEER-FALLBACK 2026-11-14: steering only exists mid-turn; when idle
      // fall back to a normal send so the button never swallows a message.
      const status = await api.agentRequest(steer && busy ? '/v1/chat/steer' : '/v1/chat/send', {
        method: 'POST', body: { text, attachments: sentAttachments },
      });
      updateStatus(status);
    } catch (error) {
      liveTurn = null;
      speakOnComplete = false;
      attachments = sentAttachments; renderAttachments(); report(error);
    }
  }

  function renderAttachments() {
    elements.attachments.replaceChildren();
    elements.attachments.hidden = !attachments.length;
    for (const [index, filename] of attachments.entries()) {
      const chip = textElement('button', 'attachment-chip', `📎 ${filename.split('/').pop()} ×`);
      chip.type = 'button'; chip.title = filename;
      chip.addEventListener('click', () => { attachments.splice(index, 1); renderAttachments(); });
      elements.attachments.append(chip);
    }
  }

  byId('chat-form').addEventListener('submit', (event) => { event.preventDefault(); send().catch(report); });
  elements.steer.addEventListener('click', () => send(true).catch(report));
  elements.stop.addEventListener('click', () => {
    debugLog('stop clicked');
    request('/v1/chat/cancel').catch(() => {});
  });
  elements.draft.addEventListener('keydown', (event) => {
    if (event.key === 'Enter' && !event.shiftKey) { event.preventDefault(); send().catch(report); }
  });
  byId('new-chat').addEventListener('click', () => newSession().catch(report));
  byId('rename-chat').addEventListener('click', async () => {
    const name = window.prompt('Session name', sessionData.sessions.find((s) => s.id === sessionData.activeId)?.name ?? '');
    if (name) { const result = await request('/v1/sessions/rename', { id: sessionData.activeId, name }); updateSessions(result.sessions); }
  });
  byId('delete-chat').addEventListener('click', async () => {
    if (!sessionData.activeId || !window.confirm('Delete this chat and its saved history?')) return;
    const result = await request('/v1/sessions/delete', { id: sessionData.activeId });
    updateSessions(result.sessions); setHistory(result.history);
  });
  elements.sessions.addEventListener('change', async () => {
    const result = await request('/v1/sessions/select', { id: elements.sessions.value });
    updateSessions(result.sessions); setHistory(result.history);
  });
  elements.model.addEventListener('change', () => request('/v1/models/select', { model: elements.model.value }).catch(() => {}));
  elements.thinking.addEventListener('change', () => request('/v1/models/thinking', { level: elements.thinking.value }).catch(() => {}));
  byId('attach-chat').addEventListener('click', async () => {
    try { attachments.push(...await api.chooseChatAttachments()); renderAttachments(); } catch (error) { report(error); }
  });
  byId('commands-chat').addEventListener('click', () => { elements.draft.value = '/help'; elements.draft.focus(); });
  elements.draft.addEventListener('dragover', (event) => { event.preventDefault(); });
  elements.draft.addEventListener('drop', (event) => {
    event.preventDefault();
    for (const file of event.dataTransfer.files) {
      if (file.type.startsWith('image/')) attachments.push(api.filePathFor(file));
    }
    renderAttachments();
  });

  function blobBase64(blob) {
    return new Promise((resolve, reject) => {
      const reader = new FileReader();
      reader.addEventListener('load', () => resolve(reader.result.split(',')[1]));
      reader.addEventListener('error', () => reject(reader.error));
      reader.readAsDataURL(blob);
    });
  }

  async function stopDictation() {
    if (!recorder || recorder.state === 'inactive') return;
    const finished = new Promise((resolve) => recorder.addEventListener('stop', resolve, { once: true }));
    recorder.stop(); await finished;
  }

  elements.dictate.addEventListener('click', async () => {
    try {
      if (recorder?.state === 'recording') { await stopDictation(); return; }
      resetNarration();
      microphone = await navigator.mediaDevices.getUserMedia({ audio: true });
      const chunks = [];
      recorder = new MediaRecorder(microphone);
      recorder.addEventListener('dataavailable', (event) => { if (event.data.size) chunks.push(event.data); });
      recorder.addEventListener('stop', async () => {
        const mime = recorder.mimeType || 'audio/webm';
        microphone.getTracks().forEach((track) => track.stop()); microphone = null;
        elements.dictate.textContent = '…';
        try {
          const recorded = await new Blob(chunks, { type: mime }).arrayBuffer();
          const wav = await recordingWav(recorded);
          const base64 = await blobBase64(wav);
          setActivity('Transcribing…', true);
          const result = await api.chimpflowTranscribe(base64, 'audio/wav');
          // §WHISPER-NOISE 2026-11-14: whisper emits [BLANK_AUDIO]/[NOISE]
          // bracket tags for unintelligible audio — keep them out of the draft.
          const spoken = String(result.text ?? '')
            .replace(/\s*\[(BLANK_AUDIO|BLANK|NOISE|MUSIC|SILENCE)\]\s*/gi, ' ')
            .trim();
          const existing = elements.draft.value.trim();
          elements.draft.value = `${existing}${existing && spoken ? '\n' : ''}${spoken}`;
          elements.draft.focus();
        } catch (error) { report(error); }
        finally {
          elements.dictate.textContent = '🎤'; recorder = null;
          elements.dictate.title = 'Start dictation';
          setActivity(busy ? 'Thinking…' : 'Ready', busy);
        }
      });
      recorder.start(); elements.dictate.textContent = '■';
      elements.dictate.title = 'Stop dictation';
      setActivity('Listening…', true);
    } catch (error) {
      microphone?.getTracks().forEach((track) => track.stop()); microphone = null; recorder = null;
      elements.dictate.textContent = '🎤'; report(error);
    }
  });

  api.onAgentEvent((type, payload) => {
    if (type === 'assistant_delta') { appendDelta(payload.text); narrateDelta(payload.text); }
    else if (type === 'history') {
      const streamedText = liveTurn?.streamed?.text ?? '';  // capture before mergeLiveHistory clears liveTurn
      if (liveTurn) mergeLiveHistory(payload);
      else setHistory(payload);
      const latest = payload.at(-1);
      if (speakOnComplete) {
        speakOnComplete = false;
        // §STALE-BUFFER: flush on ANY assistant-turn end — gating on the
        // history tail's role let a tool turn's unterminated text survive
        // into the next turn, where it played ~40s late, out of order.
        flushNarration();
        if (pendingNotice) {
          const spoken = messageElement('assistant', streamedText).textContent.trim();
          if (spoken !== pendingNotice.plain) narration.speak(pendingNotice.plain);
          pendingNotice = null;
        }
      } else {
        pendingNotice = null;
      }
    }
    else if (type === 'user_message') {
      if (!liveTurn) liveTurn = { depth: canonicalHistory.length, notices: [], streamed: null };
      speechBuffer = ''; pendingNotice = null;  // §STALE-BUFFER: discard aborted-turn leftovers
      streaming = null;
      elements.messages.append(messageElement('user', payload.text));
      scroll();
    }
    else if (type === 'status') { agentBusy = payload.busy === true; updateStatus(payload); }
    else if (type === 'stats') { latestStats = payload; renderAgentStatus(); }
    else if (type === 'turn_done') setActivity(payload.cancelled ? 'Cancelled' : 'Done', false);
    else if (type === 'tell_user') {
      liveTurn?.notices.push(payload.message);
      elements.messages.append(messageElement('system', payload.message)); scroll();
      // §NO-DOUBLE-SPEAK 2026-11-14: the final answer already arrived as
      // assistant_delta and was narrated sentence-by-sentence; skip speaking
      // the identical tell_user copy again. Distinct notices still speak.
      if (settings.chimpflow?.enabled && settings.chimpflow?.readAssistant && !dictating()) {
        const streamed = (liveTurn?.streamed?.text ?? '').trim();
        const plain = messageElement('assistant', payload.message).textContent.trim();
        const streamedPlain = messageElement('assistant', streamed).textContent.trim();
        if (streamedPlain && streamedPlain === plain) {
          // Final answer already streamed and narrated sentence-by-sentence.
        } else if (!streamedPlain || plain.startsWith(streamedPlain)) {
          // §NO-DOUBLE-SPEAK: this notice is (the start of) the message that
          // is still streaming — narrating it now queues the whole clip ahead
          // of the streamed sentences (out-of-order playback, double-speak).
          // Defer to turn end; speak only if the stream never delivered it.
          pendingNotice = { plain };
        } else {
          narration.speak(plain);  // genuinely distinct notice — speak now
        }
      }
    }
    else if (['tool_call', 'tool_result', 'tool_progress'].includes(type)) {
      // Tool activity is deliberately NOT narrated — readAssistant speaks
      // messages only (2026-11-14: narrating tool names was tried and
      // rejected as noise).
      if (type === 'tool_call') setActivity(`Running ${payload.name ?? 'tool'}…`, true);
      else if (type === 'tool_result' && busy) setActivity('Thinking…', true);
      toolCard(type, payload);
    }
    else if (type === 'approval') approvalCard(payload);
    else if (type === 'approval_resolved') { approvals.get(payload.id)?.remove(); approvals.delete(payload.id); }
    else if (type === 'sessions') updateSessions(payload);
    else if (type === 'settings') { settings = payload; updateVoiceSettings(); }
    else if (type === 'error' || type === 'turn_error' || type === 'backend_error') {
      elements.messages.append(messageElement('error', payload.message)); scroll();
    } else if (type === 'compaction') setActivity(`Compacted ${payload.beforeTokens} → ${payload.afterTokens} tokens`, busy);
  });

  renderAttachments();
  elements.send.disabled = true;
  elements.steer.disabled = true;
  return { ensure, updateStatus, workspace: () => currentWorkspace };
}
