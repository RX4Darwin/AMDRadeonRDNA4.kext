# Power after graphics work: idle, GFXOFF, sleep/wake with the gfx ring and clients (design, phase 1)

2026-09-30, branch `premetal/power` (from `premetal/w12k` fdf174a + f43d3c9). **Design only: no kext code, no GPU runs.** Everything is
labelled **[M]** measured (with a file:line), **[I]** inferred (reasoning from code/docs, not observed), **[U]** unknown (needs the card). Log
citations are to `docs/hw-logs/2026-09-30-power-evidence/idle-power-by-boot.txt` (first-occurrence extracts, same file:line numbering as
the originals on the OPENCORE stick, `rdna4fb-diag-20260930-<time>.txt`); source citations are to this tree; amdgpu citations are to
`~/work/tools/linux-amdgpu/src/drivers/gpu/drm/amd/amdgpu` (7.2.2, read-only).

## 0. Summary

1. **The premise needs correcting.** The "~100 % GFX / ~81 W that later runtime clients fail against" is **not caused by the gfx ring or the probe
   ladder** in any log I can see: it is present in **every `rdna4-vm=1` boot, including boot 2 without gfx**, from the first sample after bring-up and
   before any client ran, and absent from every non-VM boot, including the one that had the gfx ring up and ran the whole probe ladder (20 W). [M] §1.
2. **The ring itself costs nothing measurable at idle** [M] (≤ 1 W, 3 % activity: boot 3 of round 5, `053620`:661). The kext never parked the ring on the
   card (`park:` lines exist in no log) [M], so `gfxPark` has no data behind it at all.
3. **Clock gating is the big idle win** (131–300 W -> 19-20 W, already default, W29) [M]; **GFXOFF is worth about 3 W on top** (16 W vs 19 W) [M] and
   its wake path left the card at 100 % / 71-89 W for the rest of that boot (boot 5) [M], which is unexplained [U]. Given the risk (lost HQD/CP state,
   wake hazards), the plan puts *finding and fixing what pins the GC busy in VM boots* first, *idle accounting and measurement* second, and GFXOFF
   with a live ring behind measurements that can say whether it is worth doing at all.
4. **Sleep/wake** today: quiesce halts PFP/ME and the MEC, drains only compute HQDs; wake re-runs the whole bring-up, including the G3/G4 draws and the flip
   self-test, aborts every client. Needed for clients+gfx: abortable waits (a wait holds `rtLock` up to 10 s and so can stall the sleep handler),
   no draws/flip test on resume, gfx fences failed as Aborted. `rdna4-pm=1` has **never** run on the card [M]; it needs its own boot with a power-cycle plan.
5. A stepwise plan (§6) P0-P7, each behind a boot-arg (default off), with what the emulator can and cannot say (§5) and the real-card boots with expected log lines (§7).

## 1. What the logs say (measured)

Sources: the 11 diagnostic files of 2026-09-30 on the stick (rounds 5 and 6). Nothing later exists on the stick (Windows-side logs, if any, are not reachable from
here). The kernel bring-up window of the round-6 logs (`061829`, `062351`) was lost (dmesg rotated: 73/86 `RDNA4FB:` lines), so for them only the
registry, the sensors rows and the client-time lines survive.

