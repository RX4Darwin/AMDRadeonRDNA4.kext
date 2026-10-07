#!/usr/bin/env python3
"""M0 check (hub-task-427): does the IOPCIDevice vtable that our compiler derives from MacKernelSDK's headers match the one Tahoe's IOPCIFamily
really has? A nub that subclasses IOPCIDevice and overrides virtuals by slot is only right if both agree.

  check-vtable.py <clang -fdump-vtable-layouts output of a class deriving IOPCIDevice>    (tools/m0/check-vtable.sh produces it)
Compares, slot by slot, 'Class::method' (parameters ignored) from the header against the symbol the BootKernelExtensions.kc vtable points at.
Needs ~/work/tools/macos-full (Apple's collection stays outside the repo)."""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pvdis
from kc import demangle


def method(sig):
    q = re.search(r'([A-Za-z_]\w*::~?\w+)\(', sig)
    return q.group(1) if q else sig


def header_slots(path):
    slots = {}
    inside = False
    for line in open(path):
        if line.startswith("Vtable for"):
            inside = True
            continue
        if line.startswith("VTable indices"):
            break
        m = re.match(r'\s*(\d+) \| (.*)$', line)
        if inside and m and '::' in m.group(2):
            slots[int(m.group(1)) - 2] = method(m.group(2))
    return slots


def apple_slots():
    b = pvdis.kc(pvdis.BKE)
    sm = pvdis.all_symbols(b)
    out = {}
    for i, p, n in pvdis.slots(b, 'IOPCIDevice', 0, 330, sm):
        out[i] = method(demangle([n])[0]) if n != '?' else '?'
    return out


h, a = header_slots(sys.argv[1]), apple_slots()
bad = 0


def norm(s):
    return re.sub(r'::_RESERVED\w+', '::R', s)


for i in sorted(h):
    ha, aa = h[i], a.get(i, '<none>')
    if ha != aa and norm(ha).split('::')[1:] != norm(aa).split('::')[1:]:
        bad += 1
        print('slot %3d (+0x%03x): header %-52s Tahoe %s' % (i, 8 * i, ha, aa))
print('%d header slots, %d Tahoe slots, %d mismatches' % (len(h), len(a), bad))
sys.exit(1 if bad else 0)
