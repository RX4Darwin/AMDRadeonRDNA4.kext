#!/usr/bin/env python3
"""M0 helper (docs/metal-spike.md, hub-task-427): disassemble functions of Apple's AppleParavirtGPU kext out of the full install's
SystemKernelExtensions.kc, and annotate virtual calls through an IOPCIDevice-shaped object with the slot's method name (taken from the
IOPCIDevice vtable in BootKernelExtensions.kc). Read-only analysis; Apple's bytes stay in ~/work/tools/macos-full (never in git).

  pvdis.py list [substring]            defined symbols (demangled) of the kext that contain the substring
  pvdis.py dis <substring> [-n N]      disassemble every defined function whose demangled name contains the substring
  pvdis.py vt <ClassName> [from to]    vtable slots of a class (resolved through the collections), e.g. IOPCIDevice
Needs: ~/work/tools/macos-full/tools/kc.py, binutils objdump, c++filt.
"""
import os, re, struct, subprocess, sys
TOOLS = os.path.expanduser('~/work/tools/macos-full')
sys.path.insert(0, TOOLS + '/tools')
from kc import KC, demangle

SKE = TOOLS + '/out/payload/System/Library/KernelCollections/SystemKernelExtensions.kc'
BKE = TOOLS + '/out/dsc/BootKernelExtensions.kc'
KEXT = 'com.apple.driver.AppleParavirtGPU'

_cache = {}
def kc(path):
    if path not in _cache: _cache[path] = KC(path)
    return _cache[path]

def defined(k, entry):
    return [(n, v) for (n, v, sect, typ) in k.symbols(entry) if (typ & 0xe) == 0xe and v]

def all_symbols(k):
    m = {}
    for e in k.entries:
        try:
            for n, v in defined(k, e): m.setdefault(v, n)
        except Exception: pass
    return m

def vtable(k, cls, symmap=None):
    mangled = '__ZTV%d%s' % (len(cls), cls)
    for e in k.entries:
        try:
            for n, v in defined(k, e):
                if n == mangled: return e, v
        except Exception: pass
    return None, None

def slots(k, cls, lo=0, hi=400, symmap=None):
    e, va = vtable(k, cls)
    if va is None: sys.exit('no vtable for ' + cls)
    symmap = symmap or all_symbols(k)
    out = []
    for i in range(lo, hi):
        p = k.ptr(va + 16 + 8 * i)
        if p is None: break
        out.append((i, p, symmap.get(p, '?')))
    return out

def slot_names(k=None):
    k = k or kc(BKE)
    sm = all_symbols(k)
    names = {}
    for i, p, n in slots(k, 'IOPCIDevice', 0, 520, sm):
        names[8 * i] = demangle([n])[0] if n != '?' else '?'
    return names

def func_ranges(k, entry, sub):
    syms = sorted(defined(k, entry), key=lambda t: t[1])
    dm = demangle([n for n, v in syms])
    out = []
    for idx, ((n, v), d) in enumerate(zip(syms, dm)):
        if sub in d:
            nxt = syms[idx + 1][1] if idx + 1 < len(syms) else v + 0x400
            out.append((d, v, min(nxt - v, 0x4000)))
    return out

def dump(k, va, size):
    o = k.va2off(va)
    return k.d[o:o + size]

_boot_syms = None
def boot_syms():
    global _boot_syms
    if _boot_syms is None:
        b = kc(BKE)
        raw = all_symbols(b)
        names = demangle(list(raw.values()))
        _boot_syms = dict(zip(raw.keys(), names))
    return _boot_syms

def import_name(kx, addr):
    """Name of what the kext's branch stub at `addr` (__BRANCH_STUBS) or GOT slot `addr` (kext __got / __BRANCH_GOTS) ends up calling in the boot collection."""
    o = kx.va2off(addr)
    if o is None: return None
    b = kx.d[o:o + 6]
    if b[:2] == b'\xff\x25':
        addr = addr + 6 + struct.unpack('<i', b[2:6])[0]
        o = kx.va2off(addr)
        if o is None: return None
    v = struct.unpack('<Q', kx.d[o:o + 8])[0]
    if (v >> 30) & 3 == 0:   # chained rebase, cache level 0 = the boot collection: low 30 bits = offset from its base
        return boot_syms().get(kc(BKE).base + (v & 0x3fffffff))
    return None

