#!/usr/bin/env python3
"""Unit tests of the emulated GC hub's VM model (W13 E1, E2, E4, E5), driven over QEMU's qtest
protocol: no guest, no kext. Each check is a scenario on the device registers, page tables in VRAM and a
MEC queue whose packets touch memory through a VMID.

    QEMU=~/work/tools/qemu-compile/build/qemu-system-x86_64 tools/emu-qtest-vm.py

What it proves is what the EMULATOR does, nothing about the card: the TLB/level/ack rules it checks are
the model docs/w13-vmid.md section 7 specifies. The point of the negative controls is that a kext which
forgets a flush would now be CAUGHT (a stale translation is used, and named in the log).

The device is built with `test-gfx-booted` (RLC autoload assumed complete) so the CP accepts queue work;
everything else is programmed by this script the way the kext does it.
"""
import os, re, socket, struct, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
QEMU = os.environ.get("QEMU", os.path.expanduser("~/work/tools/qemu-compile/build/qemu-system-x86_64"))
STATE = os.path.join(REPO, "emu/qemu/gop-state.txt")

BAR0, BAR2, BAR5 = 0xD0000000, 0xE0000000, 0xE8000000     # VRAM, doorbells, registers
SLOT = 5

def GC0(dw): return (0x1260 + dw) * 4
def GC1(dw): return (0xa000 + dw) * 4
def OSS(dw): return (0x10a0 + dw) * 4

REG_GRBM_GFX_CNTL = GC1(0x0900)
REG_SH_MEM_CONFIG = GC1(0x09e4)
REG_SH_MEM_BASES = GC1(0x09e3)
REG_L1_TLB, REG_CTX0_CNTL, REG_FB_OFFSET = GC0(0x161b), GC0(0x1624), GC0(0x15a7)
REG_CTX1_CNTL, REG_CTX1_BASE_LO, REG_CTX1_START_LO, REG_CTX1_END_LO = GC0(0x1625), GC0(0x1691), GC0(0x16b1), GC0(0x16d1)
def INV_REQ(e): return GC0(0x1647 + e)
def INV_ACK(e): return GC0(0x1659 + e)
REG_FAULT_STATUS = GC0(0x15d0)
REG_NBIF_DB_APER_EN, REG_NBIF_S2A0 = (0xd20 + 0xc0) * 4, (0xd20 + 0x1cb) * 4
REG_CP_MEC_CNTL, REG_CP_MEC_PC_START, REG_CP_PQ_STATUS = GC1(0x2904), GC1(0x2900), GC0(0x1e58)
H = {n: GC0(o) for n, o in dict(ACTIVE=0x1fab, VMID=0x1fac, PQ_BASE=0x1fb1, PQ_BASE_HI=0x1fb2, PQ_RPTR=0x1fb3,
                                 RPTR_REP=0x1fb4, RPTR_REP_HI=0x1fb5, DOORBELL=0x1fb8, PQ_CNTL=0x1fba,
                                 WPTR_LO=0x1fdf, WPTR_HI=0x1fe0, EOP=0x1fce, EOP_HI=0x1fcf).items()}

VALID, READ, WRITE, EXEC, ISPTE = 1, 0x20, 0x40, 0x10, 1 << 63
RWX = VALID | READ | WRITE | EXEC | ISPTE
INV_ALL = 0x00f80000            # L2 PTEs, PDE0-2, L1 PTEs (what the kext and amdgpu request)
INV_L2_PTES, INV_L1_PTES = 1 << 19, 1 << 23
INV_PDES = (1 << 20) | (1 << 21) | (1 << 22)

# Guest-physical layout in VRAM (offset from BAR0); FB_OFFSET is 0 so GPU-physical == VRAM offset.
PDB2, PDB1, PDB0, PT = 0x100000, 0x101000, 0x102000, 0x103000
PT_B = 0x104000                 # a second page table for the PDE test
P_RING, P_EOP, P_RPTR = 0x210000, 0x211000, 0x212000
P1, P2, P3 = 0x200000, 0x201000, 0x202000
VA0 = 0x100000000               # the first VA of a client (GpuVm::kVaStart)
def va(i): return VA0 + 0x1000 * i
# page index in PT: 0 ring, 1 eop, 2 rptr, 3 target X
X = 3

