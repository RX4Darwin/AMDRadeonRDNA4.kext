#!/usr/bin/env python3
# Correlate the emulator's own messages with the kext's serial log lines around them.
#
#   emu-context.py <run dir> [lines of context (default 4)]
#
# tools/emu-linux.sh records (qemu.out size, serial.log size) every 0.1 s into
# offsets.txt while the VM runs. A message at byte X of qemu.out was written in the window where
# that size crossed X; the serial lines at the window's end (and a few before) are shown as
# context. Resolution is that of the sampling (~0.1 s: a few serial lines, more when the
# kernel log is busy). Routine chatter (firmware loads, dispatch counts, sensors...) is
# skipped; everything else is listed, repeated messages are grouped by kind.
import bisect
import os
import re
import sys

run = sys.argv[1]
ctx = int(sys.argv[2]) if len(sys.argv) > 2 else 4
ROUTINE = re.compile(r"host doesn't support|psp: (LOAD_IP_FW|bootloader took)|smu: (synthetic|features)|"
                     r"cs: dispatch \d+x\d+x\d+ of .* ran \d+ work-items|work: dispatch slice budget|"
                     r"mec: HQD dequeue reset|dcn: HUBP\d flip latched|AUTOLOAD_RLC: armed|"
                     r"power reset: compute state|gfx: IMU released")
samples = []
with open(os.path.join(run, "offsets.txt")) as f:
    for line in f:
        a, b = line.split()
        samples.append((int(a), int(b)))
qpos = [s[0] for s in samples]
serial = open(os.path.join(run, "serial.log"), "rb").read()


def serial_window(qoff):
    i = min(bisect.bisect_left(qpos, qoff), len(samples) - 1)
    lo = samples[i - 1][1] if i else 0
    hi = samples[i][1]
    return lo, hi


def kind(msg):
    return re.sub(r"\d+", "N", re.sub(r"0x[0-9a-fA-F]+", "N", msg))


out = []
groups = {}
off = 0
with open(os.path.join(run, "qemu.out"), "rb") as f:
    for raw in f:
        msg = raw.decode(errors="replace").rstrip("\n")
        start = off
        off += len(raw)
        if not msg.strip() or ROUTINE.search(msg):
            continue
        k = kind(msg)
        g = groups.setdefault(k, {"n": 0, "first": msg, "last": msg, "off": start})
        g["n"] += 1
        g["last"] = msg
for k, g in sorted(groups.items(), key=lambda kv: kv[1]["off"]):
    lo, hi = serial_window(g["off"])
    window = serial[max(0, lo - 4000):hi + 2000].decode(errors="replace").splitlines()
    kext = [l for l in window if "RDNA4FB" in l or "RDNA4DIAG" in l]
    # the lines ending the window and a few before it: what the kext had just printed
    cut = serial[:hi].decode(errors="replace").count("\n")
    before = [l for l in serial[:hi].decode(errors="replace").splitlines()[-60:] if "RDNA4FB" in l][-ctx:]
    out.append("### %d x  %s" % (g["n"], g["first"]))
    if g["n"] > 1:
        out.append("    last: %s" % g["last"])
    out.append("    kext lines just before its first occurrence (serial byte ~%d of %d):" % (hi, len(serial)))
    out.extend("      | " + l[:230] for l in before)
    out.append("")
print("\n".join(out))
