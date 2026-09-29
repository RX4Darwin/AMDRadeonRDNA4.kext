# W19: why does GFX sit at max clock? (audit and readout)

Branch `premetal/pm` (off `premetal/int` 610746e). Sources: amdgpu in
`~/src/linux-amd/drivers/gpu/drm/amd` (paths below are relative to it; `smu14_driver_if` is
`pm/swsmu/inc/pmfw_if/smu14_driver_if_v14_0.h`, `ppsmc` is `.../smu_v14_0_2_ppsmc.h`).
Kext line numbers are this branch's `src/compute.cpp`.

The observation (`premetal/r2-sensors-review.md`): on the real card, in Recovery, the SMU metrics
table reads about 300 W socket power, GFXCLK about 3200 MHz, hotspot about 90 C, and the decode is
correct. This document is what we send the SMU compared with amdgpu, and a ranked list of causes.
**Nothing here is proven on silicon yet**: the new readout below is what decides.

## 1. The readout (section 5 of the review, done)

`readSensorsEx` (`compute.cpp:856`), `rdna4-run sensors`, and two new diagnostic-log rows.
Offsets, all in `smu14_driver_if` `SmuMetrics_t` (:1649-1727) with `PPCLK_COUNT` 11 (:456-467),
`SVI_PLANE_COUNT` 4 (:558-563), `TEMP_COUNT` 12, `THROTTLER_COUNT` 21 (:216); `src/smu_metrics.h`
holds them and `tools/atomdump.cpp` checks them against a compiler-laid-out mirror of the struct:

| Field | Offset | Note |
|---|---|---|
| `CurrClock[PPCLK_GFXCLK]` | 0 | instantaneous, MHz |
| `AverageGfxclkFrequencyPreDs` | 46 | amdgpu reports PreDs when busy (`smu_v14_0_2_ppt.c:661-664`) |
| `MetricsCounter` | 104 | must advance between samples |
| `AvgVoltage[SVI_PLANE_VDD_GFX]` | 108 | mV |
| `AvgCurrent[SVI_PLANE_VDD_GFX]` | 116 | A |
| `AverageGfxActivity` / `AverageUclkActivity` | 124 / 126 | percent |
| `ThrottlingPercentage[21]` | 172 | per throttler; #12-17 are TDC/PPT (:207-212) |

- **Freshness.** The kext writes `0xFFFFFFFF` into `MetricsCounter`, flushes, asks for the table,
  and calls the sample `live` only if the counter changed. amdgpu invalidates the HDP read path
  after `TransferTableSmu2Dram` (`pm/swsmu/smu_cmn.c:1152-1155`), but `hdp_v7_0_funcs`
  (`amdgpu/hdp_v7_0.c:128-132`) has only `flush_hdp` and no `invalidate_hdp`, so on this ASIC
  `amdgpu_hdp_invalidate` (`amdgpu/amdgpu_hdp.c:70-76`) does nothing. The pool mapping is uncached
  (`compute.cpp` `mapPool`, `kIOMapInhibitCache`), so a stale table is the only case left, and the
  poison catches it. The kext posts `flushHdp()` before and after, like the rest of the bring-up.
- **Two samples, one second apart**, then a verdict line: `CLOCK-STEADY-AT-LOW-ACTIVITY` (clock within 10 % of the highest
  clock this run saw, activity < 10 %; it cannot know the DPM maximum, so it is a prompt for the cap probe, not a finding), `GFX-REALLY-BUSY` (activity >= 50 %), `no anomaly`, or `INCONCLUSIVE`
  (counter not advancing).
- **`sensors-idle`** is taken before any user-space selftest or bench; `sensors-pm` after them. The
  round-2 reading came from the sample *after* bench (`diagnostic-log.sh` ran bench, then sensors).
  Comparing the two rows separates a bench tail from a standing condition.
- Additive ABI: `kRDNA4MethodSensorsEx` and `RDNA4SensorsEx`, appended after `WaitFence`;
  `RDNA4_COMPUTE_ABI` unchanged. **This edits `include/rdna4compute.h`, which the README reserves for
  the VM agent; the request was explicit, so I did it, additively. Flag for the merge.**

