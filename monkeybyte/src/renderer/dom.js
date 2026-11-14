export function byId(id) {
  const element = document.getElementById(id);
  if (!element) {
    throw new Error(`Missing UI element #${id}`);
  }
  return element;
}

export function message(text, className = 'muted-message') {
  const paragraph = document.createElement('p');
  paragraph.className = className;
  paragraph.textContent = text;
  return paragraph;
}

export function errorMessage(error) {
  return error instanceof Error ? error.message : String(error);
}
