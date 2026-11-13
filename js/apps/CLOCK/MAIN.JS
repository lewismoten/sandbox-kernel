// SPDX-License-Identifier: MIT
// CLOCK: a page built with the small DOM-like API, not a web browser.
// performance.now() is uptime since the JS runtime started, not since boot.
// It is NOT a civil/real-time clock.
screen.setMode(960, 540, 16);

const themes = [
  { name: 'Night', background: '#101828', panel: '#1d2939', ink: '#ffffff', accent: '#8de0cf' },
  { name: 'Paper', background: '#eee8dc', panel: '#ffffff', ink: '#182838', accent: '#245c50' },
];
let themeIndex = 0;

// Absolute pixel boxes make the layout easy to change; this API has no CSS
// stylesheet, percentage sizes or automatic text wrapping.
function box(tag, id, x, y, width, height, text, fontSize) {
  const el = document.createElement(tag);
  el.id = id;
  el.style.position = 'absolute';
  el.style.left = x;
  el.style.top = y;
  el.style.width = width;
  el.style.height = height;
  el.style.fontSize = fontSize;
  el.textContent = text;
  document.body.appendChild(el);
  return el;
}

const title = box('text', 'title', 48, 36, 864, 45, 'CLOCK', 36);
const subtitle = box('text', 'subtitle', 48, 92, 864, 27, 'A small JavaScript uptime clock', 18);
const panel = box('div', 'panel', 48, 152, 864, 232, '', 18);
const caption = box('text', 'caption', 72, 176, 816, 27, 'APP UPTIME  /  HOURS : MINUTES : SECONDS', 18);
const clock = box('text', 'uptime', 72, 224, 816, 54, '', 36);
clock.style.textAlign = 'center';
const date = box('text', 'date-gap', 72, 304, 816, 27, 'Date: unavailable (no civil-clock API)', 18);
const note = box('text', 'clock-note', 72, 340, 816, 27, 'Elapsed time only; hours do not wrap at 24.', 18);
const themeButton = box('button', 'theme', 48, 424, 360, 48, '', 18);
themeButton.style.outlineColor = '#d39400';
const controls = box('text', 'controls', 432, 436, 480, 27, 'Enter: theme   Esc: back', 18);
const footer = box('text', 'footer', 48, 496, 864, 27, 'Edit MAIN.JS to make it yours. No browser required.', 18);

function applyTheme() {
  const theme = themes[themeIndex];
  document.body.style.background = theme.background;
  panel.style.background = theme.panel;
  for (const el of [title, subtitle, caption, clock, date, note, controls, footer]) {
    el.style.color = theme.ink;
  }
  clock.style.color = theme.accent;
  themeButton.style.background = theme.panel;
  themeButton.style.color = theme.ink;
  themeButton.textContent = 'Theme: ' + theme.name;
}

// Only replace the text when a whole second changes. Derive it from the
// clock, not a counter: a delayed callback must not make the clock run slow.
let previousSecond = -1;
function updateClock() {
  const seconds = Math.floor(performance.now() / 1000);
  if (seconds === previousSecond) return;
  previousSecond = seconds;
  const hours = String(Math.floor(seconds / 3600)).padStart(2, '0');
  const minutes = String(Math.floor(seconds / 60) % 60).padStart(2, '0');
  const remainder = String(seconds % 60).padStart(2, '0');
  clock.textContent = hours + ':' + minutes + ':' + remainder;
}

function switchTheme() {
  themeIndex = (themeIndex + 1) % themes.length;
  applyTheme();
}
themeButton.addEventListener('click', switchTheme);
document.addEventListener('keydown', (event) => {
  if ((event.code === 'KeyQ' || event.key === 'Escape') && !event.repeat) { sys.exit(0); return; }
  if (event.code === 'KeyT') {
    event.preventDefault();
    if (!event.repeat) switchTheme();
  }
});
// The native default turns Enter and Space on a focused button into clicks.
themeButton.focus();
applyTheme();
updateClock();
setInterval(updateClock, 100);
console.log('CLOCK ready');