## 2. What amdgpu sends after the features are enabled that we do not

`smu_smc_hw_setup` (`pm/swsmu/amdgpu_smu.c:1698-1913`), `smu_hw_init` (:1955-2018), `smu_late_init`
(:938-1012), and the CG/PG states amdgpu sets at the end of device init:

| # | amdgpu step | Where | Us |
|---|---|---|---|
| 1 | `SetAllowedFeaturesMask*` only when `!scpm_enabled` (:1803-1812) | `smu_v14_0.c:600-621` | we send it; **the PMFW answers 0xFD (CmdRejectedPrereq) on every boot**: `smu: allowed features 0xffff7efffffb2c67 -> 0xfd/0xfd (refused: SCPM, the pptable decides)` (`compute.cpp:1327`, hw-logs). See section 3. |
| 2 | `OverridePcieParameters` (`smu_update_pcie_parameters`, :1843) | `smu_v14_0_2_ppt.c:1388` | not sent |
| 3 | `smu_set_default_dpm_table` (:1873): reads every DPM table with `GetDpmFreqByIndex` | `smu_v14_0_2_ppt.c:464` | not sent |
| 4 | thermal range and `enable_thermal_alert` (:1879-1888) | `smu_v14_0.c:1815` | not sent |
| 5 | `smu_notify_display_change` (:1891) and `SetMinDeepSleepDcefclk` (:1901) | `smu_v14_0.c:656` | not sent |
| 6 | `smu_init_display_count(0)` (:1726), tool table (:1741), memory pool location (:1751) | `smu_v14_0.c:529` | not sent (driver table location and `RunDcBtc` are) |
| 7 | `smu_set_ac_dc` -> `NotifyPowerSource` (`smu_late_init` :963, `amdgpu_smu.c:2777-2800`): "the PMFW may boot the ASIC with a different mode" | `smu_v14_0.c:1275-1290` | not sent (AC/DC power-limit policy is whatever the PMFW booted with) |
| 8 | `smu_handle_task(COMPLETE_INIT)` (:988) -> `smu_adjust_power_state_dynamic` (:2433) -> `smu_bump_power_profile_mode` (:2394): **`SetWorkloadMask`** for the boot-up default profile, plus `smu_v14_0_deep_sleep_control(true)` | `smu_v14_0_2_ppt.c:1824-1876`; `smu_v14_0.c:1504-1549` | **not sent** (section 5) |
| 9 | at AUTO level, no soft limits are written (`dpm_level == level`, :2462) | `smu_v14_0.c:1019-1050` | none written either |
| 10 | `AllowGfxOff` after 100 ms of no gfx users (`amdgpu_gfx.c:42,929-1010`; `gfx_off_req_count` starts at 1, `amdgpu_device.c:3887`; released by `gfx_v12_0_set_powergating_state` at `gfx_v12_0.c:4130-4148`, called from `amdgpu_device_ip_late_init`, `amdgpu_device.c:2772-2773`) | `smu_v14_0.c:623-647` | we send `DisallowGfxOff` (`compute.cpp:1333`) and never allow it |
| 11 | clock gating: `gfx_v12_0_set_clockgating_state` -> `gfx_v12_0_update_gfx_clock_gating` (`gfx_v12_0.c:4342-4384`): CGCG/CGLS (:4151), MGCG (:4268), repeater/SRAM FGCG, perf clock, GUI-idle interrupt (:1909), all inside RLC safe mode | | never programmed |
| 12 | `smu_disable_dpms` is a no-op for 14.0.2/14.0.3 (`amdgpu_smu.c:2036-2044`; the 14.0.2/14.0.3 cases are :2041-2042), so amdgpu leaves DPM to the PMFW on unload | | not relevant to idle clocks |
| 13 | golden registers (`gfx_v12_0.c:259-261,3671-3691`: `DB_MEM_CONFIG`) | | not a clock item |

Also: the card reports driver-interface version **0x33**, `smu_v14_0_2` was written for **0x2E**
(`smu_v14_0_2_ppt.c:74`). amdgpu only warns on a mismatch. The metrics fields up to `AvgFanRpm` decode
consistently (review section 3), and `MetricsCounter` proves liveness, but the layout after byte 172 may
differ on 0x33; do not trust `ThrottlingPercentage` until it is seen non-zero under real throttling.

