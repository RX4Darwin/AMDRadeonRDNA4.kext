#!/usr/bin/env python3
"""coverage-report.py RESULTS.JSONL COVERAGE.JSONL: join disall.py's results with coverage-all.sh's output and print markdown tables:
modules whose every air.* call has a lowering rule (and compile with llc for gfx1201 as plain functions), by entry type and library group; top blockers."""
import collections
import json
import sys

res = {}
for l in open(sys.argv[1]):
    r = json.loads(l)
    if r.get("ok"):
        res[r["stem"] + ".bc"] = r
cov = [json.loads(l) for l in open(sys.argv[2])]


def group(lib):
    return "SkyLight" if "SkyLight.framework" in lib else "QuartzCore" if "QuartzCore.framework" in lib else "other"


def status(c):
    if "crash" in c:
        return "crash"
    if "parse" in c and not c["parse"]:
        return "parse"
    if c.get("unsupported"):
        return "blocked"
    return "ok" if c.get("codegen") else "codegen-fail"


tab = collections.defaultdict(collections.Counter)
block = collections.defaultdict(collections.Counter)
for c in cov:
    r = res.get(c["file"])
    if not r:
        continue
    for g in ("ALL", group(r["lib"])):
        tab[(g, r["type"])][status(c)] += 1
        for k in c.get("unsupported", {}):
            block[g][k.split("  [")[0]] += 1
print("| group | entry type | modules | lowered + llc ok | blocked by a missing air.* rule | codegen fail | crash |\n|---|---|---:|---:|---:|---:|---:|")
for (g, t), c in sorted(tab.items(), key=lambda kv: (kv[0][0] != "SkyLight", kv[0][0] != "QuartzCore", kv[0][0], -sum(kv[1].values()))):
    if g == "other":
        continue
    n = sum(c.values())
    print("| %s | %s | %d | %d | %d | %d | %d |" % (g, t, n, c["ok"], c["blocked"], c["codegen-fail"], c["crash"] + c["parse"]))
for g in ("SkyLight", "QuartzCore", "ALL"):
    print("\nTop missing air.* rules, %s (modules blocked):\n" % g)
    for k, v in block[g].most_common(25 if g == "ALL" else 20):
        print("- `%s` %d" % (k, v))
