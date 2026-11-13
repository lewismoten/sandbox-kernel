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
  g.fs = { readFile: N.readFile, readdir: N.readdir };
  g.sys = { exit: N.exit, stats: N.stats, memory: N.memory, launch: N.launch };
  // The screen: setMode(width, height, depth 32|16) for the page; a playing
  // <game> uses 320x200 at 8 bpp unless screen.gameLowres(false), or a larger
  // 8 bpp mode with the picture centered: screen.gameLowres(true, 384, 216)
  // (on a 1080p screen, exactly 5x with black bars).
  g.screen = {
    setMode: N.setMode,
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
