// The JS runtime spike's benchmarks (docs/js.md): objects, a DOM-style grid of
// six cards against the same grid on a canvas, and the game element playing a
// game for 30 s while JS works every frame. Each result is one log line
// starting "BENCH". The second line of /OS/AUTORUN.TXT picks this app
// (APPS/BENCH).
const results = [];
function out(name, value) {
  const line = 'BENCH ' + name + ' ' + value;
  results.push(line);
  console.log(line);
}
const delta = (a, b) => ({
  js: b.jsUs - a.jsUs,
  render: b.renderUs - a.renderUs,
  present: b.presentUs - a.presentUs,
  frames: b.frames - a.frames,
  pixels: b.pixels - a.pixels,
  rects: b.rects - a.rects,
});
const ms = (us, n) => (us / n / 1000).toFixed(3);
function perFrame(name, d) {
  out(name, `${ms(d.js + d.render + d.present, d.frames)} ms/frame: js ${ms(d.js, d.frames)} paint ${ms(d.render, d.frames)} present ${ms(d.present, d.frames)}; ${Math.round(d.pixels / d.frames)} px; n=${d.frames}`);
}

// 1. A thousand objects moved and bounced, as a game's update would.
function objects() {
  const things = [];
  for (let i = 0; i < 1000; i++) things.push({ x: (i * 37) % 640, y: (i * 91) % 480, vx: (i % 7) - 3, vy: (i % 5) - 2 });
  const step = () => {
    for (const t of things) {
      t.x += t.vx;
      t.y += t.vy;
      if (t.x < 0 || t.x > 639) t.vx = -t.vx;
      if (t.y < 0 || t.y > 479) t.vy = -t.vy;
    }
  };
  for (let i = 0; i < 10; i++) step();
  const n = 200, t0 = performance.now();
  for (let i = 0; i < n; i++) step();
  out('objects', `1000-object update ${((performance.now() - t0) / n).toFixed(3)} ms`);
  return step;
}
const step = objects();
const mem = sys.memory();
out('memory', `${mem.mallocSize} bytes in ${mem.mallocCount} blocks, ${mem.objects} objects after start`);

// 2. Six cards: a picture (a game's title screen) and a title each.
let games = [];
try {
  games = fs.readdir('/sd/GAMES');
} catch (e) {}
const six = [];
for (let i = 0; i < 6; i++) six.push(games.length ? games[i % games.length] : '');
document.body.style.background = '#0b1020';
document.body.innerHTML =
  '<div style="padding:8px; background:#1d2b53"><text style="font-size:18px; color:#ffcc33">Games</text></div>' +
  '<div id="grid" style="display:flex; flex-direction:column; gap:10px; padding:10px"></div>';
const grid = document.getElementById('grid');
const cards = [];
for (let r = 0; r < 2; r++) {
  const row = document.createElement('div');
  row.style.display = 'flex';
  row.style.flexDirection = 'row';
  row.style.gap = 10;
  for (let c = 0; c < 3; c++) {
    const name = six[r * 3 + c];
    const card = document.createElement('button');
    card.style.width = 200;
    card.style.textAlign = 'left';
    const pic = document.createElement('img');
    if (name) pic.src = '/sd/GAMES/' + name;
    pic.style.width = 188;
    pic.style.height = 118;
    const label = document.createElement('text');
    label.textContent = name || 'none';
    card.appendChild(pic);
    card.appendChild(label);
    row.appendChild(card);
    cards.push({ card, label, pic, name });
  }
  grid.appendChild(row);
}
cards[0].card.focus();