class Qtest:
    def __init__(self, extra):
        self.dir = tempfile.mkdtemp(prefix="rdna4-qtest-")
        self.sock_path = os.path.join(self.dir, "qt.sock")
        self.log = open(os.path.join(self.dir, "qemu.log"), "w+")
        args = [QEMU, "-machine", "pc", "-accel", "qtest", "-display", "none", "-m", "64", "-nodefaults",
                "-qtest", "unix:%s,server=on,wait=off" % self.sock_path, "-qtest-log", "/dev/null",
                "-device", "rdna4,addr=%d,state=%s,test-gfx-booted=on%s" % (SLOT, STATE, extra)]
        self.proc = subprocess.Popen(args, stderr=self.log, stdout=subprocess.DEVNULL)
        for _ in range(200):
            if os.path.exists(self.sock_path):
                break
            time.sleep(0.05)
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(self.sock_path)
        self.f = self.s.makefile("rw", buffering=1)
        self.setup_pci()

    def cmd(self, line):
        self.f.write(line + "\n")
        while True:
            r = self.f.readline()
            if not r:
                raise RuntimeError("qtest closed; log: " + self.logtext()[-2000:])
            r = r.strip()
            if r.startswith("IRQ"):
                continue
            if r.startswith("OK"):
                return r[2:].strip()
            raise RuntimeError("qtest: %s -> %s" % (line, r))

    def readl(self, a): return int(self.cmd("readl 0x%x" % a), 16)
    def writel(self, a, v): self.cmd("writel 0x%x 0x%x" % (a, v & 0xffffffff))
    def writeq(self, a, v): self.cmd("writeq 0x%x 0x%x" % (a, v & 0xffffffffffffffff))
    def memwrite(self, a, data): self.cmd("write 0x%x 0x%x 0x%s" % (a, len(data), data.hex()))
    def memread(self, a, n): return bytes.fromhex(self.cmd("read 0x%x 0x%x" % (a, n))[2:])

    def pci_w(self, off, v):
        self.cmd("outl 0xcf8 0x%x" % (0x80000000 | (SLOT << 11) | (off & 0xfc)))
        self.cmd("outl 0xcfc 0x%x" % v)

    def setup_pci(self):
        self.pci_w(0x10, BAR0 & 0xffffffff); self.pci_w(0x14, 0)
        self.pci_w(0x18, BAR2 & 0xffffffff); self.pci_w(0x1c, 0)
        self.pci_w(0x24, BAR5)
        self.pci_w(0x04, 0x0006)            # memory space + bus master

    # -- device registers and VRAM -------------------------------------------------
    def reg(self, off): return self.readl(BAR5 + off)
    def wreg(self, off, v): self.writel(BAR5 + off, v)
    def vram_w(self, off, data): self.memwrite(BAR0 + off, data)
    def vram_r(self, off, n=4): return self.memread(BAR0 + off, n)
    def vram_w64(self, off, v): self.vram_w(off, struct.pack("<Q", v))
    def vram_w32(self, off, v): self.vram_w(off, struct.pack("<I", v))
    def vram_r32(self, off): return struct.unpack("<I", self.vram_r(off))[0]

    def logtext(self):
        self.log.flush()
        self.log.seek(0)
        return self.log.read()

    def close(self):
        try:
            self.cmd("clock_step") if False else None
        except Exception:
            pass
        self.proc.kill()
        self.proc.wait()

def grbm(vmid=0, pipe=0, queue=0, me=0):
    return (pipe & 3) | ((me & 3) << 2) | ((vmid & 0xf) << 4) | ((queue & 7) << 8)

def write_data(vaddr, *vals):
    n = len(vals)
    return [0xC0000000 | ((n + 2) << 16) | (0x37 << 8), (5 << 8) | (1 << 20), vaddr & 0xffffffff, vaddr >> 32] + list(vals)

