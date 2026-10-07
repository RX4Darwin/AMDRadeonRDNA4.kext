import sys, struct, subprocess; sys.path.insert(0, '.')
from kc import *
kc = KC('../out/payload/System/Library/KernelCollections/SystemKernelExtensions.kc')
ENT = 'com.apple.kext.AMDRadeonX6000'
syms = kc.symbols(ENT)
defined = sorted([(v, n) for n, v, sec, t in syms if sec and v], key=lambda x: x[0])
dem = dict(zip([n for _, n in defined], demangle([n for _, n in defined])))
addr2name = {}
for v, n in defined: addr2name.setdefault(v, dem[n])
addrs = [a for a, _ in defined]
def size_of(a):
    i = addrs.index(a); return addrs[i+1] - a
def dump_table(cls, gtmi_substr='getTargetAndMethodForIndex', first=None):
    cand = [(a, n) for a, n in defined if cls + '::' + gtmi_substr in dem[n]]
    a, n = cand[0]
    o = kc.va2off(a); code = kc.d[o:o+size_of(a)]
    i = code.find(b'\x48\x8d\x0d')
    if i < 0: i = code.find(b'\x48\x8d\x05')
    disp = struct.unpack('<i', code[i+3:i+7])[0]
    base = a + i + 7 + disp
    # idx*48 - K: find the add imm
    j = code.find(b'\x48\x05')
    k = struct.unpack('<i', code[j+2:j+6])[0] if j >= 0 else 0
    return a, code, base, k
