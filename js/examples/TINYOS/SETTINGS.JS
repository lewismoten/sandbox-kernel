//! Settings
// Settings: theme and volume; Left/Right change the highlighted row. Saved when you leave.
const s = os.settings;
const v = { sel: 0, hint: 'Up/Down choose   Left/Right change   Esc back',
  rows: () => ['Theme   ' + os.THEMES[s.theme].name, 'Volume  ' + '#'.repeat(s.volume) + '.'.repeat(10 - s.volume)],
  key(k) {
    const d = k === 'ArrowRight' ? 1 : k === 'ArrowLeft' ? -1 : 0;
    if (k === 'ArrowDown') v.sel = 1; else if (k === 'ArrowUp') v.sel = 0;
    else if (d && v.sel === 0) s.theme = (s.theme + d + os.THEMES.length) % os.THEMES.length;
    else if (d) { s.volume = Math.max(0, Math.min(10, s.volume + d)); os.play([[784, 80]]); }
    else return false;
    return true;
  },
  close: os.saveSettings };
return v;
