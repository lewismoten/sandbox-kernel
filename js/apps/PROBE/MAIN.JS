// Host test probe for the JS runtime (tests/test_js.py): each check logs
// "PASS <name>" or "FAIL <name>: <detail>"; keys come from the test.
const check = (name, ok, detail) => console.log((ok ? 'PASS ' : 'FAIL ') + name + (ok ? '' : ': ' + detail));
const body = document.body;
body.innerHTML =
  '<div id="bar" class="top wide" style="display:flex; flex-direction:row; gap:4px; padding:2px">' +
  '<button id="a">A</button><button id="b">B</button><button id="c">C</button></div>' +
  '<text id="t" style="color:#ff0000">hello &amp; bye</text>';
check('innerHTML', document.getElementById('b').textContent === 'B' && body.children.length === 2, body.children.length);
check('entities', document.getElementById('t').textContent === 'hello & bye', document.getElementById('t').textContent);
check('class query', document.querySelector('.wide') === document.getElementById('bar'), 'no match');
check('tag query', document.querySelectorAll('button').length === 3, document.querySelectorAll('button').length);
const t = document.getElementById('t');
t.style.left = 12;
check('px number', t.style.left === '12px', t.style.left);
t.style.color = '#abc';
check('short color', t.style.color === '#aabbcc', t.style.color);
t.style.width = '50%';
check('outside subset ignored', t.style.width === 'auto', t.style.width);
check('parent', t.parentNode === body && body.parentNode === null, 'parent');
const r = document.getElementById('b').getBoundingClientRect();
const ra = document.getElementById('a').getBoundingClientRect();
check('flex row layout', r.x > ra.x + ra.width && r.y === ra.y, JSON.stringify([ra, r]));
const log = [];
document.getElementById('a').focus();
document.getElementById('bar').addEventListener('keydown', (e) => log.push('bar:' + e.key));
document.addEventListener('keydown', (e) => log.push('doc:' + e.key));
document.getElementById('b').addEventListener('click', () => log.push('click b'));
document.getElementById('c').addEventListener('keydown', (e) => {
  if (e.key === 'ArrowLeft') e.preventDefault();
});
const kept = [];
for (let i = 0; i < 200; i++) {
  const d = document.createElement('div');
  d.textContent = 'x' + i;
  body.appendChild(d);
  kept.push(d);
}
for (const d of kept) d.remove();
kept.length = 0;
const order = [];
setTimeout(() => order.push(2), 20);
setTimeout(() => order.push(1), 10);
let frames = 0, firstTs = -1;
const tick = (ts) => {
  if (firstTs < 0) firstTs = ts;
  if (++frames < 30) requestAnimationFrame(tick);
};
requestAnimationFrame(tick);
const cv = document.createElement('canvas');
cv.width = 8;
cv.height = 8;
const ctx = cv.getContext('2d');
ctx.fillStyle = '#00ff00';
ctx.fillRect(0, 0, 8, 8);
cv.style.position = 'absolute';
cv.style.left = 600;
cv.style.top = 400;
body.appendChild(cv);
check('readFile', fs.readFile('/sd/OS/APPS/PROBE/MAIN.JS').startsWith('// Host test probe'), 'start');
check('readdir', fs.readdir('/sd/OS/APPS').includes('PROBE'), fs.readdir('/sd/OS/APPS'));
try {
  document.createElement('table');
  check('unknown tag throws', false, 'no throw');
} catch (e) {
  check('unknown tag throws', e instanceof TypeError, e);
}
// Keys (from the test): Right at 100 ms, Enter at 200, Right at 300, Left at 400.
setTimeout(() => {
  check('arrow moves focus', document.activeElement.id === 'b', document.activeElement.id);
  check('bubbling', log.join() === 'bar:ArrowRight,doc:ArrowRight', log.join());
}, 150);
setTimeout(() => check('enter clicks', log.includes('click b'), log.join()), 250);
setTimeout(() => {
  check('preventDefault keeps focus', document.activeElement.id === 'c', document.activeElement.id);
  check('timers in order', order.join() === '1,2', order.join());
  check('frames ran', frames === 30 && firstTs >= 0, frames);
  const m = sys.memory();
  check('removed elements freed', m.objects < 1500, m.objects);
  console.log('DONE');
  sys.exit(0);
}, 500);
