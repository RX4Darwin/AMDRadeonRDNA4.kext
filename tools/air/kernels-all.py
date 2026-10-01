#!/usr/bin/env python3
"""kernels-all.py RESULTS.JSONL [TG]: run `air2amdgcn kernel` + llc (gfx1201, -O2) on every compute kernel of a disall.py run, with one fixed work-group
size (default 64,1,1; a real pipeline knows its own). Prints how many reach a gfx1201 object and why the others stop. Host only."""
import collections
import json
import os
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

tg = sys.argv[2] if len(sys.argv) > 2 else "64,1,1"
rows = [json.loads(l) for l in open(sys.argv[1])]
seen, jobs = set(), []
for r in rows:
    if r.get("ok") and r["type"] == "kernel" and r["sha256"] not in seen:
        seen.add(r["sha256"])
        jobs.append(r)


def one(r):
    with tempfile.TemporaryDirectory() as d:
        ll = os.path.join(d, "k.ll")
        p = subprocess.run(["build/air/air2amdgcn", "kernel", r["stem"] + ".bc", "--entry", r["name"], "--tg", tg, "-o", ll], capture_output=True, text=True)
        if p.returncode:
            why = p.stderr.strip().splitlines()
            line = next((x for x in why if "air2amdgcn:" in x), why[0] if why else "crash")
            for x in why:
                if x.strip().startswith("unsupported:"):
                    line = x.strip().split(" (x")[0]
                    break
            line = line.split("air2amdgcn: ")[-1]
            line = line.split(".bc: ", 1)[-1]
            return r, "stop", line[:110]
        q = subprocess.run(["llc", "-mtriple=amdgcn-amd-amdhsa", "-mcpu=gfx1201", "-O2", "-filetype=obj", ll, "-o", os.path.join(d, "k.o")], capture_output=True, text=True)
        if q.returncode:
            return r, "llc-fail", (q.stderr.strip().splitlines() or ["?"])[0][:110]
        return r, "ok", ""


with ThreadPoolExecutor(os.cpu_count()) as ex:
    out = list(ex.map(one, jobs))
c = collections.Counter(s for _, s, _ in out)
print("unique kernels %d, tg %s: reach a gfx1201 object %d, stop in the lowering %d, llc fails %d" % (len(out), tg, c["ok"], c["stop"], c["llc-fail"]))
why = collections.Counter()
for r, s, w in out:
    if s != "ok":
        key = w
        for pre in ("argument '",):
            if w.startswith(pre):
                key = "argument of kind " + w.split(" of kind ")[-1]
        why[key] += 1
for k, v in why.most_common(25):
    print("- %5d  %s" % (v, k))
print("\nexamples of llc failures / verifier failures:")
shown = collections.Counter()
for r, st, w in out:
    key = "llc" if st == "llc-fail" else "verify" if "verify" in w else None
    if key and shown[(key, w)] < 1 and sum(v for (k, _), v in shown.items() if k == key) < 6:
        shown[(key, w)] += 1
        print("  %s: %s (%s) -> %s" % (key, r["name"], r["lib"].split("__")[-1][:40], w))
good = [r for r, s, _ in out if s == "ok"]
json.dump([{"lib": r["lib"], "name": r["name"], "stem": r["stem"]} for r in good], open(os.environ.get("KERNELS_OK", "/dev/null"), "w"))
