// SPDX-License-Identifier: MIT
// TINYOS: a small OS shell written only in JavaScript. Copy this folder and change it.
//
// The shell knows one idea, a "view": { rows(): string[], sel, key(name) -> true if it used the key }.
// The home screen is a view; so is every program in MODS/. A program is a folder with APP.JSON (its name)
// is a file here, NAME.JS: its first line is `//! Name shown on home`, the rest is the body of a function that
// gets `os` and returns a view. Below its own pages the home lists the real apps in /OS/APPS (apps.list(), capability
// apps.launch) and Enter on one starts it (apps.launch); an app that calls apps.exit() brings you back here. Files
// stay flat (no subfolders) so `usb_kernel.py putapp` can install the folder.
const HOME = '/sd/OS/APPS/TINYOS', SAVE = '/sd/OS/SAVES/TINYOS/SETTINGS.CFG';
const THEMES = [
  { name: 'Night', bg: '#101828', ink: '#ffffff', hi: '#8de0cf', hiInk: '#101828' },
  { name: 'Paper', bg: '#eee8dc', ink: '#182838', hi: '#245c50', hiInk: '#ffffff' },
];
const ROWS = 12;

// ---- settings: a small JSON file in the save folder ----
const settings = { theme: 0, volume: 6 };
try { Object.assign(settings, JSON.parse(fs.readFile(SAVE))); } catch (e) { /* first run: defaults */ }
const saveSettings = () => fs.write(SAVE, JSON.stringify(settings));

// ---- sound: open, play, close when done (the device has one writer at a time) ----
let sound = null;
function play(notes) {
  if (!sandbox.has('audio') || settings.volume === 0) return;
  let a;
  try { a = audio.open(); } catch (e) { return; }   // another program may hold it
  if (sound) sound.close();
  sound = a;
  a.volume = settings.volume / 10;
  a.melody(notes);
  const ms = notes.reduce((n, x) => n + x[1], 0);
  setTimeout(() => console.log('TINYOS audio underruns ' + a.stats().underruns), ms - 50);   // before the notes run out
  setTimeout(() => { if (sound === a) { a.close(); sound = null; } }, ms + 100);   // closing drops what is still queued, so wait a little
}

// ---- the screen: a title, 12 text rows, a hint ----
screen.setMode(640, 360, 16);
function text(top, size) {
  const el = document.createElement('text');
  Object.assign(el.style, { position: 'absolute', left: 32, top, width: 576, height: size + 6, fontSize: size });
  document.body.appendChild(el);
  return el;
}
const title = text(16, 24), hint = text(328, 14);
const rows = Array.from({ length: ROWS }, (_, i) => text(52 + i * 22, 18));

let view, stack = [];
function render() {
  const th = THEMES[settings.theme];
  document.body.style.background = th.bg;
  title.style.color = hint.style.color = th.hi;
  title.textContent = view.title;
  hint.textContent = view.hint || 'Up/Down move   Enter open   Esc back';
  const lines = view.rows(), top = Math.max(0, Math.min(view.sel - ROWS + 1, lines.length - ROWS));
  rows.forEach((el, i) => {
    const at = top + i, on = at === view.sel;
    el.textContent = lines[at] === undefined ? '' : (on ? '> ' : '  ') + lines[at];
    el.style.color = on ? th.hiInk : th.ink;
    el.style.background = on ? th.hi : th.bg;
  });
}
function go(v) { stack.push(view); view = v; render(); }
function back() { if (stack.length) { view.close && view.close(); view = stack.pop(); render(); } }

// ---- programs: every file here except MAIN.JS whose first line is `//! Name` ----
const os = { fs, sandbox, settings, saveSettings, play, THEMES, HOME, render, back };
const mods = [];
for (const e of fs.readdir(HOME, { meta: true })) {
  if (e.type !== 'file' || !/\.JS$/i.test(e.name) || /^MAIN\.JS$/i.test(e.name)) continue;
  const first = /^\/\/! (.{1,24})\n/.exec(fs.readFile(HOME + '/' + e.name));
  if (first) mods.push({ file: e.name, name: first[1] });
}
mods.sort((a, b) => (a.name < b.name ? -1 : 1));
const open = (m) => {
  const v = new Function('os', fs.readFile(HOME + '/' + m.file))(os);
  v.title = m.name;
  go(v);
};

