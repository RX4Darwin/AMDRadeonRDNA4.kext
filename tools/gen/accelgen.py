#!/usr/bin/env python3
"""accelgen.py: IOAcceleratorFamily2's classes as our kext can derive from them (docs/native-driver-plan.md, option D).

Our kext is in the boot kernel collection and cannot link to IOAcceleratorFamily2 (the system collection). So the base classes are
declared here from the KDK (vtable order, instance size), and every base function our classes reach (constructor, destructor, the
virtuals they do not override, explicit Base::f calls) is a thunk in our kext: a jump through a table that src/rdna4accel.cpp fills
with the real addresses (Lilu's solveSymbol) when IOAcceleratorFamily2 loads. The base's gMetaClass is a placeholder there; our
classes' metaclasses get the real one as their superclass at the same time.

    python3 tools/gen/accelgen.py --kdk /Library/Developer/KDKs/KDK_....kdk [--output-dir /tmp/rdna4-abi]

writes src/ioaccel.hpp (the declarations), src/ioaccel_thunks.S (the thunks and their table), src/ioaccel_symbols.inc (the
names, in table order) and src/ioaccel_slots.inc (the vtable slot each virtual thunk's function must be in). The KDK's slots must match the running macOS's: rdna4accel.cpp checks the known ones before using them.
"""
import argparse, os, re, struct, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IOSERVICE_SLOTS = 266   # IOService's vtable slots (OSObject's first): the first a direct subclass of IOService adds (checked with the SDK)

# SDK classes' virtuals a family class overrides, as the SDK's headers declare them, by "name(demangled parameters)"; the mangled names
# come from the KDK. The destructor is declared separately.
OVERRIDES = {
    'getMetaClass() const': 'virtual const OSMetaClass *getMetaClass() const APPLE_KEXT_OVERRIDE;',
    'free()': 'virtual void free() APPLE_KEXT_OVERRIDE;',
    'release() const': 'virtual void release() const APPLE_KEXT_OVERRIDE;',
    'retain() const': 'virtual void retain() const APPLE_KEXT_OVERRIDE;',
    'getRetainCount() const': 'virtual int getRetainCount() const APPLE_KEXT_OVERRIDE;',
    'setProperties(OSObject*)': 'virtual IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;',
    'requestTerminate(IOService*, unsigned int)': 'virtual bool requestTerminate(IOService *provider, IOOptionBits options) APPLE_KEXT_OVERRIDE;',
    'didTerminate(IOService*, unsigned int, bool*)': 'virtual bool didTerminate(IOService *provider, IOOptionBits options, bool *defer) '
                                                     'APPLE_KEXT_OVERRIDE;',
    'configureReport(IOReportChannelList*, unsigned int, void*, void*)':
        'virtual IOReturn configureReport(IOReportChannelList *channels, IOReportConfigureAction action, void *result, void *destination) '
        'APPLE_KEXT_OVERRIDE;',
    'updateReport(IOReportChannelList*, unsigned int, void*, void*)':
        'virtual IOReturn updateReport(IOReportChannelList *channels, IOReportUpdateAction action, void *result, void *destination) '
        'APPLE_KEXT_OVERRIDE;',
    'start(IOService*)': 'virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;',
    'stop(IOService*)': 'virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;',
    'terminate(unsigned int)': 'virtual bool terminate(IOOptionBits options = 0) APPLE_KEXT_OVERRIDE;',
    'finalize(unsigned int)': 'virtual bool finalize(IOOptionBits options) APPLE_KEXT_OVERRIDE;',
    'getWorkLoop() const': 'virtual IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE;',
    'requestProbe(unsigned int)': 'virtual IOReturn requestProbe(IOOptionBits options) APPLE_KEXT_OVERRIDE;',
    'message(unsigned int, IOService*, void*)': 'virtual IOReturn message(UInt32 type, IOService *provider, void *argument = 0) '
                                                'APPLE_KEXT_OVERRIDE;',
    'externalMethod(unsigned int, IOExternalMethodArguments*, IOExternalMethodDispatch*, OSObject*, void*)':
        'virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments, IOExternalMethodDispatch *dispatch, '
        'OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;',
    'clientClose()': 'virtual IOReturn clientClose() APPLE_KEXT_OVERRIDE;',
    'attach(IOService*)': 'virtual bool attach(IOService *provider) APPLE_KEXT_OVERRIDE;',
    'connectClient(IOUserClient*)': 'virtual IOReturn connectClient(IOUserClient *client) APPLE_KEXT_OVERRIDE;',
    'clientMemoryForType(unsigned int, unsigned int*, IOMemoryDescriptor**)':
        'virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;',
    'getTargetAndMethodForIndex(IOService**, unsigned int)':
        'virtual IOExternalMethod *getTargetAndMethodForIndex(IOService **targetP, UInt32 index) APPLE_KEXT_OVERRIDE;',
    'newUserClient(task*, void*, unsigned int, IOUserClient**)':
        'virtual IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler) APPLE_KEXT_OVERRIDE;',
    'newUserClient(task*, void*, unsigned int, OSDictionary*, IOUserClient**)':
        'virtual IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties, IOUserClient **handler) '
        'APPLE_KEXT_OVERRIDE;',
}
SDK_SLOTS = {'OSObject': 35, 'IOService': 266, 'IOUserClient': 300}   # the SDK's vtable sizes (checked: the same as Tahoe 26.6.2's;
                                                                      # IOUserClient: clang's vtable layout dump and the KDK kernel's)

