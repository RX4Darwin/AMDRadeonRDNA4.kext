#!/usr/bin/env python3
"""census.py: statistics over the .ll files that disall.py wrote.

  census.py AIRDIR OUT.json            scan (one pass, all cores), write the counters
  census.py AIRDIR OUT.json --report   print markdown tables from OUT.json (groups: ALL unique modules, SkyLight, QuartzCore)

Per module: air.* call sites (full name and "family" = name without its trailing type components), llvm.* calls,
air.* metadata strings (what the argument/entry metadata says), named metadata, address spaces, vector/scalar types,
instruction opcodes, argument-type strings (texture2d<float, sample>, ...). A module counts once per "modules" column
and every occurrence in the "uses" column. ALL deduplicates by module SHA-256 (AGX variants share many modules).
"""
import collections
import json
import multiprocessing
import os
import re
import sys

CALL = re.compile(r"@(air\.[A-Za-z0-9_.]+|llvm\.[A-Za-z0-9_.]+)\(")
STR = re.compile(r'!"(air\.[A-Za-z0-9_.]+)"')
NAMED = re.compile(r"^!((?:air|llvm)\.[A-Za-z0-9_.]+) = ", re.M)
ASPACE = re.compile(r"addrspace\((\d+)\)")
VEC = re.compile(r"<(\d+) x ([a-z0-9]+)>")
SCALAR = re.compile(r"\b(half|bfloat|float|double|i1|i8|i16|i32|i64|i128)\b")
OPC = re.compile(r"^\s+(?:%[\w.]+ = )?(?:tail |musttail |notail )?([a-z_]+)")
ARGTYPE = re.compile(r'!"air\.arg_type_name", !"([^"]+)"')
TYPEPART = re.compile(r"^(?:v\d+)?[fisu]\d+$|^p\d+\w*$|^(?:fast|precise)$|^(?:s|u|f)$")


def family(name):
    """air.sample_texture_2d.v4f32 -> air.sample_texture_2d ; air.convert.f.f32.u.i32 -> air.convert"""
    parts = name.split(".")
    keep = [parts[0]]
    for p in parts[1:]:
        if TYPEPART.match(p):
            break
        keep.append(p)
    return ".".join(keep)


def scan(path):
    c = {k: collections.Counter() for k in ("call", "family", "llvm", "mdstr", "named", "aspace", "vec", "scalar", "opc", "argtype")}
    with open(path, errors="replace") as f:
        txt = f.read()
    ninst = 0
    for line in txt.split("\n"):
        if not line or line[0] in "!;@%a}":   # metadata, comment, globals, types, attributes, close brace
            continue
        if line.startswith("declare") or line.startswith("define"):
            for m in ASPACE.finditer(line):
                c["aspace"][m.group(1)] += 1
            for m in VEC.finditer(line):
                c["vec"]["<%s x %s>" % m.groups()] += 1
            continue
        m = OPC.match(line)
        if not m:
            continue
        ninst += 1
        c["opc"][m.group(1)] += 1
        for m in CALL.finditer(line):
            n = m.group(1)
            if n.startswith("air."):
                c["call"][n] += 1
                c["family"][family(n)] += 1
            else:
                c["llvm"][re.sub(r"\.(v\d+)?[fi]\d+(\.(v\d+)?[fi]\d+)*$", "", n)] += 1
        for m in ASPACE.finditer(line):
            c["aspace"][m.group(1)] += 1
        for m in VEC.finditer(line):
            c["vec"]["<%s x %s>" % m.groups()] += 1
        for m in SCALAR.finditer(line):
            c["scalar"][m.group(1)] += 1
    for m in STR.finditer(txt):
        c["mdstr"][m.group(1)] += 1
    for m in NAMED.finditer(txt):
        c["named"][m.group(1)] += 1
    for m in ARGTYPE.finditer(txt):
        c["argtype"][m.group(1)] += 1
    out = {k: dict(v) for k, v in c.items()}
    out["ninst"] = ninst
    return path, out


def lib_group(lib):
    if "SkyLight.framework" in lib:
        return "SkyLight"
    if "QuartzCore.framework" in lib:
        return "QuartzCore"
    return None