def cstring(kx, va):
    o = kx.va2off(va)
    if o is None: return None
    raw = kx.d[o:o + 80].split(b'\0')[0]
    return raw.decode('latin1') if len(raw) >= 3 and all(32 <= c < 127 for c in raw) else None

def disasm(raw, va, names, kx=None):
    open('/tmp/pvdis.bin', 'wb').write(raw)
    r = subprocess.run(['objdump', '-D', '-b', 'binary', '-mi386:x86-64', '--adjust-vma=0x%x' % va, '-M', 'att', '--no-show-raw-insn', '/tmp/pvdis.bin'],
                       capture_output=True, text=True)
    for line in r.stdout.splitlines():
        note = []
        m = re.search(r'call\s+\*0x([0-9a-f]+)\(%[a-z0-9]+\)', line)
        if m:
            off = int(m.group(1), 16)
            note.append('vslot +0x%x = %s (IOPCIDevice vtable; only meaningful on the provider)' % (off, names.get(off, '?')))
        m = re.search(r'call\s+0x([0-9a-f]+)\s*$', line)
        if m and kx and 0xc000 <= int(m.group(1), 16) < 0x20000:
            note.append('-> ' + str(import_name(kx, int(m.group(1), 16))))
        m = re.search(r'# 0x([0-9a-f]+)', line)
        if m and kx:
            a = int(m.group(1), 16)
            st = cstring(kx, a)
            if st: note.append('"%s"' % st)
            elif 0x142cd000 <= a < 0x142cd168:
                n = import_name(kx, a)
                if n: note.append('got -> ' + n)
        print(line + ('    ; ' + ' | '.join(note) if note else ''))

if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else ''
    k = kc(SKE)
    if cmd == 'list':
        sub = sys.argv[2] if len(sys.argv) > 2 else ''
        for d, v, sz in func_ranges(k, KEXT, sub): print('0x%x %6d %s' % (v, sz, d))
    elif cmd == 'dis':
        names = slot_names()
        for d, v, sz in func_ranges(k, KEXT, sys.argv[2]):
            if '_os_log_fmt' in d: continue
            print('\n==== %s  @0x%x (%d bytes)' % (d, v, sz))
            disasm(dump(k, v, sz), v, names, k)
    elif cmd == 'xref':
        # every function of the kext that calls/jumps to the given address (hex) or that references it rip-relative
        tgt = int(sys.argv[2], 16)
        segs, _ = k.entry_segs(KEXT)
        for d, v, sz in func_ranges(k, KEXT, ''):
            if '_os_log_fmt' in d or sz > 0x3000: continue
            raw = dump(k, v, sz)
            open('/tmp/pvdis.bin', 'wb').write(raw)
            r = subprocess.run(['objdump', '-D', '-b', 'binary', '-mi386:x86-64', '--adjust-vma=0x%x' % v, '-M', 'att', '--no-show-raw-insn', '/tmp/pvdis.bin'], capture_output=True, text=True)
            for line in r.stdout.splitlines():
                if re.search(r'(call|jmp)\s+0x%x\s*$' % tgt, line) or re.search(r'# 0x%x\s*$' % tgt, line):
                    print('0x%x %s: %s' % (v, d, line.strip()))
    elif cmd == 'vt':
        kk = kc(BKE)
        lo = int(sys.argv[3]) if len(sys.argv) > 3 else 0
        hi = int(sys.argv[4]) if len(sys.argv) > 4 else 400
        for i, p, n in slots(kk, sys.argv[2], lo, hi):
            print('slot %3d (+0x%03x) 0x%x %s' % (i, 8 * i, p, demangle([n])[0] if n != '?' else '?'))
    else:
        print(__doc__)
