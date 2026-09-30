# Final real-card test plan (branch `premetal/final`)

Author: Anvil (hub-task-331/334). One document for the user: the exact order, what each boot is for, what it should print, what a FAIL means and whether to go on.
Everything below was built and dry-checked off the card only (kext + `rdna4-run` build, `nm -u` unchanged, `make test`, userspace host tests, `tools/emu-qtest-vm.py` 35/35, `bash -n`);
**no boot here has run on the card**, and the emulator dry run of the whole plan is Kiln's (result in the hub-task-331 report / its doc). Evidence tags: [M] measured on the card in an earlier round, [I] inferred, [U] unknown.

## 0. What `premetal/final` contains

`premetal/vmdiag` (= `premetal/w13` d9ec129: S0 invalidate fix, S1 VMID/queue diagnostic round 2, S7-lite shared queues `rdna4-vmshared=1`, ptpages; + boots 9/9b) + `premetal/power` (P2 idle
accounting, P3 post-client idle row, P7 sleep/wake hardening, on `premetal/w12k` f43d3c9: W12k client gfx IBs, G4 colour triangle) + `premetal/emu-linux` 658e1a2 and Kiln's script commits 762bf45, 8080095.
Not included: Forge's S8 (hub-task-329) and any stale-TLB correctness fix Forge may still commit to `premetal/w13` (hub-task-334): `git merge premetal/w13` refreshes the tree when it lands; this
document does not change. **Build the kext from `premetal/final`'s tip** (`OSXCROSS=$HOME/.local/share/osxcross bash tools/build-osxcross.sh`, then copy `build/RDNA4FB.kext` and `build/rdna4-run` to the stick
together with `tools/set-boot.sh` and `tools/diagnostic-log.sh`).

## 1. How to run one boot (unchanged loop)

1. Boot 1 (or whatever the stick is set to) into macOS Recovery, open Terminal (Utilities > Terminal), run `bash /Volumes/OPENCORE/set-boot.sh <id>`, then restart (Apple menu > Restart).
2. The restarted boot again lands in Recovery (as in rounds 5-7): open Terminal and **immediately** run `bash /Volumes/OPENCORE/diagnostic-log.sh` (it waits up to 2 minutes for the bring-up, which starts 5 s after the kext
   loads). It writes `rdna4fb-diag-<date>-<time>.txt` next to the script and prints the summary table. **Keep every log; send them all.** (`tools/stage-stick.sh` generates `START-HERE.txt` with the same steps in plain words.)
3. If a boot hangs or the screen does not come back: power-cycle, pick the stick again, `set-boot.sh 1` (or 0). The NVRAM trail (`rdna4-trail`) makes the next boot skip the feature that hung, once: boot the same id a second
   time before drawing conclusions from the skip.
4. `NOVMDIAG=1 bash set-boot.sh <id>` gives the same boot without `rdna4-vm-diag=4065` (see step 3 of the order).

## 2. The order

