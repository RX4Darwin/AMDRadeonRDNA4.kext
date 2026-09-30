# Why per-client GPU VM work fails on the real card (hub-task-311)

Author: Anvil, branch `premetal/power`, 2026-09-30. **No kext code was changed and no GPU was run for this document.**
Evidence tags: **[M]** measured (log file:line, or a header/source line I read), **[I]** inferred, **[U]** unknown.
Logs: `/run/media/miguer/OPENCORE/rdna4fb-diag-2026093*.txt` and `old-logs/` (read-only). "053316" = round 5 boot 2, "061829" = round 6 boot 2,
"062351" = round 6 boot 3 (same args plus `rdna4-gfx=2`).

## 0. Verdict

**The root cause is not provable from the code and the logs. I do not propose a fix yet.** What the evidence does settle:

1. The client path and the boot self-test program the **same registers in the same order with the same values** (same `hqdInitFor`, same VA layout, same
   doorbell dword 26, same slot (0,1) for the first client). The "PQ_CONTROL 0xd0300909 vs 0xd0308909" difference is **PQ_EMPTY (bit 15, read-only
   status)** [M: `gc_12_0_0_sh_mask.h:13272`], not a configuration difference. Hypotheses about a different MQD/HQD/doorbell setup are mostly dead (§3, H3).
2. **Two separate symptoms are being mixed**, and they may have different causes:
   - **S-A** the GPU is pinned at 3417-3422 MHz / 100 % / 75-81 W **before any client exists**, in every `rdna4-vm=1` boot, including round 5 where the boot
     self-test failed with its queue stuck *and* round 6 where it passed and the queue dequeued cleanly. Same numbers in both [M §2]. So the stuck HQD of round 5
     is **not** what pins the GPU: something `vm=1` does at boot does, and it survives a clean boot test [I].
   - **S-B/S-C** client queues are never serviced (doorbell HIT still set, rptr 0) and **no client HQD can ever be dequeued, not even clients that never kicked anything** [M/I §2].
3. The boot self-test is a weak proof: it runs **no shader and no ACQUIRE_MEM/SET_SH_REG/DISPATCH**, only WRITE_DATA + RELEASE_MEM, and in round 6 the
   bring-up lines are **not in the log at all** (dmesg ring wrapped; only `RDNA4FB,Results vm=PASS` survives, 061829:306). **The first shader wave in a
   non-zero VMID on this card is the client's** (Forge, confirmed in code: `launch()` compute.cpp:3115-3139 vs `vmBootSelfTest` runtime.cpp:984-985).
4. The lead's replay shows our exact vadd IB and code object run correctly in a per-process VMID under amdgpu's setup (all 256 results right). That clears the
   **shader bytes and the PM4 stream content**; what is left is kext-side VM/queue/SH_MEM/MQD state and the busy-before-any-client state (§3).

Ranked hypotheses in §3; discriminators in §4; answer to Forge's probe list in §5; what I want Kiln to run in §6.

## 1. Which boots have what (VM boots only; `rdna4-vm=1`)

| Boots | vm boot self-test | why | clients |
|---|---|---|---|
| rounds 2-4 (2026-09-28/29, ~25 logs, e.g. `20260928-155917`…`20260929-082836`) | FAIL every time | walker/permission faults (round 2 `0x00800b3b` MAPPING_ERROR without IS_PTE, `vmfix2-findings.md`; later rounds: see the W17-W42 notes) | runtime disabled, kernel queue used |
| **round 5** `053316`, `053346` (2026-09-30) | FAIL | `0x00800880` = PERMISSION fault, VMID 8, CID 4 (CPF), read, VA 0x100001000 (the EOP page mapped R|W without EXEC) | runtime disabled |
| **round 6** `061829`, `062351` | **PASS** ("vm"="PASS boot self-test", 061829:306) | EXEC on every leaf, like Linux's tables | **all fail** |

The round 6 pass is therefore the **first ever VM-queue pass on the card**, and client queues have **never worked on the card** in any round [M: no log shows a client
VM dispatch succeeding]. "What differs between the boot test and the client" cannot be answered by "it worked before".

## 2. Timelines

### 2.1 Round 5 boot 2 (`053316`) — boot self-test FAIL

