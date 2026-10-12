# Why per-client GPU VM work fails on the real card (hub-task-311)

Author: Anvil, branch `premetal/power`, 2026-09-30. **No kext code was changed and no GPU was run for this document.**
Evidence tags: **[M]** measured (log file:line, or a header/source line I read), **[I]** inferred, **[U]** unknown.
Logs: `/run/media/miguer/OPENCORE/rdna4fb-diag-2026093*.txt` and `old-logs/` (read-only). "053316" = round 5 boot 2, "061829" = round 6 boot 2,
"062351" = round 6 boot 3 (same args plus `rdna4-gfx=2`).

## Erratum (found while defining boot 9, `docs/boot9-vm-diagnostic.md`)

**The round 6 VM boots carried `rdna4-vm-diag=4065`** (real boot-args line: `061829` line 17). The diagnostic script's `active:` list (line 10) does not list `vm-diag`, so this document
treated them as plain `rdna4-vm=1` boots. Bit 512 of that mask ("F": the GC hub's fault default page pointed at a system page, `GCVM_L2_CNTL` default-page-out-to-system set, restored
after the boot test) is active **even when the baseline passes**; the other diagnostic bits run only after a failure [M, code]. Consequences for what is written below:
- §0 item 2 and H1 ("the pin appears in every `vm=1` boot") are **confounded with F**: the round 5 and 6 VM boots all had it; no plain VM boot has been measured with clock gating on. New hypothesis **H9**: F's window or restore leaves the GC L2/hub in a state that stalls MEC service. Discriminator: boot 9 (plain, no vm-diag) vs boot 9b (round 6's exact arguments).
- "Client path == boot test" (§3) still holds for the registers compared; the boot test of round 6 was, however, the F-instrumented one.
- §9 said `+ rdna4-compute=1`: wrong. `rdna4-compute=1` stops bring-up at the survey stage; the boot keeps `rdna4-compute=7`.
The final definition of the boots is `docs/boot9-vm-diagnostic.md` on branch `premetal/vmdiag`.

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

## 11. 2026-10-06: what the Vulkan work adds (Sunneva; card evidence from another rig, Big Sur 11.6.6)

Not a root cause. New measurements that narrow where to look, and a change of default that follows from them.

**Measured on the card** (`docs/vulkan-port.md` sections 8 to 10, `docs/todo-vulkantest.md`; logs
`rdna4fb-diag-20261006-210519`, `-214537`, `-223154`), all in boots with `rdna4-compute=7 rdna4-gfx=2` and **no**
`rdna4-vm`:

- Idle after the bring-up and clock gating: 786 MHz, 3 %, 19 W (`gfxcg` row). No S-A.
- VM context 8 programmed by `vmContextInit` exactly as for a client (CNTL, table base, range), tables written by the
  CPU through the BAR in the pool, `flushHdp` and `vmInvalidate` after every change.
- Work in address space 8 from queues that are themselves in address space 0: SDMA `INDIRECT` with VMID 8 (three
  256 KiB copies, VRAM behind the BAR, VRAM past it and system memory, at VAs 0x100000000, 0x7ffe00000000 and
  0xffff800000200000, patterns checked), and the graphics ring's `INDIRECT_BUFFER` with VMID 8 (RADV: a CP DMA fill, a
  compute-shader fill, a draw with NGG vertex and pixel shaders read back exactly, 300 presented frames).
- So on this card, without MES: the GC hub's context for a non-zero VMID, its walker, its TLB invalidation and shader
  waves in a non-zero VMID (compute and graphics, dispatched from the graphics ring) all work. H5 (per-VMID shader
  memory state) and the walker-side hypotheses are much weaker for it: `SH_MEM_CONFIG/BASES` for VMID 8 were written
  once before each submission and that was enough.

