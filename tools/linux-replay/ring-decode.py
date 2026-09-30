#!/usr/bin/env python3
"""Find our IB (the 'IB VA' line of replay.txt) in the captured amdgpu gfx rings and print the PM4
packets amdgpu emitted around it. amdgpu_debugfs_ring_read: 3 x u32 (rptr, wptr, driver wptr), then
the ring."""
import re, struct, sys, glob, os
out = sys.argv[1]
m = re.search(r'IB VA 0x([0-9a-f]+)', open(os.path.join(out, 'replay.txt')).read())
if not m:
    sys.exit('no IB VA in replay.txt')
ibva = int(m.group(1), 16)
NAMES = {0x10: 'NOP', 0x3f: 'INDIRECT_BUFFER', 0x49: 'RELEASE_MEM', 0x58: 'ACQUIRE_MEM', 0x3c: 'WAIT_REG_MEM',
         0x37: 'WRITE_DATA', 0x28: 'CONTEXT_CONTROL', 0x46: 'EVENT_WRITE', 0x42: 'PFP_SYNC_ME', 0x79: 'SET_UCONFIG_REG',
         0x69: 'SET_CONTEXT_REG', 0x76: 'SET_SH_REG', 0x40: 'COPY_DATA', 0x50: 'DMA_DATA', 0x22: 'COND_EXEC',
         0xa2: 'FRAME_CONTROL', 0x2f: 'NUM_INSTANCES', 0x33: 'INDIRECT_BUFFER_CNST', 0x9b: 'SET_Q_PREEMPTION_MODE',
         0x88: 'DISPATCH_TASKMESH', 0x44: 'SET_BASE', 0x3b: 'EVENT_WRITE_EOP?', 0x8b: 'RUN_CLEANER_SHADER?'}
for f in sorted(glob.glob(os.path.join(out, 'amdgpu_ring_gfx*.bin'))):
    raw = open(f, 'rb').read()
    rptr, wptr, dwptr = struct.unpack_from('<3I', raw)
    ring = list(struct.unpack_from(f'<{(len(raw) - 12) // 4}I', raw, 12))
    n = len(ring)
    hits = [i for i in range(n) if (ring[i] >> 8) & 0xff == 0x3f and ring[i] >> 30 == 3
            and (ring[(i + 1) % n] | ring[(i + 2) % n] << 32) & ~3 == ibva]
    print(f'== {os.path.basename(f)}: {n} dwords, rptr {rptr} wptr {wptr}; our IB at {hits}')
    for h in hits:
        # walk back up to 120 dwords to a packet boundary we can parse forward from
        start = (h - 120) % n
        i, lines = start, []
        # resync: parse forward from start, discarding until we land exactly on h
        for s0 in range(start, start + 120):
            j = s0 % n
            ok = False
            k = j
            for _ in range(60):
                hdr = ring[k % n]
                if hdr >> 30 != 3:
                    k += 1
                    continue
                if k % n == h:
                    ok = True
                    break
                k += 2 + ((hdr >> 16) & 0x3fff)
                if (k - j) % n > (h - j) % n:
                    break
            if ok:
                i = j
                break
        end = h + 60
        while i < end:
            hdr = ring[i % n]
            if hdr >> 30 != 3:
                print(f'  {i % n:6d}: {hdr:08x}  (type {hdr >> 30})')
                i += 1
                continue
            op, cnt = (hdr >> 8) & 0xff, (hdr >> 16) & 0x3fff
            body = [ring[(i + 1 + k) % n] for k in range(cnt + 1)]
            mark = '  <== OUR IB' if i % n == h else ''
            print(f'  {i % n:6d}: {hdr:08x} {NAMES.get(op, hex(op)):22s} ' + ' '.join(f'{d:08x}' for d in body[:12]) + (' ...' if len(body) > 12 else '') + mark)
            i += 2 + cnt