## 3. The feature mask we enable

- `kHeldBack` (`compute.cpp:1209-1217`) removes UCLK/FCLK/DCN DPM, VMEMP/VDDIO scaling, DS_FCLK,
  DS_DCFCLK, DS_UCLK, GFXOFF, DF_CSTATE, ATHUB_MMHUB_PG from what we offer, "to keep the display safe".
- **That mask is ignored.** The card has SCPM, so the PMFW refuses the allowed mask (0xFD) and runs the
  feature set of its pptable. Decoded from the logged running mask `0x048cf19e38fffcfb`:
  - **running, and asked to be held back:** DPM_UCLK, DPM_FCLK, DPM_DCN, DS_FCLK, DS_DCFCLK, DS_UCLK,
    GFXOFF, DF_CSTATE, ATHUB_MMHUB_PG. So the display-safe mask has never been in force, and it is the
    PMFW, not us, that decides memory/fabric scaling (UCLK sits at ~195 MHz here).
  - **running, GFX side:** DPM_GFXCLK, DS_GFXCLK, GFX_ULV, FW_DSTATE, GFXOFF (allowed, but see 10), GFX_IMU,
    GFXCLK_SPREAD_SPECTRUM, OPTIMIZED_VMIN, GFX_PSM_DIDT, GFX_EDC_XVMIN, THROTTLERS, FW_CTF, FAN_CONTROL.
  - **off (PMFW decision, we cannot enable them under SCPM):** DPM_GFX_POWER_OPTIMIZER, ACDC,
    SMARTSHIFT, GTHR, GFX_DCS, GFX_READ_MARGIN, BOOT_TIME_CAL, GFX_PCC_DFLL, GFX_EDC, BOOT_POWER_OPT,
    CLOCK_POWER_DOWN_BYPASS, APT_*, GFX_DIDT_XVMIN, SOC_PCC, EDC_PWRBRK, FAN_ABNORMAL. amdgpu's
    smu14 code only maps GFX_DCS, BOOT_TIME_CAL and BOOT_POWER_OPT (`smu_v14_0_2_ppt.c:193-205`, grep
    over `pm/swsmu/smu14`), it never toggles them; the other names were not grepped.
- So **GFXCLK DPM and DS_GFXCLK are on**; the clock is not stuck for want of the feature. Whatever holds
  it at max is inside the PMFW's policy inputs, or a real load.

## 4. Gating and queues we leave behind

- **Gating.** Nothing in the kext writes CGCG/CGLS/MGCG, RLC safe mode, the GUI-idle interrupt or HDP
  gating (grep: no `CGCG`, `MGCG`, `RLC_CGTT_MGCG_OVERRIDE`, `RLC_SAFE` anywhere in `src/`). GFX runs with the
  RLC autoload's reset defaults. That costs idle power (tens of watts), not the clock target.
- **GFXOFF.** `DisallowGfxOff` stays for the whole boot. amdgpu would allow it after 100 ms idle; we
  cannot without fencing every GC MMIO/doorbell access, so it is not offered in the experiment.
- **Queues.** After bring-up: all four MEC pipes are activated (`compute.cpp:1797`), one HQD stays active
  (`compute.cpp:1948`, ME1 pipe/queue of the runtime, plus one per VM client while open), SDMA0/1 are up
  (idle), the IH ring is enabled. With `rdna4-gfx=1/2` the gfx ring and PFP/ME stay running. None of this
  is work, but **no log records `GRBM_STATUS`/`CP_STAT` after bring-up**: the experiment now logs both with
  each sample. Direct evidence against a spinning wave would be `GRBM_STATUS` bit 31 clear and the SMU
  `AverageGfxActivity` near 0.

## 5. Workload profile

amdgpu always sends `SetWorkloadMask` once after init (`smu_bump_power_profile_mode`: `workload_mask`
starts at 0 and the boot-up default profile has a refcount of 1, `amdgpu_smu.c:1357-1363`, `2394-2417`),
for `WORKLOAD_PPLIB_DEFAULT_BIT` (`smu14_driver_if:1828`), and enables the deep-sleep DS features
(`smu_v14_0_deep_sleep_control`). We send neither. The PMFW picks its GFX activity-monitor coefficients from
the workload mask; what it does with no mask at all is not in the kernel source.

