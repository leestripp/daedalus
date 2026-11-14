import { byId } from './dom.js';

const PANELS = ['workspace', 'search', 'git', 'settings'];

export function createNavigation(onPanelChange) {
  const app = byId('app');
  const buttons = [...document.querySelectorAll('.activity-button')];
  let activePanel = 'workspace';

  function setActive(panel, notify = false) {
    if (!PANELS.includes(panel)) {
      return;
    }
    activePanel = panel;
    for (const name of PANELS) {
      byId(`panel-${name}`).hidden = name !== panel;
    }
    for (const button of buttons) {
      button.setAttribute('aria-pressed', String(button.dataset.panel === panel));
    }
    app.classList.toggle('settings-active', panel === 'settings');
    if (notify) {
      onPanelChange(panel);
    }
  }

  for (const button of buttons) {
    button.addEventListener('click', () => setActive(button.dataset.panel, true));
  }
  document.addEventListener('keydown', (event) => {
    if (!event.ctrlKey || event.altKey || event.shiftKey || event.metaKey) {
      return;
    }
    const index = Number.parseInt(event.key, 10) - 1;
    if (index >= 0 && index < PANELS.length) {
      event.preventDefault();
      setActive(PANELS[index], true);
    }
  });

  return { setActive, getActive: () => activePanel };
}