class Scenario:
    """One device instance: VMID 3's page tables, one MEC queue (pipe 0 queue 1) whose packets run in VMID 3."""
    VMID, PIPE, QUEUE, DB = 3, 0, 1, 0x1a

    def __init__(self, extra=""):
        self.q = Qtest(extra)
        self.wptr = 0
        self.kicks = 0
        q = self.q
        q.wreg(REG_L1_TLB, 1); q.wreg(REG_CTX0_CNTL, 1); q.wreg(REG_FB_OFFSET, 0)
        for t in (PDB2, PDB1, PDB0, PT, PT_B):
            q.vram_w(t, bytes(0x1000))
        q.vram_w64(PDB2 + 0 * 8, (PDB1) | VALID)
        q.vram_w64(PDB1 + 4 * 8, (PDB0) | VALID)          # index(VA0, level 1) = 4
        q.vram_w64(PDB0 + 0 * 8, (PT) | VALID)
        self.map(PT, 0, P_RING); self.map(PT, 1, P_EOP); self.map(PT, 2, P_RPTR); self.map(PT, X, P1)
        self.map(PT_B, 0, P_RING); self.map(PT_B, 1, P_EOP); self.map(PT_B, 2, P_RPTR); self.map(PT_B, X, P2)
        for p in (P_RING, P_EOP, P_RPTR, P1, P2, P3):
            q.vram_w(p, bytes(0x1000))
        self.bind(PDB2)
        self.program_queue()

    def map(self, table, index, phys):
        self.q.vram_w64(table + index * 8, phys | RWX)

    def bind(self, pdb):
        """vmContextInit: CNTL (enable, depth 3, fault defaults), base, range."""
        n, q = self.VMID - 1, self.q
        q.wreg(REG_CTX1_CNTL + 4 * n, 1 | (3 << 1) | (((1 << 16) - 1) << 10))
        q.wreg(REG_CTX1_BASE_LO + 8 * n, (pdb & 0xffffffff) | 1); q.wreg(REG_CTX1_BASE_LO + 8 * n + 4, pdb >> 32)
        q.wreg(REG_CTX1_START_LO + 8 * n, 0); q.wreg(REG_CTX1_START_LO + 8 * n + 4, 0)
        q.wreg(REG_CTX1_END_LO + 8 * n, 0xffffffff); q.wreg(REG_CTX1_END_LO + 8 * n + 4, 0xffff)

    def program_queue(self):
        q = self.q
        q.wreg(REG_NBIF_DB_APER_EN, 1); q.wreg(REG_NBIF_S2A0, 0x30000007)
        q.wreg(REG_CP_PQ_STATUS, q.reg(REG_CP_PQ_STATUS) | 2)
        q.wreg(REG_CP_MEC_CNTL, (1 << 26)); q.wreg(REG_CP_MEC_PC_START, 1)
        q.wreg(REG_GRBM_GFX_CNTL, grbm(self.VMID, self.PIPE, self.QUEUE, me=1))
        q.wreg(H["VMID"], self.VMID)
        q.wreg(H["PQ_BASE"], va(0) >> 8); q.wreg(H["PQ_BASE_HI"], va(0) >> 40)
        q.wreg(H["RPTR_REP"], va(2) & 0xffffffff); q.wreg(H["RPTR_REP_HI"], va(2) >> 32)
        q.wreg(H["EOP"], va(1) >> 8); q.wreg(H["EOP_HI"], va(1) >> 40)
        q.wreg(H["DOORBELL"], (self.DB << 2) | (1 << 30))
        q.wreg(H["PQ_CNTL"], 9)                 # 2 << 9 = 1024 dwords
        q.wreg(H["WPTR_LO"], 0); q.wreg(H["WPTR_HI"], 0); q.wreg(H["PQ_RPTR"], 0)
        q.wreg(H["ACTIVE"], 1)
        q.wreg(REG_GRBM_GFX_CNTL, grbm(0))

    def submit(self, packets, timeout=5.0):
        """Write packets into the ring (VRAM, where the ring page is mapped), kick the doorbell, wait for the CP to report rptr."""
        q = self.q
        data = b"".join(struct.pack("<I", d) for d in packets)
        q.vram_w(P_RING + 4 * self.wptr, data)
        self.wptr += len(packets)
        q.writeq(BAR2 + self.DB * 4, self.wptr)
        self.kicks += 1
        end = time.time() + timeout
        while time.time() < end:
            if q.vram_r32(P_RPTR) == self.wptr:
                return True
            time.sleep(0.01)
        return False

    def write_x(self, value):
        return self.submit(write_data(va(X), value))

    def invalidate(self, req, engine=17):
        self.q.wreg(INV_REQ(engine), req)

    def stale_count(self):
        return len(re.findall(r"STALE TRANSLATION used", self.q.logtext()))

    def close(self):
        self.q.close()

checks = []
def check(ok, what):
    checks.append(bool(ok))
    print("  %s  %s" % ("ok  " if ok else "FAIL", what))