## 6. Ranked hypothesis

Measured 300 W is far above what 3.2 GHz with idle logic burns, and the edge/hotspot delta (34 C) and fan
ramp say heavy switching activity recently existed. So, in order:

1. **The sample was the tail of the benchmark, not a standing condition (most likely).**
   `diagnostic-log.sh` read sensors immediately after `bench`; the SMU averages `AverageSocketPower` and
   the clocks over a window, the fan and hotspot lag. Evidence: coherent temperatures, regulated fan, and
   power at the board PPT limit. *Test:* `sensors-idle` (before any workload) and the second sample one
   second later. If `sensors-idle` is ~idle (tens of W, low clock) this is closed, and there is no PM bug.
2. **A real load is still running (spinning wave or queue).** Verdict `GFX-REALLY-BUSY`: GFX activity
   high with no user workload. Suspects: a kernel left running by stage 7 or a selftest (`bench.cl:34`
   spins on a flag by design in the hang tests), a client queue, the gfx ring. `GRBM_STATUS`/`CP_STAT` in the
   `pm:` lines say which block is busy.
3. **The PMFW holds max GFXCLK until the driver hand-off (`SetWorkloadMask`, `NotifyPowerSource`, soft-limit
   release) that we skip.** Verdict `CLOCK-STEADY-AT-LOW-ACTIVITY`: clock high with activity ~0. The section 3
   mask refusal means the PMFW is running its own defaults, which supports "no driver policy yet". *Test:*
   `rdna4-gfxpm=1` then `=2` then `=4` (the cap probe proves the clock can be lowered and shows what power
   does). If `=1` alone drops the clock, that is the answer.
4. **Gating off and GFXOFF disallowed** raise idle power by tens of W but do not pin the clock; a
   second-order contributor, fixable only with CG programming and a GFXOFF-safe MMIO discipline.
5. **Metrics layout differs on IF 0x33.** Would make individual fields wrong, but `CurrClock`, activity and
   counter offsets sit early in the table where the review verified the decode; least likely.

## 7. What was added, opt-in

`rdna4-gfxpm=<mask>` (default off: with it absent stages 1-7 and the runtime are unchanged; the mask is
`& 0xf`, unknown bits are ignored). It runs at the end of bring-up, while trails are still allowed, only if
stage 3 (GFX) completed, and only when `featureAllowed("pm")` (a hang in a `pm:` step disables just this
feature next boot).

| Value | Effect |
|---|---|
| 8 | log a sample, change nothing |
| 1 | `SetWorkloadMask(1 << WORKLOAD_PPLIB_DEFAULT_BIT)` |
| 2 | `SetSoftMaxByFreq(GFXCLK, 0xffff)` and `SetSoftMinByFreq(GFXCLK, 0)`, the AUTO-level release (`smu_v14_0.c:1019-1050`, automatic branch) |
| 4 | probe: `SetSoftMaxByFreq(GFXCLK, 1000+1)` for one sample, then back to automatic |

Every message uses `smuSend` with a 100 ms bound; an answer other than `0x01` stops the experiment and logs it;
each step is followed by a 300 ms settle and a `pm:` line (GFXCLK, pre/post-DS, activity, VDD_GFX, power,
counter live/STALE, throttle mask, `GRBM_STATUS`, `CP_STAT`). No reset, no display or DCN register, no GFXOFF.
Trails: `pm: baseline sample`, `pm: SetWorkloadMask`, `pm: soft limits auto`, `pm: cap probe`, `pm: cap restore`.
Real-card set: boot 7 in `tools/set-boot.sh` (`rdna4-ih=1 rdna4-hang=1 rdna4-gfxpm=15`), described in
`docs/real-card-plan.md`. The emulator models `SetWorkloadMask`, the soft limits (CurrClock follows a lower soft
max), a `MetricsCounter` on the virtual clock, and a `smu-stale` option that acks the transfer without rewriting.
It does **not** model the SCPM refusal of the allowed mask (real card: 0xFD); noted, not changed.

