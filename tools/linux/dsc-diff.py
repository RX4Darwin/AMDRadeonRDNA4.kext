#!/usr/bin/env python3
"""dsc-diff.py <capture regs-*.txt> [src/pipe2_linux_*.inc]

Compares a register snapshot taken under Linux (tools/linux/linux-capture.sh --snap-only, the monitor lit by amdgpu)
with one of our lighting tables (default: the DSC 4K144 one, src/pipe2_linux_p0_dp1_dsc144.inc). The table's writes
are replayed into the value they leave in each register; only the bits the table sets are compared. Also lists the DSC
engines and timing generators besides pipe 0 that Linux has running (a stream split over two pipes, ODM combine)."""
import re
import sys

snap = {}
names = {}
for line in open(sys.argv[1]):
    p = line.split()
    if len(p) < 5 or p[0].startswith('#') or not p[4].startswith('0x'):
        continue
    key = (int(p[1]), int(p[2], 16))
    snap[key] = int(p[4], 16)
    names[key] = p[0]

table = sys.argv[2] if len(sys.argv) > 2 else 'src/pipe2_linux_p0_dp1_dsc144.inc'
want = {}   # (seg, dword) -> [mask, value, last writer's text]
for kind, seg, dword, mask, value, text in re.findall(
        r"\{ '([WU])', (\d+), (0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+), \d+, \"([^\"]*)\"", open(table).read()):
    key = (int(seg), int(dword, 16))
    mask, value = int(mask, 16), int(value, 16)
    if kind == 'W':
        mask = 0xffffffff
    m, v, _ = want.get(key, [0, 0, ''])
    want[key] = [m | mask, (v & ~mask) | (value & mask), text]

same = differ = unread = 0
print(f'{"register":44} {"ours":>10} {"mask":>10} {"Linux":>10}  last write in our table')
for key in sorted(want):
    mask, value, text = want[key]
    if key not in snap:
        unread += 1
        continue
    linux = snap[key]
    if (linux & mask) == (value & mask):
        same += 1
        continue
    differ += 1
    print(f'{names[key]:44} 0x{value & mask:08x} 0x{mask:08x} 0x{linux:08x}  {text}')
print(f'\n{same} registers as ours, {differ} differ, {unread} of ours not in the snapshot')

print('\nRunning besides pipe 0 under Linux:')
for key, v in sorted(snap.items()):
    n = names[key]
    if (re.match(r'DSC_TOP[1-3]_DSC_TOP_CONTROL$', n) and v & 1) or \
       (re.match(r'OTG[1-3]_OTG_CONTROL$', n) and v & 1) or \
       (re.match(r'ODM[0-3]_OPTC_DATA_SOURCE_SELECT$', n) and v) or \
       (re.match(r'DSCC[1-3]_DSCC_CONFIG0$', n) and v):
        print(f'  {n} = 0x{v:08x}')