function release() { if (sound) sound.close(); sound = null; if (panel) panel.release(); }   // give back what we hold
function quit() { release(); sys.exit(0); }
// ---- apps: every other app on the card; one that cannot run stays listed, with the reason on the hint line ----
const found = sandbox.has('apps.launch') ? apps.list().filter((a) => a.id !== sandbox.app.id).sort((a, b) => (a.name.toLowerCase() < b.name.toLowerCase() ? -1 : 1)) : [];
const items = mods.map((m) => ({ page: m })).concat(found.map((a) => ({ app: a })));
const failed = sandbox.launchError;
let note = failed ? 'Could not start ' + failed.id + ': ' + failed.reason : '';
const cameFrom = sandbox.returned && items.findIndex((i) => i.app && i.app.id === sandbox.returned.id);
const home = { title: 'TinyOS', sel: cameFrom > 0 ? cameFrom : 0,
  get hint() { const it = items[home.sel]; return note || (it && it.app && !it.app.ok ? it.app.name + ': ' + it.app.reason : 'Up/Down move   Enter open   Q quit'); },
  rows: () => items.map((i) => (i.page ? i.page.name : i.app.name + '   app' + (i.app.ok ? '' : ' (cannot run)'))),
  key(k) {
    note = '';
    if (k === 'ArrowDown') home.sel = Math.min(home.sel + 1, items.length - 1);
    else if (k === 'ArrowUp') home.sel = Math.max(home.sel - 1, 0);
    else if (k === 'Enter' && items.length) {
      const it = items[home.sel];
      if (it.page) open(it.page);
      else if (it.app.ok) { release(); apps.launch(it.app.id); }
      else note = it.app.name + ': ' + it.app.reason;
    } else if (k === 'KeyQ') quit();
    else return false;
    return true;
  } };
view = home;

document.addEventListener('keydown', (ev) => {
  const k = ev.key === 'Escape' ? 'Escape' : ev.code === 'KeyQ' ? 'KeyQ' : ev.key;
  ev.preventDefault();
  if (view.key && view.key(k)) render();
  else if (k === 'Escape') back();
});
render();

// ---- startup chime ----
if (!sandbox.returned) play([[523, 90], [659, 90], [784, 90], [1047, 220]]);   // not again when an app brings you back

// ---- e-ink: uptime as HH:MM in big digits, if the panel exists (about 5x7 dots at 6x) ----
const DIGITS = { 0: '01110100011001110101110011000101110', 1: '00100011000010000100001000010001110', 2: '01110100010000100010001000100011111',
  3: '11110000010000101110000010000111110', 4: '00010001100101010010111110001000010', 5: '11111100001111000001000011000101110',
  6: '00110010001000011110100011000101110', 7: '11111000010001000100010000100001000', 8: '01110100011000101110100011000101110',
  9: '01110100011000101111000010001001100', ':': '00000001000010000000001000010000000' };
const panel = sandbox.has('eink') ? eink.open() : null;
function drawClock() {
  const c = panel.canvas(), up = Math.floor(sandbox.now() / 60e6), s = String(Math.floor(up / 60) % 100).padStart(2, '0') + ':' + String(up % 60).padStart(2, '0');
  c.clear();
  [...s].forEach((ch, i) => { const g = DIGITS[ch]; for (let p = 0; p < 35; p++) if (g[p] === '1') c.fillRect(35 + i * 36 + (p % 5) * 6, 40 + Math.floor(p / 5) * 6, 6, 6); });
  c.flush();
}
if (panel) { drawClock(); setInterval(drawClock, 30000); }   // the panel allows 30 refreshes a minute; this uses 2
console.log('TINYOS ready, ' + mods.length + ' programs, ' + found.length + ' apps');