Expected on the card when it works: `pm: baseline` then one line per step; `MetricsCounter ... (live)` on
every line; the clock and power moving after the step that matters. When it fails: `(STALE)` (metrics path),
a `-> 0xfd/0xfc/0xff` answer (refused), or the last `pm:` trail after a hang.

## 8. Round 3 (real card) and W24

Logs `premetal/hw-logs/rdna4fb-diag-20260929-01*.txt` (boot 7 = `...-015441.txt`).

- **The SMU reports GFX activity 97-100 % from the earliest sample (22.7 s) on every boot**, average GFXCLK about
  3200 MHz, 285-313 W. So section 6 hypothesis 1 (bench tail) is dead, and hypotheses 2 and 3 need reading with
  the next fact.
- **`SetWorkloadMask(DEFAULT)` and the soft-limit release did nothing** (activity stays 100 %, clock 2200-2800 avg,
  power 91-193 W is the DPM hunting, not a fix).
- **The 1000 MHz soft-max cap worked**: average 1012 MHz, 46 W, activity still 100 %; lifting it brought 285 W back.
  Power follows clock at a constant "activity", so what the card burns is clock-and-voltage cost of a block the SMU
  believes is fully busy.
- **The graphics pipeline itself reads idle at the same moment.** Baseline sample: `GRBM 0x0000382c CP_STAT 0x00000000`.
  In `gc_12_0_0_sh_mask.h` GRBM_STATUS: bit 31 GUI_ACTIVE = 0, bit 29 CP_BUSY = 0, bit 27 ANY_ACTIVE = 0, bit 22
  SPI_BUSY = 0, bits 11-13 SC/DB/CB_CLEAN = 1. No shader wave, no CP work, yet activity 100 %.
- **Metrics layout on IF 0x33:** `CurrClock[GFXCLK]` reads a constant 1000 and `AvgCurrent` 8000-54000 A, so those
  offsets are wrong for this interface; averages, activity, power, temperatures and the counter look right.
  `MetricsCounter` in kernel samples was 320, 272, 305, 291, 288 (not monotonic): use it only as "the table changed".
  W24 makes the verdict ignore CurrClock and AvgCurrent (printed, labelled untrusted).
- **Leading hypothesis (revised): clock gating was never enabled, so the GFX block is never seen as idle.** Nothing
  in the kext programs `RLC_CGCG_CGLS_CTRL` or `RLC_CGTT_MGCG_OVERRIDE`; amdgpu does, in RLC safe mode, at the end
  of device init (`gfx_v12_0_update_gfx_clock_gating`, `gfx_v12_0.c:4342-4366`, CGCG `:4151`, MGCG `:4268`, called
  from `amdgpu_device_ip_late_init`, `amdgpu_device.c:2772`). With gating off the GFX clock is always running, which
  fits "activity 100 % while GRBM is idle" if the PMFW's activity measure is clock-on based, and fits the power law
  above. This is an inference, not proof.
- **Other candidates** (all checked by the survey): MES firmware running (`CP_MES_CNTL`), an RLC/IMU state
  (`RLC_STAT`, `RLC_GPM_STAT`, `RLC_SAFE_MODE`), our MEC queue polling its wptr (`CP_HQD_ACTIVE` per pipe/queue,
  `CPC_STATUS/BUSY`), the gfx ring (`CP_GFX_HQD_ACTIVE`, `CP_ME_CNTL`), SDMA (`SDMA_STATUS`, `GRBM_STATUS2`).

### W24 additions (`premetal/pm2`)