| t (s) | Line | Event |
|---|---|---|
| 21.758 | :264 | RLC autoload complete, GRBM `0x0000382c` (idle) |
| 21.779-21.782 | :285-287 | HQD ME1/pipe0/queue0 VMID0 active, PQ_CONTROL `0xd0308909`, doorbell dword 6; WRITE_DATA + PM4 + RELEASE_MEM all landed (plain queue works) |
| 21.930 | :340 | `vm: F` fault default page (diag 512 was on this boot) |
| 21.934 | :341 | HQD ME1/pipe0/queue1 VMID8 active 1, PQ_CONTROL `0xd0308909`, doorbell dword 26; fault status before the kick 0 |
| 21.936 | :343 | IH: VM fault IV, vmid 8, VA 0x100001000, src_data `0x00100001 0x50` (IH client 10 = GC VM fault) |
| 22.221 | :345-353 | boot queue test failed: fence 0, **data 0x600df00d landed**, fault status `0x00800880`; doorbell control `0x40000068` = **HIT clear, consumed**; ring wptr 13 = rptr `0xd` (whole stream consumed); GRBM `0xa800382c`, GRBM2 `0x30008000`, CPC `0xa0000041`, HQ_STATUS0 `0x40008040`, MEC pc `0x611` |
| 22.358 / 22.606 | :362, :366 | `HQD dequeue timed out (ACTIVE 1)` twice |
| 22.609 | :367 | `vm: boot self-test failed; per-client GPU VM disabled`, bring-up finished |
| 22.635-24.257 | :388-413 | cg default applied; survey: GRBM `0xa800382c`, CPC_STAT `0xa0000001`, CPF_STAT `0xb8008001`, **active HQDs 0/0 0/1**; 291 W -> 69 W at 3276 MHz, 100 % |
| later | | idle 75 W / 3422 MHz / 100 % (sensors rows) |

The WRITE_DATA (ring fetch, VMID 8 data page) worked; the fence page/EOP read faulted (CPF, instruction-fetch semantics needs EXEC). The queue
then could not be drained: the CPF was stalled on the faulting fetch. [I]

### 2.2 Round 6 boot 2 (`061829`) and boot 3 (`062351`) — boot self-test PASS, every client fails

- **Bring-up lines are gone** [M]: the dmesg section `=== dmesg: full RDNA4FB log ===` (:22) begins at 146.7 s and is cursor spam; the boot test, the cg
  sequence and the HQD lines are not in any round 6 log. The diagnostic script's `gfxcg SKIPPED: bring-up did not reach stage 7` (:539) is therefore an
  **artefact of the missing lines**, not evidence that clock gating was not applied. Forge's note 5 ("CG was NOT applied in the failing boot") is **wrong**
  [M/I]: the boot finished stage 7 (:197 "finished at stage 7", :200 "ready") and the GPU draws 76-81 W, not the 268-315 W of every ungated boot; the
  cg default applies last in `runStages` (compute.cpp:819).
- Pre-client idle baseline (:205-211): **3422 MHz, GFX activity 100 %, 76 W** then 3417 MHz / 80 W. 062351: 81 W. Non-VM gated boots of the same day: 382-394 MHz /
  42-45 W (`052606`, `053042`, `053620`, `053909`, `054257`, `061510`) [M].
- Client events, `061829` (dmesg tail; `062351` is the same pattern shifted by ~8 s):

| t (s) | Line | Event |
|---|---|---|
| 224.249 | :424 | vmid 8 client activated MEC1 pipe 0 **queue 1**, doorbell 26 |
| 224.394 | :425 | `queue 0/1 dequeue timeout (ACTIVE 1)` (145 ms after activation: 100 ms poll + ~45 ms of work) |
| 224.498 | | client closed |
| 224.668 | :428 | vmid 8 again, same slot |
| 225.833 | :429 | dequeue timeout (+1.17 s) |
| 226.112 / 226.116 | :431-432 | vmid 8 (0,1) and **vmid 9 (0,2)** activated together (the VM isolation/peer tests) |
| 228.121 | :433 | **HQD dump, vmid 9 (0,2): ACTIVE 1, VMID 9, PERSISTENT `0x0be05501`, PQ base `0:01000000` (VA 0x100000000), rptr 0, wptr `0x42`, doorbell control `0xc0000070`, PQ_CONTROL `0xd0300909`, dequeue 0** |
| 228.412 | :434 | vmid 9 dequeue timeout |
| 228.516-229.236 | :436-458 | clients that map/unmap host buffers only (rtWedged set by now: `rtDispatch` returns at runtime.cpp:2246 before any kick) — **each close still logs a dequeue timeout on (0,1)** |
| 229.43-233.9 | :459-472 | six more opens/closes (flip/anim commands) all end with `dequeue timeout` |