| # | `set-boot.sh` | What it tests | Continue if FAIL? |
|---|---|---|---|
| 1 | **1** | The proven base + the real pointer (daily-use setting). Baseline: nothing new may have broken. | **No**: stop and send the log (the merge broke the base) |
| 2 | **9** | The VM-client diagnostic, **plain** VM boot (no `rdna4-vm-diag`) | Yes: a failing client is the point of this boot |
| 3 | **9b** | Boot 9 + `rdna4-vm-diag=4065` = round 6's exact VM configuration (A/B on the confound) | Yes |
| 4 | **12** | The candidate fix: boot 9 + `rdna4-vmshared=1` (clients' compute jobs on two shared VMID-0 queues) | Yes |
| 5 | **3** | G3 + G4 on the card (triangle, colour triangle); the one that can hang the GPU | After a hang: power-cycle, `set-boot.sh 1`, send logs, **stop here** |
| 6 | **8** | Applications compute + draw through the old path (W12k `rdna4-run tri`/`tricol`) | Yes |
| 7 | **13** | Boot 8 + `rdna4-vmshared=1` (same apps, shared-queue compute path) | Yes |
| 8 | **10** | Boot 8 + `rdna4-gfxidle=1`: idle accounting + post-client idle | Yes |
| 9 | **11s** | The driver's simulated sleep cycle (`rdna4-pm=1 rdna4-sleeptest=1`): **LAST, never run on the card before**, power-cycle afterwards | (last) |
| 10 | **1** | OPTIONAL last step, after 11s and a power-cycle: the idle-display check (boot 1 in Recovery, run nothing, idle 11 min, press a key; then `diagnostic-log.sh`) | (optional) |
| end | **1** (daily) or **0** (known good, no feature) | | |

**Decision after steps 2-4** (read `docs/boot9-vm-diagnostic.md` s.3 for the full table): boot 9 clients PASS and 9b FAIL => the F diagnostic (`rdna4-vm-diag` bit 512) is the culprit: use
`NOVMDIAG=1 bash set-boot.sh <id>` for every later VM boot (3, 8, 13, 10, 11s) and drop `rdna4-vm-diag` from the plan. Both FAIL and boot 12 PASS => the shared-queue path is the fix; use it (`rdna4-vmshared=1`) for 8's
replacement (boot 13) and later boots. All three FAIL => keep going with 3/8/10/11s for the non-client results, and send the logs: `vm-survey`, `vmidtest`, `vm-trace` name the failing step.

**Display sleep during a pause** (macOS `displaysleep` = 10 min in Recovery; decision of the lead: no `rdna4-nosleep=1` in the test boots, it would change the args vs round 6 and hide the untested path): if the screen goes black during a pause, press a key or move the
mouse; if it does not come back within ~10 s, hold the power button to power off, continue with the next step as usual and report that it happened (and in which boot). The kext's DPG blank/un-blank path has **never run on the card** [M: empty section in every stick log].
**Optional last step (10):** after 11s and a power-cycle, boot 1 in Recovery, run nothing, leave it idle 11 minutes, press a key: does the picture come back? Then `diagnostic-log.sh`: its `display power` section prints `power: HDMI display blanked/unblanked via DPG` (DP: `power: display off/on`) and `ndrv: Control csc 11` lines.

## 3. What each boot should print (rows of the summary table)

General: `PASS` needs the command's own success line **and** the durable `RDNA4FB,Results` value to agree. `SKIPPED` is fine when the row's argument is absent. Reference values from round 6 [M]: idle GFX
**3-8 % / 42-45 W** in boots without VM (`061510`, `054257`); **100 % / 76-81 W** in every VM boot so far (`061829`, `062351`); clients failed in every VM boot.

### Boot 1 (base)
Expect: `runtime PASS`, `ih PASS`, `vblank PASS`, `flip PASS`, `anim PASS`, `sensors-idle`/`sensors-pm` with activity 3-8 % and ~43 W, cursor rows as in round 6, `idle-pin PASS`, `post-idle PASS`,
`vm SKIPPED`. A FAIL of any of these is a regression of the merged tree (the base path did pass in round 6): **stop**.

### Boot 9 (plain VM + diagnostics) and boot 9b (+ vm-diag)
Args: `rdna4-ih=2 rdna4-flip=1 rdna4-hang=1 rdna4-vm=1 rdna4-vmid-test=15 rdna4-gfxpm=24` (9b adds `rdna4-vm-diag=4065`). No vbl/cursor so the log window keeps the bring-up lines.
- `vm-confound INFO` says which of the two it was. `vm` = the boot self-test (PASS expected in both: round 6 passed it with diag).
- `vmidtest` (probes T0-T7 incl. the incremental-stream bisect T5c and T4d, all **before** the runtime is published) and `vmidtest-late` (T4a/T4d/T5b repeated **after clock gating**): result strings like
  `T0=P T1=P ... T5c=P T5b=P ...` (P pass, F fail, H shader hang, S skipped, D SH_MEM readback differs). **The first non-P step is the answer**: T5c names the first packet of the client stream that stalls.
- `vm-survey` names the first survey point (before/after the boot self-test, after flip, after clock gating, after the first client opens) where GRBM_STATUS bit 31 is set; `idle-pin` FAIL = the SMU sees >= 50 % activity at idle.
- `vm-trace`: op lines, how many REFUSED / timeouts, the first problem. `runtime`, `submitib`, `fault`: the client paths; PASS = the old client path works.
- **Read with one caveat (hub-task-334)**: the T-probes run after the boot self-test, which used VMID 8. Forge's emulator matrix found stale-TLB uses on VMID 8 after a client close/reopen and after host unmaps; if a probe that
  reuses VMID 8 fails with a fault at its **first** access (or the log says `STALE TRANSLATION` on the emulator), weigh that against a TLB-flush gap before calling it a client-path result; the probes that use VMID 1/15/3 are not affected.
- A healthy outcome: `idle-pin PASS` (activity < 50 %), `vm-survey` "never set", `runtime/submitib/fault PASS`, all probes P. **Any** of those failing is information, not a stop.

### Boot 12 (candidate fix)
Args: boot 9 + `rdna4-vmshared=1`. `vmshared INFO` row; expect the same diagnostics plus `runtime PASS`, `submitib PASS`, `fault PASS` **on the shared-queue path** (log lines `rdna4-vmshared=1: client jobs run on 2 shared VMID-0 queues`,
`shared-queue client ... on shared queue N`). If `runtime` fails here but `vmidtest` T3/T5 (IB in VMID 8 from a VMID-0 queue) passed in boot 9: the shared queue setup, not the VMID, is at fault; send both logs.

### Boot 3 (G3 + G4; can hang)
Args: boot 2's + `rdna4-gfx=2 rdna4-gfxprobe=1 rdna4-gfxdiag=11 rdna4-gfxcol=1`. Expect `gfx PASS` (`THE TRIANGLE IS RIGHT`, 8192 pixels) and `gfx-col PASS` (`THE COLOUR TRIANGLE IS RIGHT`, 8192); see
`docs/g4-colour.md` ("First run on the card"). FAIL: read the `gfx:` lines and the `Compute,GFXVerdict` property; the round 6 root cause (NGG EXEC starts at lane 0) is fixed in the tree, so a FAIL is new information.
A hang needs a power-cycle and boot 1; do not go on to the draw boots before understanding it.

### Boot 8 (apps draw, old compute path) and boot 13 (same, shared-queue compute path)
Boot 8: `gfx-client PASS` (kernel's synthetic client IB), `gfx-app-tri PASS` (`rdna4-run tri`), `gfx-app-tricol PASS` (`tricol`; FAIL is the G4 not-yet-proven case), `runtime/submitib/fault` as in boot 9.
Boot 13 adds `vmshared INFO`: the compute rows now test the shared path; the gfx rows must be identical to boot 8 (the draw path is unchanged). A difference between 8 and 13 in the gfx rows is a regression of S7-lite.
The W13 S0 dependency of W12k (rtFree invalidates device buffers) is in the tree.

### Boot 10 (idle)
Boot 8 + `rdna4-gfxidle=1`. Rows: `gfx-idle-acct PASS` (`Compute,GFXIdle`: `idle for N ms ... transitions T`; lines `idle: busy -> idle after N ms busy`), **`post-idle`** (3 s after the application steps: PASS < 10 % and < 40 W;
FAIL `STAYS HIGH` = the VM pin survives the clients closing), `idle-pin`. Changes no hardware state, so a FAIL is a measurement, not a regression.

### Boot 11s (the sleep test, LAST)
Args: boot 8 + `rdna4-pm=1 rdna4-gfxidle=1 rdna4-sleeptest=1`. **Never run on the card.** Nothing to do by hand: `diagnostic-log.sh` runs the **driver's own simulated sleep cycle** (`rdna4-run sleeptest`: `powerSleepRequest` + `powerWillSleep`, a 15 s pause,
`powerDidWake`, a 15 s wait for the re-bring-up) and records row `sleep`. **Why not a real system sleep** (Kiln, emulator Recovery, hub-task-344, observed): Recovery's Apple menu has only Startup Disk / Restart / Shut Down (no Sleep); `/usr/bin/pmset`
exists and `pmset sleepnow` is a **display sleep only** (powerd goes to DarkWake, the kext never receives `setPowerState(0)`), and afterwards **the screen stays black** (no "Display is turned on", no kext un-blank). So the plan never uses `pmset sleepnow` or an
Apple-menu Sleep in Recovery. A real system sleep could only be tried from a full macOS that has a Sleep entry; that is outside this plan (the old boot 11 = boot 8 + `rdna4-pm=1 rdna4-gfxidle=1` stays in `set-boot.sh` for that, unused).
Expected in the log: `power: debug sleep selector phase 1`, `power: sleep requested`, `power: quiesce begin`, `power: gfx: no client gfx IB in flight at sleep`, `power: quiesce complete`, `power: wake received`, then the whole bring-up again (PSP, RLC autoload, SMU), **`sdma: resume without a power loss? engine
pointers rptr ... wptr ...: the ring resumes at 0x...`**, `power: resume: skipping the G3/G4 draws, the gfx client self-test and the flip test`, **no** `gfx: draw` / `flip:` lines before `user-space runtime up again`; row `sleep PASS` (the pre-sleep client comes back `Aborted`).
Validated on the emulator only, **with and without a power loss** in the window (Kiln, hub-task-363): both pass. **What that does not cover**: the simulated cycle does not power-gate the card (the machine stays on), so it proves the driver's quiesce and re-bring-up, not what S3 does; and re-running PSP `LOAD_IP_FW`/autoload on a **live** GPU is
unproven on the card. If the screen goes black or the machine hangs: power-cycle, `set-boot.sh 1`, send the `rdna4-trail` and the log. Power-cycle after this boot either way.

## 4. Getting back
- **Daily use**: `set-boot.sh 1` (base + real pointer) — or `0` for the known-good argument set without any new feature.
- **Kext rollback**: replace `EFI/OC/Kexts/RDNA4FB.kext` with `backup-before-premetal/RDNA4FB.kext` on the stick (see `docs/real-card-plan.md`).
- `set-boot.sh` keeps the first config backup (`config.plist.before-set-boot`) and validates with `plutil`.
- After any GPU-side hang: power-cycle (not a warm reboot): clock-gating and SMU state persist across warm reboots ([M] `docs/gfx-pm-audit.md`).

## 4b. Emulator dry run of this plan (Kiln, hub-task-350, tree `6cf90aa` + scripts; QEMU rebuilt from that tree's `rdna4.c`)
Every boot reached Recovery, **no panic, no shell error in `diagnostic-log.sh`, zero `STALE TRANSLATION`, zero `unknown source`**. Rows that matter:
boot 9/9b/12: `runtime/submitib/fault/vm PASS`, `vmidtest` **all P** (T5c, T5b, T6, T7 included), `vmidtest-late PASS`, `vm-trace PASS` (96 op lines), `vm-survey` 5+ points; boot 12's clients ran on the shared queues
(`vmshared: shared queue 0: MEC1 pipe 0 queue 2, VMID 0, doorbell dword 74`). Boot 3: `gfx PASS`, `gfx-col FAIL` (emulator does not model G4). Boot 8: `gfx-client PASS`, `gfx-app-tri PASS`, `gfx-app-tricol FAIL` (G4).
Boot 10: `gfx-idle-acct PASS` with the expected `idle:` transitions. **Findings and what was done:**
- **Boot 13 gfx rows FAILED** (`tri: SubmitGfxIb: resource shortage`, `gfx-client FAIL setup`): `rtOpenShared` did not set the client's gfx fence. Fixed in `4365f84` (same two lines Kiln confirmed); the rule "gfx rows of boot 13 = boot 8" holds again (Kiln's scratch run); the fixed tree is not re-run in the emulator yet.
- **Boot 11s FAILED on the emulator** (`sleep FAIL`): after the simulated sleep the re-bring-up stopped at stage 4 (SDMA): the engine still held its 64-bit pointers (0x17604) and the kext restarted the ring at wptr 0, which SDMA 7 treats as "nothing to do" (card-proven rule, `ae70f2e`). Kiln's run never triggered the emulator's sleep reset (its `power reset` line is at VM start), so this **was the no-power-loss case, which is what the real card is in after the driver's simulated sleep**; it was *not* an emulator gap. Fixed in `12ac79b` (the ring resumes at the engine's own pointers after a wake; emulator: ring position = RB_RPTR mod size); **re-run **passes with and without a power loss** (Kiln, hub-task-363). What the emulator still cannot validate: the rest of the re-bring-up (PSP/firmware autoload again on a live GPU) and what a real S3 does to the card. Treat 11s as a low-value fallback.
- `post-idle FAIL` and every idle figure on the emulator come from a constant synthetic SMU (3 % / 120 W): not a kext result. `vmidtest` all-P means **the emulator does not reproduce the round 6 card failure**: what breaks the clients on the card is not modelled (this is what boots 9/9b/12 are for).
- `pmset sleepnow` in the emulator's Recovery is a display sleep only and the display does not come back (hub-task-344): see the plan for boot 11s and `docs/power-gfx.md` s.9.7.

## 5. What is not known
- Every boot id above is **unrun on the card** in this tree. The `vmidtest` probes and `rdna4-vmshared` have run on the emulator only (Kiln's whole-plan dry run: see the hub-task-331 report); whether the emulator reproduces the round 6 client failure is [U].
- Boots 2/3/8/10/11s/13 carry `rdna4-vm-diag=4065` unless `NOVMDIAG=1` (round 6 had it; whether it matters is what boots 9/9b decide).
- P2/P3/P7 (idle accounting, post-idle row, sleep hardening) have not run outside the build; see `docs/power-gfx.md` s.9.