# Return types of the family's own virtuals where they matter to us (we override or call them); the rest are declared uintptr_t (only
# their slots and mangled names matter). One type per name, so that a base's declaration and a subclass's override agree.
RETURNS = {
    'getStampMemory': 'void *', 'newEventMachine': 'IOAccelEventMachine2 *', 'createUserGPUTask': 'IOAccelTask *',
    'populateAccelConfig': 'void', 'configureDevice': 'bool', 'teardownDevice': 'void', 'createKernelGPUTask': 'IOAccelTask *',
    'newDisplayMachine': 'IOAccelDisplayMachine *', 'newGLContext': 'IOAccelContext2 *', 'newCLContext': 'IOAccelContext2 *',
    'newSurface': 'IOAccelSurface *', 'new2DContext': 'IOAccelContext2 *', 'newVideoContext': 'IOAccelContext2 *',
    'newSysMemory': 'IOAccelSysMemory *', 'newVidMemory': 'IOAccelVidMemory *', 'newResource': 'IOAccelResource2 *',
    'newMemoryMap': 'IOAccelMemoryMap *', 'init': 'bool', 'vendor_doDeviceAttribute': 'IOReturn',
}

# The kexts whose classes we derive from: (bundle id, binary under the KDK's and macOS's /System/Library/Extensions). A class's thunks are
# solved when its kext loads (src/rdna4accel.cpp).
KEXTS = [
    ('com.apple.iokit.IOAcceleratorFamily2', 'IOAcceleratorFamily2.kext/Contents/MacOS/IOAcceleratorFamily2'),
    ('com.apple.AppleGraphicsDeviceControl',
     'AppleGraphicsControl.kext/Contents/PlugIns/AppleGraphicsDeviceControl.kext/Contents/MacOS/AppleGraphicsDeviceControl'),
]

