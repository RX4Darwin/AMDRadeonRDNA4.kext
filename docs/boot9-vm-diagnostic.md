# Boot 9: the VM-client diagnostic (final real-card plan entry)

Author: Anvil, branch `premetal/vmdiag` (from `premetal/w13` adf24e0; scripts and this document only, no kext code). Context: `docs/vm-client-rootcause.md`
(hub-task-311), Forge's S1 diagnostic `docs/w13-vmid.md` s.8.1 / `src/vmtest.cpp` (`rdna4-vmid-test`). Evidence tags as there: [M] measured, [I] inferred, [U] unknown.

## 0. Why two boots, and an erratum to hub-task-311

**The round 6 VM boots carried `rdna4-vm-diag=4065`.** The real boot-args line is in every round 6 log (`061829` line 17: `... rdna4-vm=1 rdna4-vm-diag=4065 rdna4-vbl=1
rdna4-cursor=1`); the diagnostic script's `active:` list (line 10) has no entry for `vm-diag`, which is how `docs/vm-client-rootcause.md` came to describe the boot as a plain
VM boot [M]. Bit 512 of that mask is "F" (`runtime.cpp` `vmBootSelfTest`, `if (diag & 512)`): before the baseline attempt the kext points the GC hub's fault default page at a
host system page and sets `GCVM_L2_CNTL.ENABLE_DEFAULT_PAGE_OUT_TO_SYSTEM_MEMORY`, runs the test, then restores both (round 5 logged `F: hub NOT idle (GCVM_L2_STATUS
0x00000001): the page is kept`). It is active **even when the baseline passes** (the other diagnostic bits only run after a failure) [M, code]. The round 5 and round 6 VM boots had
it [M: round 5 logged the `vm: F` lines; round 6 has it in the boot-args line; W30, 2026-09-29, introduced F], and no plain `rdna4-vm=1` boot has ever been measured with clock gating on [I]. So the statement "a clean VM boot leaves the GPU pinned
at 100 % / 80 W" (hub-task-311 §0, H1) is **confounded with F**; F is a new hypothesis (**H9**: the boot test's F window or its restore leaves the GC L2/hub in a state that stalls
MEC queue service). It is also the cheapest thing to test, because it is one boot-arg.