Decoded, all [M] unless tagged:
- vmid 9 dump: doorbell control `0xc0000070` = bit 31 **HIT set**, bit 30 EN, offset 0x70 bytes = dword 28 -> **the doorbell write reached this HQD** (wptr register = 0x42 = the
  value written, 66 dwords) **and the MEC never serviced it** (rptr 0, PQ_EMPTY clear). The round 5 queue-1 doorbell had been consumed (`0x40000068`, rptr `0xd`). The kext's own
  message text agrees with this reading (runtime.cpp:1013).
- **No GC hub fault and no IH "VM fault IV" line appears anywhere in `061829` or `062351` for VMID 8/9** (the IH was on, `rdna4-ih=2`; round 5 printed its IV at once). The only fault in
  round 6 is `062351:447`: status `0x00000d3d` = MORE_FAULTS 1, WALKER 6, PERMISSION 3, MAPPING_ERROR 1, **CID 6 = CPG** (`gfxhub_v12_0.c` client table), VMID 0, read, VA 0 — the gfx
  CP, only on the `rdna4-gfx=2` boot; already seen in round 4 (`gfxring.cpp:1224` comment) and unrelated to the compute failure (061829 fails identically without it).
- Dequeue timeouts on **never-kicked** queues (rows 228.5-233.9) mean the HQD cannot be drained even when it holds nothing. [I: "never kicked" follows from the
  `rtWedged` early return; I did not see a log line per operation.]
- **[U] an unexplained inconsistency**: `selftest: dispatch: device not responding` (:216) returned within ~145 ms, but `rtDispatch` only returns NotResponding when `rtWedged` was already
  set (runtime.cpp:2246) and `info` had just reported "ready, not WEDGED". Either an earlier client (before the log window) wedged the runtime, or the stderr/stdout
  interleaving hides an earlier command. A per-operation kext log line (entry state, `rtWedged`, fence, HQD rptr/HIT) removes this uncertainty for the next boot.

## 3. Boot self-test vs client path, statically (runtime.cpp, compute.cpp)

| Item | Boot test (`vmBootSelfTest`) | First client (`rtOpen` + `launch`) | Same? |
|---|---|---|---|
| VMID / pipe / queue / doorbell | 8 / 0 / 1 / 0x1a | 8 / 0 / 1 (skips (0,0)) / `(0x0d+slot)*2` = 26 | yes |
| `hqdInitFor(false, pipe, queue, vmid, mqd, eop>>8, pq>>8, rptr VA, wpoll VA, doorbell)` | yes | yes | yes |
| VA layout (pq, eop, rptr, wptr at +0x1000 steps) and pool slot `kVmQueueBase` | yes | yes (slot 0) | yes |
| PTE flags (R|W|X + IS_PTE on all leaves, FRAG 64K) | `vmMap` | `vmMap` | yes |
| `vmContextInit`, invalidate engine 17 before activation | yes | yes | yes |
| SH_MEM_CONFIG / BASES VMIDs 8-15 | set once, via GRBM bank select | boot value relied on; **rewritten per launch** with `grbmSelect(0, pipe, queue, vmid)` (compute.cpp:3101-3106); amdgpu selects `(0,0,0,vmid)` | values equal amdgpu's `DEFAULT_SH_MEM_CONFIG`/`init_compute_vmid` [M] |
| SPI_GDBG_PER_VMID_CNTL.TRAP_EN (amdgpu sets it for VMIDs 8-15, `gfx_v12_0.c:1795-1798`) | not set | not set | yes (neither) |
| Packets | WRITE_DATA + RELEASE_MEM | **ACQUIRE_MEM (GCR sync), 10+ SET_SH_REG, DISPATCH_DIRECT, RELEASE_MEM** | **NO** |
| Shader waves in VMID 8 | none | yes | **NO** |
| Boot-queue (0,0) state | active | active | yes |
| Time / power state | kext load, ~22 s, before `Flip::run`, before default clock gating | 150+ s later, after flip and cg | **NO** |
| Kicked after a dequeue of the same slot | first use | slot just dequeued by the boot test | NO (but (0,2) on a never-used slot failed too) |
| Dequeue sequence | DEQUEUE_REQ=1, poll ACTIVE, req=0 | identical (`rtRelease`, runtime.cpp:2939-2952) | yes |

