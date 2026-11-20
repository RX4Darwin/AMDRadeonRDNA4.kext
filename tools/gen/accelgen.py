#!/usr/bin/env python3
"""accelgen.py: IOAcceleratorFamily2's classes as our kext can derive from them (docs/native-driver-plan.md, option D).

Our kext is in the boot kernel collection and cannot link to IOAcceleratorFamily2 (the system collection). So the base classes are
declared here from the KDK (vtable order, instance size), and every base function our classes reach (constructor, destructor, the
virtuals they do not override, explicit Base::f calls) is a thunk in our kext: a jump through a table that src/rdna4accel.cpp fills
with the real addresses (Lilu's solveSymbol) when IOAcceleratorFamily2 loads. The base's gMetaClass is a placeholder there; our
classes' metaclasses get the real one as their superclass at the same time.

    python3 tools/gen/accelgen.py [--kdk /Library/Developer/KDKs/KDK_....kdk]

writes src/ioaccel.hpp (the declarations), src/ioaccel_thunks.S (the thunks and their table), src/ioaccel_symbols.inc (the
names, in table order) and src/ioaccel_slots.inc (the vtable slot each virtual thunk's function must be in). The KDK's slots must match the running macOS's: rdna4accel.cpp checks the known ones before using them.
"""
import argparse, glob, os, re, struct, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IOSERVICE_SLOTS = 266   # IOService's vtable slots (OSObject's first): the first a direct subclass of IOService adds (checked with the SDK)

# IOService virtuals a class overrides, as IOService.h declares them (the mangled names come from the KDK)
OVERRIDES = {
    'D1Ev': None, 'D0Ev': None,   # the destructor (declared below)
    'getMetaClass': 'virtual const OSMetaClass *getMetaClass() const APPLE_KEXT_OVERRIDE;',
    'free': 'virtual void free() APPLE_KEXT_OVERRIDE;',
    'setProperties': 'virtual IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;',
    'requestTerminate': 'virtual bool requestTerminate(IOService *provider, IOOptionBits options) APPLE_KEXT_OVERRIDE;',
    'didTerminate': 'virtual bool didTerminate(IOService *provider, IOOptionBits options, bool *defer) APPLE_KEXT_OVERRIDE;',
    'configureReport': 'virtual IOReturn configureReport(IOReportChannelList *channels, IOReportConfigureAction action, void *result, '
                       'void *destination) APPLE_KEXT_OVERRIDE;',
    'updateReport': 'virtual IOReturn updateReport(IOReportChannelList *channels, IOReportUpdateAction action, void *result, '
                    'void *destination) APPLE_KEXT_OVERRIDE;',
    'start': 'virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;',
    'stop': 'virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;',
    'terminate': 'virtual bool terminate(IOOptionBits options = 0) APPLE_KEXT_OVERRIDE;',
    'finalize': 'virtual bool finalize(IOOptionBits options) APPLE_KEXT_OVERRIDE;',
    'getWorkLoop': 'virtual IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE;',
    'requestProbe': 'virtual IOReturn requestProbe(IOOptionBits options) APPLE_KEXT_OVERRIDE;',
    'message': 'virtual IOReturn message(UInt32 type, IOService *provider, void *argument = 0) APPLE_KEXT_OVERRIDE;',
    'newUserClient': 'virtual IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler) '
                     'APPLE_KEXT_OVERRIDE;',
}

# Return types of the base's own virtuals where they matter to us (we override or call them); the rest are declared uintptr_t (only
# their slots and mangled names matter)
RETURNS = {
    'getStampMemory': 'void *', 'newEventMachine': 'IOAccelEventMachine2 *', 'createUserGPUTask': 'IOAccelTask *',
    'populateAccelConfig': 'void', 'configureDevice': 'bool', 'teardownDevice': 'void', 'createKernelGPUTask': 'IOAccelTask *',
    'newDisplayMachine': 'IOAccelDisplayMachine2 *', 'newGLContext': 'IOAccelContext2 *', 'newCLContext': 'IOAccelContext2 *',
    'newSurface': 'IOAccelSurface *', 'new2DContext': 'IOAccelContext2 *', 'newVideoContext': 'IOAccelContext2 *',
    'newSysMemory': 'IOAccelSysMemory *', 'newVidMemory': 'IOAccelVidMemory *', 'newResource': 'IOAccelResource2 *',
    'newMemoryMap': 'IOAccelMemoryMap *',
}

# Classes the KDK names whose ancestors are other KDK names (a full class list would also come from the metaclass constructors)
CLASSES = [
    # (class, binary in the KDK, its base, a subclass that implements every pure virtual: binary, vtable symbol)
    ('IOGraphicsAccelerator2', 'IOAcceleratorFamily2', 'IOService',
     ('AMDRadeonX6000', '__ZTV43AMDRadeonX6000_AMDNavi21GraphicsAccelerator')),
]


def run(*args):
    return subprocess.run(args, capture_output=True, text=True, check=True).stdout


def demangle(names):
    return run('c++filt', *names).splitlines() if names else []


