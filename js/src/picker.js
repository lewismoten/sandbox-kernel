// SPDX-License-Identifier: MIT
// Built into JS.ELF: the ABI has no exec/spawn and GMOS only plays native games.
(() => {
  screen.setMode(640, 480, 32);
  const names = fs.readdir('/sd/OS/APPS').filter(name => {
    if (!/^[A-Z0-9_-]{1,8}$/.test(name)) return false;
    try { fs.readFile('/sd/OS/APPS/' + name + '/MAIN.JS'); return true; }
    catch { return false; }
  }).sort();
  let selected = 0;
  const rows = [];
  function row(text) {
    const el = document.createElement('text');
    el.textContent = text;
    el.style.height = 32;
    el.style.fontSize = 18;
    document.body.appendChild(el);
    rows.push(el); // retain wrappers, including styled nodes, until teardown
    return el;
  }
  document.body.style.padding = 20;
  row('JS apps');
  row('Up/Down: choose   Enter: launch   Esc: quit');
  const slots = Array.from({length: 8}, () => row(''));
  function draw() {
    const first = Math.floor(selected / 8) * 8;
    slots.forEach((el, i) => {
      const n = first + i;
      el.textContent = n < names.length ? (n === selected ? '> ' : '  ') + names[n] : '';
    });
    if (!names.length) slots[0].textContent = 'No MAIN.JS apps in /OS/APPS';
  }
  document.addEventListener('keydown', e => {
    if (e.repeat) return;
    if (e.key === 'Escape') { sys.exit(0); return; }
    if (!names.length) return;
    if (e.key === 'ArrowDown') selected = (selected + 1) % names.length;
    else if (e.key === 'ArrowUp') selected = (selected + names.length - 1) % names.length;
    else if (e.key === 'Enter') { sys.launch(names[selected]); return; }
    draw();
  });
  draw();
  console.log('app picker', names.join(','));
})();