| File (boot) | boot-args that matter | Idle reading (SMU table, 1 s apart) | GC state |
|---|---|---|---|
| `052606` (boot 1-like) | ih=2 flip=1 hang=1, **no vm, no gfx** | avg GFXCLK 789 MHz (post-DS 41), **3 %**, VDD_GFX 604 mV, **18-19 W** (`:83,:85,:148,:150`) | (no survey) |
| `053042` | same | 790 MHz, 3 %, 19 W (`:345`) | GRBM 0x0000382c, CP_STAT 0, CPC_STAT 0, HQDs: MEC 0/0 only (`:346-350`) |
| `053620` (round-5 boot 3) | **gfx=2 gfxprobe=1 gfxdiag=11**, no vm | 796 MHz, **3 %**, **20 W** after the gating step (`:661`); `sensors-pm` 18-20 W (`:924,:989`) | GRBM 0x0000382c, CP_STAT 0, HQDs: MEC 0/0 **and gfx 0/0 active** (`:662-666`); G3 0 px + ladder ran before it (`:388,:637`) |
| `053909` | same | 797 MHz, 3 %, 20 W (`:653`) | same |
| `061510` | no vm, no gfx | 805 MHz, 3 %, 19 W (`:343`) | |
| `053316`, `053346` (round-5 boot 2, **vm=1**, VM self-test FAILED) | vm=1 vm-diag | **3422 MHz, 100 %, 75 W** (`:662,:694`), `cg after +0.3 s` still 100 % / 69 W | **GRBM 0xa800382c** (GUI_ACTIVE, CP_BUSY, ANY_ACTIVE set), CPC_STAT 0xa0000001, CPC_BUSY 0x408, HQDs: MEC **0/0 and 0/1 active** (`:388-392`); `vm: boot queue test failed ... fault status 0x00800880`, `boot HQD dequeue timed out (ACTIVE 0x1)` (`:345,:366`) |
| `061829` (round-6 boot 2, **vm=1**, VM self-test PASS in the registry) | vm=1 vbl cursor, **no gfx** | **3417 MHz, 100 %, 80 W** (`:207,:249`), *before* the runtime selftest | registry `vm=PASS boot self-test`; then every client: `queue MEC1 pipe 0 queue 1 dequeue timeout (ACTIVE 0x1)` (`:425-472`) and `selftest: dispatch: device not responding` |
| `062351` (round-6 boot 3, vm=1 + gfx=2) | vm=1 gfx=2 (G3 0 px in the registry; the boot-args line of this file is empty, so probe/diag are not shown) | 3417 MHz, 100 %, 80-82 W (`:219,:261`) | same client failures |
| `054800` (boot 5) | gfxpm=24 **gfxoff=1**, no vm | **avg 0 MHz, 0 %, 16 W** 1.5 s after AllowGfxOff (`:732`; `gfxoff PASS ... 14 MHz 0% 16 W`); then the runtime woke GFX (`:1039-1040`: CP_RB_WPTR_POLL_CNTL restored, boot HQD lost and restored) and **the later readings are 3401 MHz, 100 %, 71-75 W** (`:797,:799`) | |

Findings:

- **F1 [M]** gfx ring up + failed G3 draw + full probe ladder + clock gating = **20 W / 3 %**, GRBM idle, CP_STAT 0 (`053620:661-666`). Against 19 W without the ring:
  the PFP/ME idle-polling ring is **not** the load (difference within the ±1 W of the averaging).
- **F2 [M]** `rdna4-vm=1` boots sit at **100 % / 75-81 W** from the first post-bring-up sample, in round 5 (VM test failing, GRBM GUI_ACTIVE/CP_BUSY set, the
  VMID 8 HQD on MEC pipe 0 queue 1 still ACTIVE after a failed dequeue) **and in round 6 (VM test PASS)**, with or without gfx. Non-VM boots never show it.
  So the state is tied to the VM boot self-test / per-client queue path, not to graphics work.
- **F3 [M]** in round 6 the kernel log shows **every client queue failing to dequeue** (`061829:425-471`, nine dequeue timeouts in the client activations) and clients' first dispatch failing
  ("device not responding") before anything graphics-related ran. That is the "dequeue timeouts" in the brief; it co-occurs with F2.
- **F4 [I]** The most economical reading of F2+F3 is one stuck CP/MEC state (a queue or wave that never drains) that keeps GRBM GUI_ACTIVE set, which the PMFW
  reads as 100 % activity and answers with max clock; in round 5 the stuck thing is the faulting VMID 8 HQD (**[M]** above); in round 6 **[U]**: the boot test
  dequeued (`inactive` is part of its PASS condition, `src/runtime.cpp:1454`), yet later HQD activations on the same slot still cannot dequeue. No round-6
  survey exists (`rdna4-gfxpm` bit 16 was not set). **Do not build idle policy on top of a card that cannot idle: P0 first.**
- **F5 [M]** `gfxPark` (W46) never ran on the card: no `park:` line in any log on the stick or in `docs/hw-logs`. Its premise ("the pipe keeps the SMU at 100 %") is
  contradicted by F1.
- **F6 [M]** clock gating (W29, default on) is what makes idle: round 4 boot 14 took 2541 MHz / 100 % / 131 W to 803 MHz / 3 % / 20 W (`docs/gfx-pm-audit.md` §10); reproduced
  in five non-VM boots here (`cg after (+1.3 s)` 19-20 W). In the VM boots it does **not** help (`053316:407`: +0.3 s after gating still 100 % / 69 W vs 30 W in non-VM boots).
- **F7 [M]** GFXOFF: `AllowGfxOff -> 0x01` at the end of bring-up gave 14 MHz / 0 % / **16 W** (`054800`), i.e. **−3 W versus clock-gated idle (19 W)**; the first GC access
  after it (the runtime) ended GFXOFF for the boot (sticky design), lost the boot HQD (restored by `gfxOffAfterWake`) and left 100 % / 71-89 W (`054800:797-802,1125`),
  where the same runtime steps in non-GFXOFF boots return to 3 % / 19 W. **The reason is unknown [U]**; candidates [I]: GFXOFF exit leaves a state the PMFW reads as busy
  (RLC/CP regs not in the snapshot list), or the restored HQD is not quite idle.