def run_scan(airdir, outjson):
    rows = [json.loads(l) for l in open(os.path.join(airdir, "results.jsonl"))]
    rows = [r for r in rows if r.get("ok")]
    seen, jobs, members = {}, [], {}
    for r in rows:
        stem = r["stem"] + ".ll"
        if r["sha256"] not in seen:
            seen[r["sha256"]] = stem
            jobs.append(stem)
        members.setdefault(r["sha256"], []).append({"lib": r["lib"], "kind": r["kind"], "name": r["name"], "type": r["type"], "size": r["size"]})
    res = {}
    with multiprocessing.Pool() as pool:
        for i, (p, out) in enumerate(pool.imap_unordered(scan, jobs, chunksize=16)):
            res[p] = out
            if i % 2000 == 0:
                print(i, "/", len(jobs), file=sys.stderr, flush=True)
    mods = []
    for sha, stem in seen.items():
        mods.append({"sha": sha, "stem": stem, "members": members[sha], "counts": res[stem]})
    json.dump(mods, open(outjson, "w"))
    print("scanned", len(mods), "unique modules")


def agg(mods, key):
    modules, uses = collections.Counter(), collections.Counter()
    for m in mods:
        for k, v in m["counts"].get(key, {}).items():
            modules[k] += 1
            uses[k] += v
    return modules, uses


def table(title, mods, key, top=30, only=None, total=None):
    modules, uses = agg(mods, key)
    items = [(k, modules[k], uses[k]) for k in modules if not only or only(k)]
    items.sort(key=lambda x: (-x[1], -x[2], x[0]))
    print("\n**%s** (%d distinct)\n" % (title, len(items)))
    print("| name | modules | uses |\n|---|---:|---:|")
    for k, a, b in items[:top]:
        print("| `%s` | %d | %d |" % (k, a, b))
    if len(items) > top:
        print("| _... %d more_ | | |" % (len(items) - top))


def report(jsonfile):
    mods = json.load(open(jsonfile))

    def sel(g):
        if g == "ALL":     # everything except the AGX builtin libraries (Apple-GPU internals)
            return [m for m in mods if any(x["kind"] != "BUILTIN" for x in m["members"])]
        if g == "AGX builtin":
            return [m for m in mods if all(x["kind"] == "BUILTIN" for x in m["members"])]
        return [m for m in mods if any(lib_group(x["lib"]) == g for x in m["members"])]

    for g in ("ALL", "SkyLight", "QuartzCore", "AGX builtin"):
        ms = sel(g)
        print("\n## Census: %s (%d unique modules)\n" % (g, len(ms)))
        types = collections.Counter()
        for m in ms:
            t = set(x["type"] for x in m["members"] if g == "ALL" or lib_group(x["lib"]) == g)
            for x in t:
                types[x] += 1
        print("Entry types (modules): " + ", ".join("%s %d" % kv for kv in types.most_common()))
        ni = sorted(m["counts"]["ninst"] for m in ms)
        print("\nInstructions per module: min %d, median %d, p90 %d, max %d, total %d" % (ni[0], ni[len(ni) // 2], ni[int(len(ni) * .9)], ni[-1], sum(ni)))
        top = 40 if g in ("ALL", "AGX builtin") else 60
        table("air.* intrinsic families (call sites)", ms, "family", top)
        table("air.* intrinsics, full names", ms, "call", top)
        table("air.* metadata strings (entry/argument metadata keys)", ms, "mdstr", 70)
        table("named metadata", ms, "named", 30)
        table("address spaces", ms, "aspace", 10)
        table("vector types", ms, "vec", 20)
        table("scalar types in instructions", ms, "scalar", 12)
        table("llvm.* intrinsics (families)", ms, "llvm", 25)
        table("instruction opcodes", ms, "opc", 25)
        table("argument type names (air.arg_type_name)", ms, "argtype", 40)
        if g in ("SkyLight", "QuartzCore"):
            print("\n**entry points** (name, type, instructions, air.* families)\n\n| name | type | insns | air.* families |\n|---|---|---:|---|")
            for m in sorted(ms, key=lambda m: (m["members"][0]["type"], m["counts"]["ninst"])):
                x = [y for y in m["members"] if lib_group(y["lib"]) == g][0]
                print("| `%s` | %s | %d | %s |" % (x["name"], x["type"], m["counts"]["ninst"], " ".join(sorted(k.replace("air.", "") for k in m["counts"]["family"]))))


if __name__ == "__main__":
    if len(sys.argv) >= 4 and sys.argv[3] == "--report":
        report(sys.argv[2])
    else:
        run_scan(sys.argv[1], sys.argv[2])
