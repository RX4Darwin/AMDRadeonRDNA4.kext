#!/usr/bin/env python3
"""The register state RADV had programmed when it drew its triangle, from a RADV_DEBUG=dumpibs dump
(tools/radv-triangle), as REPLAY_SET entries for tools/linux-replay.

Decodes the main IB as raw PM4 (the first hex dword of every dump line), keeps the last value of every
SET_CONTEXT_REG / SET_SH_REG / SET_UCONFIG_REG (plain, _INDEX and _PAIRS forms) written before the
N-th DRAW_INDEX_AUTO (default: the last one, the triangle; the first is RADV's clear).

    radv-state.py <dump> [--draw N] [--kinds cu] [--exclude c:0x318,...]   -> one entry per line
"""
import argparse
import re
import sys

OPS = {0x69: 'c', 0x76: 's', 0x79: 'u', 0x7a: 'u', 0xb8: 'c', 0xba: 's'}   # 0xb8/0xba: *_REG_PAIRS
PAIRS = {0xb8, 0xba}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dump')
    ap.add_argument('--draw', type=int, default=-1)
    ap.add_argument('--kinds', default='csu')
    ap.add_argument('--exclude', default='')
    a = ap.parse_args()

    text = open(a.dump).read()
    body = text.split('Main IB begin', 1)[1].split('Main IB end', 1)[0]
    dws = [int(m.group(1), 16) for m in re.finditer(r'^([0-9a-f]{8})\b', body, re.M)]

    state, snapshots = {}, []
    i = 0
    while i < len(dws):
        hdr = dws[i]
        if hdr >> 30 != 3:          # type-2 filler
            i += 1
            continue
        op, count = (hdr >> 8) & 0xff, (hdr >> 16) & 0x3fff
        payload = dws[i + 1:i + 2 + count]
        kind = OPS.get(op)
        if kind and op in PAIRS:
            for k in range(0, len(payload) - 1, 2):
                state[(kind, payload[k] & 0xffff)] = payload[k + 1]
        elif kind:
            first = payload[0] & 0xffff
            for k, v in enumerate(payload[1:]):
                state[(kind, first + k)] = v
        elif op == 0x2d:             # DRAW_INDEX_AUTO
            snapshots.append(dict(state))
        i += 2 + count

    snap = snapshots[a.draw]
    excl = set()
    for e in filter(None, a.exclude.split(',')):
        kind, reg = e.split(':')
        excl.add((kind, int(reg, 16)))
    for (kind, reg), v in sorted(snap.items()):
        if kind in a.kinds and (kind, reg) not in excl:
            print(f'{kind}:0x{reg:03x}=0x{v:08x}')
    print(f'# {len(snapshots)} draws, draw {a.draw}: {len(snap)} registers', file=sys.stderr)


if __name__ == '__main__':
    main()
