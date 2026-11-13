#!/usr/bin/env python3
"""Sandbox JS contract tools (SPDX-License-Identifier: MIT).

  contract.py check   validate js/contract.json and compare it with the kernel source:
                      every syscall number in core/sys.c sizes[] and every SBX_IOC in
                      sdk/include/sandbox/abi.h must have an entry, and the ABI minor must match.
  contract.py drift   fail if the kernel's ABI minor, syscall table (number and argument size) or ioctl
                      list no longer matches snapshot.abi_fingerprint in contract.json: review the
                      entries and gaps for what changed, then run `contract.py stamp`.
  contract.py stamp   write the current kernel fingerprint into contract.json (after that review).
  contract.py md      print the inventory tables (pasted into js/docs/contract.md).
Run from anywhere; paths are found from this file. Exit 1 on any problem."""
import hashlib, json, re, sys, pathlib
root = pathlib.Path(__file__).resolve().parents[2]
data = json.loads((root / "js/contract.json").read_text())
STATUS = {"v1", "bound", "later", "not-exposed"}
KEYS = ["id","kind","kernel","number","abi","js","shape","errors","capability","status","why","test","notes"]

def check():
    bad = []
    ids = set()
    for e in data["entries"]:
        for k in KEYS:
            if k not in e: bad.append(f"{e.get('id')}: missing {k}")
        if e["id"] in ids: bad.append(f"duplicate id {e['id']}")
        ids.add(e["id"])
        if e["status"] not in STATUS: bad.append(f"{e['id']}: status {e['status']}")
        if not e["test"].strip(): bad.append(f"{e['id']}: no test plan")
        if e["status"] not in ("v1",) and not e["why"].strip(): bad.append(f"{e['id']}: non-v1 needs why")
    for g in data["gaps"]:
        for k in ("id","title","problem","owner","proposal","priority"):
            if not g.get(k): bad.append(f"gap {g.get('id')}: missing {k}")
    sysc = (root / "core/sys.c").read_text()
    m = re.search(r"sizes\[\d+\]=\{([^}]*)\}", sysc)
    nums = {int(n) for n in re.findall(r"\[(\d+)\]=", m.group(1))}
    have = {e["number"] for e in data["entries"] if e["kind"] == "syscall"}
    for n in sorted(nums - have): bad.append(f"syscall {n} has no entry")
    abi = (root / "sdk/include/sandbox/abi.h").read_text()
    reqs = {}
    for name, c, o in re.findall(r"#define (SBX_IOC_\w+) SBX_IOC\((\d+), (\d+)\)", abi):
        reqs[name] = int(c) << 16 | int(o)
    have_i = {e["number"] for e in data["entries"] if e["kind"] == "ioctl"}
    for name, v in reqs.items():
        if v not in have_i: bad.append(f"{name} ({v:#x}) has no entry")
    kabi = re.search(r"K_ABI_MINOR (\d+)u", (root / "include/abi.h").read_text()).group(1)
    if data["snapshot"]["abi"] != "1." + kabi: bad.append(f"snapshot ABI {data['snapshot']['abi']} != 1.{kabi}")
    for b in bad: print("FAIL", b)
    c = {s: sum(e["status"] == s for e in data["entries"]) for s in STATUS}
    print(f"{len(data['entries'])} entries, {len(data['gaps'])} gaps, status {c}; {'FAIL' if bad else 'ok'}")
    return 1 if bad else 0

def fingerprint():
    """The kernel's ABI as this contract sees it: the minor, each syscall number with its size
    entry from core/sys.c, and each SBX_IOC name with its value. Returns (digest, text)."""
    sysc = (root / "core/sys.c").read_text()
    m = re.search(r"sizes\[\d+\]=\{([^}]*)\}", sysc)
    sizes = sorted((int(n), v.strip()) for n, v in re.findall(r"\[(\d+)\]=([^,}]+)", m.group(1)))
    abi = (root / "sdk/include/sandbox/abi.h").read_text()
    iocs = sorted((name, int(c) << 16 | int(o)) for name, c, o in re.findall(r"#define (SBX_IOC_\w+) SBX_IOC\((\d+), (\d+)\)", abi))
    kabi = re.search(r"K_ABI_MINOR (\d+)u", (root / "include/abi.h").read_text()).group(1)
    text = f"abi 1.{kabi}\n" + "".join(f"sys {n} {v}\n" for n, v in sizes) + "".join(f"ioctl {n} {v:#x}\n" for n, v in iocs)
    return hashlib.sha256(text.encode()).hexdigest(), text

def drift():
    want = data["snapshot"].get("abi_fingerprint", "")
    got, text = fingerprint()
    if want == got:
        print(f"no drift: kernel ABI fingerprint {got[:16]} matches contract.json"); return 0
    print(f"FAIL drift: the kernel's ABI minor, syscall table or ioctl list changed (fingerprint {got[:16]}, contract.json has {want[:16] or 'none'}).")
    print("Update contract.json for what changed (entries, gaps, snapshot.abi and kernel_commit), run contract.py check, then contract.py stamp.")
    return 1

def stamp():
    got, _ = fingerprint()
    p = root / "js/contract.json"
    s = p.read_text()
    if '"abi_fingerprint"' in s:
        s = re.sub(r'"abi_fingerprint": "[0-9a-f]*"', f'"abi_fingerprint": "{got}"', s)
    else:
        s = s.replace('"abi": "%s",' % data["snapshot"]["abi"], '"abi": "%s",\n  "abi_fingerprint": "%s",' % (data["snapshot"]["abi"], got), 1)
    p.write_text(s); print("stamped", got); return 0

def md():
    def cell(s): return str(s).replace("|", "\\|")
    for kind, title in (("syscall","Syscalls"),("ioctl","Device requests (ioctl)"),("device","Devices and events"),("record","Records")):
        print(f"### {title}\n")
        print("| ID | Kernel | # | ABI | JS API | Sync | Errors | Capability | Status | Test plan |")
        print("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
        for e in data["entries"]:
            if e["kind"] != kind: continue
            n = "" if e["number"] is None else (hex(e["number"]) if kind == "ioctl" else e["number"])
            st = e["status"] + ("" if e["status"] == "v1" else ": " + e["why"])
            print("| " + " | ".join(cell(x) for x in (e["id"], e["kernel"], n, e["abi"], e["js"], e["shape"], e["errors"], e["capability"], st, e["test"])) + " |")
        print()
    print("### Gaps\n\n| ID | Title | Problem | Owner | Proposal | Priority |\n| --- | --- | --- | --- | --- | --- |")
    for g in data["gaps"]:
        print("| " + " | ".join(cell(g[k]) for k in ("id","title","problem","owner","proposal","priority")) + " |")

if __name__ == "__main__":
    cmds = {"check": check, "drift": drift, "stamp": stamp, "md": lambda: md() or 0}
    sys.exit(cmds[sys.argv[1]]() if sys.argv[1:] and sys.argv[1] in cmds and len(sys.argv) == 2 else print(__doc__) or 2)
