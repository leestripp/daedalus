// SPDX-License-Identifier: BSL-1.0
import { byId, errorMessage } from './dom.js';
import { createNarration } from './voice.js';

const PAGES = ['Interface', 'Providers', 'Models', 'Workspace', 'MCP Servers', 'Agent', 'Web Search', 'ChimpFlow', 'Search', 'Paths'];

function node(tag, text = '', className = '') {
  const element = document.createElement(tag);
  if (className) element.className = className;
  element.textContent = text;
  return element;
}

function field(labelText, control) {
  const label = node('label', labelText, 'settings-field');
  label.append(control);
  return label;
}

function input(id, value = '', type = 'text') {
  const element = document.createElement('input');
  element.id = id; element.type = type; element.value = value ?? '';
  return element;
}

function checkbox(id, checked, text) {
  const control = input(id, '', 'checkbox'); control.checked = Boolean(checked);
  const label = node('label', '', 'check-row'); label.htmlFor = id;
  label.append(control, document.createTextNode(` ${text}`));
  return label;
}

function button(text, handler, primary = false) {
  const element = node('button', text, primary ? 'primary' : '');
  element.type = 'button'; element.addEventListener('click', handler);
  return element;
}

function select(id, values, selected) {
  const element = document.createElement('select'); element.id = id;
  for (const value of values) {
    const option = node('option', typeof value === 'string' ? value : value.label);
    option.value = typeof value === 'string' ? value : value.value;
    element.append(option);
  }
  element.value = selected;
  return element;
}

