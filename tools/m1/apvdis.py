#!/usr/bin/env python3
"""M1 helper (docs/m1-stream.md, hub-task-440): read the x86_64 slice of Apple's AppleParavirtGPUMetal.bundle without a macOS toolchain.

  apvdis.py classes                      every ObjC class of the image with superclass and ivars (name, offset, type)
  apvdis.py methods [substr]             imp address, size, and '-[Class sel]' of every method whose name contains the substring
  apvdis.py dis <substr> [--raw]         disassemble every matching method with llvm-objdump; selector references, class references, cstrings and
                                         CFStrings are resolved to names ('sel: foo:', 'str: "..."'), calls through objc_msgSend are tagged
  apvdis.py strings [substr]             C strings of __cstring with their addresses
  apvdis.py addr <hex>                   what is at this address (section, selector/string/class, containing method)

The binary is Apple's; it stays outside the repo. Path: $APV (default ~/work/tools/macos-full/m1/apv-x86_64, made with `lipo -thin x86_64` from
the bundle in the E2 payload). Needs llvm-objdump."""
import os
import re
import struct
import subprocess
import sys

APV = os.environ.get('APV', os.path.expanduser('~/work/tools/macos-full/m1/apv-x86_64'))


class Image:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        d = self.d
        magic = struct.unpack('<I', d[:4])[0]
        assert magic == 0xfeedfacf, hex(magic)
        ncmds = struct.unpack('<I', d[16:20])[0]
        off = 32
        self.segs = []       # (name, vmaddr, vmsize, fileoff, filesize)
        self.sects = {}      # 'seg,sect' -> (addr, size, fileoff)
        self.imports = []
        chained = None
        for _ in range(ncmds):
            cmd, size = struct.unpack('<II', d[off:off + 8])
            if cmd == 0x19:
                name = d[off + 8:off + 24].split(b'\0')[0].decode()
                vm, vs, fo, fs = struct.unpack('<QQQQ', d[off + 24:off + 56])
                nsec = struct.unpack('<I', d[off + 64:off + 68])[0]
                self.segs.append((name, vm, vs, fo, fs))
                p = off + 72
                for _ in range(nsec):
                    sn = d[p:p + 16].split(b'\0')[0].decode()
                    sg = d[p + 16:p + 32].split(b'\0')[0].decode()
                    addr, sz, fo2 = struct.unpack('<QQI', d[p + 32:p + 52])
                    self.sects[sg + ',' + sn] = (addr, sz, fo2)
                    p += 80
            elif cmd == 0x80000034:
                chained = struct.unpack('<II', d[off + 8:off + 16])
            off += size
        if chained:
            self._read_imports(*chained)

    def _read_imports(self, dataoff, datasize):
        d = self.d
        base = dataoff
        ver, starts, imports, symbols, count, ifmt, sfmt = struct.unpack('<IIIIIII', d[base:base + 28])
        for i in range(count):
            v = struct.unpack('<I', d[base + imports + 4 * i:base + imports + 4 * i + 4])[0]
            nameoff = v >> 9
            nm = d[base + symbols + nameoff:base + symbols + nameoff + 120].split(b'\0')[0].decode('latin1')
            self.imports.append(nm)

    def off(self, va):
        for _, vm, vs, fo, fs in self.segs:
            if vm <= va < vm + fs:
                return fo + (va - vm)
        return None

    def u(self, va, n):
        o = self.off(va)
        return int.from_bytes(self.d[o:o + n], 'little') if o is not None else None

    def ptr(self, va):
        """pointer stored at va; chained-fixup rebases resolved, binds returned as '$name'"""
        v = self.u(va, 8)
        if v is None:
            return None
        if v >> 63:
            ordinal = v & 0xffffff
            return '$' + (self.imports[ordinal] if ordinal < len(self.imports) else '?%d' % ordinal)
        if v == 0:
            return 0
        return v & 0xfffffffff

    def cstr(self, va, limit=400):
        o = self.off(va)
        if o is None:
            return None
        return self.d[o:o + limit].split(b'\0')[0].decode('utf-8', 'replace')


