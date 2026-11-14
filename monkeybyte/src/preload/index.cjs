const { contextBridge, ipcRenderer, webUtils } = require('electron');

const api = Object.freeze({
  getBootstrap: () => ipcRenderer.invoke('mb:bootstrap'),
  chooseWorkspace: () => ipcRenderer.invoke('mb:workspace:choose'),
  listDirectory: (path, showHidden) => ipcRenderer.invoke('mb:workspace:list', path, showHidden),
  readTextFile: (root, path) => ipcRenderer.invoke('mb:file:read', root, path),
  writeTextFile: (root, path, content) => ipcRenderer.invoke('mb:file:write', root, path, content),
  createFolder: (root, path) => ipcRenderer.invoke('mb:workspace:create-folder', root, path),
  createFile: (root, path, content) => ipcRenderer.invoke('mb:workspace:create-file', root, path, content),
  importFiles: (root, destination) => ipcRenderer.invoke('mb:workspace:import', root, destination),
  renamePath: (root, oldPath, newPath) => ipcRenderer.invoke('mb:workspace:rename', root, oldPath, newPath),
  deletePath: (root, path) => ipcRenderer.invoke('mb:workspace:delete', root, path),
  copyPath: (root, path, absolute = true) => ipcRenderer.invoke('mb:workspace:copy-path', root, path, absolute),
  openExternal: (root, path, reveal = false) => ipcRenderer.invoke('mb:workspace:external', root, path, reveal),
  openTerminal: (root, path) => ipcRenderer.invoke('mb:workspace:terminal', root, path),
  searchWorkspace: (root, options) => ipcRenderer.invoke('mb:search', root, options),
  gitStatus: (root) => ipcRenderer.invoke('mb:git:status', root),
  filePathFor: (file) => webUtils.getPathForFile(file),
  agentRequest: (path, options = {}) => ipcRenderer.invoke('mb:agent:request', path, options),
  chimpflowRequest: (path, settings = null) => path === '/v1/voices'
    ? ipcRenderer.invoke('mb:chimpflow:voices', settings)
    : path === '/v1/models'
      ? ipcRenderer.invoke('mb:chimpflow:models', settings)
      : Promise.reject(new Error('Unsupported ChimpFlow route.')),
  chimpflowSpeak: (text, settings = null) => ipcRenderer.invoke('mb:chimpflow:speak', text, settings),
  chimpflowTranscribe: (base64, mime) => ipcRenderer.invoke('mb:chimpflow:transcribe', base64, mime),
  chooseChatAttachments: () => ipcRenderer.invoke('mb:chat:attachments'),
  onAgentEvent: (callback) => {
    const listener = (_event, type, payload) => callback(type, payload);
    ipcRenderer.on('mb:agent:event', listener);
    return () => ipcRenderer.removeListener('mb:agent:event', listener);
  },
  onHostCommand: (callback) => {
    const listener = (_event, request) => callback(request);
    ipcRenderer.on('mb:host:command', listener);
    return () => ipcRenderer.removeListener('mb:host:command', listener);
  },
  hostCommandResult: (id, result) => ipcRenderer.invoke('mb:host:result', id, result),
  browserExecute: (command, args = {}) => ipcRenderer.invoke('mb:browser:execute', command, args),
  browserBounds: (bounds) => ipcRenderer.invoke('mb:browser:bounds', bounds),
  browserViewport: (viewport) => ipcRenderer.invoke('mb:browser:viewport', viewport),
  projectTypesList: () => ipcRenderer.invoke('mb:project-types:list'),
  projectTypeApply: (id) => ipcRenderer.invoke('mb:project-types:apply', id),
  projectTypeCreate: (id) => ipcRenderer.invoke('mb:project-types:create', id),
  projectTypeSave: (id, prompt) => ipcRenderer.invoke('mb:project-types:save', id, prompt),
  exportFountain: (root, path, destination) => ipcRenderer.invoke('mb:fountain:export', root, path, destination),
  previewFile: (root, path, mode) => ipcRenderer.invoke('mb:preview', root, path, mode),
  onBrowserChanged: (callback) => {
    const listener = (_event, metadata) => callback(metadata);
    ipcRenderer.on('mb:browser:changed', listener);
    return () => ipcRenderer.removeListener('mb:browser:changed', listener);
  },
  updateSettings: (patch) => ipcRenderer.invoke('mb:settings:update', patch),
  confirmClose: (name) => ipcRenderer.invoke('mb:confirm-close', name),
});

contextBridge.exposeInMainWorld('monkeybyte', api);
