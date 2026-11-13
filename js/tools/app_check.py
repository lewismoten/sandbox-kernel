#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Checks a Sandbox JS app before you copy it to the card: the same rules the runtime applies (src/app.c), so an app
that passes here starts on the board. tests/app_run.py runs both on the same cases and requires the same verdicts.

  app_check.py <app folder | APP.JSON>      validate one app (its manifest, its entry file, the folder name)
  app_check.py --card <sd folder>           every app under OS/APPS, and the app OS/AUTORUN.TXT names
  app_check.py --api N ...                  pretend the runtime's API is N (default: the one in src/app.h)

Prints "ok name=.. version=.. entry=.. api=N caps=a,b" or "err <why>"; exit 1 if anything fails.
APP.JSON keys: schema (1, optional), name, version ("1.0.2"), entry ("MAIN.JS"), api (whole number: the runtime API the
app needs), capabilities (list), description; x-anything is ignored. Unknown keys are errors (they are usually typos)."""
import json, pathlib, re, sys

HERE = pathlib.Path(__file__).resolve().parent
CAPS = ["log", "time", "memory", "app.lifecycle", "fs.read", "fs.read.shared", "fs.write.saves", "fs.write.cache",
        "display", "input", "audio", "eink", "serial", "power", "apps.launch"]
KEYS = {"schema", "name", "version", "entry", "api", "capabilities", "description"}
MAX = 2048


def runtime_api():
    m = re.search(r"#define APP_API_VERSION (\d+)u", (HERE.parent / "src/app.h").read_text())
    return int(m.group(1))


def _pairs(pairs):
    seen = set()
    for k, _ in pairs:
        if k in seen:
            raise ValueError(f'key "{k}" appears twice')
        seen.add(k)
    return dict(pairs)


def _depth(v, d=0):
    if d > 6:
        raise ValueError("nested too deeply")
    for x in (v.values() if isinstance(v, dict) else v if isinstance(v, list) else []):
        _depth(x, d + 1)


def _strings(v):
    if isinstance(v, str):
        yield v
    for x in (v.values() if isinstance(v, dict) else v if isinstance(v, list) else []):
        yield from _strings(x)
    if isinstance(v, dict):
        yield from v.keys()


def check_manifest(raw, api=None):
    """(True, {fields}) or (False, message). raw is bytes."""
    api = api or runtime_api()
    try:
        if len(raw) > MAX:
            return False, "APP.JSON is over 2048 bytes"
        if any(b > 0x7e or (b < 0x20 and b not in (9, 10, 13)) for b in raw):
            return False, "APP.JSON has a control or non-ASCII character"
        text = raw.decode("ascii")
        for m in re.finditer(r'\\(.?)', text):
            if m.group(1) not in ('"', "\\", "/"):
                return False, "unsupported escape in a string"
        try:
            d = json.loads(text, object_pairs_hook=_pairs, parse_constant=lambda c: (_ for _ in ()).throw(ValueError("not JSON")),
                           parse_float=lambda s: (_ for _ in ()).throw(ValueError("expected a whole number")))
        except json.JSONDecodeError as e:
            return False, f"not valid JSON ({e.msg})"
        if not isinstance(d, dict):
            return False, "APP.JSON must be a JSON object"
        _depth(d)
        for s in _strings(d):
            if len(s) > 120 or any(ord(c) < 0x20 or ord(c) > 0x7e for c in s):
                return False, "a string is bad or too long"
        for k in d:
            if k not in KEYS and not k.startswith("x-"):
                return False, f'unknown key "{k}"'
            if k in KEYS and len(k) >= 24:
                return False, "a key is missing or too long"
        for k, v in d.items():
            if k in ("schema", "api") and (not isinstance(v, int) or isinstance(v, bool) or v < 0 or v > 65535):
                return False, f'"{k}" must be a whole number'
        if d.get("schema", 1) != 1:
            return False, '"schema" must be 1'
        name = d.get("name")
        if not isinstance(name, str) or not 1 <= len(name) <= 32:
            return False, '"name" is required (1 to 32 characters)'
        if "api" not in d:
            return False, '"api" is required (the runtime API your app needs)'
        if d["api"] < 1:
            return False, '"api" must be 1 or more'
        version = d.get("version", "0")
        if "version" in d and (not isinstance(version, str) or len(version) > 15 or not re.fullmatch(r"\d{1,4}(\.\d{1,4}){0,2}", version)):
            return False, '"version" must look like 1.0.2'
        entry = d.get("entry", "MAIN.JS")
        if not isinstance(entry, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,8}\.[Jj][Ss]", entry):
            return False, '"entry" must be NAME.JS (1 to 8 letters, digits, _ or -)'
        if "description" in d and (not isinstance(d["description"], str) or len(d["description"]) > 120):
            return False, '"description" is too long (120 characters at most)'
        caps = d.get("capabilities", [])
        if not isinstance(caps, list) or any(not isinstance(c, str) for c in caps):
            return False, '"capabilities" must be a list of names'
        if len(caps) > 16:
            return False, "too many capabilities"
        for c in caps:
            if c not in CAPS:
                return False, f'unknown capability "{c}" (known: {", ".join(CAPS[4:])})'
        if len(set(caps)) != len(caps):
            return False, "a capability is listed twice"
        if d["api"] > api:
            return False, f"needs runtime API {d['api']}, this is {api}"
        extra = [c for c in CAPS[4:] if c in caps]
        return True, {"name": name, "version": version, "entry": entry.upper(), "api": d["api"], "caps": extra}
    except ValueError as e:
        return False, str(e)


def line(res):
    ok, v = res
    return f"ok name={v['name']} version={v['version']} entry={v['entry']} api={v['api']} caps={','.join(v['caps'])}" if ok else f"err {v}"


def check_app(folder, api=None):
    folder = pathlib.Path(folder)
    man = folder / "APP.JSON" if folder.is_dir() else folder
    folder = man.parent
    if not man.exists():
        return False, "APP.JSON is missing (apps without one run as legacy apps with every capability)"
    ok, v = check_manifest(man.read_bytes(), api)
    if not ok:
        return ok, v
    if not (folder / v["entry"]).exists() and not any(p.name.upper() == v["entry"] for p in folder.iterdir()):
        return False, f"{v['entry']} is not in the app folder"
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,8}", folder.name):
        return False, f"the folder name {folder.name!r} is not 1 to 8 letters, digits, _ or -"
    return True, v


def card(sd, api=None):
    sd = pathlib.Path(sd)
    bad = 0
    apps = sd / "OS/APPS"
    for a in sorted(apps.iterdir()) if apps.is_dir() else []:
        if a.is_dir():
            res = check_app(a, api)
            print(f"{a.name}: {line(res)}")
            bad += not res[0]
    auto = sd / "OS/AUTORUN.TXT"
    if auto.exists():
        lines = auto.read_text(errors="replace").split("\n")
        tok = lines[1].split()[0].upper() if len(lines) > 1 and lines[1].split() else "APPS/HELLO"
        m = re.fullmatch(r"APPS/([A-Z0-9_-]{1,8})", tok)
        if tok == "PICKER":
            print("AUTORUN: PICKER")
        elif not m:
            print(f'AUTORUN: err "{tok}": use APPS/NAME or PICKER'); bad += 1
        else:
            res = check_app(apps / m.group(1), api) if (apps / m.group(1)).is_dir() else (False, "no such app folder")
            print(f"AUTORUN: {tok} {line(res)}  (a failed start falls back to the picker)"); bad += not res[0]
    return bad


if __name__ == "__main__":
    args = sys.argv[1:]
    api = None
    if "--api" in args:
        i = args.index("--api"); api = int(args[i + 1]); del args[i:i + 2]
    if len(args) == 2 and args[0] == "--card":
        sys.exit(1 if card(args[1], api) else 0)
    if len(args) == 1 and not args[0].startswith("-"):
        res = check_app(args[0], api)
        print(line(res))
        sys.exit(0 if res[0] else 1)
    print(__doc__)
    sys.exit(2)