const phases = [];
let frame = 0, before = null;
function phase(name, frames, each) {
  phases.push({ name, frames, each });
}
let canvas = null, ctx = null;
function grids(mode) {
  const tag = mode.join('x') + ' ';
  phase('mode ' + tag, 1, () => {
    if (canvas) {
      canvas.remove();
      canvas = null;
    }
    grid.style.display = 'block';
    const us = screen.setMode(mode[0], mode[1], mode[2]);
    out('switch', `${tag}in ${us} us`);
  });
  // A key press a frame: the focus moves, two cards repaint.
  phase(tag + 'dom-keypress', 120, (i) => cards[(i + 1) % 6].card.focus());
  // Every card changes every frame: all six repaint.
  phase(tag + 'dom-all-cards', 120, (i) => {
    for (const c of cards) c.label.textContent = (c.name || 'none') + ' ' + (i % 10);
  });
  // A full-screen repaint a frame (the worst case for the tree).
  phase(tag + 'dom-full-screen', 60, (i) => {
    document.body.style.background = i % 2 ? '#0b1020' : '#0c1122';
  });
  // The same grid drawn on a canvas each frame (immediate mode).
  phase(tag + 'canvas-grid', 120, (i) => {
    if (!canvas) {
      grid.style.display = 'none';
      canvas = document.createElement('canvas');
      canvas.width = 640;
      canvas.height = 300;
      canvas.style.position = 'absolute';
      canvas.style.top = 40;
      document.body.appendChild(canvas);
      ctx = canvas.getContext('2d');
    }
    ctx.fillStyle = '#0b1020';
    ctx.fillRect(0, 0, 640, 300);
    cards.forEach((c, k) => {
      const x = 10 + (k % 3) * 210, y = 10 + Math.floor(k / 3) * 150;
      ctx.fillStyle = k === i % 6 ? '#ffd400' : '#8a96b8';
      ctx.fillRect(x, y, 200, 140);
      ctx.fillStyle = '#0b1020';
      ctx.fillRect(x + 2, y + 2, 196, 136);
      ctx.drawImage(c.pic, x + 6, y + 6, 188, 118);
      ctx.fillStyle = '#ffffff';
      ctx.fillText(c.name || 'none', x + 6, y + 127);
    });
  });
}
grids([640, 480, 32]);
grids([640, 480, 16]);
grids([960, 540, 16]);
phase('back', 1, () => {
  if (canvas) canvas.remove();
  out('switch', `640x480x32 in ${screen.setMode(640, 480, 32)} us`);
});

function run(ts) {
  const p = phases[0];
  if (!p) return play();
  if (frame === 0) before = sys.stats();
  else p.each(frame - 1);
  if (frame++ === p.frames) {
    perFrame(p.name, delta(before, sys.stats()));
    phases.shift();
    frame = 0;
  }
  requestAnimationFrame(run);
}
requestAnimationFrame(run);

// 5. The game element plays for 30 s; JS moves 1000 objects every frame meanwhile.
function play() {
  const name = six[0];
  if (!name) {
    out('game', 'no game in /sd/GAMES');
    return finish();
  }
  document.body.innerHTML = '';
  const game = document.createElement('game');
  game.style.position = 'absolute';
  game.src = '/sd/GAMES/' + name;
  document.body.appendChild(game);
  screen.gameLowres(true, 384, 216); // 1080p / 5: the 320x200 picture at exactly 5x on a 1080p screen
  game.play();
  out('switch', `game 384x216x8 in ${screen.switchUs} us`);
  // Keys as a player might: Enter through the title pages, then walk right.
  const keys = [[500, 28, 1], [600, 28, 0], [3000, 28, 1], [3100, 28, 0], [6000, 28, 1], [6100, 28, 0], [8000, 106, 1], [20000, 106, 0], [20500, 105, 1], [26000, 105, 0]];
  for (const [at, code, down] of keys) setTimeout(() => game.press(code, down), at);
  let frames = 0, js = 0;
  const start = sys.stats(), t0 = performance.now();
  const work = () => {
    const a = performance.now();
    step();
    js += performance.now() - a;
    frames++;
    if (performance.now() - t0 < 30000 && game.playing) requestAnimationFrame(work);
    else {
      const s = sys.stats(), secs = (performance.now() - t0) / 1000;
      out('game', `${name}: ${s.gameTicks} ticks in ${(s.gamePlayMs / 1000).toFixed(1)} s of play (18.2 Hz wants ${Math.round((s.gamePlayMs / 1000) * 18.2065)}), late max ${s.gameLateMaxUs} us, step max ${s.gameStepMaxUs} us`);
      out('game-js', `${frames} JS frames alongside (${(frames / secs).toFixed(1)} a second), 1000-object update ${(js / frames).toFixed(3)} ms each`);
      perFrame('game-loop', delta(start, s));
      game.stop();
      out('switch', `back to ${screen.width}x${screen.height}x${screen.depth} in ${screen.switchUs} us`);
      finish();
    }
  };
  requestAnimationFrame(work);
}
function finish() {
  out('done', results.length + ' results');
  setTimeout(() => sys.exit(0), 200);
}