**What those boots never did**: build an HQD with a non-zero `CP_HQD_VMID`. Every failing boot did, at least once, in
`vmBootSelfTest` (queue (0,1), VMID 8), before any client. That lines up with S-A appearing before any client in every
`rdna4-vm=1` boot, round 5 (test failed) and round 6 (test passed and dequeued) alike, and with section 8's first branch
("survey shows GRBM busy right after the boot test -> ... the (0,1) activation"). **H10**: an MMIO-loaded MEC queue with a
non-zero VMID is itself what leaves the MEC pinned and later queues unserviced; nothing downstream (tables, SH_MEM,
shader) is wrong. amdgpu has no such queue on gfx12 either (KFD's go through MES). Untested: nothing here ran a boot
with such a queue on this rig, and nothing here ran a kernel **MEC** queue with a packet VMID (the copy engine and the
graphics ring are other engines).

**The change (`devel/address-space`)**: `rdna4-vm=1` now means the shared kernel queues (`rdna4-vmshared` defaults to 1;
0 gives the old path), and in that mode the boot proof is `vmSharedBootTest` (`src/vmshared.cpp`): a client opened like
any other, two command buffers in its address space submitted through the shared queue with the same code `SubmitIb`
uses (`submitIbLocked`), each writing two words through its tables. No queue is built inside an address space anywhere
on that path. This is W13's step S1 and the old boot 12, made the default. `vmBootSelfTest` and its diagnostics are
unchanged and run only with `rdna4-vmshared=0`.

**The card test that discriminates** (`docs/todo-vmtest.md`): boot A, `rdna4-vm=1` as it now is: if H10 holds, idle is
not pinned, the boot test passes and `rdna4-run selftest` passes on the shared queues. Boot B, the same with
`rdna4-vmshared=0`: the old path on the same rig and day; S-A and the client failures should reappear. A passes and B
fails: H10 stands and the old path can go. Both fail: the kernel compute queue with a packet VMID (U1) is the next
suspect, and the surveys of boot A say where it stops.

## 12. 2026-10-07: the shared default on the card (Sunneva's rig, log `rdna4fb-diag-20261007-130428`)

Kext `52493F80`, boot-args `rdna4-compute=7 rdna4-vm=1 rdna4-gfx=2 rdna4-gfxclient=1 rdna4-hang=1 rdna4-trace=1`
(shared mode by default; no `rdna4-vm-diag`, no `rdna4-ih`). All [M] from that log.

**What works**

- `vmshared: shared queue 0: MEC1 pipe 0 queue 2, VMID 0, doorbell dword 74: active`, queue 1 likewise.
- `vmshared: boot test: address space 8 from shared queue 0 ...: job 1 submit 0x0 fence reached data ok (1029 us), job 2
  ... (1060 us): PASS`. **U1 is measured for a kernel MEC queue**: it runs a command buffer in the address space its
  packet names.
- `gfx client self-test ...: PASS`: a runtime client's command buffer on the graphics ring in address space 8 (W12k), first
  time on a card.
- **No S-A**: with `rdna4-vm=1`, idle before any client is 3 % at 789 MHz, 18 W (`sensors-idle`), where every earlier
  `rdna4-vm=1` boot read 100 % and 75-81 W. No queue was built inside an address space in this boot. H10 has its first
  half; the control (the same boot with `rdna4-vmshared=0`) has not been logged.
- A real kernel in a client's address space on the shared queue: `ok zero-copy vadd: 65536 items read/written through CPU
  pointers` (client in address space 8, host buffers, fences 1 and 2 reached).

**What fails, and it is one thing**: the two tests that fault on purpose.

| t (s) | Event |
|---|---|
| 113.27 | isolation test: the client in address space 9 (shared queue 1) runs `copy` reading an address only client 8 has mapped |
| 115.27 | `the kernel's fence never came`; fault status `0x0090113d` = VMID 9, CID 8, read, VA 0x100030000, MAPPING_ERROR; HQD rptr = wptr = 0xc (the ring was consumed); `GRBM 0xa840382c`, `CPC_BUSY 0x00000810` |
| 115.48 | `shared queue 1 recovery failed; that queue stays wedged`: the dequeue with RESET_WAVES went through (the HQD is initialised again 0.5 ms later), the WRITE_DATA proof on the fresh queue did not land in 200 ms |
| 115.59 | client 8 (queue 0): the zero-copy vadd passes |
| 115.60 | `dispatch through freed host VA`: a host buffer unmapped, then a dispatch that writes to it |
| 116.60 | fence 3 never came; fault status `0x0084115d` = VMID 8, CID 8, **write**, VA 0x100116000 (inside the freed buffer) |
| 116.81 | `shared queue 0 recovery failed` |
| after | every later test and every later open: `device not responding` / `unsupported function` (both queues wedged) |

So `runtime`, `submitib`, `fault`, `vm` and `gfx-app-tri` are FAIL in the table because of two faulting jobs and what
follows them, not because clients cannot run. In the emulator a faulting access is answered from the default page and
the job finishes; on the card the wave that faulted never finished, and the queue's pipe would not run a fresh queue
afterwards. The recovery's own lines (`vmshared:`) were not in the log: the script's sections after the tests did not
collect that prefix (fixed).

**The difference from Linux that concerns faults.** The hub's bring-up points the L2 fault default at a scratch page in
VRAM with `ENABLE_DEFAULT_PAGE_OUT_TO_SYSTEM_MEMORY` off; amdgpu points it at a page of system memory with the bit on.
The kext's own `linux diff` prints this on every boot (`GCVM_L2_CNTL ours 0x00080601 linux 0x00080e01`), and diagnostic F
of `vmBootSelfTest` was written to try it. Whether the hub honours a default page in VRAM at all is not known. **H11**:
it does not, so the faulting access is retried or dropped and its wave never ends.

**The change**: `faultPageToSystem` (`src/runtime.cpp`), called once DMA is up and again after a wake: one wired page of
host memory, its bus address in `GCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR`, the bit on in `GCVM_L2_CNTL`, for the whole
boot and for every address space, as amdgpu has it. `scrubFaultPage` clears it too. `rdna4-faultpage=0` keeps the VRAM
page. Compile-checked; `docs/todo-vmtest.md` has the run that tells: the same boot again, and the two fault tests either
pass or fail as before.

## 13. 2026-10-07, second run: H11 is out; what Linux does with such a fault

Log `rdna4fb-diag-20261007-131756`, kext `6F7D19C7`, the same boot-args. [M]:

- `runtime: fault default page: a page of system memory at bus 0x686863000, GCVM_L2_CNTL 0x00080601 -> 0x00080e01`:
  the hub's fault setup now equals Linux's.
- Everything that passed in section 12 passes again (boot test, `gfx-client`, zero-copy vadd, idle 3 % / 18 W).
- **The two fault tests hang exactly as before**: `0x0090113d` (VMID 9, read, VA 0x100030000), then `0x0084115d` (VMID 8,
  write, VA 0x100112000). The recovery's lines are in the log this time: `recovering shared queue 1 (guilty VMID 9)`,
  the queue initialised again, `shared queue 1 NOT recovered: wedged` 200 ms later.

So where the default page lives is not what decides it. **H11 is refuted.**

**What amdgpu does** (read in the kernel's source, `amdgpu_vm.c` and `gmc_v12_0.c`, 2026-10-07): a fault the card
reports as a *retry* fault goes to `amdgpu_vm_handle_fault`, which **writes a page-table entry for the faulting
address**. For a graphics context: "Redirect the access to the dummy page", readable, writable, executable. For a
compute context: an invalid flag combination chosen "to force a no-retry-fault". And otherwise: "Let the hw retry
silently on the PTE". The default page of the hub's registers plays no part in it. A wave that keeps retrying on a page
with no translation is this card's normal behaviour, and it is the driver's page-table update that lets it finish. The
emulator answers such an access from the default page and lets the job end, which is why the tests pass there.
**H12**: that is all that is missing here.

**The change**: `vmRedirectFault` / `vmEndRedirects` (`src/runtime.cpp`). The loops that wait for a client's job (the
dispatch, `WaitFence`, the graphics wait) look at the hub's fault status once a millisecond after their first 2 ms. A
latched fault for a client's address space, on a page with no translation, not from the command processor's own
fetches: the fault page is mapped at that address in the client's table, the fault cleared, the address space's cache
flushed; up to 96 pages a job. When the wait ends the pages are unmapped again and the fault page cleared. Compile-checked.
Limits: only the polled waits do this (not with `rdna4-ih`, where the wait sleeps in a function that cannot update a
table), and the Vulkan interface's clients do not have it yet.

Not looked at further: why the recovery of a queue whose wave is stuck does not bring the queue back. If H12 holds the
tests no longer need it; a shader that loops for ever still would.

## 14. 2026-10-07, third run: the redirect runs and the job is still dead

Log `rdna4fb-diag-20261007-132834`, kext `8CAB1DCC`, the same boot-args. [M]:

| t (s) | Event |
|---|---|
| 110.376 | isolation test: `copy` loaded for the client in address space 9 |
| 110.382 | `vmid 9: a shader touched 0x100030000, which is not mapped (fault status 0x0090113d): the dummy page answers there` (5.6 ms after the load: the redirect works as written) |
| 110.38 - 112.38 | nothing: no further fault, no fence |
| 112.379 | `1 unmapped page(s) were answered from the dummy page`; `the kernel's fence never came`; fault status 0 |
| 112.72 | freed-buffer test, address space 8: one page redirected (0x100110000) of the 64 the kernel writes; then the same silence for a second |

So after the page is there, **nothing tries the access again, and no other wave of the job faults either**, although 63
more unmapped pages lay in the freed buffer. The job is not retrying: it is dead from the first fault. H12 as stated is
refuted for this configuration.

That fits what the context register asks for. `vmContextInit` writes `GCVM_CONTEXTn_CNTL` as amdgpu has it on this card
(0x03fffc07): `RETRY_PERMISSION_OR_INVALID_PAGE_FAULT` (bit 8 on GC 12; this section first said 7, see section 15) is 0, amdgpu's "Send no-retry XNACK on fault to suppress
VM fault storm". A fault is then final. `amdgpu_vm_handle_fault` only ever sees faults the card reports as **retry**
faults; the ones Linux prints are the final kind, and what follows them on Linux is a ring timeout and a queue reset
through MES. This kext has neither MES nor a recovery that brings the queue back after such a fault (section 12).

Two ways out, and they are different work:

1. **Make the fault retryable** (the retry bit = 1) for the runtime's clients, so that the access waits for the page and
   `vmRedirectFault` has something to answer. This is the model amdgpu uses where it does not set no-retry. **Done as an
   experiment**: `vmContextInit` sets the bit when `rdna4-vm` is on; `rdna4-vmretry=0` gives the old value. Not for the
   Vulkan interface's address space. Compile-checked.
2. **Recover the queue after a final fault.** Needed anyway for a shader that never ends, and not understood: the
   dequeue with RESET_WAVES succeeds, the queue is initialised again, and the first packet on it does not run
   (`CPC_BUSY 0x00000810` / `0x00000481`, `GRBM2 0x34110000` / `0x30110000` at that point).

**H13**: with the retry bit set, the faulting access is tried again after the redirect and both tests pass. If the job is dead
all the same, way 2 is what is left.

## 15. 2026-10-07, fourth run: the wrong bit (Sunneva's mistake in section 14's change), and what it showed

Log `rdna4fb-diag-20261007-133727`, kext `2CA51B3E`. That kext set **bit 7** of `GCVM_CONTEXTn_CNTL`. On GC 12 that
is not the retry bit: `gc_12_0_0_sh_mask.h` has `PAGE_TABLE_DEPTH` at [2:1], `PAGE_TABLE_BLOCK_SIZE` at **[7:4]**,
`RETRY_PERMISSION_OR_INVALID_PAGE_FAULT` at **bit 8** and `RETRY_OTHER_FAULT` at bit 9 (read in the header, 2026-10-07).
Bit 7 is the top of the block size, as on older hubs it was the retry bit. `vmContextInit`'s own `<< 4` for the block size
and the sixteen fault-enable bits at 10..25 in Linux's 0x03fffc07 both said so; the change did not look. H13 was **not
tested** by this run.

What the run did, [M]: with a block size of 8 the walker failed on a page that is mapped.

- `boot test: ... job 1 submit 0x0 fence NOT reached data WRONG (2057525 us) ...: FAIL`; fault status `0x008009b6`
  = VMID 8, **CID 4 (CPF)**, read, VA 0x100006000, the client's own command page, 2 ms after the submit.
- The queue: rptr 0, wptr 0x14, doorbell consumed; `recovering shared queue 0`, `NOT recovered: wedged`.
- `vm: boot self-test failed; per-client GPU VM disabled`. The runtime then ran without address spaces and its whole
  self-test and the benchmarks passed (`selftest: PASS`, `bench: PASS`: the first log of that from this rig).
- **`idle-pin FAIL: PINNED: GFX activity 100 %, 66 W`**, from the failed boot test on.

That last line is worth keeping: a command processor stalled on a fetch it cannot translate, in a non-zero address
space, is enough to produce S-A, with no queue built inside an address space. It weakens H10 as the *only* way to S-A:
the September boots may have been pinned by a stalled fetch (round 5's boot test failed on exactly that, section 2.1)
as much as by the queue's existence. What stands from sections 12 to 14 is unchanged: in shared mode with correct
tables there is no S-A and clients run.

**The change**: the bit is 8 (`kVmCtxRetryFault`), kext rebuilt. H13 is now what the next run tests.

## 16. 2026-10-07, fifth run: H13 is out too. A fault ends the job; the question was always the reset

Log `rdna4fb-diag-20261007-134456`, kext `E71A9E01` (the retry bit at bit 8, the redirect in place). [M]:

- The results of sections 12 and 13 are back: `boot test ... PASS` (1026 us a job), `gfx-client PASS`, idle 3 % / 18 W.
- The fault tests: exactly as the third run. `a shader touched 0x100030000 ...` 5.8 ms after the load, then two seconds of
  nothing; one page of 64 in the write test.

So with the retry bit set as well, nothing tries the access again. **H13 is refuted.** Three ways of letting a faulting
job finish (the default page as Linux has it, a page-table entry at the address, the retry bit) have each run on the
card and none did anything. On this card, as set up here and as amdgpu sets it up, **a shader fault ends its job**. The
self-test's expectation that the job finishes comes from the emulator, which answers the access from the default page.

What amdgpu does with such a job is reset the queue, and on this chip it has a reset that needs no firmware scheduler,
which this repository's notes say does not exist ("no source-backed no-MES queue reset"). `mes_v12_0_reset_queue_mmio`
(`mes_v12_0.c`, read 2026-10-07), inside RLC safe mode:

| Queue | What it writes |
|---|---|
| compute | the queue selected; `CP_HQD_DEQUEUE_REQUEST` = 2; **`SPI_COMPUTE_QUEUE_RESET` = 1**; wait for `CP_HQD_ACTIVE` bit 0 to clear |
| graphics | `GRBM_GFX_INDEX` to broadcast; `CP_VMID_RESET` with `RESET_REQUEST` = 1 << vmid and the pipe's queue bit; wait for `CP_GFX_HQD_ACTIVE` |
| copy engine | `SDMA0_QUEUE_RESET_REQ` = 1 << queue; wait for the bit to clear |

`recoverSharedQueue` did the first with `SQ_CMD` (kill the guilty address space's waves) where Linux has the SPI reset,
and outside safe mode. On the card the queue dequeued and came up again and its first packet never ran: the waves of the
dead job were still there. Register addresses from `gc_12_0_0_offset.h`: `SPI_COMPUTE_QUEUE_RESET` 0x1f73, `CP_VMID_RESET`
0x1e53, `SDMA0_QUEUE_RESET_REQ` 0x006c, all BASE_IDX 0 (the same header gives 0x1fc1, 0x1fab and 0x111b for the three
registers this kext already had, which agree).

**The change** (`devel/address-space`, compile-checked):

- **Out**: `vmRedirectFault` / `vmEndRedirects` and the retry bit. The fault page in system memory stays (it is Linux's
  setting and does no harm).
- **`recoverSharedQueue`** is amdgpu's compute-queue reset: safe mode, `CP_HQD_DEQUEUE_REQUEST` = 2,
  `SPI_COMPUTE_QUEUE_RESET` = 1, wait, safe mode off; then the HQD again and the WRITE_DATA proof, as before.
- **`vmJobFaulted`**: the polled waits for a client's job stop as soon as the hub has latched a fault for the client's
  address space (after their first 2 ms), instead of running out the timeout. The queue is reset and the call answers
  `kIOReturnVMError`, not a timeout.
- **`rdna4-run`**'s isolation test takes that answer as what it is, the expected outcome, and still checks that none of
  the other client's data arrived. The freed-buffer test already accepted any answer but a timeout.

**H14**: with the SPI reset the queue runs again after a dead job, so a fault costs the faulting client its job and
nobody else anything. What the next run shows: `vmshared: runtime: shared queue N reset (RLC safe mode acknowledged):
inactive`, then `shared queue N recovered (WRITE_DATA proof landed)`.

**For later, not done here**: the graphics ring and the copy engine have the same hole. Lost work on the graphics ring
halts it until the next boot (`gfxClientWedge`, also the Vulkan interface's lost-work rule), and a copy that does not
finish leaves the copy engine stopped. Linux's two other branches above are the way out of both.

## 17. 2026-10-07, sixth run: faults are caught and reported; the queue still does not come back

Log `rdna4fb-diag-20261007-135449`, kext `61554AD5`, new `rdna4-run`. [M]:

- `ok  VM isolation: client B could not read client A's VA (the job faulted and was stopped)` and `ok    dispatch through
  freed host VA faulted cleanly ((iokit/common) misc. VM failure)`: **both fault tests pass.** The kext sees the fault
  2.8 ms after the job starts (`dispatch ended by a fault in address space 9 after 2852 us`) and answers
  `kIOReturnVMError`.
- The reset: `shared queue 1 reset (RLC safe mode acknowledged): inactive` within 0.2 ms, the HQD initialised again, and
  `shared queue 1 NOT recovered: wedged` 200 ms later. The same on queue 0. **H14 is refuted**: amdgpu's MMIO reset
  for a compute queue takes the queue down cleanly here, and the fresh queue still runs nothing.
- Everything after the faults on those queues fails as before (`VM peer`, the `SubmitIb` tests, later opens).

One thing was wrong in section 16: it called a reset of the whole pipe "Linux's fallback". The function I had in mind
(`gfx_v12_reset_compute_pipe`) is not in the current `gfx_v12_0.c`, and today's `amdgpu_gfx_reset_mes_compute` does its
work through MES (suspend all queues, have the firmware name the hung ones, resume). The MMIO queue reset of section 16
is real and was read; in Linux it runs inside that MES sequence, which is not here.

What the dumps of the failed recoveries have in common, across all runs: on pipe 1 the MEC's instruction pointer reads
0x3fb5 before the reset and again when the proof has failed; `CPC_BUSY` 0x810 (pipe 1) and 0x480/0x481 (pipe 0).

**The change** (kext `F15C3784`, compile-checked):

- `recoverSharedQueue` has a second stage when the proof fails: the queue's state is logged, then **that one pipe of the
  MEC is restarted** the way the bring-up starts all four (`mecStart`: its reset bit in `CP_MEC_RS64_CNTL` pulsed, in
  safe mode), the queue initialised once more and the proof tried again; the instruction pointer before and after and
  the queue's state after are logged. This is an experiment from the observation above, not a sequence read anywhere.
- `rtOpenShared` puts a new client on the other shared queue when its own is out of service, so that one dead job
  costs one queue and not the runtime.

Whatever the second stage does, the next log has the state of a fresh queue that will not run, which no log so far has.

## 18. 2026-10-07, seventh run: the pipe restart does nothing; what the fresh queue looks like

Log `rdna4fb-diag-20261007-140244`, kext `F15C3784`. [M] Both fault tests `ok` again; the second stage ran and changed
nothing: `MEC pipe 1 restarted (RLC safe mode acknowledged), CP_MEC_RS64_CNTL 0x3c000000 -> 0x3c000000, instruction
pointer 0x5044 -> 0x5044`, then `NOT recovered`; the same on pipe 0. The pipe restart is taken out again.

The three dumps of shared queue 1, bit names from `gc_12_0_0_sh_mask.h` (read for this section, not from memory):

| | before anything | after the reset and a fresh `hqdInitFor` | after the restart and a second `hqdInitFor` |
|---|---|---|---|
| `GRBM_STATUS` | 0xa840382c (bit 22, SPI busy) | 0xa800382c | 0xa800382c |
| `CP_CPC_STATUS` | 0x80000001: MEC1 | 0xa0000041: MEC1, ROQ1, CPG_CPC | 0xa0000041 |
| `CP_CPC_BUSY_STAT` | 0x810: MEC1 EOP queue, pipe 1 | 0x001: MEC1 load | 0x808: MEC1 message, pipe 1 |
| read / write pointer | 0xc / 0xc | 0 / 0 | 0 / 0 |
| `CP_HQD_PQ_DOORBELL_CONTROL` | 0xc0000130 (EN, HIT) | 0x00000130 | 0x00000130 |
| `CP_HQD_HQ_STATUS0` | 0x40008040 (idle, slot connected) | 0x40000040 | 0x40000040 |
| `CP_HQD_EOP_RPTR` | 0x40000000 | 0 | 0 |
| MEC instruction pointer | 0x3fb5 | 0x5044 | 0x5044 |

What that says:

- **Before the reset the queue itself is in order.** The CP has fetched everything (read pointer at the write pointer),
  the doorbell is enabled, and the dequeue request is answered within 150 us. What hangs is the shader's waves: SPI
  busy, and the MEC waiting for an end-of-pipe that does not come.
- **The queue made afresh is what does not work.** The doorbell enable `hqdInitFor` writes (twice) reads back off, so
  the kick never arrives and the write pointer stays 0. The second `hqdInitFor`, 200 ms later, found the queue active
  and asked it to drain; it took 140 ms where the first took 0.3 ms, which is the drain loop running to its 100 ms
  limit: a dequeue request to the fresh queue is not answered. So after amdgpu's reset the pipe's firmware no longer
  serves that queue by registers. Under Linux the next step is the MES mapping the queue again, and there is no MES
  here.
- The reset does remove the waves (SPI busy is gone after it).

H15, from the first two points: **reset the waves and leave the queue alone.** An experiment; amdgpu never writes
`SPI_COMPUTE_QUEUE_RESET` without the dequeue request.

**The change** (kext `8C8B6470`, compile-checked): `recoverSharedQueue` first writes `SPI_COMPUTE_QUEUE_RESET` alone (in
safe mode, the queue selected) and sends the proof packet through the queue as it is: `waves reset, the queue left as
it was (...): it runs` or `it does not run`. Only if it does not run comes amdgpu's reset and the fresh queue as
before, now with the doorbell enable read back at once and a millisecond later (`made again: doorbell control ...`),
which says whether the write is ignored or undone.

## 19. 2026-10-07, eighth run: H15 holds. The queue comes back, and the whole self-test passes

Log `rdna4fb-diag-20261007-141737`, kext `8C8B6470`, boot C's arguments. [M]

```
runtime: recovering shared queue 1 (guilty VMID 9) without a GPU reset
runtime: shared queue 1: waves reset, the queue left as it was (RLC safe mode acknowledged): it runs
runtime: shared queue 1 recovered (WRITE_DATA proof landed)
runtime: dispatch ended by a fault in address space 9 after 2806 us; shared queue 1 recovered without a GPU reset
```

The same on queue 0 for the freed-buffer fault (address space 8, 2955 us). From the start of recovery to the proof
packet having run: 1.1 ms each time. The full reset behind it never ran.

After it, every test has a result of its own and all are `ok`: isolation, two clients at once, zero-copy vadd, the
freed-buffer fault, the three `SubmitIb` tests, the refusals, the benchmarks. The table: `runtime PASS`, `submitib
PASS`, `fault PASS`, `vm PASS`, `gfx-client PASS`, `gfx-app-tri PASS` (8192 px), `gfx-app-tricol PASS` (the colour
triangle through the runtime, its first pass on the card), `idle-pin PASS` and `post-idle PASS` at 3 % and 18 W.

So, for a compute job whose shader touches an address that is not mapped, on this card without MES:

1. the fault shows in `GCVM_L2_PROTECTION_FAULT_STATUS` with the job's VMID within 3 ms, and that ends the wait;
2. the job cannot be made to finish (sections 13 to 16);
3. `SPI_COMPUTE_QUEUE_RESET = 1` with the queue selected, in RLC safe mode, removes its waves, and the queue, which was
   never stuck itself, carries on;
4. the dequeue request amdgpu sends with that write must **not** be sent: it takes the queue down, and nothing here
   can bring one up again afterwards (sections 17 and 18).

**After the run** the full reset and the remade queue are removed from `recoverSharedQueue` (they did not run in this
log and never recovered a queue in the two before). The kext that ran is `8C8B6470`; the one with that removal is
`39B99DC7`, which ran the same boot right after (log `rdna4fb-diag-20261007-142500`) with the same result: both
queues recovered (`it runs`), every test `ok`, the same rows at PASS, idle 3 % and 18 W.

Not covered by any of this: a queue hung by something other than a shader's waves (a command buffer the CP cannot
fetch), more than one job in the ring when the fault comes, the graphics ring and the copy engine, and the old path
(`rdna4-vmshared=0`), whose control boot is still owed.

## 20. 2026-10-07, the control boot: the old path fails on the same rig, kext and day, and H10 is corrected

Log `rdna4fb-diag-20261007-143954`, kext `1D3AE3CC` (the one of the tenth run), the passing boot's arguments with
`rdna4-vmshared=0` and `rdna4-vmid-test=6` (surveys only). [M]

- The old boot self-test **passes** (`"vm"="PASS boot self-test"`): a queue built inside address space 8 (MEC1 pipe 0
  queue 1) runs its job, and is dequeued cleanly afterwards (`HQD 0/1 ACTIVE 0`, no timeout line).
- **From that point the GPU is pinned**: `survey before vmBootSelfTest: GRBM 0x0000382c`, `after vmBootSelfTest: GRBM
  0xa800382c`, `GRBM_STATUS2 0x10008000` (CPF busy, UTCL2 busy), `CPF 0x90000001`, `L2_BUSY 1`, SMU 100 % activity; it
  stays so through every later survey point. `idle-pin FAIL` at 100 % and 70 W, `post-idle FAIL` the same.
- The first client then loads the same HQD again: `ACTIVE 1 VMID 8 rptr 0 wptr 0 doorbell 0x00000068 HQ_STATUS0
  0x40000040 EOP_RPTR 0x00000000`. The doorbell enable is off. It never fetches, and its release ends in `dequeue
  timeout`; 56 such lines in the log. `runtime`, `submitib`, `fault` and `vm` FAIL.
- The graphics ring is not affected: `gfx`, `gfx-client`, `gfx-app-tri` and `gfx-app-tricol` PASS in this boot too.

That is the September failure, reproduced with one argument changed. And the state of the client's queue is, register
for register, the state of the remade queue of the seventh run (section 18): doorbell enable off although written,
`CP_HQD_EOP_RPTR` 0, `CP_HQD_HQ_STATUS0` 0x40000040, write pointer 0, a dequeue request that is never answered,
`GRBM_STATUS` 0xa800382c. That queue was in address space **0**.

So H10 as written in section 11 ("a queue inside a non-zero address space is what pins the engine") is not right. A
queue inside address space 8 ran its job in this very boot. What the two failures share is the other thing: **a
compute queue was dequeued and then loaded again through its registers.** Read that way:

- the old path dequeues at the end of its boot self-test and at every client's close, and loads a queue at every
  open; after the first dequeue nothing loaded by registers runs;
- amdgpu's reset dequeues, and has the MES load the queue again; with no MES the load by registers does nothing
  (sections 17 and 18);
- the shared path loads its two queues once at boot and never dequeues them, and the recovery of section 19 works
  because it leaves the dequeue out.

This is an inference from two cases with one signature, not something measured in isolation. The boot that would
measure it: load a kernel queue in address space 0, dequeue it, load it again, and send one packet. Nothing needs it
now. One place does dequeue and reload the shared queues: the runtime's sleep path. Whether that survives is not
known; compute across sleep has not been tried on the card with `rdna4-vm=1`.

**Consequence:** the old path (`rdna4-vmshared=0`: `vmBootSelfTest`, a queue per client) cannot work on this card
without MES, and the shared path is verified. The old path can go.

