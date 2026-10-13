# Minimal reader for x86_64 kernel collections (Mach-O filesets): entries, symbols, chained-fixup pointers. Forge's local analysis helper for E2.
import struct, subprocess, functools
class KC:
    def __init__(s, path):
        s.d = open(path, 'rb').read()
        d = s.d
        magic, cpu, sub, ft, ncmds, sz, fl, _ = struct.unpack('<IiiIIIII', d[:32])
        assert magic == 0xfeedfacf
        s.entries = {}
        s.segs = []          # (name, vmaddr, vmsize, fileoff, filesize) of the whole KC
        off = 32
        for i in range(ncmds):
            cmd, cs = struct.unpack('<II', d[off:off+8])
            if cmd == 0x80000035:
                vm, fo, no, _ = struct.unpack('<QQII', d[off+8:off+32])
                s.entries[d[off+no:off+cs].split(b'\0')[0].decode()] = (vm, fo)
            if cmd == 0x19:
                n = d[off+8:off+24].split(b'\0')[0].decode()
                vm, vs, fo, fs = struct.unpack('<QQQQ', d[off+24:off+56]); s.segs.append((n, vm, vs, fo, fs))
            off += cs
        s.base = min(v for n, v, *_ in s.segs)
    def entry_segs(s, name):
        vm, fo = s.entries[name]
        d = s.d
        m, c, su, t, nc, sz, fl, _ = struct.unpack('<IiiIIIII', d[fo:fo+32]); o = fo + 32
        segs = []; sym = None
        for i in range(nc):
            cmd, cs = struct.unpack('<II', d[o:o+8])
            if cmd == 0x19:
                n = d[o+8:o+24].split(b'\0')[0].decode(); a, b, c2, e = struct.unpack('<QQQQ', d[o+24:o+56])
                nsec = struct.unpack('<I', d[o+64:o+68])[0]
                secs = []; p = o + 72
                for k in range(nsec):
                    sn = d[p:p+16].split(b'\0')[0].decode(); sv, ss, so = struct.unpack('<QQI', d[p+32:p+52]); secs.append((sn, sv, ss, so)); p += 80
                segs.append((n, a, b, c2, e, secs))
            if cmd == 0x2: sym = struct.unpack('<IIII', d[o+8:o+24])
            o += cs
        return segs, sym
    def symbols(s, name):
        segs, sym = s.entry_segs(name)
        symoff, nsyms, stroff, strsize = sym
        out = []
        d = s.d
        for n in range(nsyms):
            strx, typ, sect, desc, val = struct.unpack('<IBBHQ', d[symoff+16*n:symoff+16*n+16])
            nm = d[stroff+strx:stroff+strx+200].split(b'\0')[0].decode('latin1')
            out.append((nm, val, sect, typ))
        return out
    def va2off(s, va):
        for n, vm, vs, fo, fs in s.segs:
            if vm <= va < vm + fs: return fo + (va - vm)
        return None
    def ptr(s, va):
        """resolve a chained-fixup (x86_64 KC format) or plain pointer stored at va"""
        o = s.va2off(va)
        if o is None: return None
        v = struct.unpack('<Q', s.d[o:o+8])[0]
        if v >> 63 == 0 and (v >> 32) == 0xffffff80 or v > 0xffffff7000000000: return v   # plain
        target = v & 0x3fffffff                     # dyld_chained_ptr_64_kernel_cache_rebase.target (30 bits), offset from the KC base
        return s.base + target
def demangle(names):
    p = subprocess.run(['c++filt'], input='\n'.join(n[1:] if n.startswith('__Z') else n for n in names), capture_output=True, text=True)
    return p.stdout.splitlines()