- **`rdna4-gfxpm` bit 16 (`gfxpm=31` = boot 7)**: read-only `pm: survey <stage>:` lines after stages 3, 4, 5, 6, 7,
  the gfx ring, the pm baseline and the end of bring-up. Each survey logs: `GRBM_STATUS`, `GRBM_STATUS2`,
  `GRBM_STATUS_SE0-3` (`0x0da5,0x0da6,0x0dae,0x0daf`), `CP_STAT`, `CP_BUSY_STAT` (`0x0f3f`), `CP_CPC_STATUS` and
  `CP_CPC_BUSY_STAT`, `CP_CPF_STATUS` and `CP_CPF_BUSY_STAT` (`0x0e28`), `RLC_CNTL`, `RLC_STAT`, `RLC_GPM_STAT`,
  `RLC_SAFE_MODE` (`0x0980`), `CP_MES_CNTL`, `CP_ME_CNTL`, `CP_MEC_RS64_CNTL`, both SDMA status registers,
  `RLC_CGTT_MGCG_OVERRIDE` (`0x4c48`, override bits set = gating held off) and `RLC_CGCG_CGLS_CTRL` (`0x4c49`,
  CGCG_EN bit 0, CGLS_EN bit 1), the list of active HQDs for MEC pipes 0-3 x queues 0-7 (`CP_HQD_ACTIVE`, banked by
  `GRBM_GFX_CNTL`) and gfx pipes 0-1 x queues 0-1 (`CP_GFX_HQD_ACTIVE`, `0x1e80`), and the SMU's average clock,
  activity, power and counter. Register offsets: `gc_12_0_0_offset.h`; bit meanings: `gc_12_0_0_sh_mask.h`.
  It is read-only apart from selecting the register bank with `GRBM_GFX_CNTL`, which it restores to 0.
  Reading the first stage where activity turns 100 % says whether it precedes any of our compute queues.
- **Verdict** ignores CurrClock and AvgCurrent; new words `SMU-REPORTS-GFX-BUSY` and `LOW-ACTIVITY`.
  The diagnostic parser failed on the card because the implausible-case line did not start with
  `sensors-pm: verdict`; it does now, and the row extractors use the averaged clock.
- **`rdna4-gfxcap=<MHz>` (proposal, default off)**: soft max through the SetSoftMaxByFreq path proven in round 3
  (param `MHz + 1`, `smu_v14_0.h:54`), accepted 200-5000, `0` lifts the cap. It is a stopgap (46 W at 1000 MHz);
  the SMU keeps the limit across warm reboots, so removing the boot-arg alone does not lift it.
- **Round 5 proposal, not implemented**: an opt-in `rdna4-gfxcg=1` that mirrors `gfx_v12_0_update_gfx_clock_gating`
  in RLC safe mode, if the survey shows CGCG/MGCG off and no engine busy.

### W24 review fixes

- The end-of-bring-up survey (and every survey) now requires `done >= StageGfx` and `bringupStepAllowed`, so no
  `GRBM_GFX_CNTL` write happens while GC may be in reset or during a shutdown quiesce.
- The HQD scan follows amdgpu's GC 12.0.0/12.0.1 geometry (`gfx_v12_0.c:1416-1424`: one MEC of 2 pipes x 4 queues,
  one ME of 1 pipe x 8 queues); a read of `0xffffffff` counts as "not implemented", never as active, and the count is logged.
- A `pm: gfxcap` trail is recovered like the W19 cap (the next boot lifts the soft max once). Any boot with a pm
  boot-arg logs a reminder that a soft max survives warm reboots; `set-boot.sh 10` (`rdna4-gfxcap=0`) lifts it.
  A cap-less boot with no pm boot-arg cannot know a cap is in force, so it stays silent by design.

## 9. W27: opt-in clock gating and GFXOFF (`premetal/pm3`)

### rdna4-gfxcg=<mask>