## 4. Ranked hypotheses

### H1 — `vm=1` boot leaves the GC/MEC pipe busy and unserviceable (covers S-A, S-B, S-C) — most likely [I]
For: S-A exists in both VM boots, before any client, and never in a non-VM boot; identical pin in round 5 (queue 0/1 stuck, CPC busy) and round 6 (queue dequeued cleanly)
=> the pin does not need a stuck HQD. S-C: dequeue cannot complete even with nothing queued. S-B: vmid 9's kick is latched but not serviced, no fault logged.
Against: `inactive` was true at the end of the round 6 boot test (Results vm=PASS requires it) [M]; queue (0,1) was reached by the MEC at boot in rounds 2 (first-access fault), 5 (rptr advanced) and 6 (PASS) [M]; queue (0,0) works after the same cg sequence in non-VM boots [M].
What it could be [U]: any `vm=1` boot step: the `GRBM_GFX_CNTL` VMID bank writes for VMIDs 8-15, `GCVM_CONTEXT8` enable/disable, fault-default page state, the (0,1) activation itself.
Discriminate: (a) static — cannot. (b) emulator — only if it models GUI_ACTIVE/CPC busy: ask Kiln (§6). (c) Linux — not applicable (amdgpu never uses MMIO queues). **(d) boot 9: survey at 5 points** — before the boot test, right after the boot test, after the flip, after cg, after the first client open — each logging
`GRBM_STATUS/2`, `CPC_STAT/BUSY`, `CPF_STAT/BUSY`, `CP_STAT`, `RLC_GPM_STAT`, **all 8 HQD slots** (ACTIVE, VMID, rptr, wptr, DOORBELL_CONTROL incl. HIT, PQ_CONTROL), `RLC_CP_SCHEDULERS`, SMU activity. The first point where GRBM bit 31 sets names the step. Then bisect with one boot-arg per step (skip SH_MEM writes; skip the test's HQD; run the test on a VMID-0 queue = Forge T1/T3).

### H2 — the first packet stream with ACQUIRE_MEM/SET_SH_REG/DISPATCH in a non-zero VMID stalls the MEC pipe — second [I]
For: boot test never exercises it (Forge, and the code); first dispatch times out; a stalled queue 0/1 that cannot be drained would block the shared MEC pipe and explain why queue (0,2) (vmid 9) is never serviced and why every later dequeue fails; hung VMID-8 waves would also defeat DRAIN.
Against: S-A exists before any shader; the lead's replay ran the same IB/code in a per-process VMID (clears bytes, not our VMID/queue/SH_MEM setup); no VM fault at all, so not a page fault (unless retried silently: our CONTEXT_CNTL fault bits equal Linux's `0x03fffc07`, W42); vmid 9's rptr stayed 0 (the *first* packet, ACQUIRE_MEM, was not consumed — a stall inside a later packet would show rptr > 0 [I]; a pipe already blocked by queue 1 would not).
Discriminate: (d) **bisect the packet stream on a fresh queue**: WRITE_DATA; +ACQUIRE_MEM; +SET_SH_REGs; +DISPATCH of an `s_endpgm` shader (no memory access); +the vadd shader; +RELEASE_MEM — log rptr/HIT/CPC after each; stop at the first stall. Plus a **sibling probe**: after the stall, kick a WRITE_DATA on another queue of the same pipe and one on pipe 1: queue-local or pipe-wide wedge? Forge's T5/T5b cover vadd only as a whole; the bisect is the missing piece (§5).

### H3 — client MQD/HQD/doorbell setup differs from the boot test — low, mostly refuted [M]
Same function, same inputs (§3). Doorbell delivery proven by HIT/wptr (vmid 9 dump). PQ_CONTROL diff is PQ_EMPTY. MQD in memory is identical code. Residual: none found statically.

### H4 — slot reuse of (0,1) after the boot test's dequeue — low
Against: queue (0,2) on a never-used slot failed as well [M]. For: Forge's observation that client (0,1) = boot slot. Discriminate: Forge T2 (never-used (1,1), other pipe).

