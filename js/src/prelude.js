// SPDX-License-Identifier: MIT; the browser-like globals of the JS runtime,
// written in JS over the native object N (jsrt.c): events with bubbling,
// document, timers, requestAnimationFrame, console, performance and fs.
// The tree, layout, drawing, files and the game are native; this only wires
// them together. Compiled into JS.ELF (tools/embed_js.py).
(function (g, N) {
  'use strict';
  const L = Symbol('listeners');
  const EP = N.Element.prototype;
  function report(e) {
    N.log('Uncaught ' + (e && e.stack ? e + '\n' + e.stack : String(e)));
  }
  function listen(type, fn) {
    if (typeof fn !== 'function') return;
    const m = this[L] || (this[L] = {});
    const a = m[type] || (m[type] = []);
    if (!a.includes(fn)) a.push(fn);
  }
  function unlisten(type, fn) {
    const a = this[L] && this[L][type];
    const i = a ? a.indexOf(fn) : -1;
    if (i >= 0) a.splice(i, 1);
  }
  class Event {
    constructor(type, init) {
      this.type = type;
      this.bubbles = true;
      this.defaultPrevented = false;
      this.target = null;
      this.currentTarget = null;
      Object.assign(this, init);
    }
    preventDefault() {
      this.defaultPrevented = true;
    }
    stopPropagation() {
      this.stopped = true;
    }
  }
  class KeyboardEvent extends Event {}
  function fire(node, ev) {
    const a = node[L] && node[L][ev.type];
    if (!a) return;
    ev.currentTarget = node;
    for (const fn of a.slice()) {
      if (N.exiting()) break;
      try {
        fn.call(node, ev);
      } catch (e) {
        report(e);
      }
    }
  }
  function dispatch(ev) {
    ev.target = this;
    for (let n = this; n && !ev.stopped; n = n.parentNode) {
      fire(n, ev);
      if (!ev.bubbles) return !ev.defaultPrevented;
    }
    if (!ev.stopped) fire(document, ev);
    return !ev.defaultPrevented;
  }
  // innerHTML: a small HTML subset (the tags createElement knows; id, class
  // and style attributes; text) parsed in JS into elements. No entities other
  // than &lt; &gt; &amp; &quot;, no comments, no scripts.
  const camel = (s) => s.trim().replace(/-([a-z])/g, (m, c) => c.toUpperCase());
  const unescape = (s) => s.replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&quot;/g, '"').replace(/&amp;/g, '&');
  const VOID = { img: 1, canvas: 0 };
  Object.defineProperty(EP, 'innerHTML', {
    set(html) {
      while (this.firstChild) this.firstChild.remove();
      const stack = [this];
      const re = /<(\/?)([a-zA-Z0-9]+)((?:\s+[a-zA-Z-]+="[^"]*")*)\s*(\/?)>|([^<]+)/g;
      for (let m; (m = re.exec(html)); ) {
        const top = stack[stack.length - 1];
        if (m[5] !== undefined) {
          const t = unescape(m[5].replace(/\s+/g, ' ').trim());
          if (t) top.textContent = top.textContent ? top.textContent + ' ' + t : t;
        } else if (m[1]) {
          if (stack.length > 1) stack.pop();
        } else {
          const el = N.createElement(m[2]);
          for (const a of m[3].matchAll(/([a-zA-Z-]+)="([^"]*)"/g)) {
            const v = unescape(a[2]);
            if (a[1] === 'style') {
              for (const d of v.split(';')) {
                const i = d.indexOf(':');
                if (i > 0) el.style[camel(d.slice(0, i))] = d.slice(i + 1).trim();
              }
            } else if (a[1] === 'class') el.className = v;
            else if (a[1] === 'tabindex') el.tabIndex = +v;
            else el[a[1]] = v;
          }
          top.appendChild(el);
          if (!m[4] && !VOID[m[2].toLowerCase()]) stack.push(el);
        }
      }
    },
  });
  EP.addEventListener = listen;
  EP.removeEventListener = unlisten;
  EP.dispatchEvent = dispatch;
  const document = {
    body: N.body,
    createElement: N.createElement,
    getElementById: N.getElementById,
    querySelector: N.querySelector,
    querySelectorAll: N.querySelectorAll,
    get activeElement() {
      return N.focused() || N.body;
    },
    addEventListener: listen,
    removeEventListener: unlisten,
  };
  // Timers and frames: the native loop calls onTimers and onFrame when due.
  const timers = new Map();
  let nextTimer = 0;
  function wake() {
    let at = Infinity;
    for (const t of timers.values()) if (t.at < at) at = t.at;
    N.wakeAt(at);
  }
  function addTimer(fn, ms, every, args) {
    const id = ++nextTimer;
    ms = Math.max(0, +ms || 0);
    timers.set(id, { fn, at: N.now() + ms, every: every ? Math.max(1, ms) : 0, args });
    wake();
    return id;
  }
  g.setTimeout = (fn, ms, ...args) => addTimer(fn, ms, false, args);
  g.setInterval = (fn, ms, ...args) => addTimer(fn, ms, true, args);
  g.clearTimeout = g.clearInterval = (id) => {
    timers.delete(id);
    wake();
  };
  N.onTimers = (now) => {
    for (const [id, t] of [...timers]) {
      if (N.exiting()) break;
      if (t.at > now || !timers.has(id)) continue;
      if (t.every) t.at = Math.max(t.at + t.every, now);
      else timers.delete(id);
      try {
        t.fn(...t.args);
      } catch (e) {
        report(e);
      }
    }
    wake();
  };
  let frames = [];
  let nextFrame = 0;
  g.requestAnimationFrame = (fn) => {
    frames.push([++nextFrame, fn]);
    N.wantFrame();
    return nextFrame;
  };
  g.cancelAnimationFrame = (id) => {
    frames = frames.filter((f) => f[0] !== id);
  };
  N.onFrame = (ts) => {
    const due = frames;
    frames = [];
    for (const f of due) {
      if (N.exiting()) break;
      try {
        f[1](ts);
      } catch (e) {
        report(e);
      }
    }
  };
  // Input: keydown/keyup at the focused element (or body), bubbling to document.
  N.onKey = (type, key, code, repeat) => {
    const ev = new KeyboardEvent(type, { key, code, repeat });
    return (N.focused() || N.body).dispatchEvent(ev);
  };
  N.onEvent = (target, type) => target.dispatchEvent(new Event(type, { bubbles: type !== 'focus' && type !== 'blur' }));
  N.onError = report;
  function show(v) {
    if (typeof v === 'string') return v;
    if (v instanceof Error) return v.stack ? v + '\n' + v.stack : String(v);
    try {
      const s = JSON.stringify(v);
      return s === undefined ? String(v) : s;
    } catch {
      return String(v);
    }
  }
  const log = (...a) => N.log(a.map(show).join(' '));
  g.console = { log, info: log, warn: log, error: log, debug: log };
  g.performance = { now: N.now };
  g.fs = {
    readFile: N.readFile,
    // readdir(path) is the names; readdir(path, { meta: true }) is [{ name, type: 'file' | 'dir', size, modified, created }]
    // with long names. modified and created are the card's FAT date and time words, opaque fingerprints, not Unix times.
    readdir: (path, opts) => (opts && opts.meta ? N.readdirMeta(path) : N.readdir(path)),
    open: N.open,
    stat: N.stat,
    // readBytes(path) -> Uint8Array of the whole file (at most 1 MiB; read it in pieces with open() for more).
    readBytes(path) {
      const f = N.open(path);
      try {
        if (f.size > 1 << 20) throw new RangeError('EFBIG: ' + path + ' is over 1 MiB; read it in pieces with fs.open');
        const out = new Uint8Array(f.size);
        let at = 0;
        while (at < out.length) { const n = f.readInto(out, at); if (!n) break; at += n; }
        return at === out.length ? out : out.subarray(0, at);
      } finally { f.close(); }
    },
    // createWriter(path) -> { write(data), commit(), abort(), close(), size, path }: stages in memory, writes the card at commit.
    createWriter: N.createWriter,
    // write(path, data): stage and commit in one call. Returns the bytes written.
    write(path, data) {
      const w = N.createWriter(path);
      try { w.write(data); return w.commit(); } catch (e) { w.abort(); throw e; }
    },
    writeQuota: N.writeQuota,
  };
  g.sys = { exit: N.exit, stats: N.stats, memory: N.memory, launch: N.launch, startError: N.startError };
  // sandbox: the runtime and the app. version.api is what a manifest's "api" is compared with; capabilities are the
  // app's granted names; require(name) throws an Error naming the capability when it is not granted.
  {
    const a = N.app();
    const caps = Object.freeze(N.caps());
    g.sandbox = Object.freeze({
      version: Object.freeze({ api: a.api, runtime: a.runtime }),
      app: Object.freeze({ id: a.id, name: a.name, version: a.version, legacy: a.legacy }),
      capabilities: caps,
      has: (name) => caps.includes(name),
      require: N.require,
      // Time (slice 4b): now() is whole microseconds on the kernel's clock (exact); sleep(ms) is a promise the loop
      // wakes in chunks of at most 100 ms; seed is the number Math.random started from, seedRandom(n) restarts it.
      now: N.nowUs,
      sleep: N.sleep,
      get seed() {
        return N.seed();
      },
      seedRandom: N.seedRandom,
    });
    g.sleep = N.sleep;
    g.Math.random = N.random;
  }
  // Raw input (slice 4b): input.open() -> a queue with on('key' | 'attach' | 'detach' | 'reset', fn), off, pressed(code),
  // down, attached, close(). The document's keydown and keyup are separate and unchanged.
  {
    const queues = new Set();
    g.input = {
      open() {
        const h = N.inputOpen();
        const on = { key: new Set(), attach: new Set(), detach: new Set(), reset: new Set() };
        const entry = { on };
        let closed = false;
        const q = {
          on(type, fn) {
            if (!on[type]) throw new TypeError("input.on: unknown event '" + type + "' (key, attach, detach, reset)");
            if (typeof fn !== 'function') throw new TypeError('input.on: a function');
            on[type].add(fn);
            return q;
          },
          off(type, fn) {
            if (on[type]) on[type].delete(fn);
            return q;
          },
          pressed: (code) => N.keyDown(code),
          get down() {
            return N.keysDown();
          },
          get attached() {
            return N.inputAttached();
          },
          get closed() {
            return closed;
          },
          close() {
            if (closed) return;
            closed = true;
            queues.delete(entry);
            h.close();
          },
        };
        queues.add(entry);
        return q;
      },
      get attached() {
        return N.inputAttached();
      },
    };
    N.onRaw = (kind, a, b, c) => {
      const type = ['key', 'attach', 'detach', 'reset'][kind];
      const ev =
        kind === 0
          ? { type, code: a, value: b, repeat: b === 2, time: c }
          : kind === 3
            ? { type, reason: a, released: b, time: c }
            : { type, time: a };
      for (const e of [...queues])
        for (const fn of [...e.on[type]]) {
          try {
            fn(ev);
          } catch (err) {
            report(err);
          }
        }
    };
  }
  // Audio (slice 4c): audio.open({rate, channels, onneed, targetMs}) -> the one writer on the kernel's 48 kHz ring. write(Int16Array)
  // returns the frames it took (maybe fewer, maybe 0) and never blocks; onneed(frames) is called when the ring has room;
  // tone(hz, ms, volume) queues a square-wave beep; stats() has the kernel's counters; volume, muted, close().
  {
    let cur = null;
    N.onAudio = (id, frames) => {
      const a = cur;
      if (!a || a._id !== id || typeof a.onneed !== 'function') return;
      try {
        a.onneed(frames);
      } catch (err) {
        report(err);
      }
    };
    g.audio = {
      open(opts) {
        const o = opts || {};
        const id = N.audioOpen(o.rate, o.channels, o.targetMs);
        let vol = 1,
          muted = false,
          fn = null,
          closed = false;
        const push = () => N.audioSet(id, Math.round(vol * 256), muted ? 1 : 0, typeof fn === 'function' ? 1 : 0);
        const live = () => {
          if (closed) throw new Error('EBADF: the audio device is closed');
        };
        const a = {
          _id: id,
          write(pcm) {
            live();
            return N.audioWrite(id, pcm);
          },
          tone(hz, ms, volume) {
            live();
            const v = volume === undefined ? 1 : volume;
            if (!(v >= 0 && v <= 1)) throw new RangeError('EINVAL: tone volume 0 to 1');
            N.audioTone(id, Math.round(hz), Math.ceil(ms), Math.round(v * 256));
          },
          // notes: [[hz, ms], ...] (hz 0 rests), queued in order
          melody(notes, volume) {
            for (const n of notes) a.tone(n[0], n[1], volume);
          },
          stop() {
            live();
            N.audioStop(id);
          },
          stats() {
            live();
            return N.audioStats(id);
          },
          get onneed() {
            return fn;
          },
          set onneed(f) {
            if (f !== null && f !== undefined && typeof f !== 'function') throw new TypeError('audio.onneed: a function');
            fn = f || null;
            if (!closed) push();
          },
          get volume() {
            return vol;
          },
          set volume(v) {
            if (!(v >= 0 && v <= 1)) throw new RangeError('EINVAL: volume 0 to 1');
            vol = v;
            if (!closed) push();
          },
          get muted() {
            return muted;
          },
          set muted(m) {
            muted = !!m;
            if (!closed) push();
          },
          get closed() {
            return closed;
          },
          close() {
            if (closed) return;
            closed = true;
            if (cur === a) cur = null;
            N.audioClose(id);
          },
        };
        cur = a;
        if (typeof o.onneed === 'function') a.onneed = o.onneed;
        return a;
      },
    };
  }
  // The screen: setMode(width, height, depth 32|16) for the page; a playing
  // <game> uses 320x200 at 8 bpp unless screen.gameLowres(false), or a larger
  // 8 bpp mode with the picture centered: screen.gameLowres(true, 384, 216)
  // (on a 1080p screen, exactly 5x with black bars).
  g.screen = {
    setMode: N.setMode,
    // Slice 4b: modes() lists the kernel's modes (common sizes at 32, 16 and 8 bpp); supports(w, h, depth) is its rule;
    // lease({width, height, depth}) hands the surface to the app (see docs/apps.md); leased says who has it.
    modes: N.modes,
    supports: N.modeOk,
    lease: N.lease,
    get leased() {
      return N.leased();
    },
    gameLowres: N.gameLowres,
    get width() {
      return N.screen().width;
    },
    get height() {
      return N.screen().height;
    },
    get depth() {
      return N.screen().depth;
    },
    get switchUs() {
      return N.screen().switchUs;
    },
  };
  g.document = document;
  g.window = g;
  g.Event = Event;
  g.KeyboardEvent = KeyboardEvent;
});