def tlb_scenario(title, extra=""):
    print(title)
    return Scenario(extra)

def test_stale_until_invalidated():
    sc = tlb_scenario("E1 a translation stays cached until an invalidation of that VMID covers it")
    try:
        check(sc.write_x(0x11111111) and sc.q.vram_r32(P1) == 0x11111111, "first write walks the tables and lands in P1")
        sc.map(PT, X, P2)                                             # the client remaps X without telling the hub
        check(sc.write_x(0x22222222), "second write is processed")
        check(sc.q.vram_r32(P1) == 0x22222222 and sc.q.vram_r32(P2) == 0, "stale: it still lands in the OLD page (P1), not P2")
        check(sc.stale_count() >= 1, "the emulator names the stale translation in its log")
        sc.invalidate(1 << 4 | INV_ALL)                               # a different VMID (4): VMID 3 is not covered
        sc.write_x(0x44444444)
        check(sc.q.vram_r32(P2) == 0, "invalidating VMID 4 does not help VMID 3 (still P1)")
        sc.invalidate((1 << sc.VMID) | INV_ALL)
        sc.write_x(0x33333333)
        check(sc.q.vram_r32(P2) == 0x33333333, "after invalidating VMID 3 the write goes to P2")
    finally:
        sc.close()

def test_level_bits():
    sc = tlb_scenario("E1 the invalidation's level bits select which cache is flushed (L1 PTEs, L2 PTEs, PDEs)")
    try:
        sc.write_x(0x1)                                               # walk: L1 and L2 now hold X -> P1
        sc.map(PT, X, P2)
        sc.invalidate((1 << sc.VMID) | INV_L1_PTES)
        sc.write_x(0x2)
        check(sc.q.vram_r32(P2) == 0, "L1-only invalidate: L2 still holds the old page and refills L1 with it")
        sc.invalidate((1 << sc.VMID) | INV_L2_PTES)
        sc.write_x(0x3)
        check(sc.q.vram_r32(P2) == 0, "then L2-only invalidate: the refilled L1 entry still serves the old page")
        sc.invalidate((1 << sc.VMID) | INV_L1_PTES | INV_L2_PTES)
        sc.write_x(0x4)
        check(sc.q.vram_r32(P2) == 0x4, "L1+L2 invalidate: the new page")
        # A directory entry: point the PDB0 entry at another page table, leaf caches cleared but not the PDEs.
        sc.q.vram_w64(PDB0, PT_B | VALID)
        sc.map(PT_B, X, P3)
        sc.invalidate((1 << sc.VMID) | INV_L1_PTES | INV_L2_PTES)
        sc.write_x(0x5)
        check(sc.q.vram_r32(P3) == 0, "leaf-only invalidate after a PDE change: the cached PDE still leads to the old page table")
        sc.invalidate((1 << sc.VMID) | INV_ALL)
        sc.write_x(0x6)
        check(sc.q.vram_r32(P3) == 0x6, "with the PDE bits too: the new page table")
    finally:
        sc.close()

def test_context_rebind_needs_flush():
    sc = tlb_scenario("E1 writing the context's PAGE_TABLE_BASE does not flush (the amdgpu flush sequence exists for this)")
    try:
        sc.write_x(0x1)
        # Another client's tables: same layout, X maps to P2.
        for t, nxt in ((0x110000, 0x111000), (0x111000, 0x112000), (0x112000, 0x113000)):
            sc.q.vram_w(t, bytes(0x1000))
        sc.q.vram_w(0x113000, bytes(0x1000))
        sc.q.vram_w64(0x110000, 0x111000 | VALID); sc.q.vram_w64(0x111000 + 4 * 8, 0x112000 | VALID)
        sc.q.vram_w64(0x112000, 0x113000 | VALID)
        for i, p in ((0, P_RING), (1, P_EOP), (2, P_RPTR), (X, P2)):
            sc.map(0x113000, i, p)
        sc.bind(0x110000)
        sc.write_x(0x2)
        check(sc.q.vram_r32(P2) == 0 and sc.stale_count() >= 1, "base rewritten, no invalidate: the write still uses the OLD client's mapping")
        sc.invalidate((1 << sc.VMID) | INV_ALL)
        sc.write_x(0x3)
        check(sc.q.vram_r32(P2) == 0x3, "base rewritten + invalidate: the new client's mapping")
    finally:
        sc.close()

