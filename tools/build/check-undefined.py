#!/usr/bin/env python3
# Every symbol the kext leaves undefined must come from what it links against: the kernel, IOPCIFamily (both from the KDK) or Lilu.
# Anything else OpenCore cannot bind, and it then drops the whole kext without a word: the rig booted without its driver, no desktop,
# for a function of ours declared in one namespace and defined in another (__Z10accelFlagsv, 2026-11-21).
#
#   tools/build/check-undefined.py <kext binary>     (src/kext.mk; skipped, said so, without a KDK)
import glob
import os
import subprocess
import sys

# Lilu's API (lib/Lilu): its classes and functions as our objects reference them
LILU_PREFIXES = ('__ZN13KernelPatcher', '__ZN7LiluAPI', '_lilu', '__ZN6FileIO', '__ZN8MachInfo', '__ZN6WIOKit', '__ZN11UserPatcher',
                 '__ZN7CPUInfo', '__ZN9NVStorage', '__ZN6Crypto', '__ZN11Compression', '__ZN12Disassembler')
LILU_NAMES = {'__Z6strstrPKcS0_m', '____chkstk_darwin'}   # (Lilu's strstr overload; the stack probe the kernel resolves)


def symbols(path, flags):
    out = subprocess.run(['nm', '-arch', 'x86_64'] + flags + [path], capture_output=True, text=True).stdout
    return {line.split()[-1] for line in out.splitlines() if line.strip()}


def main():
    binary = sys.argv[1]
    kdks = sorted(glob.glob('/Library/Developer/KDKs/KDK_*.kdk'))
    if not kdks:
        print('check-undefined: SKIPPED (no KDK in /Library/Developer/KDKs)')
        return 0
    kdk = kdks[-1]
    defined = symbols(os.path.join(kdk, 'System/Library/Kernels/kernel'), ['-gU'])
    defined |= symbols(os.path.join(kdk, 'System/Library/Extensions/IOPCIFamily.kext/Contents/MacOS/IOPCIFamily'), ['-gU'])
    left = sorted(s for s in symbols(binary, ['-u'])
                  if s not in defined and s not in LILU_NAMES and not s.startswith(LILU_PREFIXES))
    if left:
        print('check-undefined: undefined in %s and not the kernel\'s, IOPCIFamily\'s or Lilu\'s (OpenCore would drop the kext):' % binary)
        for s in left:
            print('  ' + s)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