Mirrors amdgpu's late-init clock gating for GC 12.0.1. Source of truth: `amdgpu_device_ip_late_init`
(`amdgpu_device.c:2772`) -> `gfx_v12_0_set_clockgating_state` (`gfx_v12_0.c:4370-4384`) ->
`gfx_v12_0_update_gfx_clock_gating` (`gfx_v12_0.c:4342-4366`), with cg_flags CGCG, CGLS, MGCG, 3D CGCG/CGLS,
REPEATER_FGCG, FGCG, PERF_CLK (`soc24.c:369-381`). The kext applies it after every boot self-test, inside RLC
safe mode (`amdgpu_gfx_rlc_enter_safe_mode`, `amdgpu_rlc.c:38-53`; `gfx_v12_0_set_safe_mode` writes
`RLC_SAFE_MODE = CMD | (1 << MESSAGE)` and waits for CMD to clear, `gfx_v12_0.c:4029-4041`; unset writes CMD,
`:4043-4047`), each register read-modify-write and written only if changed (amdgpu's `def != data`).

| Bit | Step | amdgpu | Registers written (gc_12_0_0_offset.h) |
|---|---|---|---|
| 1 | coarse | `update_coarse_grain_clock_gating`, `gfx_v12_0.c:4151-4247` | `RLC_CGTT_MGCG_OVERRIDE` (0x4c48) clear CGCG(3)/CGLS(4)/GFX3D(7); `RLC_CGCG_CGLS_CTRL` (0x4c49) idle threshold 0x36 + CGCG_EN, delay 0xF + CGLS_EN; `RLC_CGCG_CGLS_CTRL_3D` (0x4cc5) same; `CP_RB_WPTR_POLL_CNTL` (0x0f62) 0x00900100; `CP_INT_CNTL` (0x1de9) CMP_BUSY/CNTX_BUSY/CNTX_EMPTY/GFX_IDLE enables; `SDMA0/1_RLC_CGCG_CTRL` (0x0055/0x0655) CGCG_INT_ENABLE |
| 2 | medium | `update_medium_grain_clock_gating`, `:4268-4300` | `RLC_CGTT_MGCG_OVERRIDE` clear GRBM_CGTT_SCLK(5)/RLC_CGTT_SCLK(1)/GFXIP_MGCG(2) |
| 4 | fine | `update_repeater_fgcg :4302`, `update_sram_fgcg :4323`, `update_perf_clk :4055` | `RLC_CGTT_MGCG_OVERRIDE` clear REPEATER_FGCG(9)+RLC_REPEATER_FGCG(0), then FGCG(8), then PERFMON_CLOCK_STATE(10) |
| 8 | gui idle | `gfx_v12_0_enable_gui_idle_interrupt`, `:1909-1935` (ME0 pipe 0 only, `:1874-1886`) | `CP_INT_CNTL_RING0` (0x1e0a) same four bits |

`rdna4-gfxcg=15` is all of them in amdgpu's order; `=0` is the `enable=false` mirror (`:4249-4265`, `:4290-4298`).
Before/after logging: `cg:` lines per register (old -> new), `pm: cg before` / `cg after (+0.3 s)` / `cg after (+1.3 s)`
samples and two `pm: survey` blocks. Bounded: the safe-mode wait is 20 ms; if it does not acknowledge, nothing is
written. Trails `pm: cg ...` (a hang disables only the `pm` feature next boot, never "pm: cap", so no cap recovery is
triggered).

### rdna4-gfxoff=1 and the GC-access guard

`AllowGfxOff` (0x28, `smu_v14_0_2_ppsmc.h:86`) is sent once, as the very last step of bring-up, only if the kext
is not inside a GC access (`gfxOffAllow`). The SMU has no `GetGfxOffStatus` message on 14.0.2, so the effect is read
from the metrics table (SMU only, 1.5 s later). The real-card hazard is a GC register or doorbell access while the
block is powered down. amdgpu prevents that by keeping GFXOFF disallowed around direct GC accesses
(`amdgpu_gfx_off_ctrl(adev, false)`, `gfx_v12_0.c:5074,5247,5283`) and allowing it only 100 ms after the last user
(`amdgpu_gfx.c:925-985`). The kext guarantees it structurally:

1. The kext reaches GC only through `rd()`/`wr()` with the GC hardware id and through four doorbell writes
   (compute, VM queue, SDMA, gfx ring). `env.mmio` is dereferenced nowhere else in the GC-capable code (grep:
   `compute.cpp` `rd`, `wr`, `flushHdp`; HDP flush is NBIF). IH uses OSSSYS/DMU registers only, flip uses DMU
   only, sensors use the SMU mailbox and the pool. All of them therefore either never touch GC or go through
   the two chokepoints.
2. Both chokepoints construct a `GcAccess` (compute.cpp, "W27: GFXOFF guard"). It increments `gcBusy`, then
   checks `gcState`; if GFXOFF is allowed (or being allowed) it decrements, calls `gcWake()` (`DisallowGfxOff`,
   0x29, which the SMU answers once GFX is powered; retried once) and retries. It proceeds only after seeing
   `gcState == On` with its increment in place.
3. `gfxOffAllow()` first sets `gcState = Allowing`, executes a locked instruction (full barrier on x86), waits at
   most 50 ms for `gcBusy == 0` and only then sends the message; accessors racing in wait on `gcLock`. If anything
   is still inside, Allow is not sent.
4. The wake is sticky (until the next boot, or a re-bring-up, which starts with `gcWake()`): GFXOFF covers only the
   idle window between the end of bring-up and the first GC access. `rdna4-run info`/`sensors` do not end it.
5. `smuSend` takes `smuLock`, so a wake from any thread cannot interleave with another SMU message.
6. Not covered, on purpose: interrupt context (nothing there reaches GC) and a failed wake (logged as `WAKE
   FAILED`; the following access would still be attempted).

Emulator: models the hazard, not the power saving. After `AllowGfxOff` any GC register read/write or doorbell write
is counted and logged as `GC ... while GFXOFF is allowed (violation N)` (reads return all ones, writes drop); the
metrics report activity 0 and 8 W while allowed (the model's numbers, not silicon's). The dry run must show zero
violations with the guard (verified: 0 in the dry runs). The negative case (guard compiled out, expecting violations) was NOT run.

### Review B1/S1/S3 fixes (`premetal/pm3`)

- **B1, the guard across boots.** A fresh kext instance starts with `gcState == On`, so the first GC read (the
  stage-1 survey, `defensiveStart`) was unguarded if a previous boot left GFXOFF allowed and the ASIC kept it.
  Layer 1: `quiesceForShutdown` calls `gcWake(...)` before its first GC access (this also clears the flag).
  Layer 2: an NVRAM flag (`...:rdna4-gfxoff`) is written before `AllowGfxOff` and cleared after a successful
  `DisallowGfxOff`; `gfxOffPreflight` reads it at the start of `start()`, before the survey, and sends
  `DisallowGfxOff` first. The SMU mailbox is MP1 (`SmuMsg/SmuArg/SmuResp` = MP1 C2PMSG_66/82/90, `gfxregs.hpp`), not
  GC, so it is reachable while GC is powered down; whether it answers that early is not proven on the card (the
  PSP loads the SMU firmware in stage 2 on a cold boot; after a warm restart the previous boot's PMFW is still
  running). If it does not answer, `kGcHold`: `start()` returns before the survey and every GC access this boot is
  dropped. **Caveat:** the code comment in `runStages` says NVRAM is not published yet at attach; the preflight logs
  whether the flag was readable at attach, and `runStages` checks again after the 5 s wait (a late flag still lifts
  GFXOFF before stage 2, but the stage-1 reads cannot then be protected, and the log says so).
- **S1.** A failed `DisallowGfxOff` now sets `kGcHold`: `GcAccess::ok` is false, `rd()` returns `kBad`, `wr()` and the
  doorbell writes do nothing; nothing proceeds to a block that may be powered down.
- **S3, emulator.** `gfxoff_active` already survives a reset; new options `gfxoff-preset` (every reset leaves the GC
  block powered down) and `smu-preloaded` (the POST-loaded PMFW answers the mailbox from reset on) model "the ASIC kept
  GFXOFF across the restart", and `qom-set /machine/peripheral/rdna4 gfxoff-force` changes it at run time.

### Re-review fixes S5/S6 and the hooks

- **S5:** `gfxOffFlagSet` returns whether the read-back agrees; `gfxOffAllow` does not send `AllowGfxOff` unless the flag read back as 1 (log: "persistent flag could not be written").
- **S6:** a timed-out `AllowGfxOff` (response 0) is treated as possibly allowed: the flag stays set and `gcState` is Off, so the first GC access goes through the guard and sends `DisallowGfxOff`. Explicit refusals clear the flag as before.
- **Hooks:** the real Allow needs exactly `rdna4-gfxoff=1` (any other value, e.g. 10, is ignored). `=2` and `=3` are honoured only when CPUID reports a hypervisor (the emulated device); on hardware they log "TEST HOOK ... ignored".
