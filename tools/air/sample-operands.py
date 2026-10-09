#!/usr/bin/env python3
"""sample-operands.py -- S5: pin down the AIR texture-intrinsic operand ABI.

Context: docs/m3-air-spike.md section 8.1(b).  That section lists, as the first
blocker of the whole graphics path:

    the meaning of the extra operands of sample_* (the i1/float/i32 tail encodes
    bias/level/gradient/offset/min-lod; UNKNOWN, to be derived from the corpus
    variants, since 2-D, 1-D-array and 3-D forms are all present)

Textures are the dominant blocker by a wide margin: about 6,300 of the 8,438
kernels that fail to lower stop on a texture/sampler argument or call, and 18 of
the 24 SkyLight fragment shaders stop on air.sample_texture_2d alone.  Nothing
about this question needs the card, macOS, a reboot or a download -- the answer
is already sitting in the corpus that disall.py wrote.

WHAT THIS DOES
    Walks the disassembled AIR (.ll) tree, finds every call to an air.* texture
    intrinsic, and reports the DISTINCT OPERAND SHAPES: for each intrinsic, which
    argument positions are compile-time constants, what those constants are, and
    how often each combination occurs, with an example module per shape.

WHY THAT ANSWERS IT
    Metal's sampling options are fixed at the call site in the shader source
    (sample(s,c) / level(l) / bias(b) / gradient2d(dx,dy) / min_lod_clamp(m),
    plus an optional int2 offset).  Each source form must compile to one
    constant-flag pattern in the AIR tail.  So the set of distinct patterns is
    small, and each one maps to exactly one llvm.amdgcn.image.sample.* variant.
    Reading the patterns off 8,600 real call sites beats guessing.

HYPOTHESIS TO FALSIFY (from the single example in m3-air-spike s.4, the
3-instruction SimpleTextureFragment, annotated here so the output can refute it):

    air.sample_texture_2d.v4f32(
        ptr addrspace(1) tex, ptr addrspace(2) samp, <2 x float> coord,
        i1   A,     ; true in the example -- offset-present? lod-is-default?
        <2 x i32> B,; zeroinitializer     -- the int2 offset
        i1   C,     ; false               -- bias-vs-level select? compare?
        float D,    ; 0.0                 -- bias or explicit lod
        float E,    ; 0.0                 -- min_lod_clamp
        i32  F)     ; 0                   -- ? (gather component / aniso / cache)

    Predictions, each checkable in the output below:
      P1  B is constant in nearly every call (offsets are literals in Metal) and
          non-zero only in a small minority.
      P2  D is a non-constant SSA value exactly when the source said bias(x) or
          level(x); so the (A,C) constant pair should PARTITION the calls into a
          handful of groups, and D's constant-ness should correlate with them.
      P3  F is 0 almost everywhere; any non-zero F clusters in gather_* calls.
      P4  The number of distinct (A,C,F) constant triples is <= ~8, matching the
          count of Metal lod_options forms.
    If P2/P4 fail, the tail is not a flag encoding and we read AGX's own lowering
    instead (the AGXMetal *_rt.metallib builtins are in the same corpus).

USAGE (host only; needs python3 and the .ll tree disall.py already produced)

    python3 tools/air/sample-operands.py ~/work/tools/macos-full/25G83/metallib/air

    python3 tools/air/sample-operands.py DIR --json ops.json   # machine-readable
    python3 tools/air/sample-operands.py DIR --intrinsic sample_texture_2d
    python3 tools/air/sample-operands.py DIR --examples 3      # more examples/shape

Read-only: opens files, writes only the report (and --json if asked).
"""

import argparse
import json
import os
import re
import sys
from collections import Counter, defaultdict

# Which intrinsic families this is about.  air.get_* and air.*_sampler are
# included because the descriptor ABI needs them too (m3-air-spike s.8.1(d)).
FAMILY_RE = re.compile(
    r"@(air\.(?:sample|gather|read|write)_texture[A-Za-z0-9_.]*"
    r"|air\.get_(?:width|height|depth|array_size|num_mip_levels|num_samples"
    r"|read_sampler|null_texture)[A-Za-z0-9_.]*"
    r"|air\.(?:load|store)[A-Za-z0-9_.]*imageblock[A-Za-z0-9_.]*)\("
)