def test_ack_latency():
    sc = tlb_scenario("E2 an invalidation completes when its ACK has been polled (inv-ack-reads=3)", ",inv-ack-reads=3")
    try:
        sc.write_x(0x1)
        sc.map(PT, X, P2)
        sc.invalidate((1 << sc.VMID) | INV_ALL)
        check(sc.q.reg(INV_ACK(17)) & (1 << sc.VMID) == 0, "ACK is not set right after the request")
        sc.write_x(0x2)
        check(sc.q.vram_r32(P2) == 0, "a submission before the ACK still sees the old translation (the invalidation has not taken effect)")
        sc.q.reg(INV_ACK(17))
        acked = sc.q.reg(INV_ACK(17)) & (1 << sc.VMID)
        check(acked != 0, "ACK shows after the third read")
        sc.write_x(0x3)
        check(sc.q.vram_r32(P2) == 0x3, "then the new translation")
    finally:
        sc.close()

def test_engines_and_noack():
    sc = tlb_scenario("E2 all 18 engines invalidate; inv-noack never completes", "")
    try:
        ok = True
        for eng in (0, 5, 17):
            sc.write_x(0x1); sc.map(PT, X, P1)
            sc.write_x(0x1)
            sc.map(PT, X, P2)
            sc.invalidate((1 << sc.VMID) | INV_ALL, eng)
            ack = sc.q.reg(INV_ACK(eng)) & (1 << sc.VMID)
            sc.write_x(0x7 + eng)
            ok &= ack != 0 and sc.q.vram_r32(P2) == 0x7 + eng
            sc.map(PT, X, P1)
            sc.invalidate((1 << sc.VMID) | INV_ALL, eng)
        check(ok, "engines 0, 5 and 17: ACK and effect")
    finally:
        sc.close()
    sc = tlb_scenario("E2 inv-noack (the card's behaviour in the 2026-09-28 logs)", ",inv-noack=on")
    try:
        sc.write_x(0x1)
        sc.map(PT, X, P2)
        sc.invalidate((1 << sc.VMID) | INV_ALL)
        check(sc.q.reg(INV_ACK(17)) & (1 << sc.VMID) == 0, "no ACK ever")
        sc.write_x(0x2)
        check(sc.q.vram_r32(P2) == 0, "and no effect: the stale translation stays")
    finally:
        sc.close()

def test_tlb_off_is_the_old_model():
    sc = tlb_scenario("E1 negative control: tlb-off walks the tables every time (the pre-W13 model)", ",tlb-off=on")
    try:
        sc.write_x(0x1)
        sc.map(PT, X, P2)
        sc.write_x(0x2)
        check(sc.q.vram_r32(P2) == 0x2 and sc.stale_count() == 0, "with no TLB a missing flush is invisible: exactly what the old emulator hid")
    finally:
        sc.close()

def test_sh_mem_per_vmid():
    print("E4 SH_MEM_CONFIG / SH_MEM_BASES are banked by VMID, not by queue")
    q = Qtest("")
    try:
        q.wreg(REG_GRBM_GFX_CNTL, grbm(3)); q.wreg(REG_SH_MEM_CONFIG, 0xc00c); q.wreg(REG_SH_MEM_BASES, 0x00010002)
        q.wreg(REG_GRBM_GFX_CNTL, grbm(4)); q.wreg(REG_SH_MEM_CONFIG, 0xc00d)
        q.wreg(REG_GRBM_GFX_CNTL, grbm(3, pipe=0, queue=2, me=1))
        check(q.reg(REG_SH_MEM_CONFIG) == 0xc00c and q.reg(REG_SH_MEM_BASES) == 0x00010002,
              "VMID 3 reads back its values through another pipe/queue selection")
        q.wreg(REG_GRBM_GFX_CNTL, grbm(4, queue=1))
        check(q.reg(REG_SH_MEM_CONFIG) == 0xc00d and q.reg(REG_SH_MEM_BASES) == 0, "VMID 4 has its own; its BASES were never written")
        q.wreg(REG_GRBM_GFX_CNTL, grbm(5))
        check(q.reg(REG_SH_MEM_CONFIG) == 0, "VMID 5 was never programmed")
        q.wreg(REG_GRBM_GFX_CNTL, grbm(0)); q.wreg(REG_SH_MEM_CONFIG, 0xc00e)
        q.wreg(REG_GRBM_GFX_CNTL, grbm(3))
        check(q.reg(REG_SH_MEM_CONFIG) == 0xc00c, "a VMID 0 write does not leak into VMID 3")
    finally:
        q.close()