Hence **two boots, in this order**: **9** (plain VM boot + the instrumentation), then **9b** (exactly round 6's VM configuration + the instrumentation). The pair is an A/B on the
confound; every other hypothesis (H1-H8) is read from either boot's survey/probe lines.

## 1. The boots (`tools/set-boot.sh 9` / `9b`)

| Boot | boot-args added to the base (`keepsyms=1 debug=0x100 npci=0x2000 -v -lilubetaall rdna4-trace=1 rdna4-compute=7 rdna4-pspdump=1`) |
|---|---|
| **9** | `rdna4-ih=2 rdna4-flip=1 rdna4-hang=1 rdna4-vm=1 rdna4-vmid-test=15 rdna4-gfxpm=24` |
| **9b** | boot 9 + `rdna4-vm-diag=4065` |

- **Round 6's boot 2 minus `rdna4-vbl=1 rdna4-cursor=1`**: both flood the kernel log (the round 6 window began 146 s into the boot and held no bring-up line [M]).
- **`rdna4-compute=7` stays.** (The lead's question: "compute=1 stops at the survey stage".) `rdna4-compute=<stage>` runs bring-up up to that stage and **1 is the survey stage only**; 7 is
  the full bring-up whose `compute:` lines (mec/vm/runtime/pm) this boot needs [M: `runStages`, `kFeatures`]. My hub-task-311 note "+ `rdna4-compute=1`" was a mistake.
- **`rdna4-vmid-test=15`** = Forge's mask (S1 round 2, `premetal/w13` 0cfa416: hub/L2 + SMU in every survey, `Compute,VMSurvey`/`Compute,VMOps` persistence, refusal traces, **bit 8 = the client-style probes T4a/T4d/T5b repeated after clock gating**, row `vmidtest-late`): 1 probes (T0 SH_MEM + SPI_GDBG readback, T1/T2 slot reuse, T3 U1/U2, T5 vadd via IB, T4a/b/c client-style queue first activation / reactivation /
  VMID 3, **T5c incremental stream bisect + sibling queues**, T5b launch-exact stream, T6 no-EXEC page, T7 negative control), 2 read-only surveys of engines and all 8 MEC HQDs at the
  flow points (before and after `vmBootSelfTest`, after the flip test, after clock gating, after the first client opens), 4 a bounded (64-line) client-operation trace.
- **`rdna4-gfxpm=24`** = sample (8) + per-stage survey (16) and nothing else: the existing `pm: survey ...` lines after stages 3-7 and the `cg before/after` samples with SMU activity and
  power. It adds the **SMU figure at each bring-up stage**, which the `vmidtest` surveys lack today (gap G1 below). No SMU message other than the metrics read.
- No gfx ring, no gfxoff, no vm-diag in boot 9. Blast radius = Forge's S1 (one spare MEC slot, three VMID contexts zeroed afterwards).

## 2. What to do and what the script extracts

`bash set-boot.sh 9` -> reboot -> log in -> `bash diagnostic-log.sh` (unchanged usage). New in `tools/diagnostic-log.sh`:

- `active:` now lists `vm-diag vm-exec vm-ispte vm-force-fail vmid-test gfxpm gfxcg gfxoff gfxcap` (the omission hid the confound).
- Sections: `dmesg: VM/queue diagnostic` (every `vmidtest:` line), `dmesg: runtime client lines` (every `runtime:` line, host-buffer noise removed; the old feature filter dropped every
  runtime line without `vmid` in it, e.g. "dispatch timed out ..." and the recovery lines [M]), `dmesg: MEC / HQD / VM boot-test lines`, and `registry copies that survive the kernel log
  wrapping` (`Compute,VMSurvey`, `Compute,VMOps`, `RDNA4FB,Results`; the first two exist only when Forge's persistence lands, G2 below; the section says so when absent).
- Rows (summary table):

| Row | Reads | PASS / FAIL meaning |
|---|---|---|
| `vm-confound` | the boot-args | INFO only: "plain VM boot" (9) or "rdna4-vm-diag=N set ... the round 6 configuration" (9b) |
| `vmidtest` | `RDNA4FB,Results vmidtest` (`T0=P T1=P ... T5c=H ...`) | FAIL if any `=F`, `=H` (shader hang) or `=D` (SH_MEM readback differs); the **first** non-P step is the answer; also FAIL if the probes never finished (no result) |
| `vm-survey` | the `vmidtest: survey <tag>: GRBM 0x...` lines | PASS always when lines exist; the text names the **first survey point where `GRBM_STATUS` bit 31 (GUI_ACTIVE) is set**, or says it never is, with every point's value |
| `vm-trace` | the `vmidtest: op ...` lines | counts of op lines, REFUSED, TIMED OUT/TIMEOUT; the first problem line |
| `idle-pin` | `sensors-pm[2]` (taken before the selftest) | FAIL `PINNED` when SMU GFX activity >= 50 % at idle (the round 6 signature: 100 % / 75-81 W); PASS with the figures otherwise |
| existing `vm`, `runtime`, `submitib`, `fault`, `sensors-*` | unchanged | |

Checked offline with the two parsing blocks fed synthetic lines of the real log format (round 5 lines for the HQD/GRBM shapes): rows come out as above (scratch harness, not committed).

## 3. How to read the result (hypotheses of `docs/vm-client-rootcause.md`)

| Observation | Meaning | Next |
|---|---|---|
| Boot 9: `idle-pin` PASS (idle low) **and** clients work (`runtime`/`vm` rows PASS) | **F (H9) was the cause**: make it opt-in only with its own arg; default VM boots never set it | done; boot 9b confirms (expected FAIL) |
| Boot 9 clean, boot 9b pinned/failing | same, stronger (A/B) | |
| Both pinned, `vm-survey` first busy point is `before vmBootSelfTest` | not VM at all (bring-up up to that point: stage 7 or earlier) | compare with a non-VM boot's surveys (they read 0x0000382c) |
| First busy point = `after vmBootSelfTest` | the boot test (or what its teardown leaves) pins the GC (H1) | bisect inside `vmBootSelfTest` (arg-gated skips: SH_MEM writes, context enable/disable, HQD activate/dequeue) |
| First busy point = `after the flip test` / `after clock gating` | flip (SDMA copy) or CG (H7) | `rdna4-gfxcg=0` boot |
| `vmidtest` `T5c=H` (or first `=F`) names a step | **H2**: that packet stalls the pipe: ACQUIRE_MEM -> GCR fields for a non-zero VMID; SET_SH_REG/DISPATCH -> SH_MEM/SPI per VMID; the wave -> instruction fetch | fix in `launch()`/queue setup; Forge's siblings result says pipe-wide or queue-local |
| `T4a=F` (client-style queue WRITE_DATA fails in the probe) | client-queue setup/slot state (H3/H4) despite identical code paths | compare T1/T2 |
| `T4c` (VMID 3) passes, `T4a` (VMID 8) fails | per-VMID state for 8-15 (H5: TRAP_EN, SH_MEM) | set TRAP_EN like amdgpu, re-test |
| all probes `P`, but clients still fail and pipe idle | the failure is post-publish: clock gating (H7; the probes run at ~22 s, before flip/cg, see G5) or something the runtime does | `vm-trace` lines + a `gfxcg=0` boot |
| `vm-trace` shows `REFUSED rtWedged` first | the wedge predates the op; read the preceding TIMED OUT | resolves the hub-task-311 [U] (needs gap G3) |

## 4. Gaps in S1 I sent to Forge (hub-task-322; Forge owns the code; I did not edit it)

G0 the survey prints no `GCVM_L2_CNTL` / fault-default address / `GCVM_L2_STATUS` / `CONTEXT1..15_CNTL`, yet the F confound is an L2-state hypothesis; G1 no SMU activity/clock/power
in `vmIdSurvey` (the pin is defined by the SMU figure; `rdna4-gfxpm=24` covers the per-stage ones meanwhile); G2 surveys and trace are dmesg-only (no `Compute,VMSurvey` /
`Compute,VMOps` properties); G3 the op trace has no refusal lines (`rtWedged` returns in `rtDispatch`/`rtSubmitIb`/`rtWaitFence` log nothing) and no wedge-setting lines; G4 no
engine survey after the first client dispatch; G5 H7 is not discriminated (all probes run before the flip/clock gating, the clients fail after); G6 no never-kicked dequeue probe (T4d)
early and late. Until they land, the script's rows degrade gracefully (messages say what is missing). Status of Forge's answer: see the hub-task-322 report.

## 5. Emulator dry run (Kiln)
Expected on the emulator (from Forge's doc and the round 6 analysis): clients work there (round 6 card failures are not modelled), so the dry run checks that the boot's **extraction
works end to end**: `vm-survey` prints 5+ points (all `0x0000382c`-shaped), `vmidtest` row PASS or names a modelling gap, `vm-trace` has lines, `idle-pin` PASS (emulator sensors), no row
crashes on missing registry properties. A run is requested from Kiln for boot 9 and 9b on `premetal/dryrun` + this branch's scripts; the result is in the hub-task-322 report.

## 6. What is not done / not claimed
No kext code here; no GPU run; nothing about boot 9 has been run on the card or (yet) on the emulator. H9 is a hypothesis: F is active during the round 6 boot test and I could not
see its restore (round 6 bring-up lines are lost), so whether F leaves state behind is [U].