- **F8 [M, code]** `gfxOffAllowedNow` refuses GFXOFF while `gfxMode != 0` (`rdna4-gfx` on) or any client is open (`src/compute.cpp:1324-1340`): GFXOFF and a live gfx ring/clients have
  never been combined on the card. The refusal text: "the gfx ring (rdna4-gfx) is up and its CP_RB0/doorbell registers are not restored after a wake".
- **F9 [M]** SMU surface we use: `DisallowGfxOff`/`AllowGfxOff` (0x29/0x28), `SetSoftMin/MaxByFreq` (GFXCLK only, cap probe), `SetWorkloadMask` (DEFAULT bit; round 3: "did nothing" at idle).
  The PMFW refuses the allowed-features mask (SCPM, 0xFD) and runs its pptable's features: DPM_GFXCLK, DS_GFXCLK, GFXOFF, DPM_UCLK/FCLK/DCN, DS_FCLK/DCFCLK/UCLK all on
  (`docs/gfx-pm-audit.md` §3): **the display's clock domains are the PMFW's, not ours**.

## 2. What amdgpu does (read, not assumed)

| Topic | amdgpu 7.2.2 | Meaning for us |
|---|---|---|
| GFXOFF request counting | `amdgpu_gfx_do_off_ctrl` (`amdgpu_gfx.c:833-885`): `gfx_off_req_count` (starts at 1, `amdgpu_device.c:3878`); `enable=false` increments and, at count 0, cancels the delayed work and sends *disallow*; `enable=true` decrements and, at 0, **schedules the Allow after `GFX_OFF_DELAY_ENABLE` = 100 ms** (`:40`, `:861`; work fn `amdgpu_device.c:3019-3029`) | refcounted, delayed Allow; the kext's Allow is one-shot and its wake sticky |
| Who raises the count | **direct GC register access only**: debugfs/ioctl reads (`amdgpu_kms.c:909-921`), register dumps (`gfx_v12_0.c:5212-5266`), RAS, SMU; and `set_powergating_state` (`gfx_v12_0.c:4104-4122`, GC 12.0.0/12.0.1) | the kext's `GcAccess` chokepoints (`compute.cpp:1189-1245`) are the analogue |
| **Ring submissions do not touch GFXOFF** | `gfx_v12_0_ring_begin_use/end_use` (`gfx_v12_0.c:5481-5492`) = power **profile** + isolation only; the CP is woken by the doorbell/RLC path (queues are mapped by MES and their state survives) | the kext has no MES: its bare RB0/HQDs are **not** preserved (W29 round 4, F7); this is the core reason GFXOFF+ring is not "just amdgpu" |
| Workload profile | `amdgpu_gfx_profile_ring_begin_use/end_use` (`amdgpu_gfx.c:2459-2495`): FULLSCREEN3D (gfx rings present) or COMPUTE profile on first use, **released after `GFX_PROFILE_IDLE_TIMEOUT` = 1 s** of no fences/submissions (`amdgpu_gfx.h:61`; idle work `:2425-2440`) | optional P4 |
| Clock gating | enabled at late init, `gfx_v12_0_set_clockgating_state` | done (W29) |
| Suspend | phase 1: **PG+CG ungate** (GFXOFF disallowed, gating off), DF C-state disallow, **display suspended in phase 1** (`amdgpu_device.c:3040-3060,3108`); engines suspend in reverse IP order; SMC mp1 state set | the kext: DCN is not quiesced by the compute path at all (display blank is the framebuffer's `setDisplayPower`, `device.cpp:1498`) |
| Resume | phase 1 COMMON/GMC/IH, phase 2 the rest, **phase 3 display last** (`:3320-3395`) | wake order we want: GPU first, display last |

## 3. Design (a): idle with the ring up, and GFXOFF

### 3.1 What "idle" must mean, and the rule the display depends on

- **Busy** = any of: an open compute/gfx submission in flight (a fence not retired), a client inside a runtime call that touches GC, the bring-up thread, the boot HQD executing.
  **Idle** = none of these for **100 ms** (amdgpu's delay) — software-defined, independent of the SMU's own activity figure (which F2 shows can be wrong).
- **DCN never loses its clocks.** The display's domains (DCN, UCLK/FCLK DPM, DS_DCFCLK, DF C-state) are run by the PMFW (F9). Policy: the kext may send **only** `Allow/DisallowGfxOff`,
  `SetSoftMin/MaxByFreq` for **GFXCLK**, and `SetWorkloadMask`; nothing for DCN/FCLK/UCLK/DS/DF C-state, no feature-mask writes, and **no GC-domain action is taken on the display's behalf or
  vice versa**: the flip/present paths use DMU registers only and the IH uses OSSSYS/DMU (`docs/gfx-pm-audit.md` §9 guard analysis), so GFXOFF (a GC-only power state) cannot cut them. [I from code;
  the W29 round-4 guard run kept the desktop alive through GFXOFF, [M] `054800` flip/present lines after the wake at `:1039-1050`.]

### 3.2 Recommendation and order

1. **Do not park the ring** when clients can exist (F1: no gain). `gfxPark` stays probe-boot-only; with W12k clients it would make `RDNA4_FLAG_GFX` false (already so).
2. **Fix F2 first (P0/P1).** Until a VM boot can idle, no GFXOFF/idle work can be evaluated, and clients fail anyway (F3). This is the highest-value item (60 W, and client correctness).
3. **Idle accounting (P2)**: a begin/end counter around every path that uses the GPU for a client (`rtSubmitGfxIb`, `rtSubmitIb`, `rtDispatch`, DMA copies, present), pure software; with it the
   idle state is *observable* (log lines, registry property) before any SMU message depends on it.
4. **Re-measure GFXOFF's value (P3/P5)** with the ring up; the expected saving is small (−3 W, F7) against state-loss risk. Only if P5 shows the gfx ring (CP_RB0, doorbell range, goldens) survives
   or can be cheaply re-initialised by `gfxOffAfterWake`, and P1 explains the sticky-100 % after a wake, proceed to the delayed Allow (P6).
5. **Workload profile (P4)** is the amdgpu-style hook that *can* matter for performance under load (round 3: no idle effect). Low risk (one SMU message), benefit unmeasured.

### 3.3 The delayed-Allow policy if P6 happens (amdgpu model, adapted)

```
begin_use(ctx):   gcUsers++ ; cancel idle timer ; (GcAccess already DisallowGfxOff's if Off)
end_use(ctx):     gcUsers-- ; if gcUsers == 0 and no fences outstanding and no open client queue work -> arm idle timer (100 ms)
idle timer fires: if still idle -> gcLock; state=Allowing; barrier; wait gcBusy==0 (50 ms bound); snapshot; AllowGfxOff   (today's gfxOffAllow, refusal rules kept)
wake (any GcAccess while Off): DisallowGfxOff -> gfxOffAfterWake: RLC_GPM_STAT poll; restore snapshot; HQD restore; **gfx ring restore (new)**; then state=On
```

Interactions:

- **W12k submits/waits**: `rtSubmitGfxIb` -> `gfxClientEmit` already goes through `wr()`/`gfxKick` (both `GcAccess`), so the *wake* happens before the packets (as `launch()` does with
  `gcEnsureAwake`, `compute.cpp:2856,3090`: wake before emitting, not at the doorbell). A gfx wake must also restore the **ring** before the first emit: that is the new piece of
  `gfxOffAfterWake` (today it restores only the boot HQD and registers); `gfxClientReady()` must refuse while a wake is in progress (`gcState != On`, `bringupRunning`-style). `rtWaitGfxFence` polls host
  memory only (no GC access) so a wait does not keep GFX awake by itself: `end_use` is called when the fence retires (`gfxClientRetire`), and a **100 ms poll by the idle timer** retires fences of clients
  that never wait (today retirement is lazy, on the next submit/wait).
- **Compute runtime**: same counter in `rtDispatch`, `rtSubmitIb`, `rtCopy`(SDMA doorbell is GC-domain for SDMA 5+, amdgpu comment `amdgpu_device.c:3140`), `rtAlloc*` (maps via SDMA). Open compute
  clients' HQDs are **not** restored after a wake (W29 S6: Allow refused while a client is open): P6 must either keep that refusal (GFXOFF only with zero clients = the desktop-idle case the user actually cares
  about) or add client HQD restore. **Recommended: keep that refusal**: Allow only when **no client is open at all** (the desktop-idle case), and re-Allow after the last close.
  This keeps the state-loss surface at: boot HQD + gfx ring.
- **Display**: nothing to do; it does not use GC (3.1). The `Flip::run` self-test and `rtPresent` do not raise `gcUsers`.

### 3.4 What keeps it busy today, ranked [I, for P0 to test]

1. A queue/wave stuck after the boot VM self-test or a client queue activation (F2/F3): **the dominant 60 W**. P0 reads `GRBM_STATUS`, `CPC_STAT`, `CP_HQD_ACTIVE` per pipe/queue, `GCVM_L2_STATUS`,
   fault status **after** the VM self-test and **after the first client open**.
2. Stale GFXOFF-wake state (F7): P5/P6 territory.
3. Nothing in the gfx ring (F1).

## 4. Design (b): sleep/wake with the gfx ring and clients

### 4.1 Today (code)

- Sleep: `RDNA4ComputeService::setPowerState(0)` (only when `rdna4-pm=1` registered the service's power states, `src/runtime.cpp:416-425`, `src/userclient.cpp:15-38`) -> `powerWillSleep`
  (`runtime.cpp:2972`): takes `rtLock`, `rtReady=false`, drains the boot HQD and every client HQD (W6 recovery on timeout), `ihStop`, **halts PFP/ME and MEC** (`CpMeCntl`, `CpMecRs64Cntl`),
  `dmaTeardown`. **No gfx drain**: a client gfx IB in flight is cut; nothing fails its fence.
- Wake: `powerDidWake` (`compute.cpp:577`) -> `resumeMain` (`:553`): `beginBringup`, `resetRuntimeForResume` (`runtime.cpp:3038`: frees every buffer/program/heap, clients marked `aborted`, present state dropped),
  then **`runStages()` from the top** (PSP/SMU firmware, gfx autoload, SDMA, compute, dispatch, kernel, VM self-test, **gfx ring, G3 (+G4/ladder/self-test per boot-args), flip self-test, clock gating, GFXOFF probe**),
  `publishRuntime` again (`RDNA4_FLAG_RESUMED`, `runtime.cpp:375-397`). `stageGfxRing` calls `gfxClientReset` (W12k): pending counters cleared.
- Apps: every selector of an old connection returns `kIOReturnAborted` (`ownerStateLocked`); they must close and reopen. Clients inside a wait **hold `rtLock`** (compute `rtWaitFence`, gfx
  `rtWaitGfxFence`, up to `RDNA4_MAX_TIMEOUT_MS` = 10 s), so `powerWillSleep`'s `Locked g(rtLock)` blocks behind them. [I] up to 10 s of sleep delay; IOKit's sleep ack window is tens of
  seconds but this is untested.
- Display: separate path: the framebuffer's `deviceSetPower` -> `RDNA4Device::setDisplayPower` (`device.cpp:1498`) blanks/unblanks the DP stream or the HDMI DPG; no ordering with the compute service's
  PM callback is established in the code [I]. The compute bring-up on wake touches the display only via IH (`ihStart` DCN interrupt enables, vblank/pflip) and the **flip self-test** (`Flip::run`, `compute.cpp:806`:
  it flips the screen to a test surface and back).
- Never measured on the card: `rdna4-pm=1` and `rdna4-sleeptest=1` are emulator-only (`docs/real-card-plan.md:76-78`) [M: docs].

### 4.2 Needed (pre-Metal scope)

1. **Abortable waits.** An atomic `sleepRequested` (set at the very start of `setPowerState(0)`, *without* `rtLock`) that `rtWaitFence`/`rtWaitGfxFence` poll every iteration; they return
   `kIOReturnAborted` at once, releasing `rtLock`. Bounded sleep latency, and pending fences fail as Aborted instead of timing out (a timeout would wedge the gfx ring: `gfxClientWedge`).
2. **gfx at sleep.** Before halting PFP/ME: stop accepting submits (`gfxClientReady` -> NotReady), wait a short bound (100 ms) for the last fence; if not reached, just drop (no wedge: the ring is being reset anyway).
   Zero `gfxClientPending`, set `gfxParked`-like `gfxAsleep`. Wake: `stageGfxRing` re-inits (already), `gfxClientReset` (already).
3. **Resume skips what is not needed**: no G3/G4/ladder draws and no `gfxclient` self-test on resume (they are bring-up *tests*; `rdna4-gfxcol`/`gfxclient` fire on wake today); no flip self-test unless the display is
   confirmed on. A `resumePending` guard in `runStages` (the flag already exists, `compute.cpp:585`) is enough; boot-arg `rdna4-resume-tests=1` keeps the old behaviour for the emulator.
4. **Order vs the display.** Target the amdgpu order: *display suspends first, resumes last* (`amdgpu_device.c:3040-3108,3320-3395`). We cannot order the framebuffer and the service's PM callbacks
   directly; what we can guarantee is that **the compute path never touches DCN state except IH re-enable and only after the GPU bring-up finished** (no flip self-test on resume) and that **the ring/clients are
   aborted before the display's unblank needs anything from the GPU** (it does not: scanout reads the boot surface from VRAM through DCHUB). [I]
5. **What apps see**: `kIOReturnAborted` on all selectors, `RDNA4_FLAG_RESUMED` in Info after reopening, new buffers (old handles gone), new VMIDs; gfx IBs/fences of the old connection never complete. Document in
   `include/rdna4compute.h`'s header and `docs/w12k-gfx-submit.md`.
6. **GFXOFF across sleep**: amdgpu disallows it (PG ungate) in suspend phase 1; ours: `gcWake(0xffffffff)` starts every bring-up (`compute.cpp:656`) and `quiesceForShutdown` wakes first (`runtime.cpp:300`);
   `powerWillSleep` has **no** `gcWake` (its GC accesses wake through `GcAccess`, fine) but should wake explicitly first for a clean log and to cancel any P6 idle timer.

### 4.3 Risks [U]

S3 on this Hackintosh has never been exercised with the card; whether VRAM and the PSP/TMR state survive, whether the wake needs the cold path (PSP `LOAD_TOC` etc. runs in stage 2 anyway), and whether
the card returns to POST state are unknown. A wake that hangs a stage leaves the NVRAM trail (`rdna4-trail`) as with any bring-up hang. **Plan: one dedicated boot, last in the batch, with the power-cycle
instruction first in the START-HERE**, exactly like boot 5/GFXOFF ("ALWAYS LAST").

## 5. Design (c): emulator versus card

**What `emu/qemu/rdna4.c` models for power [M: code]:** the SMU mailbox and the messages `AllowGfxOff`/`DisallowGfxOff` (`:1754-1777`: Allow enters "at once"; Disallow clears the HQD registers and resets
the clock-gating registers to reset values, `gpm_restoring_reads = 2` for the RLC restore poll), `SetWorkloadMask`, `SetSoft{Min,Max}ByFreq` (CurrClock follows a lower soft max), a live `MetricsCounter`,
the GFXOFF **hazard detector** (any GC register or doorbell access while allowed is counted/logged, `:2692-2730`, `:6356`; reads all-ones, writes dropped), options `gfxoff-preset`, `smu-preloaded`, `smu-stale`,
`gfxoff-force`; metrics values are fixed (3 % / 2100 MHz / 120 W; 0 % / 8 W while GFXOFF is active, `:1735-1739`).

| Question | Emulator | Card |
|---|---|---|
| Message order and acks (Allow/Disallow, workload, soft limits), refusal handling | **yes** | yes |
| The `GcAccess` protocol: zero violations across submits, waits, dispatches, flip, sleep/wake, with the idle timer of P6 | **yes** (violations counter) | hangs only on the card |
| Idle accounting (P2), abortable waits, sleep/wake state machine (`rdna4-sleeptest=1`, `rdna4-run sleeptest`: phase 1 `powerWillSleep`, phase 2 `powerDidWake`), client abort semantics, fence failure, no draws on resume | **yes** | confirms |
| Restore logic after a wake (HQD, registers; new: gfx ring) against the **model's** loss map | yes, but the loss map is the model's guess (HQD + CG regs) | **only the card knows what GFXOFF really loses** (F7: only CP_RB_WPTR_POLL_CNTL showed) |
| Does the gfx ring (CP_RB0, RB doorbell, golden regs) survive GFXOFF? | no (not modelled: add to the model as "worst case lost", like the HQD) | **yes (P5)** |
| Power numbers, activity, clocks, return-to-idle time, what pins 100 % (F2) | no (fixed metrics, no queue activity model) | **yes** |
| Why VM boots stay busy | could be checked only if the model grows a "stuck HQD" state; **not worth it** | yes (P0) |
| S3 (PCI D3, VRAM retention, firmware reload on wake) | no | **yes**, dedicated boot |
| Display stays alive through GFXOFF/sleep | scan-out not modelled as a clock domain | yes |

## 6. Stepwise plan (every step behind a boot-arg, default off; each lands separately; emulator first, then the card)

| # | Step | Boot-arg | Change | Verify (emulator) | Card |
|---|---|---|---|---|---|
| P0 | **Diagnose the VM-boot 100 %.** Read-only logging: after `vmBootSelfTest` and after the first client open, log `GRBM_STATUS`, `CPC_STAT/BUSY`, `CP_HQD_ACTIVE` for every MEC pipe/queue, `GCVM_L2_STATUS`, fault status, the SMU sample (the existing `gfxPmSurvey`, `compute.cpp`); add `sensors` rows around the first `rdna4-run selftest` | `rdna4-gfxpm=24` (exists: survey+sample) + new `rdna4-gfxpm` bit 32 (survey after VM test/first open) | logging only | survey lines appear in order, no GC access violations | **boot 9** (§7): tells which of {HQD stuck, fault retry, hub busy} holds GRBM GUI_ACTIVE |
| P1 | **Fix what P0 finds** (not designable now: candidates: dequeue sequencing of the boot test queue, slot reuse of pipe0/queue1, UTCL fault mode after a pass) | per finding, default off until proven | TBD | the VM self-test + client queue tests unchanged | boot 9b: VM boot idles at ~20 W and client dequeues succeed |
| P2 | **Idle accounting** (`gcUsers`, `gfxIdleSince`, registry property `Compute,GFXIdle`, a `idle: busy->idle after N ms` log line; W12k fences retired by a 100 ms poll) | `rdna4-gfxidle=1` | software only | counters balance over a mixed compute/gfx/DMA load; retire poll works for clients that never wait | boot 8 + `rdna4-run tri`; the line must print after the tri |
| P3 | **Post-client idle measurement**: add `rdna4-run sensors` before/after `tri`/`tricol` to `diagnostic-log.sh` and a `gfx-app-idle` row (PASS when activity < 10 % and power < 40 W 2 s after the last client exits) | none (diag script) | script only | n/a (emulator metrics are fixed) | boot 8 |
| P4 | **Workload profile** à la `amdgpu_gfx_profile_ring_begin_use`: `SetWorkloadMask` (COMPUTE/3D bit) on first use, back to DEFAULT after 1 s idle | `rdna4-gfxprofile=1` | one SMU message pair, serialized by `smuLock` | messages seen in order, none while DisallowGfxOff races | boot 8 with a bench loop: clock/power under load vs without; expect perf change or none (unmeasured) |
| P7 | **Sleep/wake hardening** (§4.2 items 1-6) | `rdna4-pm=1` (existing) + `rdna4-resume-tests=0` default-on new behaviour only under `rdna4-pm=1` | `sleepRequested`, gfx drain at sleep, resume guard | `rdna4-run sleeptest` with a client mid-wait and mid-gfx-IB: wait returns Aborted fast, no wedge, wake bring-up prints no draw/flip-test lines, `RDNA4_FLAG_RESUMED` | **boot 11**, last, power-cycle note |

### 6.1 Later / perf items (dropped from pre-Metal by the lead, hub-task-311)

GFXOFF is worth about 3 W (16 W vs 19 W, F7) against real risk (lost HQD/CP state, wake hazards, the sticky-100 % after a wake that is still unexplained). P5 and P6 are therefore **not** part of the pre-Metal plan; they stay here, unchanged, as perf items for after Metal. P0 is folded into the VM-client investigation (`docs/vm-client-rootcause.md`); P2-P4 and P7 stay in the plan, after that blocker.

| # | Step (later) |
|---|---|
| P5 | **GFXOFF with the ring up, measurement only**: Allow at the end of bring-up even though `gfxMode != 0`; snapshot `CP_RB0_*`, `CP_GFX_HQD_*`, doorbell range, golden regs; after the first wake compare, log, **re-init the ring (`gfxRingResume`) and prove it with a ring WRITE_DATA+fence**; result row `gfxoff-ring` |
| P6 | **Delayed GFXOFF** (§3.3): idle timer 100 ms, re-Allow after the last client closes, refuse while any client is open |

(The full rows, with boot-args, verification and boot 10/10b, are in the git history of this file, commit 761122f.)

Stop conditions: any `WAKE FAILED`, any fence timeout after a wake, any GC hang -> the boot's log + power cycle, and the step stays off. P7 (and the later P5/P6) are the ones that can hang the card; P0/P2/P3/P4 cannot
touch GC beyond reads/logging and one SMU message.

## 7. Real-card boots and expected lines

Existing lines (from the code/logs) are shown as they print; **new** ones (proposed) are marked `(new Pn)`.

**Boot 9 — idle audit of a VM boot (P0).** Args: boot 2 (`rdna4-ih=2 rdna4-flip=1 rdna4-hang=1 rdna4-vm=1 rdna4-vm-diag=4065 rdna4-vbl=1 rdna4-cursor=1`) + `rdna4-gfxpm=24` (+ P0's bit).
- Healthy ring-idle reference (from F1/`053042:346-350`): `pm: survey ...: GRBM 0x0000382c ... CP_STAT 0x00000000 ... CPC_STAT 0x00000000 ... active HQDs: MEC(pipe/queue) 1: 0/0`.
- Pinned signature to look for (from `053316:388-392`): `GRBM 0xa800382c` (bits 31/29/27), `CPC_STAT 0xa0000001`, `active HQDs: MEC(pipe/queue) 2: 0/0 0/1`, `SMU ... GFX activity 100 %`, then `runtime: vmid 8: queue MEC1 pipe 0 queue 1 dequeue timeout (ACTIVE 0x00000001)`.
- (new P0) `vm: after boot test: GRBM 0x... CPC_STAT 0x... HQD ME1 pipe0/queue1 ACTIVE 0|1 fault status 0x...` and the same after the first client open. Rows in `diagnostic-log.sh`: `sensors-idle`
  (existing) reads 3-4 % / ~19 W if P1 is not needed, 100 % / 75-81 W if the pinned state persists.

**Boot 8 (W12k) extended (P2/P3/P4).** Args: boot 8 (+ `rdna4-gfxidle=1`, optionally `rdna4-gfxprofile=1`). After `rdna4-run tri`: `sensors` within 2 s of exit: expect ≈ the F1 idle (19-20 W, 3 %)
**if** P0/P1 show the VM state is healthy; the `gfx-app-idle` row; (new P2) `idle: gfx busy->idle after <ms> ms (gcUsers 0, fences retired)` within ~100 ms of the fence; (new P4) `smu: SetWorkloadMask 0x... (begin_use)` and, ≥ 1 s later,
`SetWorkloadMask 0x... (idle)`.

**Boot 10 — GFXOFF with the ring up (P5), ALWAYS LAST before sleep.** Args: boot 8's + `rdna4-gfxoff=1 rdna4-gfxoff-ring=1` (no probe/diag).
- Existing: `gfxoff: AllowGfxOff -> 0x01`; `gfxoff: 1.5 s after AllowGfxOff (SMU only, no GC access): avg GFXCLK pre-DS 0 post-DS 0 MHz, GFX activity 0 %, ... socket 16 W` (F7 reference: 16 W);
  on the first client/GC access `gfxoff: GFXOFF ended by ...: DisallowGfxOff -> 0x01`, `gfxoff: NAME changed across the power-down: old -> restored new` per register, `gfxoff wake: HQD active ...`.
- (new P5) `gfxoff: gfx ring after the wake: CP_RB0_CNTL 0x... CP_RB0_BASE 0x... (changed|same as before), doorbell range ... ; ring re-init: ok; ring proof: WRITE_DATA 0x600DF00D fence ok`, registry row `gfxoff-ring PASS/FAIL`.
  **Decisive**: if the ring registers read the same after the wake, GFXOFF+ring is cheap; if they changed, the restore line must show the ring test passing.
- Then `rdna4-run tri` must still PASS (`gfx-app-tri`) and the idle reading afterwards must **return** to ~19 W (F7's sticky 100 % after a wake is the thing to watch: `sensors-pm` after `tri`).

**Boot 11 — sleep/wake (P7), last, only after boots 8-10 passed.** Args: boot 8 + `rdna4-pm=1` (+ `rdna4-sleeptest=1` only under the emulator). Sequence: `rdna4-run tri` (PASS), user sleeps the Mac for ≥ 10 s,
wakes it, `rdna4-run tri` again. Expected lines: `power: quiesce begin (runtime not ready)`, `power: IH disabled, flip timer hook stopped`, `power: quiesce complete; compute engines halted`, (new P7) `power: gfx: N pending client IBs dropped`,
`power: wake received; re-bring-up scheduled on the bring-up thread`, `power: old runtime buffers, heaps and VM contexts discarded; client objects retained as aborted`, **no** `gfx: draw:` / `flip:` self-test lines between wake
and `user-space runtime up again: RDNA4ComputeService` (new P7), then the second `tri`: the new connection's Info carries `RDNA4_FLAG_RESUMED` (`rdna4-run info` does not print it today; P7 adds a line) and `PASS  tri:`. A client that was waiting during sleep prints `Aborted` within ~100 ms of the sleep.
If the machine does not come back: power-cycle; the last `rdna4-trail` names the stage.

## 8. What I did not do and what is uncertain

- No kext code, no GPU runs, nothing on the stick written. Not checked: Windows-side logs (none reachable), so if later rounds (after 062351) changed the VM-boot picture, F2/F3 must be re-read against them.
- F4's mechanism is a hypothesis; P0 exists to replace it.
- The −3 W GFXOFF figure is a single boot (`054800`) with a 1.5 s settle; the 16 W reading shows `VDD_GFX 39 mV` and the tool flags it `INCONCLUSIVE` (activity implausible), but power 16 W and 14-0 MHz agree.
- `SetWorkloadMask` has no measured effect at idle (round 3); its benefit under load is unmeasured.
- amdgpu reads are to the 7.2.2 tree; the PMFW's behaviour for GC 12.0.1 with SCPM is not in the kernel source.