def test_ih_lut_and_fault_vector():
    sc = tlb_scenario("E5 a VM fault raises the GC hub vector with the VMID and the PASID from IH_VMID_LUT")
    try:
        q = sc.q
        ring, wb = 0x01000000, 0x01100000
        q.memwrite(ring, bytes(64)); q.memwrite(wb, bytes(8))
        q.wreg(OSS(0x83), ring >> 8); q.wreg(OSS(0x84), 0)
        q.wreg(OSS(0x86), wb); q.wreg(OSS(0x85), 0)
        q.wreg(OSS(0x80), 1 | (1 << 8) | (1 << 17) | (2 << 28))         # RB_ENABLE, WPTR_WRITEBACK, ENABLE_INTR, MC_SPACE bus
        q.wreg(OSS(3), 0x1234)                                          # IH_VMID_3_LUT
        q.wreg(OSS(4), 0x0beef)
        check(q.reg(OSS(3)) == 0x1234, "the LUT registers read back")
        q.wreg(REG_FAULT_STATUS, 0)
        sc.submit(write_data(va(9), 0xdead), timeout=1.0)              # VA 9 is unmapped: a write fault in VMID 3
        iv = q.memread(ring, 32)
        dw = struct.unpack("<8I", iv)
        fault_iv = (dw[0] & 0xff) == 0x0a and ((dw[0] >> 8) & 0xff) == 0
        check(fault_iv, "a client-10 source-0 vector was written (dw0 0x%08x)" % dw[0])
        check(((dw[0] >> 24) & 0xf) == 3, "it carries VMID 3")
        check((dw[3] & 0xffff) == 0x1234, "its PASID is the LUT entry of VMID 3 (0x%04x)" % (dw[3] & 0xffff))
        check(dw[4] == (va(9) >> 12) & 0xffffffff, "src_data[0] is VA >> 12 (0x%x)" % dw[4])
        check((dw[5] & 0x60) == 0x20, "src_data[1] says WRITE (0x%x)" % dw[5])
        # Re-target VMID 3 to another PASID after the vector was produced: the vector keeps the old one.
        q.wreg(OSS(3), 0x4321)
        check((struct.unpack("<8I", q.memread(ring, 32))[3] & 0xffff) == 0x1234, "an already written vector keeps its PASID")
    finally:
        sc.close()

REG_FB_BASE, REG_FB_TOP = GC0(0x1614), GC0(0x1615)
MC_BASE = 0x8000000000           # FB_BASE 0x8000 << 24
P_PRIV_RING, P_PRIV_RPTR, P_PRIV_EOP, P_PRIV_FENCE, P_IB = 0x220000, 0x221000, 0x222000, 0x223000, 0x224000

def ib_packet(addr, dwords, vmid):
    return [0xC0000000 | (2 << 16) | (0x3f << 8), addr & 0xffffffff, addr >> 32, dwords | (1 << 23) | (vmid << 24)]

def release_mem(mc, seq):
    return [0xC0000000 | (6 << 16) | (0x49 << 8), 0x00000000, 1 << 29, mc & 0xffffffff, mc >> 32, seq, 0, 0]