# The family classes we derive from (each after its base): (class, its base, a subclass in the KDK that implements its pure virtuals:
# binary, vtable symbol; None if it has none[, the kext's index in KEXTS: 0 if left out])
FAMILY = 'IOAcceleratorFamily2'
AMD = 'AMDRadeonX6000'
PARAVIRT = 'AppleParavirtGPU'
CLASSES = [
    ('IOGraphicsAccelerator2', 'IOService', (AMD, '__ZTV43AMDRadeonX6000_AMDNavi21GraphicsAccelerator')),
    ('IOAccelTask', 'OSObject', None),
    ('IOAccelMemoryMap', 'OSObject', (AMD, '__ZTV32AMDRadeonX6000_AMDAccelMemoryMap')),
    ('IOAccelEventMachine2', 'OSObject', (AMD, '__ZTV35AMDRadeonX6000_AMDAccelEventMachine')),
    ('IOAccelEventMachineFast2', 'IOAccelEventMachine2', (AMD, '__ZTV35AMDRadeonX6000_AMDAccelEventMachine')),
    ('IOAccelDisplayMachine', 'IOService', (AMD, '__ZTV37AMDRadeonX6000_AMDAccelDisplayMachine')),
    ('IOAccelResource2', 'OSObject', (AMD, '__ZTV31AMDRadeonX6000_AMDAccelResource')),
    ('IOAccelMemory', 'OSObject', (FAMILY, '__ZTV16IOAccelSysMemory')),   # (its pure ones named after the family's own system memory)
    ('IOAccelSysMemory', 'IOAccelMemory', None),
    ('IOAccelVidMemory', 'IOAccelMemory', (AMD, '__ZTV32AMDRadeonX6000_AMDAccelVidMemory')),
    ('IOAccelDisplayPipe', 'IOService', None),
    ('IOAccelSurface', 'IOUserClient', (AMD, '__ZTV30AMDRadeonX6000_AMDAccelSurface')),   # user client type 0 (newSurface)
    ('IOAccelSubmitter2', 'IOUserClient', (AMD, '__ZTV32AMDRadeonX6000_AMDAccel2DContext')),
    ('IOAccelContext2', 'IOAccelSubmitter2', (AMD, '__ZTV32AMDRadeonX6000_AMDAccel2DContext')),
    ('IOAccel2DContext2', 'IOAccelContext2', (AMD, '__ZTV32AMDRadeonX6000_AMDAccel2DContext')),   # type 2 (new2DContext): the 2D plug-in's
    ('AppleGraphicsDeviceControl', 'IOService', (PARAVIRT, '__ZTV23AppleParavirtGPUControl'), 1),
]


# Enums the family's virtuals take by value: declared with their underlying type (a value of an incomplete class does not compile; an
# enum mangles by its name, as a class does)
ENUMS = ['eIOAccelSurfaceShapeBits', 'eLockType']
KNOWN_TYPES = {'UInt32', 'UInt64', 'IOOptionBits', 'IOReturn', 'OSObject', 'IOService', 'OSDictionary', 'OSArray', 'OSString', 'OSData',
               'OSNumber', 'OSSymbol', 'IOMemoryDescriptor', 'IOBufferMemoryDescriptor', 'IOWorkLoop', 'IOPCIDevice', 'IOUserClient',
               'IOFramebuffer', 'IOReportChannelList', 'IORangeAllocator', 'IOInterruptEventSource', 'IOTimerEventSource', 'IOCommandGate',
               'IOEventSource', 'IOLock', 'IORecursiveLock', 'IOSimpleLock', 'OSSet', 'OSCollection', 'OSIterator', 'OSBoolean',
               'IOMemoryMap', 'IODMACommand', 'IOGeneralMemoryDescriptor', 'IOSubMemoryDescriptor', 'IOMultiMemoryDescriptor',
               'IOExternalMethodArguments'}


def run(*args):
    return subprocess.run(args, capture_output=True, text=True, check=True).stdout


def demangle(names):
    return run('c++filt', *names).splitlines() if names else []