def read_objc(img):
    cl = img.sects['__DATA_CONST,__objc_classlist']
    classes = []
    for i in range(cl[1] // 8):
        c = img.ptr(cl[0] + 8 * i)
        classes.append(parse_class(img, c))
    return classes


def parse_class(img, c):
    superclass = img.ptr(c + 8)
    ro = img.ptr(c + 32)
    ro &= ~7
    name = img.cstr(img.ptr(ro + 24))
    k = {'addr': c, 'name': name, 'super': None, 'methods': [], 'cmethods': [], 'ivars': [], 'size': img.u(ro + 8, 4)}
    if isinstance(superclass, str):
        k['super'] = superclass.replace('$_OBJC_CLASS_$_', '')
    elif superclass:
        sro = img.ptr(superclass + 32) & ~7
        k['super'] = img.cstr(img.ptr(sro + 24))
    k['methods'] = parse_methods(img, img.ptr(ro + 32), '-', name)
    meta = img.ptr(c)
    if isinstance(meta, int) and meta:
        mro = img.ptr(meta + 32) & ~7
        k['cmethods'] = parse_methods(img, img.ptr(mro + 32), '+', name)
    ivl = img.ptr(ro + 48)
    if ivl:
        n = img.u(ivl + 4, 4)
        for i in range(n):
            e = ivl + 8 + 32 * i
            offp = img.ptr(e)
            k['ivars'].append((img.u(offp, 4), img.cstr(img.ptr(e + 8)), img.cstr(img.ptr(e + 16)), img.u(e + 28, 4)))
    return k


def parse_methods(img, ml, sign, cls):
    if not ml:
        return []
    ef = img.u(ml, 4)
    count = img.u(ml + 4, 4)
    entsize = ef & 0xffff
    small = bool(ef & 0x80000000)
    direct = bool(ef & 0x40000000)
    out = []
    for i in range(count):
        e = ml + 8 + entsize * i
        if small:
            n = struct.unpack('<i', img.d[img.off(e):img.off(e) + 4])[0]
            t = struct.unpack('<i', img.d[img.off(e + 4):img.off(e + 4) + 4])[0]
            m = struct.unpack('<i', img.d[img.off(e + 8):img.off(e + 8) + 4])[0]
            nref = e + n
            sel = img.cstr(nref) if direct else img.cstr(img.ptr(nref))
            imp = e + 8 + m
            types = img.cstr(e + 4 + t)
        else:
            sel = img.cstr(img.ptr(e))
            types = img.cstr(img.ptr(e + 8))
            imp = img.ptr(e + 16)
        out.append((imp, '%s[%s %s]' % (sign, cls, sel), types))
    return out


class Index:
    def __init__(self, img):
        self.img = img
        self.classes = read_objc(img)
        self.methods = []
        for k in self.classes:
            for m in k['methods'] + k['cmethods']:
                self.methods.append(m)
        self.methods.sort()
        text = img.sects['__TEXT,__text']
        self.text = (text[0], text[0] + text[1])
        self.starts = [m[0] for m in self.methods]
        self.sel = {}
        sr = img.sects['__DATA,__objc_selrefs']
        for i in range(sr[1] // 8):
            a = sr[0] + 8 * i
            p = img.ptr(a)
            if isinstance(p, int) and p:
                self.sel[a] = img.cstr(p)
        self.cls = {}
        for k in self.classes:
            self.cls[k['addr']] = k['name']
        self.next_ip = {}
        cs = img.sects.get('__DATA_CONST,__cfstring')
        self.cfstr = {}
        if cs:
            for i in range(cs[1] // 32):
                a = cs[0] + 32 * i
                s = img.ptr(a + 16)
                self.cfstr[a] = img.cstr(s) if isinstance(s, int) else None

    def method_at(self, va):
        import bisect
        i = bisect.bisect_right(self.starts, va) - 1
        return self.methods[i] if i >= 0 else None

    def bounds(self, imp):
        import bisect
        i = bisect.bisect_right(self.starts, imp)
        end = self.starts[i] if i < len(self.starts) else self.text[1]
        return min(end, self.text[1])

    def annotate(self, line):
        notes = []
        m = re.search(r'## (?:literal pool symbol address: )?(\S+)$', line)
        r = re.search(r'0x([0-9a-f]+)\(%rip\)', line)
        if r:
            ip = re.match(r'\s*([0-9a-f]+):\s', line)
            nxt = self.next_ip.get(int(ip.group(1), 16)) if ip else None
            if nxt is not None:
                tgt = nxt + int(r.group(1), 16)
                s = self.sel.get(tgt)
                if s is not None:
                    notes.append('sel: ' + s)
                elif tgt in self.cfstr:
                    notes.append('cfstr: ' + repr(self.cfstr[tgt]))
                else:
                    cst = self.img.cstr(tgt, 60)
                    seg = self.section_of(tgt)
                    if seg == '__TEXT,__cstring' and cst:
                        notes.append('str: ' + repr(cst))
                    elif seg == '__DATA_CONST,__got' or seg == '__DATA,__got':
                        p = self.img.ptr(tgt)
                        notes.append('got: ' + (p if isinstance(p, str) else hex(p or 0)))
                    elif seg == '__DATA,__objc_classrefs' or tgt in self.cls:
                        pass
                    elif seg:
                        notes.append('%s+0x%x' % (seg, tgt - self.img.sects[seg][0]))
        return line + ('   ; ' + ' | '.join(notes) if notes else '')

    def section_of(self, va):
        for k, (a, s, _) in self.img.sects.items():
            if a <= va < a + s:
                return k
        return None


_disasm = None


def run_objdump(start, stop):
    """lines of the instruction listing in [start, stop): llvm-objdump ignores --start-address inside a symbol on this stripped image, so the whole
    __text is disassembled once (cached next to the binary) and sliced here"""
    global _disasm
    if _disasm is None:
        cache = APV + '.text.dis'
        if not os.path.exists(cache) or os.path.getmtime(cache) < os.path.getmtime(APV):
            r = subprocess.run(['llvm-objdump', '--macho', '-d', '--no-show-raw-insn', APV], capture_output=True, text=True)
            open(cache, 'w').write(r.stdout)
        _disasm = []
        for ln in open(cache):
            m = re.match(r'\s*([0-9a-f]+):\s', ln)
            if m:
                _disasm.append((int(m.group(1), 16), ln.rstrip('\n')))
    return [ln for a, ln in _disasm if start <= a < stop]


def main():
    img = Image(APV)
    ix = Index(img)
    cmd = sys.argv[1] if len(sys.argv) > 1 else ''
    arg = sys.argv[2] if len(sys.argv) > 2 else ''
    if cmd == 'classes':
        for k in sorted(ix.classes, key=lambda k: k['name']):
            print('@%s : %s  (instance size %s, %d methods, %d class methods)' % (k['name'], k['super'], k['size'], len(k['methods']), len(k['cmethods'])))
            for off, nm, ty, al in k['ivars']:
                print('    +0x%03x %s  %s' % (off, nm, ty))
    elif cmd == 'methods':
        for imp, nm, ty in ix.methods:
            if arg in nm:
                print('0x%x %6d %s' % (imp, ix.bounds(imp) - imp, nm))
    elif cmd == 'dis':
        for imp, nm, ty in ix.methods:
            if arg in nm:
                print('\n==== %s  @0x%x (%d bytes)  types %s' % (nm, imp, ix.bounds(imp) - imp, ty))
                lines = run_objdump(imp, ix.bounds(imp))
                addrs = [int(re.match(r'\s*([0-9a-f]+):', l).group(1), 16) for l in lines]
                for a, b in zip(addrs, addrs[1:] + [ix.bounds(imp)]):
                    ix.next_ip[a] = b
                for ln in lines:
                    print(ln if '--raw' in sys.argv else ix.annotate(ln))
    elif cmd == 'strings':
        a, sz, _ = img.sects['__TEXT,__cstring']
        o = a
        while o < a + sz:
            s = img.cstr(o)
            if arg in s:
                print('0x%x %s' % (o, s))
            o += len(s.encode()) + 1
    elif cmd == 'addr':
        va = int(arg, 16)
        print('section', ix.section_of(va), 'sel', ix.sel.get(va), 'cstr', img.cstr(va, 80), 'method', ix.method_at(va))
    else:
        print(__doc__)


if __name__ == '__main__':
    main()