export function createSettingsView(saveSettings, api) {
  const content = byId('settings-content');
  const tabs = byId('settings-tabs');
  let active = 'Interface';
  let ui = {};
  let agent = {};
  let selectedProvider = '';
  let selectedModel = '';
  let selectedMcp = '';
  let selectedPrompt = '';
  let selectedType = '';
  let chimpflowEditing = false;
  let chimpflowModelOptions = [];
  let chimpflowVoiceOptions = [];
  let projectData = { types: [] };
  const narration = createNarration(api.chimpflowSpeak, report,
    () => status('Voice preview finished.'));

  for (const page of PAGES) {
    const tab = node('button', page, `settings-tab${page === active ? ' active' : ''}`);
    tab.type = 'button'; tab.dataset.settingsPage = page;
    tab.setAttribute('aria-pressed', String(page === active));
    tab.addEventListener('click', () => {
      active = page;
      if (page === 'ChimpFlow') chimpflowEditing = false;
      status(page === 'ChimpFlow'
        ? 'Discovery and previews run locally. Selecting a voice plays a sample.'
        : 'Changes apply immediately.');
      renderTabs(); render();
    });
    tabs.append(tab);
  }

  function renderTabs() {
    for (const tab of tabs.children) {
      const current = tab.dataset.settingsPage === active;
      tab.classList.toggle('active', current); tab.setAttribute('aria-pressed', String(current));
    }
  }

  function status(message) { byId('settings-status').textContent = message; }
  function report(error) { status(errorMessage(error)); }

  async function post(patch, successMessage = 'Saved to the shared Daedalus settings store.') {
    try {
      agent = await api.agentRequest('/v1/settings', { method: 'POST', body: patch });
      status(successMessage);
      if (active === 'ChimpFlow') chimpflowEditing = false;
      render();
      return agent;
    } catch (error) { report(error); throw error; }
  }

  function heading(text) { content.append(node('h3', text)); }
  function grid(...children) { const element = node('div', '', 'settings-grid wide'); element.append(...children); content.append(element); }
  function actions(...children) { const element = node('div', '', 'settings-actions'); element.append(...children); content.append(element); }
  function list(items, selected, choose, remove) {
    const listElement = node('div', '', 'settings-list');
    for (const item of items) {
      const name = typeof item === 'string' ? item : item.name;
      const row = node('div', '', `settings-list-row${name === selected ? ' selected' : ''}`);
      const chooseButton = button(name, () => choose(name));
      row.append(chooseButton);
      if (remove) row.append(button('Delete', () => remove(name)));
      listElement.append(row);
    }
    if (!items.length) listElement.append(node('p', 'No entries configured.', 'muted-message'));
    content.append(listElement);
  }

  function renderInterface() {
    heading('Editor');
    const font = select('font-size-select', [10, 11, 12, 13, 14, 15, 16, 17, 18, 20, 22, 24].map((value) => ({ value: String(value), label: `${value} px` })), String(ui.fontSize));
    const tabs = select('tab-size-select', [{ value: '2', label: '2 spaces' }, { value: '4', label: '4 spaces' }, { value: '8', label: '8 spaces' }], String(ui.tabSize));
    font.addEventListener('change', () => saveSettings({ fontSize: Number(font.value) }));
    tabs.addEventListener('change', () => saveSettings({ tabSize: Number(tabs.value) }));
    grid(field('Font size', font), field('Tab size', tabs));
    for (const [id, key, label] of [
      ['word-wrap', 'wordWrap', 'Word wrap'], ['show-line-numbers', 'showLineNumbers', 'Show line numbers'],
      ['highlight-current-line', 'highlightCurrentLine', 'Highlight current line'],
      ['show-hidden', 'showHidden', 'Show hidden files'],
    ]) {
      const row = checkbox(id, ui[key], label);
      row.querySelector('input').addEventListener('change', (event) => saveSettings({ [key]: event.target.checked }));
      content.append(row);
    }
    heading('Theme & Browser');
    const theme = select('theme-select', ['system', 'dark', 'light'], ui.theme);
    theme.addEventListener('change', () => saveSettings({ theme: theme.value }));
    const home = input('browser-home-url', ui.browserHomeUrl, 'url'); home.placeholder = 'https://example.com';
    home.addEventListener('change', () => saveSettings({ browserHomeUrl: home.value.trim() }));
    grid(field('Color theme', theme), field('Browser home URL', home));
  }

  function renderProviders() {
    heading('Providers');
    list(agent.providers ?? [], selectedProvider, (name) => { selectedProvider = name; render(); }, async (name) => {
      if (window.confirm(`Delete provider ${name}?`)) await post({ deleteProvider: name });
    });
    actions(button('New provider', () => { selectedProvider = ''; render(); }));
    const provider = (agent.providers ?? []).find((item) => item.name === selectedProvider) ?? {};
    const name = input('provider-name', provider.name); name.placeholder = 'openai-compatible';
    const host = input('provider-host', provider.host, 'url'); host.placeholder = 'https://api.example.com';
    const key = input('provider-key', '', 'password'); key.placeholder = provider.hasApiKey ? 'Saved key — leave blank to keep' : 'API key';
    const chat = input('provider-chat', provider.chatEndpoint || '/v1/chat/completions');
    const discovery = input('provider-discovery', provider.discoveryEndpoint || '/v1/models');
    const timeout = input('provider-timeout', provider.timeoutSeconds ?? 30, 'number');
    const plugin = input('provider-plugin', provider.plugin); plugin.placeholder = 'Auto-detect';
    grid(field('Name', name), field('Base URL', host), field('API key', key), field('Chat endpoint', chat),
      field('Discovery endpoint', discovery), field('Timeout (seconds)', timeout), field('Plugin', plugin));
    actions(button('Save provider', () => post({ provider: {
      name: name.value.trim(), host: host.value.trim(), apiKey: key.value,
      chatEndpoint: chat.value.trim(), discoveryEndpoint: discovery.value.trim(),
      timeoutSeconds: Number(timeout.value), plugin: plugin.value.trim(),
    } }), true));
  }

  function renderModels() {
    heading('Models');
    list(agent.models ?? [], selectedModel, (name) => { selectedModel = name; render(); }, async (name) => {
      if (window.confirm(`Delete model ${name}?`)) await post({ deleteModel: name });
    });
    actions(button('New model', () => { selectedModel = ''; render(); }));
    const model = (agent.models ?? []).find((item) => item.name === selectedModel) ?? {};
    const name = input('model-name', model.name);
    const provider = select('model-provider', (agent.providers ?? []).map((item) => item.name), model.provider || agent.providers?.[0]?.name);
    const modelId = input('model-id', model.modelId);
    const context = input('model-context', model.maxContextSize ?? 0, 'number');
    const temperature = input('model-temperature', model.temperature ?? 0, 'number'); temperature.step = '0.1'; temperature.min = '0'; temperature.max = '2';
    const promptNames = [{ value: '', label: 'Default / project prompt' }, ...(agent.prompts ?? []).map((item) => item.name)];
    const prompt = select('model-prompt', promptNames, agent.modelPrompts?.[model.name] ?? '');
    grid(field('Display name', name), field('Provider', provider), field('Provider model ID', modelId),
      field('Maximum context', context), field('Temperature', temperature), field('System prompt', prompt));
    actions(button('Save model', async () => {
      await post({ model: { name: name.value.trim(), provider: provider.value,
        modelId: modelId.value.trim(), maxContextSize: Number(context.value), temperature: Number(temperature.value) } });
      await post({ modelPrompt: { model: name.value.trim(), prompt: prompt.value } });
    }, true), button('Make default', () => post({ defaultModel: name.value })));
    content.append(node('p', `Default model: ${agent.defaultModel || 'Not set'}`, 'settings-note'));
  }

  function renderPrompts() {
    heading('Project Types');
    for (const type of projectData.types ?? []) {
      const row = node('section', '', `settings-card${projectData.current?.id === type.id ? ' selected' : ''}`);
      row.append(node('h4', `${type.name}${projectData.current?.id === type.id ? ' — active' : ''}`),
        node('p', type.description, 'settings-note'));
      const edit = document.createElement('textarea'); edit.rows = 6; edit.value = type.prompt ?? '';
      row.append(field('System prompt', edit));
      const buttons = node('div', '', 'settings-actions');
      buttons.append(button('Apply to workspace', () => api.projectTypeApply(type.id).then(refreshAgent).catch(report)),
        button('Save type prompt', () => api.projectTypeSave(type.id, edit.value).then(refreshAgent).catch(report)),
        button('New workspace from type', async () => {
          const settings = await api.projectTypeCreate(type.id);
          if (settings.workspace) await saveSettings({ workspace: settings.workspace });
        }));
      row.append(buttons); content.append(row);
    }
    const newType = input('new-project-type', ''); newType.placeholder = 'custom-type-id';
    actions(newType, button('Create project type', () => api.projectTypeSave(newType.value.trim(), 'Project conventions…').then(refreshAgent).catch(report)));
    heading('Reusable System Prompts');
    list(agent.prompts ?? [], selectedPrompt, (name) => { selectedPrompt = name; render(); }, async (name) => {
      if (window.confirm(`Delete prompt ${name}?`)) await post({ deletePrompt: name });
    });
    actions(button('New prompt', () => { selectedPrompt = ''; render(); }));
    const prompt = (agent.prompts ?? []).find((item) => item.name === selectedPrompt) ?? {};
    const name = input('prompt-name', prompt.name);
    const text = document.createElement('textarea'); text.id = 'prompt-text'; text.rows = 14; text.value = prompt.text ?? '';
    const isDefault = checkbox('prompt-default', prompt.isDefault, 'Use as the default system prompt');
    grid(field('Prompt name', name)); content.append(field('Prompt text', text), isDefault);
    actions(button('Save prompt', () => post({ prompt: { name: name.value.trim(), text: text.value,
      isDefault: isDefault.querySelector('input').checked } }), true));
    content.append(node('p', 'A model-specific binding overrides this default. Bind models on the Models page.', 'settings-note'));
  }

  function renderMcp() {
    heading('MCP Servers');
    list(agent.mcpServers ?? [], selectedMcp, (name) => { selectedMcp = name; render(); }, async (name) => {
      if (window.confirm(`Delete MCP server ${name}?`)) await post({ deleteMcpServer: name });
    });
    actions(button('New MCP server', () => { selectedMcp = ''; render(); }));
    const server = (agent.mcpServers ?? []).find((item) => item.name === selectedMcp) ?? {};
    const name = input('mcp-name', server.name);
    const transport = select('mcp-transport', ['stdio', 'streamable-http'], server.transport || 'stdio');
    const command = input('mcp-command', server.command);
    const args = input('mcp-args', (server.args ?? []).join(' '));
    const url = input('mcp-url', server.url, 'url');
    const token = input('mcp-token', '', 'password'); token.placeholder = server.hasBearerToken ? 'Saved token — leave blank to keep' : 'Bearer token';
    const env = document.createElement('textarea'); env.rows = 4; env.placeholder = '{"API_KEY":"value"}'; env.id = 'mcp-env';
    const enabled = checkbox('mcp-enabled', server.enabled ?? true, 'Enabled');
    grid(field('Name', name), field('Transport', transport), field('Command', command),
      field('Arguments', args), field('HTTP URL', url), field('Bearer token', token));
    content.append(field('Environment JSON (blank values keep saved secrets)', env), enabled);
    actions(button('Save MCP server', () => post({ mcpServer: {
      name: name.value.trim(), transport: transport.value, command: command.value.trim(),
      args: args.value.trim() ? args.value.trim().split(/\s+/) : [], url: url.value.trim(),
      bearerToken: token.value, env: env.value.trim() ? JSON.parse(env.value) : {},
      enabled: enabled.querySelector('input').checked,
    } }), true));
  }

  function renderAgent() {
    heading('Agent Policy & Limits');
    const values = agent.agent ?? {};
    const auto = checkbox('agent-auto-approve', agent.autoApprove, 'Auto-approve commands (use with care)');
    const mcp = checkbox('agent-mcp-enabled', values.mcpEnabled ?? true, 'Enable MCP servers');
    const maxIterations = input('agent-max-iterations', values.maxIterations ?? 100, 'number');
    const maxTool = input('agent-max-tool', values.maxToolResultSize ?? 10000, 'number');
    const maxRequest = input('agent-max-request', values.maxRequestSize ?? 4000000, 'number');
    const maxResponse = input('agent-max-response', values.maxResponseSize ?? 5000000, 'number');
    const disabledTools = document.createElement('textarea'); disabledTools.rows = 4; disabledTools.value = (values.disabledTools ?? []).join('\n');
    const disabledSkills = document.createElement('textarea'); disabledSkills.rows = 4; disabledSkills.value = (values.disabledSkills ?? []).join('\n');
    grid(field('Maximum iterations', maxIterations), field('Maximum tool result bytes', maxTool),
      field('Maximum request bytes', maxRequest), field('Maximum response bytes', maxResponse));
    content.append(auto, mcp, field('Disabled tools — one per line', disabledTools), field('Disabled skills — one per line', disabledSkills));
    actions(button('Save agent settings', () => post({ autoApprove: auto.querySelector('input').checked,
      agent: { maxIterations: Number(maxIterations.value), maxToolResultSize: Number(maxTool.value),
        maxRequestSize: Number(maxRequest.value), maxResponseSize: Number(maxResponse.value),
        mcpEnabled: mcp.querySelector('input').checked,
        disabledTools: disabledTools.value.split(/\r?\n/).map((value) => value.trim()).filter(Boolean),
        disabledSkills: disabledSkills.value.split(/\r?\n/).map((value) => value.trim()).filter(Boolean) } }), true));
  }

  function renderWebSearch() {
    heading('Web Search Failover Chain');
    content.append(node('p', 'Enabled providers are tried in priority order. Keys and passwords remain in the shared store.', 'settings-note'));
    for (const provider of agent.webSearch ?? []) {
      const section = node('section', '', 'settings-card');
      section.append(node('h4', provider.name));
      const enabled = checkbox(`search-${provider.name}-enabled`, provider.enabled, 'Enabled');
      const priority = input(`search-${provider.name}-priority`, provider.priority, 'number');
      const key = input(`search-${provider.name}-key`, '', 'password'); key.placeholder = provider.hasApiKey ? 'Saved key — leave blank to keep' : 'API key';
      const url = input(`search-${provider.name}-url`, provider.baseUrl, 'url');
      const username = input(`search-${provider.name}-username`, provider.username);
      const password = input(`search-${provider.name}-password`, '', 'password'); password.placeholder = provider.hasPassword ? 'Saved password — leave blank to keep' : 'Password';
      const form = node('div', '', 'settings-grid wide');
      form.append(field('Priority', priority), field('API key', key), field('SearXNG base URL', url),
        field('Username', username), field('Password', password));
      section.append(enabled, form, button('Save provider', () => post({ webSearchProvider: {
        name: provider.name, enabled: enabled.querySelector('input').checked,
        priority: Number(priority.value), apiKey: key.value, baseUrl: url.value.trim(),
        username: username.value, password: password.value,
      } }), true));
      content.append(section);
    }
  }

  function renderChimpFlow() {
    heading('ChimpFlow Voice');
    const values = agent.chimpflow ?? {};
    chimpflowEditing = false;
    const enabled = checkbox('chimpflow-enabled', values.enabled, 'Enable ChimpFlow');
    const read = checkbox('chimpflow-read', values.readAssistant ?? true, 'Read assistant responses aloud');
    const dictate = checkbox('chimpflow-dictation', values.dictationEnabled ?? true, 'Enable dictation');
    const url = input('chimpflow-url', values.serverUrl || 'http://127.0.0.1', 'url');
    const port = input('chimpflow-port', values.port ?? 8090, 'number');
    const key = input('chimpflow-key', '', 'password'); key.placeholder = values.hasApiKey ? 'Saved key — leave blank to keep' : 'API key (or CHIMPFLOW_KEY)';
    const currentModel = values.ttsModel || 'kokoro-82m';
    const modelOptions = chimpflowModelOptions.length ? chimpflowModelOptions : [currentModel];
    const tts = select('chimpflow-tts-model', modelOptions.map((value) => ({ value, label: value })), currentModel);
    const stt = input('chimpflow-stt-model', values.sttModel || 'whisper');
    const currentVoice = values.voice || '';
    const voiceOptions = chimpflowVoiceOptions.length ? chimpflowVoiceOptions : [currentVoice];
    const voice = select('chimpflow-voice', voiceOptions.map((value) => ({ value, label: value || 'Select a voice' })), currentVoice);
    grid(field('Server URL', url), field('Port', port), field('API key', key), field('TTS model', tts),
      field('STT model', stt), field('Voice', voice));
    content.append(enabled, read, dictate,
      node('p', 'Kokoro is the everyday default. Chatterbox is for quality-critical narration on suitable hardware. Pocket TTS is legacy only because it can produce garbled output.', 'settings-note'));

    const markEditing = () => { chimpflowEditing = true; };
    for (const control of [enabled, read, dictate, url, port, key, tts, stt, voice]) {
      control.addEventListener('input', markEditing);
      control.addEventListener('change', markEditing);
    }

    const formValues = () => ({
      enabled: enabled.querySelector('input').checked,
      readAssistant: read.querySelector('input').checked,
      dictationEnabled: dictate.querySelector('input').checked,
      serverUrl: url.value.trim(), port: Number(port.value), apiKey: key.value,
      ttsModel: tts.value, sttModel: stt.value.trim(), voice: voice.value,
    });

    const ids = (items) => (items ?? []).map((item) => typeof item === 'string' ? item : item.id ?? item.name).filter(Boolean);
    const populate = (control, values, preferred, fallback = '') => {
      const selected = values.includes(preferred) ? preferred
        : values.includes(fallback) ? fallback : values[0] ?? '';
      control.replaceChildren();
      for (const value of values) control.append(node('option', value));
      control.value = selected;
    };

    async function previewVoice() {
      if (!voice.value) { status('Select a ChimpFlow voice first.'); return; }
      previewButton.disabled = true;
      status('Generating voice preview…');
      await narration.speak('MonkeyByte voice preview. This is a local ChimpFlow preview.', formValues());
      previewButton.disabled = false;
    }

    const previewButton = button('Preview Voice', previewVoice);
    previewButton.disabled = !voice.value;
    voice.addEventListener('change', previewVoice);

    async function discover(refreshModels) {
      chimpflowEditing = true;
      loadButton.disabled = true;
      previewButton.disabled = true;
      status(refreshModels ? 'Refreshing ChimpFlow models and voices…' : `Loading voices for ${tts.value}…`);
      try {
        if (refreshModels) {
          const response = await api.chimpflowRequest('/v1/models', formValues());
          if (active !== 'ChimpFlow' || !content.contains(tts)) return;
          const models = ids(response.models ?? response);
          chimpflowModelOptions = models;
          populate(tts, models, tts.value, 'kokoro-82m');
        }
        const response = await api.chimpflowRequest('/v1/voices', formValues());
        if (active !== 'ChimpFlow' || !content.contains(tts)) return;
        const voices = ids(response.voices ?? response);
        chimpflowVoiceOptions = voices;
        populate(voice, voices, voice.value);
        status(`Discovered ${refreshModels ? `${tts.options.length} model(s) and ` : ''}${voice.options.length} voice(s) for ${tts.value}.`);
      } catch (error) {
        report(error);
      } finally {
        loadButton.disabled = false;
        previewButton.disabled = !voice.value;
      }
    }

    const loadButton = button('Refresh Models / Voices', () => discover(true));
    tts.addEventListener('change', () => discover(false));
    const saveButton = button('Save ChimpFlow', async () => {
      try {
        await post({ chimpflow: formValues() }, 'ChimpFlow settings saved and reloaded.');
      } catch (error) {
        status(`Failed to save: ${errorMessage(error)}`);
      }
    }, true);
    actions(previewButton, loadButton, saveButton);
  }

  function renderSearch() {
    heading('Search Exclusions');
    const excludes = document.createElement('textarea'); excludes.id = 'search-excludes'; excludes.rows = 12;
    excludes.value = (ui.searchExcludes ?? []).join('\n');
    content.append(field('Glob patterns — one per line', excludes));
    actions(button('Save exclusions', () => saveSettings({ searchExcludes: excludes.value.split(/\r?\n/).map((value) => value.trim()).filter(Boolean) }), true));
  }

  function renderPaths() {
    heading('Paths');
    grid(field('Daedalus home', input('daedalus-home-path', agent.paths?.home ?? '(system default)').cloneNode()),
      field('Global skills', input('global-skills-path', agent.paths?.globalSkills ?? '').cloneNode()));
    for (const control of content.querySelectorAll('input')) control.readOnly = true;
    content.append(node('p', 'Skills remain editable SKILL.md directories. Project sessions live below the current workspace; provider, model, MCP, prompt, and policy settings use the shared Daedalus store.', 'settings-note'));
  }

  function render() {
    if (active === 'ChimpFlow' && chimpflowEditing) return;
    content.replaceChildren();
    if (active === 'Interface') renderInterface();
    else if (active === 'Providers') renderProviders();
    else if (active === 'Models') renderModels();
    else if (active === 'Workspace') renderPrompts();
    else if (active === 'MCP Servers') renderMcp();
    else if (active === 'Agent') renderAgent();
    else if (active === 'Web Search') renderWebSearch();
    else if (active === 'ChimpFlow') renderChimpFlow();
    else if (active === 'Search') renderSearch();
    else if (active === 'Paths') renderPaths();
  }

  async function refreshAgent() {
    try {
      agent = await api.agentRequest('/v1/settings');
      projectData = await api.projectTypesList();
      render();
    }
    catch (error) { report(error); }
  }

  api.onAgentEvent((type, payload) => {
    if (type === 'settings') { agent = payload; render(); }
  });

  return {
    update(settings) { ui = settings; render(); },
    refreshAgent,
  };
}
