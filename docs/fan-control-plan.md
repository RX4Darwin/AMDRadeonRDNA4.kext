# Fan Control (RDNA4Gadget tab), plan

Status, 2026-11-25: steps 1 and 2 done and card-run (below); the app interface and the tab are next. Asked for by Sunneva with the Anti-Lag work.

## What the card offers

RDNA4's SMU (14.0.2) runs the fan itself (feature FAN_CONTROL, bit 29, `kSmuFeatFanControl`). As for RDNA3, Linux's amdgpu exposes no raw
PWM for it: fan settings go through the SMU's **OverDrive table** (`OverDriveTable_t` in `smu14_driver_if_v14_0.h`, table TABLE_OVERDRIVE),
which amdgpu shows as `gpu_od/fan_ctrl/`: the fan curve (`FanLinearTempPoints` / `FanLinearPwmPoints`, a few points), `FanMinimumPwm`,
`AcousticLimitRpmThreshold` / `AcousticTargetRpmThreshold`, `FanTargetTemperature`, `FanZeroRpmEnable` and `FanZeroRpmStopTemp`, with
`FeatureCtrlMask` saying which parts apply. The board's limits for each are in the PPTable's OverDrive limits (`OverDriveLimitsMin/Max`),
which the vBIOS sets; a value outside them is refused or clamped by the SMU.

## What the kext has

- The SMU's driver table area (`kSmuTableOffset`, 64 KiB in the pool) and `TransferTableSmu2Dram` (`src/compute.cpp`, the metrics table
  the readings come from, average fan RPM included).
- Not yet: `TransferTableDram2Smu` (the write direction), the OverDrive and PPTable layouts, any way for an app to change SMU state.

## Plan

1. **Read (kext, no change to the card's state):** fetch the OverDrive table and the PPTable's OverDrive limits at start and log them
   (curve points, minimum PWM, zero-RPM, target temperature, limits). Card-run once: this also confirms the layouts against the vBIOS.
2. **Write (kext):** `TransferTableDram2Smu` of the OverDrive table with the fan fields changed and their `FeatureCtrlMask` bits set, as
   amdgpu's `smu_v14_0_2_od_edit_dpm_table` does (`PP_OD_COMMIT_DPM_TABLE`); every value checked against the PPTable's limits first, and a
   reset that writes back the table read at start. Kept in the kext: re-applied after a GPU reset and after system sleep.
3. **Interface:** the runtime service's properties, as Super Resolution's settings would be: `AMDRadeonRDNA4,FanControl` = {Mode: Auto |
   Curve, Curve: [[°C, %]...], ZeroRPM: bool, ZeroRPMStop: °C, TargetTemp: °C, MinimumPWM: %, AcousticLimit: RPM} set by root only (the app
   asks for an administrator's password once, as for installs, or a small root helper of the package applies the saved settings at boot).
   The current values and the limits read back as a property for the app.
4. **RDNA4Gadget "Fan Control" tab:** Auto (the vBIOS's curve) or Custom; the curve as a small editable chart (points draggable within the
   limits), zero-RPM switch and stop temperature, minimum fan speed, acoustic limit; the current RPM and temperature live beside it
   (from the readings the app already shows); Apply and Reset to defaults. Settings kept by the app and applied again at login.
5. **Card tests:** a curve that raises the fan at idle (RPM follows within seconds), zero-RPM off then on, reset, a value outside the
   limits refused, system sleep and wake (settings kept), a reboot (back to the vBIOS's unless re-applied).

## Step 1 result (2026-11-25, rig: RX 9070 XT)

The SMU reports driver interface **0x33** (Linux's header, the layouts used here, is 0x2E; amdgpu runs retail RX 9070 cards with it, and
every value read is plausible, so the layout holds). Logged at start (`smu: OverDrive ...`, `src/smu_overdrive.h` offsets):

- PPTable FeaturesToRun 0x048cf19e3afffcfb: fan control on.
- Limits (basic): OverDrive features 0x1b39 (fan curve, fan legacy and zero fan all allowed); fan curve PWM 15-100 %, temperatures
  25-100 C; minimum PWM 15-100; acoustic target and limit 500-5000 RPM; target temperature 25-105 C; zero RPM 0-1.
- Running (the vBIOS's): features 0, fan mode 0 (auto), curve all zero (the firmware's own), minimum PWM 15, acoustic target 1750 and
  limit 5000 RPM, target temperature 85 C, zero RPM on (stops below 50 C), max operating 110 C.

## Step 2 result (2026-11-25, rig)

`sysctl debug.rdna4_fan` (root to set) reads what is applied and takes one change at a time: `reset`, `curve T:P,T:P,T:P,T:P,T:P` (five
points, temperatures rising, speeds not falling), `zero 0|1`, `stop C` (30-80 C here), `target C`, `minpwm P` (excludes the curve, as in
amdgpu), `acoustic TARGET LIMIT`. Each value is checked against the board's limits, the feature bits set as amdgpu's od_edit_dpm_table
does, and the table uploaded (TransferTableDram2Smu 0x13, table 8); a refusal by the SMU (0xff, OD_FAIL_e in bits 16+) keeps what was
applied. The kext keeps the table read at the first start (the way back) and applies custom settings again at every SMU start. The
driver table is now held by a lock for each transfer (`smuTableLock`): sensor reads and fan control share it.

On the card: idle with zero RPM 0 RPM at 48 C; `zero 0` -> 960 RPM within 15 s; `curve 25:40,40:50,60:60,80:80,100:100` -> about 2200-2340
RPM, 48 -> 38 C; a 10 % point refused (EINVAL), nothing changed; `reset` -> the vBIOS's settings, fan stopped again. Not tested yet:
applying again after system sleep (the rig cannot be woken remotely).

## Safety and open questions

- The SMU keeps its own thermal protection (FW CTF, throttling) whatever the curve says; still, the app never offers below the vBIOS's
  minimums, and Reset restores the table read at start.
- The PPTable and OverDrive layouts must match the SMU firmware's driver interface version (`kSmuMsgGetDriverIfVersion`, amdgpu expects
  0x2E): read the version first and refuse to write on any other.
- Whether OverDrive is enabled on this board's vBIOS (some lock it): step 1 tells.
- Effort: kext work with reboots of the test machine for steps 1 to 3; the tab is small.
