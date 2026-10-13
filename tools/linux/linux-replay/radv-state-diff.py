#!/usr/bin/env python3
"""Register differences between two RADV dumpibs captures at their last draw, with gfx12.json names.

    radv-state-diff.py <dumpA> <dumpB> [--json <gfx12.json>]

Uses radv-state.py (next to this file) for the state of each dump; MESA_GFX12_JSON names the registers.
Used for docs/g4-colour.md: RADV's G3 capture against its G4 (colour) capture.
"""
import json, os, subprocess, sys, argparse

HERE = os.path.dirname(os.path.abspath(__file__))
BASE = {'c': 0xa000, 's': 0x2c00, 'u': 0xc000}


def state(dump):
    out = subprocess.run([sys.executable, os.path.join(HERE, 'radv-state.py'), dump], capture_output=True, text=True, check=True).stdout
    d = {}
    for line in out.splitlines():
        k, v = line.split('=')
        kind, reg = k.split(':')
        d[(kind, int(reg, 16))] = int(v, 16)
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--json', default=os.environ.get('MESA_GFX12_JSON', ''))
    a = ap.parse_args()
    if not a.json:
        sys.exit('radv-state-diff.py: pass --json or set MESA_GFX12_JSON (Mesa src/amd/registers/gfx12.json)')
    J = json.load(open(a.json))
    names = {r['map']['at'] // 4: r['name'] for r in J['register_mappings']}
    sa, sb = state(a.a), state(a.b)
    for k in sorted(set(sa) | set(sb)):
        if sa.get(k) != sb.get(k):
            n = names.get(BASE[k[0]] + k[1], '?')
            fa = '0x%08x' % sa[k] if k in sa else '(unset)'
            fb = '0x%08x' % sb[k] if k in sb else '(unset)'
            print('%s:0x%03x %-34s A=%-12s B=%s' % (k[0], k[1], n, fa, fb))


if __name__ == '__main__':
    main()