class Binary:
    def __init__(self, path):
        self.path = path
        self.data = open(path, 'rb').read()
        self.syms, self.byname = {}, {}
        for line in run('nm', '-m', path).splitlines():
            m = re.match(r'([0-9a-f]{16}) \(([^)]*)\) .*? (\S+)$', line)
            if m:
                a = int(m.group(1), 16)
                self.syms.setdefault(a, m.group(3))
                self.byname[m.group(3)] = a
        self.ext = {}
        for line in run('otool', '-rv', path).splitlines():
            p = line.split()
            if len(p) >= 8 and re.match(r'^[0-9a-f]{8}$', p[0]) and p[4] == 'True':
                self.ext[int(p[0], 16)] = p[-1]
        self.secs, cur = [], {}
        for line in run('otool', '-l', path).splitlines():
            t = line.split()
            if len(t) == 2 and t[0] in ('sectname', 'segname', 'addr', 'size', 'offset'):
                cur[t[0]] = t[1]
                if t[0] == 'offset':
                    self.secs.append((int(cur['addr'], 16), int(cur['size'], 16), int(cur['offset'])))

    def word(self, a):
        for addr, size, off in self.secs:
            if addr <= a < addr + size:
                return struct.unpack_from('<Q', self.data, off + a - addr)[0]
        return 0

    def vtable(self, sym):
        """The function each slot holds, slot 0 at the address point (the vtable symbol + 16)."""
        start = self.byname[sym]
        end = min(a for a in self.syms if a > start)
        out = []
        for a in range(start + 16, end, 8):
            out.append(self.ext.get(a) or self.syms.get(self.word(a), '0x%x' % self.word(a)))
        while out and out[-1].startswith('0x'):
            out.pop()
        return out

    def class_size(self, cls):
        """sizeof the class, from its MetaClass constructor's OSMetaClass(name, super, size) call."""
        n = len(cls)
        sym = '__ZN%d%s9MetaClassC2Ev' % (n, cls)
        dis = run('otool', '-tV', '-p', sym, self.path).splitlines()
        for line in dis[2:40]:
            m = re.search(r'movl\s+\$0x([0-9a-f]+), %ecx', line)
            if m:
                return int(m.group(1), 16)
            if 'OSMetaClassC2' in line:
                break
        sys.exit('accelgen: no size in %s' % sym)


def params_of(demangled):
    m = re.match(r'.*?::([~\w]+)\((.*)\)( const)?$', demangled)
    return (m.group(1), m.group(2), bool(m.group(3))) if m else (None, None, False)


