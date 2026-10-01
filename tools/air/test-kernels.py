#!/usr/bin/env python3
"""test-kernels.py: run the AIR-lowered kernels on the card through air-run and compare with a CPU reference.

  REPLAY_COMPUTE_OK=1 tools/air/test-kernels.py [--only NAME] [--build-dir build/air]

Needs build/air/air-run and build/air/k/<kernel>.hsaco (tools/air/run-spike.sh builds both). Standard library only.
Each case prints PASS/FAIL with the first mismatch; exit status 1 when any case failed or a launch failed.
"""
import argparse
import os
import random
import struct
import subprocess
import sys
import tempfile


def run(build, hsaco, name, tg, groups, args, work):
    """args: list of ('buf', bytes) | ('zero', nbytes) | ('u32', n). Returns {arg_index: bytes} of the buffers after the launch."""
    cmd = [os.path.join(build, "air-run"), os.path.join(build, "k", hsaco + ".hsaco"), name, "--tg", ",".join(map(str, tg)),
           "--groups", ",".join(map(str, groups)), "--out", work]
    for i, (k, v) in enumerate(args):
        if k == "buf":
            p = os.path.join(work, "in%d.bin" % i)
            open(p, "wb").write(v)
            cmd.append("buf:" + p)
        elif k == "zero":
            cmd.append("zero:%d" % v)
        else:
            cmd.append("u32:%d" % v)
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    sys.stdout.write("".join("    | %s\n" % l for l in r.stdout.splitlines()))
    if r.returncode:
        sys.stdout.write("    | stderr: %s\n" % r.stderr.strip())
        return None
    return {i: open(os.path.join(work, "buf%d.bin" % i), "rb").read() for i, (k, v) in enumerate(args) if k in ("buf", "zero")}


def case_group_uniform_add_float(build, work):
    # Apple's own group_uniform_add_float (CorePhotogrammetry): out[tid] += group_sum[group] for tid < num_input.
    # num_input (1024) is larger than the grid (1000 threads) on purpose: the hardware guard of the lowering must stop threads 1000..1023.
    rnd = random.Random(1)
    N, grid, ngroups, num_input = 1024, 1000, 16, 1024
    out0 = [rnd.uniform(-100, 100) for _ in range(N)]
    gs = [rnd.uniform(-10, 10) for _ in range(ngroups)]
    f32 = lambda x: struct.unpack("<f", struct.pack("<f", x))[0]
    out0 = [f32(x) for x in out0]
    gs = [f32(x) for x in gs]
    args = [("buf", struct.pack("<%df" % N, *out0)), ("buf", struct.pack("<%df" % ngroups, *gs)), ("buf", struct.pack("<I", num_input)),
            ("u32", grid), ("u32", 1), ("u32", 1)]
    res = run(build, "group_uniform_add_float", "group_uniform_add_float", (64, 1, 1), (ngroups, 1, 1), args, work)
    if res is None:
        return False
    got = struct.unpack("<%df" % N, res[0][:4 * N])
    want = [f32(out0[i] + gs[i // 64]) if i < min(grid, num_input) else out0[i] for i in range(N)]
    bad = [i for i in range(N) if got[i] != want[i]]
    if bad:
        i = bad[0]
        print("    FAIL: %d of %d wrong; first at %d: got %r want %r" % (len(bad), N, i, got[i], want[i]))
        return False
    print("    PASS: %d floats (%d updated, %d untouched incl. the 24 threads past the grid)" % (N, grid, N - grid))
    return True


def case_reduce_sum(build, work):
    # Apple's ReduceSumKernel_uint (CoreRE3DGSFoundation): each 1024-thread group sums its 1024 inputs through threadgroup memory, a barrier and a
    # simdgroup sum; thread 0 stores the sum to output[group].
    rnd = random.Random(2)
    groups, tg = 5, 1024
    n = groups * tg
    data = [rnd.getrandbits(32) for _ in range(n)]
    args = [("buf", struct.pack("<%dI" % n, *data)), ("zero", 4 * groups), ("u32", n), ("u32", 1), ("u32", 1)]
    # the lowered kernel's signature is (input, output, grid.x, grid.y, grid.z): the builtin arguments are computed, not passed
    res = run(build, "ReduceSumKernel_uint", "ReduceSumKernel_uint", (tg, 1, 1), (groups, 1, 1), args, work)
    if res is None:
        return False
    got = struct.unpack("<%dI" % groups, res[1][:4 * groups])
    want = [sum(data[g * tg:(g + 1) * tg]) & 0xffffffff for g in range(groups)]
    if list(got) != want:
        print("    FAIL: got %s want %s" % (list(got), want))
        return False
    print("    PASS: %d group sums of %d uints" % (groups, tg))
    return True


CASES = {"group_uniform_add_float": case_group_uniform_add_float, "ReduceSumKernel_uint": case_reduce_sum}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only")
    ap.add_argument("--build-dir", default="build/air")
    a = ap.parse_args()
    if os.environ.get("REPLAY_COMPUTE_OK") != "1":
        print("test-kernels: runs on the GPU; set REPLAY_COMPUTE_OK=1 (reviewer's go-ahead)")
        return 1
    ok = True
    for name, fn in CASES.items():
        if a.only and a.only != name:
            continue
        print("== %s" % name)
        with tempfile.TemporaryDirectory() as work:
            ok &= bool(fn(a.build_dir, work))
    print("ALL PASS" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