CALL_LINE_RE = re.compile(r"\b(?:tail\s+)?call\b")
DECLARE_RE = re.compile(r"^\s*declare\b")

# A constant operand, for our purposes: anything that is not an SSA register.
SSA_RE = re.compile(r"^%[A-Za-z0-9_.$\-]+$")


def split_top_level(text):
    """Split an argument list on commas that are not nested in <>, [], {} or ()."""
    out, depth, cur, in_str = [], 0, [], False
    for ch in text:
        if in_str:
            cur.append(ch)
            if ch == '"':
                in_str = False
            continue
        if ch == '"':
            in_str = True
            cur.append(ch)
        elif ch in "<[{(":
            depth += 1
            cur.append(ch)
        elif ch in ">]})":
            depth -= 1
            cur.append(ch)
        elif ch == "," and depth == 0:
            out.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if cur and "".join(cur).strip():
        out.append("".join(cur).strip())
    return out


def balanced_args(line, open_idx):
    """Return the text between the parens starting at open_idx, or None."""
    depth = 0
    for i in range(open_idx, len(line)):
        c = line[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return line[open_idx + 1:i]
    return None


# Parameter attributes LLVM may put between the type and the value.
ATTRS = {"nocapture", "readonly", "readnone", "writeonly", "nonnull", "noundef",
         "immarg", "inreg", "signext", "zeroext", "returned", "nofree",
         "captures(none)", "captures(address)", "dereferenceable", "align"}


def classify(arg):
    """('<2 x i32>', 'zeroinitializer', True) from '<2 x i32> zeroinitializer'."""
    toks = arg.split()
    if not toks:
        return ("?", "?", False)
    # Type is everything up to the last token that is not an attribute, which is
    # fiddly for '<2 x float>' etc; do it by taking the LAST whitespace-separated
    # token as the value and the rest as the type, then strip attributes.
    value = toks[-1]
    type_toks = []
    for t in toks[:-1]:
        base = t.split("(")[0]
        if base in ATTRS or t.startswith("align") or t.startswith("dereferenceable"):
            continue
        type_toks.append(t)
    ty = " ".join(type_toks) if type_toks else "?"
    is_const = not bool(SSA_RE.match(value))
    return (ty, value, is_const)


def shape_key(args):
    """Operand shape: type + (constant value | VAR) per position."""
    parts = []
    for a in args:
        ty, val, is_const = classify(a)
        parts.append("%s %s" % (ty, val if is_const else "VAR"))
    return " | ".join(parts)


def scan(root, want=None):
    per_intr = defaultdict(Counter)          # intrinsic -> shape -> count
    examples = defaultdict(lambda: defaultdict(list))  # intrinsic -> shape -> files
    const_by_pos = defaultdict(lambda: defaultdict(Counter))  # intr -> pos -> value
    files_with = defaultdict(set)
    nfiles = 0
    skipped = 0

    for dirpath, _dirs, names in os.walk(root):
        for name in names:
            if not name.endswith((".ll", ".air.ll")):
                continue
            path = os.path.join(dirpath, name)
            nfiles += 1
            try:
                with open(path, "r", errors="replace") as fh:
                    text = fh.read()
            except OSError:
                skipped += 1
                continue
            if "@air." not in text:
                continue
            for line in text.splitlines():
                if "@air." not in line or DECLARE_RE.match(line):
                    continue
                if not CALL_LINE_RE.search(line):
                    continue
                m = FAMILY_RE.search(line)
                if not m:
                    continue
                intr = m.group(1)
                if want and want not in intr:
                    continue
                raw = balanced_args(line, m.end() - 1)
                if raw is None:
                    skipped += 1
                    continue
                args = split_top_level(raw)
                key = shape_key(args)
                per_intr[intr][key] += 1
                files_with[intr].add(os.path.relpath(path, root))
                ex = examples[intr][key]
                if len(ex) < 8:
                    ex.append(os.path.relpath(path, root))
                for i, a in enumerate(args):
                    ty, val, is_const = classify(a)
                    const_by_pos[intr][i][val if is_const else "VAR"] += 1
    return per_intr, examples, const_by_pos, files_with, nfiles, skipped


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir", help="the .ll tree disall.py wrote")
    ap.add_argument("--intrinsic", default=None,
                    help="only intrinsics whose name contains this substring")
    ap.add_argument("--examples", type=int, default=1,
                    help="example modules to print per shape (default 1)")
    ap.add_argument("--top", type=int, default=25,
                    help="shapes to print per intrinsic (default 25)")
    ap.add_argument("--json", default=None, help="also write the raw result here")
    args = ap.parse_args()

    if not os.path.isdir(args.dir):
        print("not a directory: %s" % args.dir, file=sys.stderr)
        print("expected the output of tools/air/disall.py (see docs/m3-air-spike.md s.9)",
              file=sys.stderr)
        return 2

    per_intr, examples, const_by_pos, files_with, nfiles, skipped = scan(
        args.dir, args.intrinsic)

    if not per_intr:
        print("no texture intrinsic calls found under %s" % args.dir)
        print("(scanned %d .ll files)" % nfiles)
        return 1

    print("=== S5: AIR texture-intrinsic operand census ===")
    print("tree: %s" % args.dir)
    print("files scanned: %d   unparsed call lines: %d" % (nfiles, skipped))
    print()

    order = sorted(per_intr.items(), key=lambda kv: -sum(kv[1].values()))
    print("--- intrinsics by call sites ---")
    for intr, shapes in order:
        print("  %-52s %7d calls  %5d modules  %4d distinct shapes"
              % (intr, sum(shapes.values()), len(files_with[intr]), len(shapes)))
    print()

    for intr, shapes in order:
        total = sum(shapes.values())
        print("=" * 78)
        print("%s  (%d calls, %d distinct operand shapes)" % (intr, total, len(shapes)))
        print("=" * 78)
        print()
        print("  per-position constants (VAR = a runtime value):")
        for pos in sorted(const_by_pos[intr]):
            vals = const_by_pos[intr][pos].most_common(6)
            rendered = ", ".join("%s x%d" % (v, c) for v, c in vals)
            print("    arg %-2d  %s" % (pos, rendered))
        print()
        print("  shapes:")
        for shape, count in shapes.most_common(args.top):
            print("    %6d  (%5.1f%%)  %s" % (count, 100.0 * count / total, shape))
            for ex in examples[intr][shape][:max(0, args.examples)]:
                print("              e.g. %s" % ex)
        if len(shapes) > args.top:
            print("    ... %d more shapes" % (len(shapes) - args.top))
        print()

    print("=" * 78)
    print("How to read this (docs/m3-air-spike.md s.8.1(b), predictions P1-P4):")
    print("  P1  the <2 x i32> position is constant nearly everywhere, and")
    print("      zeroinitializer in the large majority -> it is the int2 offset.")
    print("  P2  the float positions switch between a constant 0.0 and VAR; the")
    print("      i1 positions should partition the calls into a few groups that")
    print("      correlate with that -> the i1s select bias/level/gradient.")
    print("  P3  the trailing i32 is 0 almost everywhere; non-zero values should")
    print("      cluster in gather_* (the gather component).")
    print("  P4  <= ~8 distinct (i1, i1, i32) constant triples, matching Metal's")
    print("      lod_options forms. Many more than that = not a flag encoding;")
    print("      fall back to reading AGX's own lowering in the AGXMetal")
    print("      *_rt.metallib builtins, which are in the same corpus.")
    print()
    print("Then: each surviving group maps to one llvm.amdgcn.image.sample.*")
    print("variant, which is the rule table air2amdgcn.cpp is missing.")

    if args.json:
        blob = {
            "tree": args.dir,
            "files_scanned": nfiles,
            "intrinsics": {
                intr: {
                    "calls": sum(sh.values()),
                    "modules": len(files_with[intr]),
                    "shapes": [
                        {"shape": s, "count": c, "examples": examples[intr][s][:8]}
                        for s, c in sh.most_common()
                    ],
                    "const_by_pos": {
                        str(p): dict(const_by_pos[intr][p])
                        for p in sorted(const_by_pos[intr])
                    },
                }
                for intr, sh in order
            },
        }
        with open(args.json, "w") as fh:
            json.dump(blob, fh, indent=1, sort_keys=True)
        print()
        print("wrote %s" % args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
