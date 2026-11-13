//! Files
// Files: TinyOS's own folder (all an app may read). Enter reads a file, Esc closes it, then goes home.
let text = null, at = 0;
const list = os.fs.readdir(os.HOME, { meta: true });
const v = { sel: 0,
  rows: () => text || list.map((e) => e.name + '   ' + e.size),
  key(k) {
    if (text) { if (k !== 'Escape') return false; text = null; v.sel = at; return true; }
    if (k === 'ArrowDown') v.sel = Math.min(v.sel + 1, list.length - 1);
    else if (k === 'ArrowUp') v.sel = Math.max(v.sel - 1, 0);
    else if (k === 'Enter' && list[v.sel]) { at = v.sel; v.sel = -1; text = os.fs.readFile(os.HOME + '/' + list[at].name).split('\n').map((l) => l.slice(0, 64)); }
    else return false;
    return true;
  } };
return v;
