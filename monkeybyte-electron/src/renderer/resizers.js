import { byId } from './dom.js';

export function createResizers({ onPreview, onCommit }) {
  function attach(id, key, minimum, maximum, direction) {
    const grip = byId(id);
    let drag = null;

    function commit() {
      if (drag) {
        onCommit({ [key]: drag.width });
        drag = null;
      }
      grip.classList.remove('dragging');
    }

    grip.addEventListener('pointerdown', (event) => {
      const app = byId('app');
      const current = key === 'sidebarWidth'
        ? app.querySelector('#sidebar').getBoundingClientRect().width
        : app.querySelector('#chat-panel').getBoundingClientRect().width;
      drag = { startX: event.clientX, startWidth: current, width: current };
      grip.setPointerCapture(event.pointerId);
      grip.classList.add('dragging');
      event.preventDefault();
    });
    grip.addEventListener('pointermove', (event) => {
      if (!drag) {
        return;
      }
      const delta = (event.clientX - drag.startX) * direction;
      drag.width = Math.round(Math.min(maximum, Math.max(minimum, drag.startWidth + delta)));
      onPreview({ [key]: drag.width });
    });
    grip.addEventListener('pointerup', commit);
    grip.addEventListener('pointercancel', commit);
    grip.addEventListener('keydown', (event) => {
      if (!['ArrowLeft', 'ArrowRight'].includes(event.key)) {
        return;
      }
      event.preventDefault();
      const app = byId('app');
      const current = key === 'sidebarWidth'
        ? app.querySelector('#sidebar').getBoundingClientRect().width
        : app.querySelector('#chat-panel').getBoundingClientRect().width;
      const sign = event.key === 'ArrowRight' ? 1 : -1;
      const width = Math.round(Math.min(maximum, Math.max(minimum, current + sign * 16)));
      onPreview({ [key]: width });
      onCommit({ [key]: width });
    });
  }

  attach('sidebar-resizer', 'sidebarWidth', 150, 500, 1);
  attach('chat-resizer', 'chatWidth', 250, 900, -1);
}
