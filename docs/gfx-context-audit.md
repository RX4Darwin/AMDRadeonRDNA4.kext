# W31: the round-4 draw, the "garbage context registers", and the CP-side readback

Round 4 (`hw-logs/rdna4fb-diag-20260929-081934.txt` boot 5, `-083603.txt` boot 11): still 0 pixels, the PS marker not
written (no pixel wave ran), the goldens applied as amdgpu does (rev_id 1, so only `DB_MEM_CONFIG`, already `0x8035`). The MMIO
readback showed the SH registers right and the **context** registers as random values that differ between boots.

## 1. What the evidence really says

**The MMIO context readback lags one context; it does not show that our writes are lost.** Boot 11, same boot, same registers:

| Draw | `CTX:` readback (hw-logs `-083603`) |
|---|---|
| baseline (line 343) | `VGT_SHADER_STAGES_EN=0xfc1ffec8 CB_TARGET_MASK=0x147f58a7 ...` random |
| `diag 8` (line 362), `diag 2` (381), `diag 1` (399) | `VGT_SHADER_STAGES_EN=0x04400000 CB_TARGET_MASK=0x0000000f CB_COLOR_CONTROL=0x00cc0010 ...` **exactly the stream's values** |

After one draw has run, the MMIO view holds what the previous draw's stream set; before the first draw it holds an uninitialised
context. So the CP does execute our `SET_CONTEXT_REG` packets (the classic packets are accepted), and the first-draw garbage is the
context the MMIO port happens to read, not the one the draw used. (An inference from the log, not a measurement of the active
context: that is what the new CP-side readback measures.) `VGT_PRIMITIVE_TYPE` reads 0 in every case: it is a UCONFIG register,
probably not readable this way.

**New clue that MMIO does not explain: the CPG reads VA 0.** Round 4 shows (boot 11, `-083603` lines 333-340):

- `GC hub fault status 0x00000d3d (VMID 0, CID 0x6, read), VA 0x0` already at "before draw" (the `sdma:` line at 270 read 0), so it happened
  during the gfx ring bring-up or the runtime stages after it; CID 6 is `CPG` (`gfxhub_v12_0.c:37-59`, the graphics command processor);
- two IH VM-fault vectors right after the draw was kicked: `ring 158` and `ring 174`, VMID 0, VA 0.

Something in the graphics command processor fetches from address 0. It cannot be told from the log what, or when: the round-3 code
only read the fault status after the draw. Round 5 pins the step (section 4).

## 2. amdgpu / Mesa compared with what the kext does

| What | amdgpu / Mesa | kext |
|---|---|---|
| CP start | `gfx_v12_0_cp_gfx_start` (`amdgpu/gfx_v12_0.c:2699-2710`): `CP_MAX_CONTEXT = max_hw_contexts - 1`, `CP_DEVICE_ID = 1`, un-halt. No `CLEAR_STATE`, no preamble, no `SET_BASE`, no CE on gfx12. | same (`gfxring.cpp` `gfxRingResume`, `CpMaxContext`, `CpDeviceId`) |
| Ring resume | `gfx_v12_0_cp_gfx_resume` (`:2748+`): `CP_RB_WPTR_DELAY`, `CP_RB_VMID = 0`, `CP_RB0_CNTL`, wptr, `RPTR_ADDR`, `WPTR_POLL_ADDR`, 1 ms, `CP_RB0_CNTL`, base, `CP_RB_ACTIVE`, doorbell | same order |
| Clear state | only handed to the RLC (`RLC_CSIB_*`, `gfx_v12_0.c:1938-1947`) | `gfxCsbInit` |
| Per-IB context control | `gfx_v12_0_ring_emit_cntxcntl` (`:4655`): `0x80000000` "otherwise this package is just NOPs", plus load bits on a context switch | the stream's `CONTEXT_CONTROL` is `0x80000000/0x80000000` as Mesa's (`si_state.c:5060-5063`) |
| IB packet | `gfx_v12_0_ring_emit_ib_gfx` (`:4523`): `length | vmid << 24`, no `INDIRECT_BUFFER_VALID` | `Pm4::indirectBufferGfx` (ring, IB and wrap tests pass) |
| Microcode start | with the PSP autoload `gfx_v12_0_config_gfx_rs64` (`:2142-2210`): `PRGRM_CNTR_START` + pipe resets; the data-cache bases `CP_GFX_RS64_DC_BASE0/1` are only written in the direct-load path (`:2494-2505`, `:2639-2650`) | `cpConfigRs64` mirrors `config_gfx_rs64`; the bases were never read |
| Context registers | Mesa on gfx12 uses `SET_CONTEXT_REG_PAIRS` (`sid.h:236-265`), which **must** set `RESET_FILTER_CAM` (`ac_pm4.c:243-259`); the kernel gfx12 code never emits `SET_CONTEXT_REG` (only defined, `nvd.h:516`) | classic `SET_CONTEXT_REG`/`SET_SH_REG`/`SET_UCONFIG_REG` (the notes: still valid gfx12 packets, `pm4_it_opcodes_gfx12.h:58-61`); SH and, per section 1, context writes land |

Nothing found there differs from amdgpu in the ring init. The classic-versus-PAIRS question is not the failure: both the SH readback and
the second draw's context readback show the classic packets taking effect.

