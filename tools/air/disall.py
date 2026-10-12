#!/usr/bin/env python3
"""disall.py: extract every AIR module of every metallib under a tree and run llvm-dis (LLVM 22) on it.

  disall.py TREE OUTDIR [--llvm-dis llvm-dis] [-j N]

For each file: OUTDIR/<tree-relative path with '/' -> '__'>/<NNNN>_<function>.bc and .ll (.ll only when llvm-dis succeeds).
Writes OUTDIR/results.jsonl (one line per module: lib, kind, index, name, type, size, sha256, ok, error) and
OUTDIR/skipped.jsonl (files that are not an MTLB / fat / builtin container, with the reason).
Apple's files stay where they are; OUTDIR must be outside the repository (it holds Apple-derived text).
"""
import argparse
import concurrent.futures as cf
import hashlib
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import metallib  # noqa: E402


def job(args):
    lib_dir, fn, bc, llvm_dis = args
    sha = hashlib.sha256(bc).hexdigest()
    stem = os.path.join(lib_dir, "%04d_%s" % (fn.index, metallib.safe(fn.name)))
    with open(stem + ".bc", "wb") as f:
        f.write(bc)
    r = subprocess.run([llvm_dis, stem + ".bc", "-o", stem + ".ll"], capture_output=True, text=True)
    err = r.stderr.strip()
    if r.returncode != 0:
        try:
            os.unlink(stem + ".ll")
        except FileNotFoundError:
            pass
    return {"index": fn.index, "name": fn.name, "type": fn.type_name, "size": len(bc), "sha256": sha, "ok": r.returncode == 0,
            "error": err[:400], "stem": stem}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tree")
    ap.add_argument("out")
    ap.add_argument("--llvm-dis", default="llvm-dis")
    ap.add_argument("-j", type=int, default=os.cpu_count())
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    files = []
    for dp, _dn, fns in os.walk(a.tree):
        for f in sorted(fns):
            p = os.path.join(dp, f)
            if not os.path.islink(p):
                files.append(p)
    files.sort()
    res = open(os.path.join(a.out, "results.jsonl"), "w")
    skp = open(os.path.join(a.out, "skipped.jsonl"), "w")
    with cf.ThreadPoolExecutor(a.j) as ex:
        for p in files:
            rel = os.path.relpath(p, a.tree)
            try:
                data, libs = metallib.open_file(p)
            except Exception as e:  # not a container we know
                skp.write(json.dumps({"lib": rel, "reason": str(e)}) + "\n")
                continue
            lib_dir = os.path.join(a.out, rel.replace("/", "__"))
            os.makedirs(lib_dir, exist_ok=True)
            jobs = []
            for lib in libs:
                for fn in lib.functions:
                    try:
                        bc = metallib.module_bytes(data, fn)
                    except metallib.FormatError as e:
                        res.write(json.dumps({"lib": rel, "kind": lib.kind, "index": fn.index, "name": fn.name, "ok": False, "error": "extract: %s" % e}) + "\n")
                        continue
                    jobs.append((lib_dir, fn, bc, a.llvm_dis))
            n_ok = 0
            for lib_kind, r in zip([lib.kind for lib in libs for _ in lib.functions], ex.map(job, jobs)):
                r["lib"], r["kind"] = rel, lib_kind
                n_ok += r["ok"]
                res.write(json.dumps(r) + "\n")
            res.flush()
            print("%-110s %6d modules, %6d disassemble" % (rel[-110:], len(jobs), n_ok), flush=True)
            data.close()


if __name__ == "__main__":
    main()
