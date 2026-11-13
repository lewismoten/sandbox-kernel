// Touches every place the runtime allocates natively: elements and their text and canvases, files (read handles, a writer that
// grows), the display lease, timers, the audio, e-ink and serial owners. Each step is wrapped, so an allocation that fails
// only ends that step; tests/alloc_run.py fails each allocation in turn and checks nothing crashes or leaks.
// The work runs in a timer callback, after the script has loaded: a failure while QuickJS compiles a script is a different case (see jsrt.c).
function main() {
const step = (f) => { try { f(); } catch (e) {} };
step(() => {
  const d = document.createElement('div'); d.textContent = 'hello world'; document.body.appendChild(d);
  const b = document.createElement('button'); b.textContent = 'ok'; d.appendChild(b);
  const c = document.createElement('canvas'); c.width = 64; c.height = 32; d.appendChild(c);
  const g = c.getContext('2d'); g.fillStyle = '#ff0000'; g.fillRect(1, 1, 8, 8); g.fillText && g.fillText('x', 2, 2);
  d.style.color = '#00ff00'; d.textContent = 'a longer line of text than before'; d.style.width = '200px';
  const i = document.createElement('img'); d.appendChild(i);
  d.removeChild(b);
});
step(() => { const f = fs.open('/sd/OS/APPS/ALLOCS/DATA.TXT'); f.read(8); f.seek(4); f.text(16); f.close(); });
step(() => { fs.readBytes('/sd/OS/APPS/ALLOCS/DATA.TXT'); fs.readdir('/sd/OS/APPS/ALLOCS', { meta: true }); fs.stat('/sd/OS/APPS/ALLOCS/DATA.TXT'); });
step(() => { fs.write('/sd/OS/SAVES/ALLOCS/ONE.SAV', 'a short save'); });
step(() => {
  const w = fs.createWriter('/sd/OS/SAVES/ALLOCS/TWO.SAV');
  const chunk = new Uint8Array(3000); for (let k = 0; k < 8; k++) w.write(chunk);   // grows the staging buffer more than once
  w.commit();
});
step(() => { const l = screen.lease({ width: 320, height: 200, depth: 8 }); l.pixels.fill(3); l.present(); l.release(); });
step(() => { const q = input.open(); q.on('key', () => {}); });
step(() => { sleep(20).then(() => {}); sleep(40).then(() => {}); setTimeout(() => {}, 10); });
step(() => { const a = audio.open({ onneed(n) { a.tone && a.tone(440, 10); } }); a.melody([[262, 20]]); });
step(() => { const p = eink.open(); if (p) { p.update(0, 0, 8, 2, new Uint8Array([0xff, 0xff])); p.release(); } });
step(() => { const s = serial.open({ mode: 'rw' }); s.write(new Uint8Array([1, 2, 3])); s.read(4); });
step(() => { power.hold('noBlank'); });
setTimeout(() => sys.exit(0), 150);
}
setTimeout(main, 10);