## 3. Ranked suspects for "no pixel wave, CPG reads VA 0"

1. **The CPG's stray fetch at VA 0** (`GC hub fault 0x0d3d`, CID `CPG`, present before the draw). The RS64 PFP/ME read their stack and data
   through `CP_GFX_RS64_DC_BASE0/1`; if those (or the IC bases) are 0 on this path, the first handler that needs data memory (a draw, PWS, a
   big `SET_*` run) reads VA 0. amdgpu programs them only in the direct-load path, so a PSP/RLC-autoload boot relies on the firmware
   having set them. New evidence prints them (`RS64 DC_BASE0 ...`).
2. **An ignored packet or a state the first draw does not have.** The CP-side readback (`probe mid`/`probe post`) says whether the CP holds
   the stream's values when the draw starts; if they match, everything after the draw packet (GE, SPI, SC, CB) is the suspect, if not, the
   listed registers were dropped.
3. Unsourced hardware reset values of registers the stream leaves alone (notes open question 2).
4. NGG details (user SGPRs, `INST_PREF_SIZE`, `GS_ALLOC_REQ`): the ladder; the round-4 ladder (8, 2, 1) shows they do not change the outcome.
5. Colour-buffer write path (GL2/MALL): the marker variant (bit 8) already says no pixel wave ran, which points before the CB.

## 4. What round 5 shows (`rdna4-gfxprobe=1`, set-boot boots 10-12)

- `fault after <step>`: the GC hub fault status after each gfx bring-up step (start, ring setup/unhalt, ring test, WRITE_DATA + fence, IB
  test, ring wrap) and after each draw; the status is cleared each time, so the first step that reports `CID 0x6 read` is the step whose
  packets make the CPG read VA 0.
- `RS64 DC_BASE0 0x<hi>_<lo> DC_BASE1 ... | PFP IC_BASE ... | ME IC_BASE ... | INSTR_PNTR0/1`: at bring-up and before the draw.
- The draw stream is split at `NUM_INSTANCES` into a state IB and a draw IB; `COPY_DATA` packets read 22 key registers into memory between them
  (`probe mid`) and after the draw (`probe post`). The report lists what the CP returned and `N of M registers equal what the stream wrote`,
  naming the ones that differ. Any register the CP returns as the stream's value was set in the context the draw uses.

Reading it: `probe mid` all equal and still no pixels and no marker: state is right, look downstream of the CP (GE/SPI/PA), the `fault after`
line and the RS64 bases decide whether the CP itself is unhealthy. Some registers differ: they were not applied (a `SET_*` packet the CP ignores or
a filter); fix that packet form. DC_BASE 0: program the firmware's data base as the direct-load path does.

## 5. W33: the W31 review follow-ups (`premetal/w31-review.md`)

- **S1, which packet makes the CPG read and write VA 0.** The IH vectors of round 4 are a *read and a write* at VA 0 (`src_data[1]` 0x40 and 0x20) and they recur with every
  submission that carries a `RELEASE_MEM`; the IH ring only comes up after the ring tests, so a fault during the bring-up left no IV. Instead of moving IH earlier
  (the ring tests would then depend on the IH stage), `gfxPacketProbe` (`rdna4-gfxprobe=1`, right after the ring test) submits **one packet per submission** and reads
  the fault status after each: `single NOP`, `single WRITE_DATA`, `single RELEASE_MEM` (fence), `single ACQUIRE_MEM` (the GL2 write-back flush). The status is cleared first
  (`fault after single-packet probe start`), each packet gets a 2 ms settle time, and the first `fault after single <PACKET>` line that names `CID 0x6` is the packet.
  If none does, the fault comes from the later steps (fence test, IB test, wrap: each has its own mark).
- **S2, is the CP-side read decisive?** `gfxSentinelCheck` (`rdna4-gfxprobe=1`, before the baseline draw) writes sentinel A to CB_SHADER_MASK (a register the draw stream sets
  again, so no state is left), reads it back with `COPY_DATA` on the ring, writes sentinel B, reads again, and reads once over MMIO. Verdicts printed on the `sentinel:` line:
  A then B = the CP-side read tracks writes (`probe mid` is trustworthy); A then A = it lags one write like MMIO (the first draw's `probe mid` is stale, do not read it as
  "not applied"); the same stale value twice = it does not observe context writes. The ladder also prints `probe consistency (mid, equal/compared)` with the baseline next to
  every variant; a baseline below its variants says the same lag.
- **S3, fault marks and RS64 evidence run on every `rdna4-gfx=2` boot** (they always did: they are not behind `rdna4-gfxprobe`), and they clear the status. The commit message of
  W31 said otherwise; the decision (with the lead) is to keep it, so on boots 4, 5 and 10-12 the `before draw` fault line means "since the previous mark". RS64 evidence now reads both ME0
  pipes (the registers are per pipe). Read-only, and DC_BASE must not be programmed from the kext without the PSP-loaded data image's address.
- **A bug found on the way:** the PS marker (`kGfxTestOffset + 0x40`) and probe slot 0 (`kGfxProbeOffset`) were the same dword, so `VGT_SHADER_STAGES_EN got 0xc0de0001` in the
  emulator's `diag 8` probe was the marker, not a register. The probe buffers moved to `kGfxRptrOffset + 0x200/0x280`.
