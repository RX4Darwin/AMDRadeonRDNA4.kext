#!/usr/bin/env python3
"""Layout of an Objective-C @encode struct with field names ("name"type...), as stored in ivar type strings: offsets and sizes with natural alignment.
Used for APVDeviceInfoStruct (docs/m0-pvgpu.md): python3 objctype.py <ClassName> <ivar>"""
import re
import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

SIZES = {'c': 1, 'C': 1, 'B': 1, 's': 2, 'S': 2, 'i': 4, 'I': 4, 'l': 8, 'L': 8, 'q': 8, 'Q': 8, 'f': 4, 'd': 8, '*': 8, '^': 8, '@': 8, '#': 8, ':': 8}


def parse(t, i=0):
    """returns (kind, size, align, children, next_index). kind: basic char, 'struct', 'union', 'array'"""
    c = t[i]
    if c in '{(':
        close = '}' if c == '{' else ')'
        j = i + 1
        while t[j] not in '="' + close:
            j += 1
        name = t[i + 1:j]
        kids = []
        if t[j] == '=':
            j += 1
        while t[j] != close:
            if t[j] == '"':
                k = t.index('"', j + 1)
                fname = t[j + 1:k]
                j = k + 1
            else:
                fname = None
            kind, size, align, ch, j = parse(t, j)
            kids.append((fname, kind, size, align, ch))
        if c == '{':
            off = 0              # bytes
            bits = 0             # bit position inside the current 32-bit unit (when in a bitfield run)
            amax = 1
            out = []
            for fname, kind, size, align, ch in kids:
                if kind == 'b':
                    if bits == 0:
                        off = (off + 3) // 4 * 4
                    if bits + size > 32:
                        off += 4
                        bits = 0
                    out.append((fname, 'b%d' % size, size, 4, ch, (off, bits)))
                    bits += size
                    if bits == 32:
                        off += 4
                        bits = 0
                    amax = max(amax, 4)
                    continue
                if bits:
                    off += 4
                    bits = 0
                off = (off + align - 1) // align * align
                out.append((fname, kind, size, align, ch, off))
                off += size
                amax = max(amax, align)
            if bits:
                off += 4
            size = (off + amax - 1) // amax * amax
            return 'struct', size, amax, out, j + 1
        size = max([k[2] if k[1] != 'b' else 4 for k in kids] + [0])
        amax = max([k[3] for k in kids] + [1])
        return 'union', (size + amax - 1) // amax * amax, amax, [(f, k, s, a, ch, 0) for f, k, s, a, ch in kids], j + 1
    if c == '[':
        j = i + 1
        n = 0
        while t[j].isdigit():
            n = n * 10 + int(t[j])
            j += 1
        kind, size, align, ch, j = parse(t, j)
        return 'array', n * size, align, [(None, kind, size, align, ch, 0), n], j + 1
    if c == '^':
        kind, size, align, ch, j = parse(t, i + 1) if t[i + 1] in '{([' or t[i + 1] in SIZES or t[i + 1] == 'v' else ('x', 0, 1, [], i + 2)
        return '^', 8, 8, [], j
    if c == 'v':
        return 'v', 0, 1, [], i + 1
    if c == 'b':
        j = i + 1
        n = 0
        while j < len(t) and t[j].isdigit():
            n = n * 10 + int(t[j])
            j += 1
        return 'b', n, 1, [], j        # size = number of bits
    if c == '@' and i + 1 < len(t) and t[i + 1] == '"':
        k = t.index('"', i + 2)
        return '@', 8, 8, [], k + 1
    return c, SIZES[c], SIZES[c], [], i + 1


def flatten(kind, size, align, kids, prefix='', base=0, out=None):
    out = [] if out is None else out
    if kind == 'struct':
        for fname, k, s, a, ch, off in kids:
            if isinstance(off, tuple):
                out.append((prefix + '.' + (fname or '?'), 'bits %d..%d' % (off[1], off[1] + s - 1), 4, base + off[0]))
            else:
                flatten(k, s, a, ch, prefix + ('.' + fname if fname else ''), base + off, out)
    elif kind == 'union':
        subs = [k for k in kids if k[1] == 'struct']
        if subs:   # SupportFlagsNNNN: union { u32 value; struct { bitfields } }
            out.append((prefix + '.value', 'u32', 4, base))
            flatten(subs[0][1], subs[0][2], subs[0][3], subs[0][4], prefix, base, out)
        else:
            out.append((prefix, 'u32|f32', size, base))
    elif kind == 'array':
        out.append((prefix, 'array', size, base))
    else:
        out.append((prefix, kind, size, base))
    return out


if __name__ == '__main__':
    import apvdis
    img = apvdis.Image(apvdis.APV)
    ix = apvdis.Index(img)
    for k in ix.classes:
        if k['name'] == sys.argv[1]:
            for off, nm, ty, al in k['ivars']:
                if nm == sys.argv[2]:
                    kind, size, align, kids, _ = parse(ty)
                    print('%s.%s: size %d align %d' % (sys.argv[1], nm, size, align))
                    for name, ty2, sz, o in flatten(kind, size, align, kids):
                        print('  +0x%03x %-3d %-14s %s' % (o, sz, ty2, name.lstrip('.')))
