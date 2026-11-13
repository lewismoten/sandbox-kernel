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
    // Launch state (apps.launch capability, see below): what the app that launched this one passed, who returned here, and a launch that failed.
    const li = N.launchInfo();
    let launchArgs;
    if (li.args) { try { launchArgs = JSON.parse(li.args); } catch (e) { launchArgs = undefined; } }
    g.sandbox = Object.freeze({
      launchArgs: launchArgs === undefined ? undefined : Object.freeze(launchArgs),
      returned: li.returned && Object.freeze(li.returned),
      launchError: li.failed && Object.freeze(li.failed),
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
    // Apps (capability apps.launch; gap G5): list() -> [{id, name, version, api, capabilities, ok, reason}] from /OS/APPS/*/APP.JSON (metadata
    // only); launch(id, {args}) stops this app (handles released) and starts that one, args (JSON, up to 200 bytes) arrive as
    // sandbox.launchArgs; exit(code) returns to the app that launched this one (sandbox.returned = {id, code} there), or to the AUTORUN app or
    // the picker. A launch that does not start returns to the caller with sandbox.launchError = {id, reason} and its 'launcherror' event.
    g.apps = Object.freeze({
      list: N.appsList,
      launch(id, opts) {
        const args = opts && opts.args;
        const text = args === undefined ? '' : JSON.stringify(args);
        if (text !== undefined && text.length > 200) throw new RangeError('apps.launch: args are over 200 bytes of JSON');
        N.appsLaunch(String(id), text);
      },
      exit: N.appsExit,
      on(type, fn) {
        if (type !== 'launcherror') throw new RangeError("apps.on: 'launcherror'");
        if (typeof fn !== 'function') throw new TypeError('apps.on: a function');
        if (li.failed) setTimeout(() => fn(li.failed), 0);
      },
    });
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
  // E-ink (slice 4d): eink.open() -> the one owner of the 250x122 panel, or null when there is no panel (eink.available is false).
  // A page is 3904 bytes, 32 bytes a row, bit 7 leftmost, 1 = black. page(bytes) / update(x,y,w,h,bits) / canvas() send a page; the
  // result is true when it went to the panel now and false when it was queued (too soon: only the newest queued page is kept and the
  // runtime sends it when the panel limits allow). release() gives the screen back to the kernel (also done at stop); close() keeps it up.
  {
    const PW = 250,
      PH = 122,
      RB = 32;
    g.eink = {
      get available() {
        return !!N.einkProbe();
      },
      open() {
        const id = N.einkOpen();
        if (id === null) return null;
        const info = Object.freeze(N.einkInfo(id));
        const bits = new Uint8Array(RB * PH);
        let closed = false;
        const live = () => {
          if (closed) throw new Error('EBADF: the e-ink panel is closed');
        };
        const send = () => N.einkPage(id, bits) === 1;
        const int = (v, k) => {
          if (!Number.isInteger(v)) throw new RangeError('EINVAL: ' + k + ' must be an integer');
          return v;
        };
        const put = (x, y, black) => {
          const i = y * RB + (x >> 3),
            m = 0x80 >> (x & 7);
          bits[i] = black ? bits[i] | m : bits[i] & ~m;
        };
        // Copies a 1-bit bitmap (rows of ceil(w/8) bytes, bit 7 leftmost, 1 = black) into the page at x, y; no clipping: out of bounds is EINVAL.
        const blit = (x, y, w, h, src) => {
          int(x, 'x');
          int(y, 'y');
          int(w, 'w');
          int(h, 'h');
          if (w < 1 || h < 1 || x < 0 || y < 0 || x + w > PW || y + h > PH) throw new RangeError('EINVAL: the region ' + x + ',' + y + ' ' + w + 'x' + h + ' is outside the 250x122 page');
          const rb = (w + 7) >> 3;
          if (!(src instanceof Uint8Array) || src.length !== rb * h) throw new RangeError('EINVAL: a ' + w + 'x' + h + ' bitmap is ' + rb * h + ' bytes (' + rb + ' a row)');
          for (let r = 0; r < h; r++) for (let c = 0; c < w; c++) put(x + c, y + r, src[r * rb + (c >> 3)] & (0x80 >> (c & 7)));
        };
        const fill = (x, y, w, h, black) => {
          const x0 = Math.max(0, x),
            y0 = Math.max(0, y),
            x1 = Math.min(PW, x + w),
            y1 = Math.min(PH, y + h);
          for (let yy = y0; yy < y1; yy++) for (let xx = x0; xx < x1; xx++) put(xx, yy, black);
        };
        const p = {
          info,
          get bits() {
            return bits;
          },
          page(bytes) {
            live();
            if (!(bytes instanceof Uint8Array) || bytes.length !== RB * PH) throw new RangeError('EINVAL: a page is exactly 3904 bytes (a Uint8Array)');
            bits.set(bytes);
            return send();
          },
          // Changes one region of the page and sends it. {send: false} only edits (flush() sends).
          update(x, y, w, h, src, opts) {
            live();
            blit(x, y, w, h, src);
            return opts && opts.send === false ? undefined : send();
          },
          flush() {
            live();
            return send();
          },
          // A canvas-like drawing surface on the same page: draw, then flush(). Shapes are clipped to the page.
          canvas() {
            live();
            const k = {
              width: PW,
              height: PH,
              clear(black) {
                bits.fill(black ? 0xff : 0);
              },
              setPixel(x, y, black = true) {
                if (x >= 0 && y >= 0 && x < PW && y < PH) put(Math.floor(x), Math.floor(y), black);
              },
              fillRect(x, y, w, h, black = true) {
                fill(Math.floor(x), Math.floor(y), Math.floor(w), Math.floor(h), black);
              },
              drawBitmap(x, y, w, h, src) {
                blit(x, y, w, h, src);
              },
              flush() {
                return p.flush();
              },
            };
            return k;
          },
          // The OS name (1 to 16 characters), an optional 40x40 mark (200 bytes) and a build line (up to 16 characters) on the kernel's status screen.
          setTitle(name, o) {
            live();
            return N.einkTitle(id, String(name), o && o.mark, o && o.build) === 1;
          },
          stats() {
            live();
            return N.einkStats(id);
          },
          get closed() {
            return closed;
          },
          release() {
            if (closed) return;
            closed = true;
            N.einkClose(id, 1);
          },
          close() {
            if (closed) return;
            closed = true;
            N.einkClose(id, 0);
          },
        };
        return p;
      },
    };
  }
  // Serial (slice 4d): serial.open({mode: 'r' | 'w' | 'rw'}) -> the one owner of the kernel's byte channel. read(into) takes what is waiting (0
  // when empty), write(bytes | string) returns how many bytes were taken (maybe fewer); port.on('data', fn) calls fn(Uint8Array) as bytes
  // arrive (bounded reads, 256 bytes an event, 1 KiB a turn: what the kernel cannot hold it drops and counts in stats()).
  {
    let cur = null;
    const utf8 = (s) => {
      const out = [];
      for (const ch of s) {
        const c = ch.codePointAt(0);
        if (c < 0x80) out.push(c);
        else if (c < 0x800) out.push(0xc0 | (c >> 6), 0x80 | (c & 63));
        else if (c < 0x10000) out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
        else out.push(0xf0 | (c >> 18), 0x80 | ((c >> 12) & 63), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
      }
      return new Uint8Array(out);
    };
    N.onSerial = (id, ab) => {
      const s = cur;
      if (!s || s._id !== id) return;
      const data = new Uint8Array(ab);
      for (const fn of [...s._on])
        try {
          fn(data);
        } catch (err) {
          report(err);
        }
    };
    g.serial = {
      open(opts) {
        const m = (opts && opts.mode) || 'rw';
        if (m !== 'r' && m !== 'w' && m !== 'rw') throw new RangeError("EINVAL: mode is 'r', 'w' or 'rw'");
        const id = N.serialOpen(m === 'r' ? 1 : m === 'w' ? 2 : 3);
        let closed = false;
        const live = () => {
          if (closed) throw new Error('EBADF: the serial port is closed');
        };
        const on = new Set();
        const s = {
          _id: id,
          _on: on,
          mode: m,
          read(into) {
            live();
            return N.serialRead(id, into);
          },
          write(data) {
            live();
            return N.serialWrite(id, typeof data === 'string' ? utf8(data) : data);
          },
          on(type, fn) {
            live();
            if (type !== 'data') throw new RangeError("serial.on: the only event is 'data'");
            if (typeof fn !== 'function') throw new TypeError('serial.on: a function');
            on.add(fn);
            N.serialWant(id, 1);
          },
          off(type, fn) {
            if (type !== 'data' || closed) return;
            on.delete(fn);
            if (!on.size) N.serialWant(id, 0);
          },
          stats() {
            live();
            return N.serialStats(id);
          },
          get closed() {
            return closed;
          },
          close() {
            if (closed) return;
            closed = true;
            on.clear();
            if (cur === s) cur = null;
            N.serialClose(id);
          },
        };
        cur = s;
        return s;
      },
    };
  }
  // Power (slice 4d): power.hold(reason, {noBlank, noSlowdown}) -> a Hold (default: no blank only); hold.release() (twice is fine). Holds are
  // given back at stop. power.asleep -> {blank, slowdown} (what the kernel has put to sleep now), power.screenDark -> true while the screen
  // is blank and this app holds no blank; power.on('blank' | 'wake', fn). While the screen is dark the audio pump stops synthesizing and
  // does not call onneed (the kernel drops audio then) unless the app holds noBlank; the pump starts again on wake.
  {
    const ev = { blank: new Set(), wake: new Set() };
    const holds = new Map();
    N.onPower = (dark) => {
      for (const fn of [...ev[dark ? 'blank' : 'wake']])
        try {
          fn({ type: dark ? 'blank' : 'wake' });
        } catch (err) {
          report(err);
        }
    };
    g.power = {
      hold(reason, o) {
        const f = o || {};
        const flags = (f.noBlank === undefined || f.noBlank ? 1 : 0) | (f.noSlowdown ? 2 : 0);
        const id = N.powerHold(flags, String(reason));
        let released = false;
        const h = {
          reason: String(reason).slice(0, 40),
          noBlank: !!(flags & 1),
          noSlowdown: !!(flags & 2),
          get released() {
            return released;
          },
          release() {
            if (released) return;
            released = true;
            holds.delete(id);
            N.powerRelease(id);
          },
        };
        holds.set(id, h);
        return h;
      },
      get holds() {
        return N.powerList().map(([id, flags, reason]) => ({ reason, noBlank: !!(flags & 1), noSlowdown: !!(flags & 2) }));
      },
      get asleep() {
        const b = N.powerState();
        return { blank: !!(b & 1), slowdown: !!(b & 2) };
      },
      get screenDark() {
        return !!(N.powerState() & 1) && !N.powerList().some((h) => h[1] & 1);
      },
      on(type, fn) {
        if (!ev[type]) throw new RangeError("power.on: 'blank' or 'wake'");
        if (typeof fn !== 'function') throw new TypeError('power.on: a function');
        ev[type].add(fn);
        N.powerWatch(1);
      },
      off(type, fn) {
        if (!ev[type]) return;
        ev[type].delete(fn);
        if (!ev.blank.size && !ev.wake.size) N.powerWatch(0);
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