def main():
    ap = argparse.ArgumentParser()
    kdks = sorted(glob.glob('/Library/Developer/KDKs/KDK_*.kdk'))
    ap.add_argument('--kdk', default=kdks[-1] if kdks else None)
    a = ap.parse_args()
    if not a.kdk:
        sys.exit('accelgen: no KDK (/Library/Developer/KDKs)')
    ext = os.path.join(a.kdk, 'System/Library/Extensions')
    decls, thunks, forward, slotted = [], [], set(), []   # slotted: (thunk index, vtable symbol, slot) to check at run time

    for cls, binname, base, (implbin, implvt) in CLASSES:
        b = Binary(os.path.join(ext, binname + '.kext/Contents/MacOS', binname))
        impl = Binary(os.path.join(ext, implbin + '.kext/Contents/MacOS', implbin))
        n = len(cls)
        slots = b.vtable('__ZTV%d%s' % (n, cls))
        implslots = impl.vtable(implvt)
        size = b.class_size(cls)
        prefix = ('__ZN%d%s' % (n, cls), '__ZNK%d%s' % (n, cls))
        own = []
        body = []
        for i, sym in enumerate(slots):
            mine = sym.startswith(prefix)
            if i < IOSERVICE_SLOTS:
                if not mine:
                    continue
                key = sym[len(prefix[0] if sym.startswith(prefix[0]) else prefix[1]):]
                name = 'D1Ev' if key == 'D1Ev' else 'D0Ev' if key == 'D0Ev' else re.sub(r'^\d+', '', re.match(r'\d+(\w+?)E', key).group(0))[:-1]
                if name not in OVERRIDES:
                    sys.exit('accelgen: %s overrides %s (slot %d): not in OVERRIDES' % (cls, sym, i))
                if OVERRIDES[name]:
                    body.append('\t' + OVERRIDES[name])
                if name not in ('D1Ev', 'D0Ev'):
                    slotted.append((len(thunks), '__ZTV%d%s' % (n, cls), i))
                    thunks.append(sym)
                continue
            pure = sym == '___cxa_pure_virtual'
            src = implslots[i] if pure else sym
            own.append((i, src, pure))
        dem = demangle([s for _, s, _ in own])
        body.append('')
        body.append('\t// its own virtuals, slots %d-%d (the KDK\'s order); pure ones named after %s\'s' % (IOSERVICE_SLOTS, len(slots) - 1, implvt))
        for (i, sym, pure), d in zip(own, dem):
            name, params, const = params_of(d)
            if not name:
                sys.exit('accelgen: cannot read %s (slot %d)' % (d, i))
            for t in re.findall(r'\b([A-Z]\w+)\b', params):
                if t not in ('UInt32', 'UInt64', 'IOOptionBits', 'IOReturn', 'OSObject', 'IOService', 'OSDictionary', 'OSArray', 'OSString',
                             'OSData', 'OSNumber', 'OSSymbol', 'IOMemoryDescriptor', 'IOBufferMemoryDescriptor', 'IOWorkLoop', 'IOPCIDevice',
                             'IOUserClient', 'IOFramebuffer', 'IOReportChannelList'):
                    forward.add(t)
            params = params.replace('task*', 'task_t')
            ret = RETURNS.get(name, 'uintptr_t')
            for t in re.findall(r'\b([A-Z]\w+)\b', ret):
                forward.add(t)
            body.append('\tvirtual %s %s(%s)%s%s; // %d' % (ret, name, params, ' const' if const else '', ' = 0' if pure else '', i))
            if not pure:
                slotted.append((len(thunks), '__ZTV%d%s' % (n, cls), i))
                thunks.append(sym)
        thunks += ['__ZN%d%sC2EPK11OSMetaClass' % (n, cls), '__ZN%d%sD2Ev' % (n, cls)]
        decls.append((cls, base, size, body, '__ZN%d%s10gMetaClassE' % (n, cls)))

    hpp = ['// Generated by tools/gen/accelgen.py from %s: do not edit.' % os.path.basename(a.kdk),
           '// IOAcceleratorFamily2\'s classes as our kext derives from them (src/rdna4accel.cpp fills the thunks).',
           '#pragma once', '#include <IOKit/IOService.h>', '#include <IOKit/IOUserClient.h>', '', 'class IOPCIDevice;', 'class IOFramebuffer;']
    hpp += ['class %s;' % t for t in sorted(forward)]
    for cls, base, size, body, gmeta in decls:
        hpp += ['', 'class %s : public %s {' % (cls, base), 'public:',
                '\tclass MetaClass : public OSMetaClass { public: OSObject *alloc() const APPLE_KEXT_OVERRIDE; };',
                '\tstatic MetaClass gMetaClass;   // a placeholder in our kext (ioaccel_thunks.S): our subclasses\' superclass link is patched',
                'protected:', '\t%s(const OSMetaClass *meta);' % cls, '\tvirtual ~%s() APPLE_KEXT_OVERRIDE;' % cls, 'public:']
        hpp += body
        hpp += ['', 'private:', '\tuint8_t opaque_[0x%x - sizeof(%s)];' % (size, base), '};',
                'static_assert(sizeof(%s) == 0x%x, "the KDK\'s size");' % (cls, size),
                'enum : size_t { k%sSize = 0x%x };' % (cls, size)]
    open(os.path.join(ROOT, 'src/ioaccel.hpp'), 'w').write('\n'.join(hpp) + '\n')

    asm = ['// Generated by tools/gen/accelgen.py from %s: do not edit.' % os.path.basename(a.kdk),
           '// Each base function our classes reach: a jump through _rdna4AccelThunks (filled by src/rdna4accel.cpp), else to rdna4AccelMissing.',
           '\t.text']
    for i, sym in enumerate(thunks):
        asm += ['\t.p2align 4', '\t.private_extern %s' % sym, '%s:' % sym, '\tjmpq *_rdna4AccelThunks+%d(%%rip)' % (8 * i)]
    asm += ['', '\t.data', '\t.p2align 3', '\t.private_extern _rdna4AccelThunks', '_rdna4AccelThunks:']
    asm += ['\t.quad _rdna4AccelMissing'] * len(thunks)
    for cls, base, size, body, gmeta in decls:
        asm += ['\t.p2align 4', '\t.private_extern %s' % gmeta, '%s:' % gmeta, '\t.space 64']
    open(os.path.join(ROOT, 'src/ioaccel_thunks.S'), 'w').write('\n'.join(asm) + '\n')

    inc = ['// Generated by tools/gen/accelgen.py from %s: do not edit. _rdna4AccelThunks\' symbols, in order.' % os.path.basename(a.kdk)]
    inc += ['"%s",' % s for s in thunks]   # (the Mach-O names, as Lilu's solveSymbol takes them: src/pvhooks.cpp)
    open(os.path.join(ROOT, 'src/ioaccel_symbols.inc'), 'w').write('\n'.join(inc) + '\n')

    inc = ['// Generated by tools/gen/accelgen.py from %s: do not edit. {thunk, vtable, slot}: the running vtable\'s slot must hold the thunk\'s'
           ' function (else the KDK\'s layout is not this macOS\'s).' % os.path.basename(a.kdk)]
    inc += ['{ %d, "%s", %d },' % t for t in slotted]
    open(os.path.join(ROOT, 'src/ioaccel_slots.inc'), 'w').write('\n'.join(inc) + '\n')
    print('accelgen: %d classes, %d thunks' % (len(decls), len(thunks)))


if __name__ == '__main__':
    main()