def test_u1_priv_queue_model():
    """What the S1 self-test (src/vmtest.cpp, T3/T7) expects of the emulator: a VMID-0 privileged MEC queue runs an
    IB in the VMID of the IB packet. This is the emulator's statement of assumption U1, NOT evidence for the card."""
    sc = tlb_scenario("E6/S1 model: a VMID-0 PRIV_STATE queue runs an IB in the packet's VMID (assumption U1 as code); a VMID with no context faults with that VMID")
    try:
        q = sc.q
        q.wreg(REG_FB_BASE, MC_BASE >> 24); q.wreg(REG_FB_TOP, (MC_BASE >> 24) + 15)
        q.vram_w(P_IB, bytes(0x1000))
        sc.map(PT, 4, P_IB)                                            # VA page 4 = the IB, in VMID 3's tables
        for page in (P_PRIV_RING, P_PRIV_RPTR, P_PRIV_EOP, P_PRIV_FENCE):
            q.vram_w(page, bytes(0x1000))
        def prog():
            q.wreg(REG_GRBM_GFX_CNTL, grbm(0, pipe=1, queue=3, me=1))
            q.wreg(H["VMID"], 0)
            q.wreg(H["PQ_BASE"], (MC_BASE + P_PRIV_RING) >> 8); q.wreg(H["PQ_BASE_HI"], (MC_BASE + P_PRIV_RING) >> 40)
            q.wreg(H["RPTR_REP"], (MC_BASE + P_PRIV_RPTR) & 0xffffffff); q.wreg(H["RPTR_REP_HI"], (MC_BASE + P_PRIV_RPTR) >> 32)
            q.wreg(H["EOP"], (MC_BASE + P_PRIV_EOP) >> 8); q.wreg(H["EOP_HI"], (MC_BASE + P_PRIV_EOP) >> 40)
            q.wreg(H["DOORBELL"], (0x32 << 2) | (1 << 30))
            q.wreg(H["PQ_CNTL"], 9 | (1 << 30))                      # 1024 dwords, PRIV_STATE
            q.wreg(H["WPTR_LO"], 0); q.wreg(H["WPTR_HI"], 0); q.wreg(H["PQ_RPTR"], 0)
            q.wreg(H["ACTIVE"], 1)
            q.wreg(REG_GRBM_GFX_CNTL, grbm(0))
        prog()
        wp = [0]
        def job(vmid, seq):
            pk = ib_packet(va(4), 8, vmid) + release_mem(MC_BASE + P_PRIV_FENCE, seq)
            q.vram_w(P_PRIV_RING + 4 * wp[0], b"".join(struct.pack("<I", d) for d in pk))
            wp[0] += len(pk)
            q.writeq(BAR2 + 0x32 * 4, wp[0])
            end = time.time() + 2.0
            while time.time() < end:
                if q.vram_r32(P_PRIV_FENCE) == seq:
                    return True
                time.sleep(0.01)
            return False
        ib = write_data(va(X), 0xa5a50003) + [0xffff1000] * 3
        ib = ib[:8] + [0xffff1000] * (8 - len(ib)) if len(ib) < 8 else ib
        q.vram_w(P_IB, b"".join(struct.pack("<I", d) for d in ib))
        check(job(3, 0x11) and q.vram_r32(P1) == 0xa5a50003, "IB packet VMID 3 on the VMID-0 queue: the fence lands and the IB wrote through VMID 3's tables")
        check(q.reg(REG_FAULT_STATUS) == 0, "no fault")
        q.vram_w32(P1, 0)
        sc.map(PT, 4, P_IB)
        ok = job(9, 0x12)                                              # VMID 9: context never enabled
        st = q.reg(REG_FAULT_STATUS)
        check(not ok and q.vram_r32(P1) == 0 and st != 0 and ((st >> 20) & 0xf) == 9,
              "IB packet VMID 9 (no context): no fence, no data, fault status names VMID 9 (0x%08x)" % st)
    finally:
        sc.close()