### H5 — SH_MEM/SPI per-VMID state — low-medium
Values match amdgpu [M]; never read back on the card [U]. `launch()` rewrites them with `grbmSelect(0, pipe, queue, vmid)` while amdgpu uses `(0,0,0,vmid)`: a bank select with ME 0 + queue 1 is harmless for VMID-banked registers **by the register model, not shown on the card** [U]. TRAP_EN: amdgpu enables it for 8-15 and leaves it off for VMIDs < 8 — the lead's replay ran in a gfx-range VMID. Discriminate: (d) read SH_MEM_CONFIG/BASES and SPI_GDBG_PER_VMID_CNTL for VMIDs 1-15 (Forge has it); **repeat the client queue with VMID 3** (gfx range, like the replay): if VMID 3 works and 8 not, it is per-VMID state.

### H6 — PDE/PTE flags, TLB — low
No hub fault anywhere in round 6. Flags equal Linux's leaves (W42 `vm-walk.txt`, `...5f1`). `rtFree` lacks an invalidate (W13 S0) — real for buffer reuse, not for the first dispatch. Code pages are mapped R|W|X (runtime.cpp:2204). Discriminate: Forge T6/T7 (expected faults), plus a read of `GCVM_L2_PROTECTION_FAULT_STATUS` after the first dispatch.

### H7 — clock-gating interplay (CGCG/CGLS and MEC queue wake) — low-medium
Cg is applied in the round 6 boots [I, from watts]. Against: pin is identical in round 5 where survey `cg before` already shows 100 % at 291 W *before* cg; non-VM gated boots are fine on queue (0,0). Discriminate (d): **boot 9 with `rdna4-gfxcg=0`** (was the pre-W29 behaviour: 300 W idle, desktop unaffected): if clients work with gating off, it is H7. Cheap and read-only in effect.

### H8 — dequeue semantics (DRAIN waits for resident waves; amdgpu may disable the doorbell first) — unknown [U]
`rtRelease` and the boot test use `DEQUEUE_REQ=1`, same as `gfx_v12_0_kiq_init_register` (gfx_v12_0.c:3328). For a hung queue DRAIN cannot finish; RESET_WAVES (2) exists (`recoverComputeQueue`). Discriminate: after a DRAIN timeout, try RESET_WAVES and log whether ACTIVE clears, and whether the next HQD on that pipe is serviced. Not a root cause by itself.

