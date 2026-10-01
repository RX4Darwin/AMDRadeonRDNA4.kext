#!/usr/bin/env python3
# Edit an OSX-KVM OpenCore config.plist for RDNA4FB VM testing.
#
#   vm-ocplist.py <config.plist> "<boot-args>" [RDNA4FB.kext ...] [--resolution WxH] [--timeout SECS]
#
# - sets the macOS boot-args and makes OpenCore replace the NVRAM copy;
# - makes the picker wait instead of auto-booting after 2 s (the stock default
#   entry is the OpenCore disk itself, so a timeout just relaunches OpenCore);
# - disables WhateverGreen (RDNA4FB owns the display path under test);
# - optionally injects the named kexts (already copied to EFI/OC/Kexts) through
#   Kernel -> Add, after everything already listed — i.e. after Lilu, in the
#   order given (tools/emu-full.sh passes several);
# - --timeout SECS: the picker's auto-boot timeout (default 0 = wait for a key;
#   the full macOS VM of tools/emu-full.sh uses a few seconds once a default is set);
# - optionally sets the GOP resolution OpenCore picks (UEFI -> Output ->
#   Resolution), i.e. the console size macOS boots with.
import plistlib
import sys

GUID = '7C436110-AB2A-4BBB-A880-FE41995C9F82'
argv = sys.argv[1:]
resolution = None
if '--resolution' in argv:
    i = argv.index('--resolution')
    resolution = argv[i + 1]
    del argv[i:i + 2]
timeout = 0
if '--timeout' in argv:
    i = argv.index('--timeout')
    timeout = int(argv[i + 1])
    del argv[i:i + 2]
path, args = argv[0], argv[1]
kexts = argv[2:]

with open(path, 'rb') as f:
    cfg = plistlib.load(f)

nv = cfg['NVRAM']
nv['Add'].setdefault(GUID, {})['boot-args'] = args
dele = nv['Delete'].setdefault(GUID, [])
if 'boot-args' not in dele:
    dele.append('boot-args')
cfg['Misc']['Boot']['Timeout'] = timeout
if resolution:
    cfg.setdefault('UEFI', {}).setdefault('Output', {})['Resolution'] = resolution

adds = [k for k in cfg['Kernel']['Add'] if k.get('BundlePath') not in kexts]
for k in adds:
    if k.get('BundlePath') == 'WhateverGreen.kext':
        k['Enabled'] = False
for kext in kexts:
    exe = kext[:-len('.kext')] if kext.endswith('.kext') else kext
    adds.append({
        'Arch': 'x86_64',
        'BundlePath': kext,
        'Comment': 'injected by vm-opencore.sh',
        'Enabled': True,
        'ExecutablePath': 'Contents/MacOS/' + exe,
        'MaxKernel': '',
        'MinKernel': '',
        'PlistPath': 'Contents/Info.plist',
    })
cfg['Kernel']['Add'] = adds

with open(path, 'wb') as f:
    plistlib.dump(cfg, f)
print('boot-args:', args)
if resolution:
    print('resolution:', resolution)
print('kexts enabled:', [k['BundlePath'] for k in adds if k.get('Enabled')])
