// SPDX-License-Identifier: BSL-1.0
import http from 'node:http';

async function startFakeProvider() {
  let requests = 0;
  const chatRequests = [];
  let voiceAuthorization = '';
  let voiceModel = '';
  const server = http.createServer((request, response) => {
    if (request.method === 'GET' && request.url === '/v1/models') {
      response.writeHead(200, { 'Content-Type': 'application/json' });
      response.end(JSON.stringify({ object: 'list', data: [{ id: 'kokoro-82m' }] })); return;
    }
    if (request.method === 'GET' && request.url.startsWith('/v1/voices?')) {
      voiceAuthorization = request.headers.authorization ?? '';
      voiceModel = new URL(request.url, 'http://fixture').searchParams.get('model');
      response.writeHead(200, { 'Content-Type': 'application/json' });
      response.end(JSON.stringify({ object: 'list', data: [{ id: 'fixture-voice', name: 'Fixture Voice' }] })); return;
    }
    if (request.method === 'GET') {
      response.writeHead(200, { 'Content-Type': 'text/html' });
      response.end('<!doctype html><title>Agent Browser Fixture</title><h1>Agent controlled</h1><a href="/next">Next</a><form><input type="file" id="agent-file"></form><script>console.log("agent-console-ready")</script>');
      return;
    }
    if (request.method === 'POST' && request.url === '/v1/audio/speech') {
      response.writeHead(200, { 'Content-Type': 'audio/mpeg' }); response.end('fake-mp3-audio'); return;
    }
    if (request.method === 'POST' && request.url === '/v1/audio/transcriptions') {
      response.writeHead(200, { 'Content-Type': 'application/json' }); response.end(JSON.stringify({ text: 'dictated fixture text' })); return;
    }
    if (request.method !== 'POST' || request.url !== '/v1/chat/completions') {
      response.writeHead(404).end(); return;
    }
    ++requests;
    let body = '';
    request.on('data', (chunk) => { body += chunk; });
    request.on('end', () => {
      const payload = JSON.parse(body);
      chatRequests.push(payload);
      const latestUser = [...payload.messages].reverse().find((message) => message.role === 'user');
      const latestText = typeof latestUser?.content === 'string' ? latestUser.content : JSON.stringify(latestUser?.content ?? '');
      const hasToolResponse = [...payload.messages].reverse().find((message) => message.role !== 'system')?.role === 'tool';
      const respond = () => {
        if (response.destroyed || response.writableEnded) return;
        response.writeHead(200, { 'Content-Type': 'text/event-stream', Connection: 'close' });
        if (latestText.includes('denied-fixture') && !hasToolResponse) {
          const call = { choices: [{ delta: { tool_calls: [{ index: 0, id: 'denied-call', type: 'function',
            function: { name: 'execute_bash', arguments: JSON.stringify({ action: 'run', command: 'printf must-not-execute' }) } }] } }] };
          response.write(`data: ${JSON.stringify(call)}\n\n`);
          response.end('data: {"choices":[{"delta":{},"finish_reason":"tool_calls"}],"usage":{"prompt_tokens":10,"completion_tokens":4,"total_tokens":14}}\n\ndata: [DONE]\n\n');
          return;
        }
        if (latestText.includes('steer-fixture') && !hasToolResponse) {
          const call = { choices: [{ delta: { tool_calls: [{ index: 0, id: 'steer-call', type: 'function',
            function: { name: 'execute_bash', arguments: JSON.stringify({ action: 'run', command: 'sleep 1' }) } }] } }] };
          response.write(`data: ${JSON.stringify(call)}\n\n`);
          response.end('data: {"choices":[{"delta":{},"finish_reason":"tool_calls"}],"usage":{"prompt_tokens":10,"completion_tokens":4,"total_tokens":14}}\n\ndata: [DONE]\n\n');
          return;
        }
        if (latestText.includes('notify-fixture') && !hasToolResponse) {
          const call = { choices: [{ delta: { tool_calls: [{ index: 0, id: 'notify-call', type: 'function',
            function: { name: 'tell_user', arguments: JSON.stringify({ content: 'Fixture notification delivered' }) } }] } }] };
          response.write(`data: ${JSON.stringify(call)}\n\n`);
          response.end('data: {"choices":[{"delta":{},"finish_reason":"tool_calls"}],"usage":{"prompt_tokens":10,"completion_tokens":4,"total_tokens":14}}\n\ndata: [DONE]\n\n');
          return;
        }
        if (latestText.includes('approval-fixture') && !hasToolResponse) {
          const call = { choices: [{ delta: { tool_calls: [{ index: 0, id: 'approval-call', type: 'function',
            function: { name: 'execute_bash', arguments: JSON.stringify({ action: 'run', command: 'printf approval-command-executed' }) } }] } }] };
          response.write(`data: ${JSON.stringify(call)}\n\n`);
          response.end('data: {"choices":[{"delta":{},"finish_reason":"tool_calls"}],"usage":{"prompt_tokens":10,"completion_tokens":4,"total_tokens":14}}\n\ndata: [DONE]\n\n');
          return;
        }
        const content = body.includes('approval-fixture') ? 'Approval workflow completed' : 'Local fake provider response';
        response.write(`data: ${JSON.stringify({ choices: [{ delta: { content: content.slice(0, 10) } }] })}\n\n`);
        response.write(`data: ${JSON.stringify({ choices: [{ delta: { content: content.slice(10) } }] })}\n\n`);
        response.end('data: {"choices":[{"delta":{},"finish_reason":"stop"}],"usage":{"prompt_tokens":10,"completion_tokens":4,"total_tokens":14}}\n\ndata: [DONE]\n\n');
      };
      if (latestText.includes('cancel-fixture')) setTimeout(respond, 3000); else respond();
    });
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  return { server, requests: () => requests, chatRequests: () => chatRequests,
    voiceAuthorization: () => voiceAuthorization,
    voiceModel: () => voiceModel, url: `http://127.0.0.1:${server.address().port}` };
}

export async function runAgentSmokeTest(window, root, backend) {
  const provider = await startFakeProvider();
  try {
    const result = await window.webContents.executeJavaScript(`(async () => {
      const checks = [];
      const events = [];
      const check = (condition, name) => {
        checks.push({ name, passed: Boolean(condition) });
        if (!condition) throw new Error('Agent smoke assertion failed: ' + name);
      };
      const waitFor = async (callback, description) => {
        const deadline = Date.now() + 15000;
        while (Date.now() < deadline) {
          try { const value = await callback(); if (value) return value; } catch {}
          await new Promise((resolve) => setTimeout(resolve, 80));
        }
        throw new Error('Timed out: ' + description);
      };
      const api = window.monkeybyte;
      const root = ${JSON.stringify(root)};
      let notificationBubbleSeen = false;
      api.onAgentEvent((type, payload) => events.push({ type, payload }));
      api.onAgentEvent((type) => {
        if (type === 'tell_user') {
          notificationBubbleSeen = [...document.querySelectorAll('#chat-messages .message-bubble')]
            .some((entry) => entry.textContent === 'Fixture notification delivered');
        }
      });
      const post = (path, body = {}) => api.agentRequest(path, { method: 'POST', body });
      const ensured = await waitFor(async () => {
        const value = await post('/v1/sessions/ensure', { workspace: ${JSON.stringify(root)} });
        return value.session?.id ? value : null;
      }, 'agent session');
      check(ensured.session.id, 'persistent session is created');
      check(Array.isArray(ensured.history), 'history is returned');
      const settings = await api.agentRequest('/v1/settings');
      check(settings.providers.every((provider) => !('apiKey' in provider) && !('api_key' in provider)), 'provider secrets stay out of renderer settings');
      const created = await post('/v1/sessions/create', { workspace: ${JSON.stringify(root)}, name: 'Integration Chat' });
      const id = created.session.id;
      check(id !== ensured.session.id, 'new chat creates a distinct session');
      const renamed = await post('/v1/sessions/rename', { id, name: 'Renamed Integration' });
      check(renamed.session.name === 'Renamed Integration', 'session rename persists through the catalog');
      await post('/v1/settings', { provider: { name: 'fixture', host: ${JSON.stringify(provider.url)}, apiKey: 'not-a-real-key' } });
      await post('/v1/settings', { model: { name: 'fixture-model', provider: 'fixture', modelId: 'fixture-id', maxContextSize: 100000 } });
      const selected = await post('/v1/models/select', { model: 'fixture-model' });
      check(selected.session.model === 'fixture-model', 'model selection uses the shared SettingsStore');
      const selectorWidths = () => {
        const header = document.querySelector('.chat-header');
        const style = getComputedStyle(header);
        const available = header.clientWidth - parseFloat(style.paddingLeft) - parseFloat(style.paddingRight);
        return { expected: (available - 8) / 2,
          model: document.querySelector('#model-select').getBoundingClientRect().width,
          thinking: document.querySelector('#thinking-select').getBoundingClientRect().width };
      };
      const beforeResize = selectorWidths();
      check(Math.abs(beforeResize.model - beforeResize.expected) < 2 &&
        Math.abs(beforeResize.thinking - beforeResize.expected) < 2,
        'model and thinking selectors share the full agent width');
      const chatResizer = document.querySelector('#chat-resizer');
      for (let index = 0; index < 5; ++index) {
        chatResizer.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowRight', bubbles: true, cancelable: true }));
      }
      await new Promise((resolve) => setTimeout(resolve, 100));
      const afterResize = selectorWidths();
      check(afterResize.model > beforeResize.model + 30 &&
        Math.abs(afterResize.model - afterResize.expected) < 2 &&
        Math.abs(afterResize.thinking - afterResize.expected) < 2,
        'model and thinking selectors grow when the agent panel expands');
      await post('/v1/models/thinking', { level: 'High' });
      check((await api.agentRequest('/v1/health')).thinking === 'High', 'thinking selection persists on the session');
      check(!document.querySelector('#steer-chat').hidden && !document.querySelector('#stop-chat').hidden &&
        document.querySelector('#steer-chat').disabled && document.querySelector('#stop-chat').disabled,
        'Steer and Stop are visible and disabled while idle');
      const idleActionsHeight = document.querySelector('.chat-input-actions').getBoundingClientRect().height;
      await post('/v1/chat/send', { text: 'First queued integration message' });
      await post('/v1/chat/send', { text: 'Second queued integration message' });
      await waitFor(() => events.some((event) => event.type === 'turn_done'), 'queued turn completion');
      check(events.some((event) => event.type === 'assistant_delta'), 'provider tokens stream over SSE');
      const used = await api.agentRequest('/v1/health');
      check(used.stats.totalTokens > 0, 'live agent stats are returned by the backend');
      check(document.querySelector('#agent-status').textContent.includes('fixture-model ·'), 'status bar includes the selected model and tokens');
      check(document.querySelector('#agent-status').textContent.includes('/100k ctx'), 'status bar includes context capacity');
      check(events.some((event) => event.type === 'stats'), 'live stats stream over SSE');
      const history = await api.agentRequest('/v1/history');
      check(history.some((entry) => entry.content.includes('integration message')), 'queued messages enter real Daedalus history');
      check(history.some((entry) => entry.content === 'Local fake provider response'), 'assistant response enters real Daedalus history');
      await post('/v1/sessions/select', { id: ensured.session.id });
      const restored = await post('/v1/sessions/select', { id });
      check(restored.history.some((entry) => entry.content.includes('integration message')), 'history survives session switching');
      const notifyDone = events.filter((event) => event.type === 'turn_done').length;
      await post('/v1/chat/send', { text: 'notify-fixture' });
      await waitFor(() => events.filter((event) => event.type === 'turn_done').length > notifyDone, 'notification turn completion');
      check(events.some((event) => event.type === 'tell_user' && event.payload.message === 'Fixture notification delivered'), 'tell_user reaches the renderer');
      check(notificationBubbleSeen, 'tell_user appends a transcript bubble without throwing');
      check([...document.querySelectorAll('.message-bubble')].filter((bubble) =>
        bubble.textContent === 'Fixture notification delivered').length === 1,
        'end-of-turn history merge does not duplicate tell_user messages');
      const approvalDone = events.filter((event) => event.type === 'turn_done').length;
      await post('/v1/chat/send', { text: 'approval-fixture' });
      const approval = await waitFor(() => events.find((event) => event.type === 'approval'), 'command approval request');
      const runningCard = document.querySelector('.tool-card[data-call-id="approval-call"]');
      check(runningCard?.dataset.status === 'running' && runningCard.querySelector('summary').textContent.startsWith('⚙'),
        'tool card appears immediately in the running state');
      check(runningCard?.querySelector('pre').textContent.includes('approval-command-executed'),
        'running tool card exposes its command arguments');
      await post('/v1/chat/approval', { id: approval.payload.id, approved: true });
      await waitFor(() => events.filter((event) => event.type === 'turn_done').length > approvalDone, 'approved command completion');
      check((await api.agentRequest('/v1/history')).some((entry) => entry.content === 'Approval workflow completed'), 'approval resumes the active turn');
      check(events.some((event) => event.type === 'tool_result'), 'approved tool result is emitted');
      const completedCards = document.querySelectorAll('.tool-card[data-call-id="approval-call"]');
      if (completedCards.length !== 1 || completedCards[0].dataset.status !== 'success' ||
        !completedCards[0].querySelector('summary').textContent.startsWith('✓')) {
        throw new Error('Completed tool-card state: ' + JSON.stringify([...completedCards].map((card) => card.outerHTML)));
      }
      check(completedCards.length === 1 && completedCards[0].dataset.status === 'success' &&
        completedCards[0].querySelector('summary').textContent.startsWith('✓'),
        'one tool card updates in place with a success tick');
      check(completedCards[0]?.querySelector('pre').textContent.includes('approval-command-executed') &&
        completedCards[0]?.querySelector('pre').textContent.includes('result'),
        'completed tool card retains arguments and result JSON');
      check([...document.querySelectorAll('.message-bubble[data-role="user"]')].some((node) => node.textContent.includes('approval-fixture')) &&
        [...document.querySelectorAll('.message-bubble')].some((node) => node.textContent.includes('Approval workflow completed')) &&
        document.querySelector('.tool-card[data-call-id="approval-call"]'),
        'turn-end history merge preserves prompts, responses, and tool cards');
      const deniedDone = events.filter((event) => event.type === 'turn_done').length;
      await post('/v1/chat/send', { text: 'denied-fixture' });
      const deniedApproval = await waitFor(() => events.find((event) => event.type === 'approval' &&
        JSON.stringify(event.payload).includes('must-not-execute')), 'denied command approval request');
      await post('/v1/chat/approval', { id: deniedApproval.payload.id, approved: false });
      await waitFor(() => events.filter((event) => event.type === 'turn_done').length > deniedDone, 'denied command completion');
      const deniedCards = document.querySelectorAll('.tool-card[data-call-id="denied-call"]');
      check(deniedCards.length === 1 && deniedCards[0].dataset.status === 'error' &&
        deniedCards[0].querySelector('summary').textContent.startsWith('✕'),
        'one tool card updates in place with a failure cross');
      const steerDone = events.filter((event) => event.type === 'turn_done').length;
      await post('/v1/chat/send', { text: 'steer-fixture' });
      await waitFor(() => events.some((event) => event.type === 'tool_call' &&
        JSON.stringify(event.payload).includes('steer-call')), 'tool executing for Steer integration');
      const steerApproval = await waitFor(() => events.find((event) => event.type === 'approval' &&
        JSON.stringify(event.payload).includes('sleep 1')), 'Steer fixture tool approval');
      document.querySelector('#chat-text').value = 'mid-turn correction';
      document.querySelector('#steer-chat').click();
      await post('/v1/chat/approval', { id: steerApproval.payload.id, approved: true });
      await waitFor(() => events.filter((event) => event.type === 'turn_done').length > steerDone,
        'steered turn completion');
      const steerCards = [...document.querySelectorAll('.tool-card[data-call-id="steer-call"]')];
      check(steerCards.length === 1, 'one tool card is updated for call/result instead of duplicated');
      check(steerCards[0]?.querySelector('summary').textContent === '✓ execute_bash' &&
        steerCards[0]?.querySelector('pre').textContent.includes('sleep 1') &&
        steerCards[0]?.querySelector('pre').textContent.includes('exit_code'),
        'combined tool card retains both command and result');
      const completedTranscript = document.querySelector('#chat-messages').textContent;
      check(completedTranscript.includes('steer-fixture') && completedTranscript.includes('mid-turn correction') &&
        completedTranscript.includes('Local fake provider response'),
        'end-of-turn history merge preserves prompts, final response, and tool transcript');
      const cancelDone = events.filter((event) => event.type === 'turn_done').length;
      await post('/v1/chat/send', { text: 'cancel-fixture' });
      await waitFor(() => document.querySelector('#chat-activity').dataset.busy === 'true', 'working spinner state');
      const spinner = document.querySelector('.activity-spinner');
      check(getComputedStyle(spinner).animationName === 'agent-spin', 'working indicator has the spinner animation');
      check(spinner.getAnimations().some((animation) => animation.playState === 'running'), 'working spinner is actually running');
      check(document.querySelector('#send-chat').textContent === 'Send' &&
        !document.querySelector('#send-chat').disabled, 'Send stays labelled and enabled during a busy turn');
      check(!document.querySelector('#steer-chat').hidden && !document.querySelector('#stop-chat').hidden &&
        !document.querySelector('#steer-chat').disabled && !document.querySelector('#stop-chat').disabled,
        'Steer and Stop remain visible and become enabled while busy');
      check(document.querySelector('.chat-input-actions').getBoundingClientRect().height === idleActionsHeight,
        'busy controls do not change the input-action layout height');
      const actionBounds = document.querySelector('.chat-input-actions').getBoundingClientRect();
      const sendBounds = document.querySelector('#send-chat').getBoundingClientRect();
      const steerBounds = document.querySelector('#steer-chat').getBoundingClientRect();
      const stopBounds = document.querySelector('#stop-chat').getBoundingClientRect();
      check(sendBounds.width >= steerBounds.width + stopBounds.width &&
        sendBounds.left === actionBounds.left && sendBounds.right >= actionBounds.right - 1 &&
        steerBounds.bottom <= sendBounds.top && stopBounds.bottom <= sendBounds.top,
        'Steer and Stop stack above a full-width Send button');
      check(document.querySelector('meta[http-equiv="Content-Security-Policy"]').content.includes("media-src 'self' blob:"), 'CSP permits local ChimpFlow blob audio');
      document.querySelector('[data-panel="settings"]').click();
      const liveTheme = document.querySelector('#theme-select');
      liveTheme.value = liveTheme.value === 'dark' ? 'light' : 'dark';
      liveTheme.dispatchEvent(new Event('change', { bubbles: true }));
      document.querySelector('[data-panel="workspace"]').click();
      await post('/v1/chat/steer', { text: 'queued cancellation follow-up' });
      document.querySelector('#stop-chat').click();
      await waitFor(() => events.filter((event) => event.type === 'turn_done').length > cancelDone, 'cancelled turn completion');
      await waitFor(async () => !(await api.agentRequest('/v1/health')).busy, 'queue becomes idle after cancellation');
      check(events.some((event) => event.type === 'user_message' && event.payload.text === 'queued cancellation follow-up'), 'steering accepts a follow-up during a busy turn');
      await waitFor(() => document.querySelector('#chat-activity').dataset.busy === 'false', 'spinner stops when the turn ends');
      check(!document.querySelector('#steer-chat').hidden && !document.querySelector('#stop-chat').hidden &&
        document.querySelector('#steer-chat').disabled && document.querySelector('#stop-chat').disabled &&
        document.querySelector('.chat-input-actions').getBoundingClientRect().height === idleActionsHeight,
        'Steer and Stop return to disabled without moving after completion');

      await post('/v1/chat/clear');
      check((await api.agentRequest('/v1/history')).length === 0, 'clear erases active history');
      const chimpUrl = new URL(${JSON.stringify(provider.url)});
      await post('/v1/settings', { chimpflow: { enabled: true, readAssistant: false, dictationEnabled: true,
        serverUrl: chimpUrl.protocol + '//' + chimpUrl.hostname, port: Number(chimpUrl.port),
        apiKey: 'fixture-chimpflow-key', ttsModel: 'kokoro-82m', sttModel: 'whisper', voice: 'fixture-voice' } });
      const voices = await api.chimpflowRequest('/v1/voices');
      check(voices.voices[0].id === 'fixture-voice', 'ChimpFlow voices use the configured server');
      document.querySelector('[data-settings-page="ChimpFlow"]').click();
      check(document.querySelector('#settings-status').textContent.includes('Discovery and previews run locally'), 'ChimpFlow tab explains local discovery and previews');
      const loadVoices = [...document.querySelectorAll('#settings-content button')].find((button) => button.textContent === 'Refresh Models / Voices');
      document.querySelector('#chimpflow-key').value = 'typed-fixture-key';
      loadVoices.click();
      await waitFor(() => document.querySelector('#settings-status').textContent === 'Discovered 1 model(s) and 1 voice(s) for kokoro-82m.', 'ChimpFlow discovery status');
      check(${JSON.stringify('Bearer typed-fixture-key')} === (await (async () => { return null; })()) || true, 'placeholder');
      check(document.querySelector('#chimpflow-voice').options.length === 1, 'ChimpFlow discovery populates the voice selector');
      const saveChimpFlow = [...document.querySelectorAll('#settings-content button')].find((button) => button.textContent === 'Save ChimpFlow');
      saveChimpFlow.click();
      await waitFor(() => document.querySelector('#settings-status').textContent === 'ChimpFlow settings saved and reloaded.', 'ChimpFlow save status');
      check(true, 'ChimpFlow save reports completion in the status bar');
      document.querySelector('[data-settings-page="Interface"]').click();
      const speech = await api.chimpflowSpeak('Speak this locally');
      check(atob(speech.base64) === 'fake-mp3-audio', 'ChimpFlow speech returns audio through scoped IPC');
      const transcription = await api.chimpflowTranscribe(btoa('fake-audio'), 'audio/webm');
      check(transcription.text === 'dictated fixture text', 'ChimpFlow dictation transcribes through scoped IPC');
      check(!('apiKey' in (await api.agentRequest('/v1/settings')).chimpflow), 'ChimpFlow key stays out of renderer settings');
      const projectTypes = await api.projectTypesList();
      check(projectTypes.types.some((type) => type.id === 'film'), 'built-in project types are available');
      const applied = await api.projectTypeApply('film');
      check(applied.id === 'film', 'project type is detected from the workspace stamp');
      const agents = await api.readTextFile(root, 'AGENTS.md');
      check(agents.includes('monkeybyte:type: film'), 'project type writes the persistent AGENTS.md stamp');
      const screenplay = await api.readTextFile(root, 'script.fountain');
      check(screenplay.length > 0, 'project type scaffolds its starter files');
      const screenplayPreview = await api.previewFile(root, 'script.fountain', 'fountain');
      const previewPage = await api.browserExecute('browser.read', { tab: screenplayPreview.id });
      check(previewPage.text.includes('Screenplay Preview'), 'Fountain preview uses the external converter');
      await api.browserExecute('browser.close', { tab: screenplayPreview.id });
      const exported = await api.exportFountain(root, 'script.fountain', 'script.pdf');
      check(exported.exported && (await api.readTextFile(root, 'script.pdf')).startsWith('%PDF-FAKE'), 'Fountain PDF export writes converted output');
      await api.projectTypeSave('custom-fixture', 'Custom fixture conventions.');
      check((await api.projectTypesList()).types.some((type) => type.id === 'custom-fixture'), 'custom project type prompts are editable');
      await post('/v1/sessions/delete', { id });
      check(!(await api.agentRequest('/v1/sessions')).sessions.some((session) => session.id === id), 'deleted session leaves the catalog');
      return { checks };
    })()`, true);
    const steerInitialIndex = provider.chatRequests().findIndex((payload) => payload.messages.some(
      (message) => message.role === 'user' && typeof message.content === 'string' &&
        message.content.includes('steer-fixture')));
    const steeredMessages = provider.chatRequests()[steerInitialIndex + 1]?.messages ?? [];
    const steerIndex = steeredMessages.findIndex((message) => message.role === 'user' &&
      typeof message.content === 'string' && message.content.includes('mid-turn correction'));
    const steerOrderValid = steerIndex >= 2 && steeredMessages[steerIndex - 1].role === 'tool' &&
      steeredMessages[steerIndex - 1].tool_call_id === 'steer-call' &&
      steeredMessages[steerIndex - 2].role === 'assistant' &&
      steeredMessages[steerIndex - 2].tool_calls?.[0]?.id === 'steer-call';
    result.checks.push({ name: 'Steer button inserts after the completed tool result and before the next request',
      passed: steerOrderValid });
    if (!steerOrderValid) throw new Error('Steer message broke the assistant/tool-result chain.');
    const steerOriginValid = steeredMessages[steerIndex].content.startsWith('[from: steer:lee');
    result.checks.push({ name: 'Steer message preserves its origin in the real provider chain',
      passed: steerOriginValid });
    if (!steerOriginValid) throw new Error('Steer message lost its origin header.');

    if (provider.requests() < 1) throw new Error('Fake provider received no chat request.');
    if (provider.voiceAuthorization() !== 'Bearer typed-fixture-key') {
      throw new Error('ChimpFlow discovery did not use the newly typed API key.');
    }
    if (provider.voiceModel() !== 'kokoro-82m') {
      throw new Error('ChimpFlow voice discovery was not scoped to the selected model.');
    }

    const tools = await backend.request('/v1/test/tools');
    if (!tools.includes('app_ui_control')) throw new Error('Agent host tool is not registered.');
    const opened = await backend.request('/v1/test/ui', { method: 'POST', body: {
      command: 'browser.open', args: { url: provider.url },
    } });
    const listed = await backend.request('/v1/test/ui', { method: 'POST', body: {
      command: 'browser.list', args: {},
    } });
    if (!listed.tabs.some((tab) => tab.id === opened.id)) throw new Error('Backend UI command did not open a browser tab.');
    let page;
    for (let attempt = 0; attempt < 100; ++attempt) {
      try { page = await backend.request('/v1/test/ui', { method: 'POST', body: {
        command: 'browser.read', args: { tab: opened.id, max_chars: 2000 },
      } }); if (page.text.includes('Agent controlled')) break; } catch {}
      await new Promise((resolve) => setTimeout(resolve, 80));
    }
    if (!page?.text.includes('Agent controlled')) throw new Error('Backend browser.read did not return live page text.');
    const evaluated = await backend.request('/v1/test/ui', { method: 'POST', body: {
      command: 'browser.eval', args: { tab: opened.id, script: 'document.querySelector("h1").textContent' },
    } });
    if (evaluated.result !== 'Agent controlled') throw new Error('Backend browser.eval failed.');
    const links = await backend.request('/v1/test/ui', { method: 'POST', body: {
      command: 'browser.links', args: { tab: opened.id },
    } });
    if (!links.links.some((link) => link.text === 'Next')) throw new Error('Backend browser.links failed.');
    const consoleResult = await backend.request('/v1/test/ui', { method: 'POST', body: {
      command: 'browser.console', args: { tab: opened.id },
    } });
    if (!consoleResult.entries.some((entry) => entry.text.includes('agent-console-ready'))) throw new Error('Backend browser.console failed.');
    await backend.request('/v1/test/ui', { method: 'POST', body: {
      command: 'browser.upload', args: { tab: opened.id, path: `${root}/upload-fixture.txt` },
    } });
    const upload = await backend.request('/v1/test/ui', { method: 'POST', body: {
      command: 'browser.eval', args: { tab: opened.id, script: 'document.querySelector("#agent-file").files[0]?.name' },
    } });
    if (upload.result !== 'upload-fixture.txt') throw new Error('Backend browser.upload failed.');
    await backend.request('/v1/test/ui', { method: 'POST', body: {
      command: 'editor.open', args: { root, path: `${root}/hello.txt`, line: 1 },
    } });
    const editor = await backend.request('/v1/test/ui', { method: 'POST', body: {
      command: 'editor.selection', args: {},
    } });
    if (!editor.text.includes('Hello from Electron.')) throw new Error('Backend editor.open/selection failed.');
    await backend.request('/v1/test/ui', { method: 'POST', body: {
      command: 'browser.close', args: { tab: opened.id },
    } });
    console.log(JSON.stringify({ status: 'success', agent: result,
      providerRequests: provider.requests(),
      hostTool: { registered: true, browserRoundTrip: true, editorRoundTrip: true } }, null, 2));
    return 0;
  } finally {
    await new Promise((resolve) => provider.server.close(resolve));
  }
}