class Binary:
    def __init__(self, path):
        self.path = path
        self.data = open(path, 'rb').read()
        if self.data[:4] == b'\xca\xfe\xba\xbe':   # fat: the x86_64 slice (CPU_TYPE_X86_64 0x01000007)
            n = struct.unpack_from('>I', self.data, 4)[0]
            for k in range(n):
                cpu, _, off, size, _ = struct.unpack_from('>5I', self.data, 8 + 20 * k)
                if cpu == 0x01000007:
                    self.data = self.data[off:off + size]
        self.syms, self.byname = {}, {}
        for line in run('nm', '-arch', 'x86_64', '-m', path).splitlines():
            m = re.match(r'([0-9a-f]{16}) \(([^)]*)\) .*? (\S+)$', line)
            if m:
                a = int(m.group(1), 16)
                self.syms.setdefault(a, m.group(3))
                self.byname[m.group(3)] = a
        self.ext = {}
        for line in run('otool', '-arch', 'x86_64', '-rv', path).splitlines():
            p = line.split()
            if len(p) >= 8 and re.match(r'^[0-9a-f]{8}$', p[0]) and p[4] == 'True':
                self.ext[int(p[0], 16)] = p[-1]
        self.secs, cur = [], {}
        for line in run('otool', '-arch', 'x86_64', '-l', path).splitlines():
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
        dis = run('otool', '-arch', 'x86_64', '-tV', '-p', sym, self.path).splitlines()
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
    ap.add_argument('--kdk', required=True,
                    help='exact Kernel Debug Kit bundle to inspect (never auto-select the newest installed KDK)')
    ap.add_argument('--output-dir', default=ROOT,
                    help='directory for generated files (default: repository root; use a temporary directory to review first)')
    a = ap.parse_args()
    if not os.path.isdir(a.kdk):
        sys.exit('accelgen: KDK directory does not exist: %s' % a.kdk)
    ext = os.path.join(a.kdk, 'System/Library/Extensions')
    if not os.path.isdir(ext):
        sys.exit('accelgen: KDK has no System/Library/Extensions: %s' % ext)
    outdir = os.path.abspath(a.output_dir)
    os.makedirs(os.path.join(outdir, 'src'), exist_ok=True)
    decls, thunks, forward, slotted = [], [], set(), []   # slotted: (thunk index, vtable symbol, slot) to check at run time
    bins, sdk_root, nslots = {}, {}, dict(SDK_SLOTS)

    def binary(name):   # a kext's name (its binary in its bundle) or a path under Extensions
        if name not in bins:
            bins[name] = Binary(os.path.join(ext, name if '/' in name else name + '.kext/Contents/MacOS/' + name))
        return bins[name]

    def note_types(text):   # the classes and structs it names (pointers and references to them)
        for t in re.findall(r'\b([A-Za-z_]\w*)(?: const)?\s*[*&]', text):
            if t not in KNOWN_TYPES and t not in ('char', 'int', 'void', 'long', 'short', 'bool', 'float', 'double', 'unsigned', 'signed',
                                                  'task', 'volatile', 'const'):
                forward.add(t)

    kext_of = {}
    for entry in CLASSES:
        cls, base, impl = entry[:3]
        kext = entry[3] if len(entry) > 3 else 0
        kext_of[cls] = kext
        b = binary(KEXTS[kext][1])
        n = len(cls)
        vt = '__ZTV%d%s' % (n, cls)
        slots = b.vtable(vt)
        implslots = binary(impl[0]).vtable(impl[1]) if impl else []
        size = b.class_size(cls)
        sdk = base if base in SDK_SLOTS else sdk_root[base]
        sdk_root[cls] = sdk
        first = nslots[base]   # this class's first own slot
        nslots[cls] = len(slots)
        prefix = ('__ZN%d%s' % (n, cls), '__ZNK%d%s' % (n, cls))
        entries = []   # (slot, symbol to name it by, pure, overriding)
        for i, sym in enumerate(slots):
            if i < first and not sym.startswith(prefix):
                continue   # inherited unchanged
            if sym.startswith(prefix) and (sym.endswith('D1Ev') or sym.endswith('D0Ev')):
                thunks.append((kext, sym))   # the destructor (declared below): the base's vtable, emitted in our kext, holds them
                continue
            if sym == '__ZNK%d%s12getMetaClassEv' % (n, cls):
                continue   # declared first, defined in our kext: the key function (below)
            pure = sym == '___cxa_pure_virtual'
            if pure and not (impl and i < len(implslots)):
                sys.exit('accelgen: %s slot %d is pure and no subclass names it' % (cls, i))
            entries.append((i, implslots[i] if pure else sym, pure, i < first))
        body = ['\t// slots %d-%d its own (the KDK\'s order), the rest overrides; pure ones named after %s\'s'
                % (first, len(slots) - 1, impl[1] if impl else '-')]
        for (i, sym, pure, over), d in zip(entries, demangle([e[1] for e in entries])):
            name, params, const = params_of(d)
            if not name and pure:   # a pure one whose namer's function is local: the name a class below it gives the slot
                parent = {e[0]: e[1] for e in CLASSES}
                def derives(c):   # whether class c has cls among its bases
                    while c in parent:
                        c = parent[c]
                        if c == cls:
                            return True
                    return False
                for other in CLASSES:
                    if not derives(other[0]):
                        continue
                    osyms = binary(KEXTS[other[3] if len(other) > 3 else 0][1]).vtable('__ZTV%d%s' % (len(other[0]), other[0]))
                    if i < len(osyms) and osyms[i].startswith('__Z'):
                        cand = params_of(demangle([osyms[i]])[0])
                        if cand[0]:
                            name, params, const = cand
                            break
            if not name:
                sys.exit('accelgen: %s: cannot read %s (slot %d, its first own %d, symbol %s)' % (cls, d, i, first, sym))
            key = '%s(%s)%s' % (name, params, ' const' if const else '')
            if over and i < SDK_SLOTS[sdk]:   # an SDK class's virtual: its own declaration
                if key not in OVERRIDES:
                    sys.exit('accelgen: %s overrides %s (slot %d): not in OVERRIDES' % (cls, key, i))
                body.append('\t' + OVERRIDES[key] + ' // %d' % i)
            else:
                note_types(params)
                ret = RETURNS.get(name, 'uintptr_t')
                note_types(ret)
                body.append('\tvirtual %s %s(%s)%s%s%s; // %d' % (ret, name, params.replace('task*', 'task_t'), ' const' if const else '',
                                                               ' APPLE_KEXT_OVERRIDE' if over else '', ' = 0' if pure else '', i))
            if not pure:
                slotted.append((len(thunks), vt, i))
                thunks.append((kext, sym))
        for extra in ('__ZN%d%sC2EPK11OSMetaClass' % (n, cls), '__ZN%d%sD2Ev' % (n, cls)):
            if extra not in b.byname:
                sys.exit('accelgen: %s has no %s' % (cls, extra))
            thunks.append((kext, extra))
        decls.append((cls, base, size, body, '__ZN%d%s10gMetaClassE' % (n, cls)))
        forward.discard(cls)

    forward -= {e[0] for e in CLASSES}
    hpp = ['// Generated by tools/gen/accelgen.py from %s: do not edit.' % os.path.basename(a.kdk),
           '// IOAcceleratorFamily2\'s classes as our kext derives from them (src/rdna4accel.cpp fills the thunks).',
           '#pragma once', '#include <IOKit/IOService.h>', '#include <IOKit/IOUserClient.h>', '#include <IOKit/IORangeAllocator.h>', '',
           'class IOPCIDevice;', 'class IOFramebuffer;']
    hpp += ['class %s;' % e[0] for e in CLASSES]
    hpp += ['enum %s : unsigned int {};' % t for t in ENUMS]
    hpp += ['class %s;' % t for t in sorted(forward - set(ENUMS))]
    for cls, base, size, body, gmeta in decls:
        # getMetaClass first and defined in our kext (rdna4accel.cpp RDNA4_ACCEL_BASE): the key function, so that the compiler emits the
        # class's vtable in our kext. A subclass's Base::f() calls go through it (-fapple-kext: the base's vtable symbol, never a direct
        # call); undefined, it was bound to junk and the family's start panicked (2026-11-20).
        hpp += ['', 'class %s : public %s {' % (cls, base), 'public:',
                '\tvirtual const OSMetaClass *getMetaClass() const APPLE_KEXT_OVERRIDE;   // the key function (see accelgen.py)',
                '\tclass MetaClass : public OSMetaClass { public: OSObject *alloc() const APPLE_KEXT_OVERRIDE; };',
                '\tstatic MetaClass gMetaClass;   // a placeholder in our kext (ioaccel_thunks.S): our subclasses\' superclass link is patched',
                'protected:', '\t%s(const OSMetaClass *meta);' % cls, '\tvirtual ~%s() APPLE_KEXT_OVERRIDE;' % cls, 'public:']
        hpp += body
        hpp += ['', 'private:', '\tuint8_t opaque_[0x%x - sizeof(%s)];' % (size, base), '};',
                'static_assert(sizeof(%s) == 0x%x, "the KDK\'s size");' % (cls, size),
                'enum : size_t { k%sSize = 0x%x };' % (cls, size)]
    open(os.path.join(outdir, 'src/ioaccel.hpp'), 'w').write('\n'.join(hpp) + '\n')

    asm = ['// Generated by tools/gen/accelgen.py from %s: do not edit.' % os.path.basename(a.kdk),
           '// Each base function our classes reach: a jump through _rdna4AccelThunks (filled by src/rdna4accel.cpp), else to rdna4AccelMissing.',
           '\t.text']
    for i, (_, sym) in enumerate(thunks):
        asm += ['\t.p2align 4', '\t.private_extern %s' % sym, '%s:' % sym, '\tjmpq *_rdna4AccelThunks+%d(%%rip)' % (8 * i)]
    asm += ['', '\t.data', '\t.p2align 3', '\t.private_extern _rdna4AccelThunks', '_rdna4AccelThunks:']
    asm += ['\t.quad _rdna4AccelMissing'] * len(thunks)
    for cls, base, size, body, gmeta in decls:
        asm += ['\t.p2align 4', '\t.private_extern %s' % gmeta, '%s:' % gmeta, '\t.space 64']
    open(os.path.join(outdir, 'src/ioaccel_thunks.S'), 'w').write('\n'.join(asm) + '\n')

    inc = ['// Generated by tools/gen/accelgen.py from %s: do not edit. _rdna4AccelThunks\' {kext (ioaccel_kexts.inc), symbol}, in order.'
           % os.path.basename(a.kdk)]
    inc += ['{ %d, "%s" },' % t for t in thunks]   # (the Mach-O names, as Lilu's solveSymbol takes them: src/pvhooks.cpp)
    open(os.path.join(outdir, 'src/ioaccel_symbols.inc'), 'w').write('\n'.join(inc) + '\n')

    inc = ['// Generated by tools/gen/accelgen.py from %s: do not edit. {thunk, vtable, slot}: the running vtable\'s slot must hold the thunk\'s'
           ' function (else the KDK\'s layout is not this macOS\'s).' % os.path.basename(a.kdk)]
    inc += ['{ %d, "%s", %d },' % t for t in slotted]
    open(os.path.join(outdir, 'src/ioaccel_slots.inc'), 'w').write('\n'.join(inc) + '\n')
    inc = ['// Generated by tools/gen/accelgen.py: do not edit. The kexts whose classes we derive from: {bundle id, binary}.']
    inc += ['{ "%s", "/System/Library/Extensions/%s" },' % k for k in KEXTS]
    open(os.path.join(outdir, 'src/ioaccel_kexts.inc'), 'w').write('\n'.join(inc) + '\n')
    inc = ['// Generated by tools/gen/accelgen.py: do not edit. Each derived-from class\'s kext (ioaccel_kexts.inc).']
    inc += ['{ "%s", %d },' % (e[0], kext_of[e[0]]) for e in CLASSES]
    open(os.path.join(outdir, 'src/ioaccel_classes.inc'), 'w').write('\n'.join(inc) + '\n')
    print('accelgen: %d classes, %d thunks' % (len(decls), len(thunks)))


if __name__ == '__main__':
    main()