def test_vmid_rebind_protocol():
    """S8's B-V1: one VMID re-targeted between two clients over MMIO on an idle VMID: write the page-directory base, set the IH LUT, invalidate
    (src/vmshared.cpp vmAcquire). The negative control skips the invalidate: the emulator's TLB must then serve the OLD client's mapping and name it."""
    sc = tlb_scenario("S8 B-V1: a VMID rebound between two clients: rebind + invalidate isolates them; without the invalidate the emulator catches it")
    try:
        q = sc.q
        # Client A = the scenario's tables (X -> P1). Client B = a second set (X -> P2).
        for t in (0x110000, 0x111000, 0x112000, 0x113000):
            q.vram_w(t, bytes(0x1000))
        q.vram_w64(0x110000, 0x111000 | VALID); q.vram_w64(0x111000 + 4 * 8, 0x112000 | VALID); q.vram_w64(0x112000, 0x113000 | VALID)
        for i, p in ((0, P_RING), (1, P_EOP), (2, P_RPTR), (X, P2)):
            sc.map(0x113000, i, p)
        ring, wb = 0x01000000, 0x01100000
        q.memwrite(ring, bytes(256)); q.memwrite(wb, bytes(8))
        q.wreg(OSS(0x83), ring >> 8); q.wreg(OSS(0x84), 0); q.wreg(OSS(0x86), wb); q.wreg(OSS(0x85), 0)
        q.wreg(OSS(0x80), 1 | (1 << 8) | (1 << 17) | (2 << 28))
        V = sc.VMID
        def rebind(pdb, pasid, invalidate=True):
            sc.bind(pdb)                                      # CNTL, base, range
            q.wreg(OSS(V), pasid)                             # IH_VMID_n_LUT
            if invalidate:
                sc.invalidate((1 << V) | INV_ALL)
        rebind(PDB2, 0x11)
        sc.write_x(0xa1)
        check(q.vram_r32(P1) == 0xa1 and q.vram_r32(P2) == 0, "client A's job wrote through A's tables (P1)")
        rebind(0x110000, 0x22)
        sc.write_x(0xb1)
        check(q.vram_r32(P2) == 0xb1 and q.vram_r32(P1) == 0xa1, "after rebind + invalidate client B's job wrote through B's tables (P2), A's page untouched")
        check(sc.stale_count() == 0, "no stale translation was used by the correct protocol")
        q.wreg(REG_FAULT_STATUS, 0)
        sc.submit(write_data(va(9), 0xdead), timeout=1.0)    # a fault while the VMID is B's
        iv = struct.unpack("<8I", q.memread(ring, 32))
        check((iv[3] & 0xffff) == 0x22, "the fault vector names client B's PASID (0x%04x)" % (iv[3] & 0xffff))
        q.wreg(REG_FAULT_STATUS, 0)
        # Negative control: rebind to A but SKIP the invalidate (a kext bug): the TLB still holds B's translation for X.
        rebind(PDB2, 0x11, invalidate=False)
        before = sc.stale_count()
        sc.write_x(0xa2)
        check(q.vram_r32(P2) == 0xa2 and q.vram_r32(P1) == 0xa1, "negative control: without the invalidate client A's job reaches B's page (the cross-client leak a missing flush causes)")
        check(sc.stale_count() > before, "negative control: the emulator names the STALE TRANSLATION")
        sc.invalidate((1 << V) | INV_ALL)
        sc.write_x(0xa3)
        check(q.vram_r32(P1) == 0xa3, "with the invalidate A's job is back in A's tables")
    finally:
        sc.close()

def test_fault_is_not_stale():
    """Kiln's dry run (hub-task-333) flagged 16640 'STALE TRANSLATION' lines, every one at a VA the kext had logged a fault for: a faulting access that
    passed through a cached upper-level PDE made the span walker return the fault-default page, which the verification walk could not reproduce. A fault
    is not a stale translation; a REAL stale translation must still be caught (negative control)."""
    sc = tlb_scenario("S8 fix: an access that FAULTS after cached PDEs are not reported as a stale translation; a real stale one still is")
    try:
        sc.write_x(0x1)                                      # caches the PDEs and the leaf of the region
        check(sc.stale_count() == 0, "a normal access: no stale report")
        sc.q.wreg(REG_FAULT_STATUS, 0)
        sc.submit(write_data(va(9), 0xdead), timeout=1.0)    # VA 9: same page table, no PTE: a write fault through cached PDEs
        check(sc.q.reg(REG_FAULT_STATUS) != 0, "the access faulted (status 0x%08x)" % sc.q.reg(REG_FAULT_STATUS))
        check(sc.stale_count() == 0, "a fault through cached PDEs is NOT reported as a stale translation (was: one report per faulting access)")
        sc.map(PT, X, P2)                                    # a real stale translation
        sc.write_x(0x2)
        check(sc.stale_count() >= 1 and sc.q.vram_r32(P1) == 0x2, "negative control: a real stale leaf is still caught")
    finally:
        sc.close()

def main():
    if not os.path.exists(QEMU):
        print("QEMU not found: %s (set QEMU=)" % QEMU)
        return 2
    for t in (test_stale_until_invalidated, test_level_bits, test_context_rebind_needs_flush, test_ack_latency,
              test_engines_and_noack, test_tlb_off_is_the_old_model, test_sh_mem_per_vmid, test_ih_lut_and_fault_vector,
              test_u1_priv_queue_model, test_vmid_rebind_protocol, test_fault_is_not_stale):
        try:
            t()
        except Exception as e:                                          # a scenario that cannot run is a failure, loudly
            checks.append(False)
            print("  FAIL  %s: %s" % (t.__name__, e))
    bad = checks.count(False)
    print("\n%d checks, %d failed" % (len(checks), bad))
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
