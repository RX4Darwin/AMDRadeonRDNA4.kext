# Fan Control (RDNA4Gadget tab), plan

Status: plan, 2026-11-25. Nothing built. Asked for by Sunneva with the Anti-Lag work.

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

## Safety and open questions

- The SMU keeps its own thermal protection (FW CTF, throttling) whatever the curve says; still, the app never offers below the vBIOS's
  minimums, and Reset restores the table read at start.
- The PPTable and OverDrive layouts must match the SMU firmware's driver interface version (`kSmuMsgGetDriverIfVersion`, amdgpu expects
  0x2E): read the version first and refuse to write on any other.
- Whether OverDrive is enabled on this board's vBIOS (some lock it): step 1 tells.
- Effort: kext work with reboots of the test machine for steps 1 to 3; the tab is small.