## 5. Answer to Forge (hub-task-317): probe coverage
Good coverage: T1/T2 (H4), T3 (VMID-0 queue with IB in VMID n = the replay done on the card, H2/H5 partially), T4 (client-style queue without shader), T6/T7 (H6), SH_MEM readback (H5), T0 (H1).
**Not covered:**
1. **H1 timing**: T0 is "before/after" the diagnostic; I need the survey at 5 points in the normal flow: before `vmBootSelfTest`, **right after it**, after `Flip::run`, after cg, after the first client open (the last needs a runtime hook), and "is something left active at idle" = all 8 HQD slots + `RLC_CP_SCHEDULERS`.
2. **H2 bisect**: T5b replays the whole `launch()` stream at once; add the incremental stream (WRITE_DATA -> +ACQUIRE_MEM -> +SET_SH_REG -> +DISPATCH(s_endpgm) -> +vadd -> +RELEASE_MEM) on one fresh HQD, rptr/HIT/CPC/fault after each, bounded, stop at first stall.
3. **Sibling probe** after a stall: WRITE_DATA on another queue of the same pipe and on pipe 1 (pipe-wide or queue-local wedge).
4. **VMID 3 vs 8** for the client-style queue (gfx-range like amdgpu's replay vs KFD range) and TRAP_EN readback.
5. **H8**: after the first DRAIN timeout try RESET_WAVES, log ACTIVE.
6. Per-operation kext log in `rtDispatch/rtSubmitIb/rtRelease` (entry `rtWedged`, rptr, HIT) — settles the §2.2 [U].
Most valuable single probe: **the packet-stream bisect on a fresh queue (2) together with the 5-point survey (1)**. Registers I want: at every point `GRBM_STATUS`,`GRBM_STATUS2`,`CPC_STAT`,`CPC_BUSY_STAT`,`CPF_STAT`,`CPF_BUSY_STAT`,`CP_STAT`,`RLC_GPM_STAT`,`RLC_CP_SCHEDULERS`, `CP_MEC_RS64_CNTL`, `CP_PQ_STATUS`, `CP_MEC_DOORBELL_RANGE_*`, and per HQD: `ACTIVE, VMID, PQ_RPTR, PQ_WPTR_LO/HI, PQ_DOORBELL_CONTROL (HIT = bit 31), PQ_CONTROL (PQ_EMPTY = bit 15), HQ_STATUS0, EOP_RPTR`. The HIT bit (`0x80000000`) means "a doorbell arrived that the MEC has not serviced" [I from the dump pair in rounds 5/6].

## 6. What I want from Kiln's Linux emulator loop (hub-task-319)
- Boots 1/2/3/8 equivalents with `rdna4-vm=1` then `rdna4-run selftest` and the VM tests: does the client failure reproduce? If the emulator passes where the card fails, list what the emulator does not model: per-pipe MEC serialization, dequeue with resident waves, GUI_ACTIVE/CPC busy, the first shader in a non-zero VMID.
- Does the emulator's `GRBM_STATUS` reflect a HQD left active / an unserviced doorbell (HIT)? A boot where the emulator is forced to leave HQD (0,1) active should reproduce S-A in the emulator, which would show whether the model can carry H1 at all.

## 7. Linux ground truth (c): what can and cannot be read
- Already captured read-only (`tools/linux-groundtruth.sh`, `docs/linux-ground-truth.md`): VM contexts 0-15, GC hub/L2, CG flags, CP gfx/HQD/RS64 and page-table walks (`vm-walk.txt`). `sudo bash tools/linux-groundtruth.sh compute` while a compute app (or `tools/linux-replay`) runs gives amdgpu's CG/MEC/HQD state for a busy compute queue. **This is a command for the user to run; I have not run it.**
- **Not available without writing a bank-select register**: `SH_MEM_CONFIG/BASES` and `SPI_GDBG_PER_VMID_CNTL` *per VMID* are banked by `GRBM_GFX_CNTL.VMID`. The debugfs `amdgpu_regs2` interface can select a bank (`AMDGPU_DEBUGFS_REGS2_IOC_SET_STATE`) and `umr` does it, but **umr is not installed here** and I have not tested the ioctl, so I give no command line I cannot vouch for. The values amdgpu programs are in source (`gfx_v12_0.c:1773-1846`) and equal ours for SH_MEM; reading them back on the card (Forge) is the cheaper route.
- amdgpu's MQD/HQD for a KIQ/compute ring on gfx12 (MES) are not MMIO-activated queues, so a Linux HQD dump says how amdgpu's queues look, not how ours must; it would only help H8 (dequeue sequencing) and H1 (which HQD registers a healthy pipe shows).

## 8. Proposed fix
**None proposed.** A fix now would be a guess dressed as a finding. The decision tree for after boot 9:
- survey shows GRBM busy right after the boot test -> fix inside the boot test's own teardown/setup (one of: SH_MEM writes, context disable, the (0,1) activation); bisect with arg-gated skips;
- survey clean until the first client; bisect stalls at ACQUIRE_MEM -> examine GCR fields (range/size, GL2 op) for a non-zero VMID; at SET_SH_REG/DISPATCH -> SH_MEM/SPI per-VMID (TRAP_EN); at the wave -> instruction fetch/aperture;
- works with `rdna4-gfxcg=0` -> H7: order/timing of cg vs MEC queues (move cg before the client path or hold off queues);
- works with VMID 3 -> per-VMID state for 8-15 (TRAP_EN, SH_MEM).

## 9. Real-card boot 9 (proposal; the lead approves)
Args: round 6 boot 2's (`rdna4-vm=1 rdna4-flip=1 rdna4-hang=1 rdna4-ih=2`, **without** vbl/cursor so the dmesg window keeps bring-up) `+ rdna4-compute=1` (keeps the compute lines), `rdna4-gfxpm=24` + the 5-point survey above, Forge's `rdna4-vmid-test=1` probes last. Read-only except Forge's own queues on unused slots; no shader beyond `s_endpgm`/vadd. Power-cycle after. Also run once with `rdna4-gfxcg=0` (H7) only if the survey does not already name the step.
Expected lines: `pm: survey <point>: GRBM ... CPC_STAT ... HQD 0/0..1/3 ...` at each point; `vm: boot ...` lines present; `runtime: vmid N ... op <name> wedged=<0|1> rptr ...`.

## 10. What I did not do
No kext code, no GPU runs, no SSH to the card, no write to the stick, no sudo. I did not read all ~40 VM logs line by line: rounds 2-4 were classified by their results rows only (§1); the round 2 fault is in `vmfix2-findings.md`. I could not reconstruct §2.2's first client exactly ([U]).
