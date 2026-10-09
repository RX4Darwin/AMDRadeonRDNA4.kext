# Step 4 prerequisite: getting off Recovery — a full macOS Tahoe install on the card

Status: **plan, nothing here has been run.** Written from a study of
[nullmoth/1401](https://github.com/nullmoth/1401) (`docs/HOW-IT-WORKS.md`, `p1401/policy.py`,
`p1401/nullmoth.py`, the rules table in its README) and
[nullmoth/nvidia-macos-driver](https://github.com/nullmoth/nvidia-macos-driver)
(`docs/HOW-IT-WORKS.md` §4), checked against this repo's `README.md`,
`docs/real-card-plan.md` and `docs/final-test-plan.md`. 1401 is the closest prior art we have:
a third-party GPU driver for a card macOS has no support for, shipped to machines that are not
the author's, on x86 OpenCore. It is the only published project that has solved the
"install macOS on a machine whose GPU has no installer-time driver" problem end to end.

## 1. Why this blocks the roadmap

Every real-card result in this repo — rounds 1 through 7, `docs/hw-logs/*`, every
`rdna4fb-diag-*.txt` — was taken in **macOS Recovery**, driven by
`diagnostic-log.sh` from the OPENCORE stick. That loop is excellent for bring-up and must stay.
But Recovery cannot produce any of the following, and all of them are on the critical path:

| Needed for | Why Recovery cannot give it |
|---|---|
| Real S3 sleep/wake (roadmap step 4) | Recovery's Apple menu has Startup Disk / Restart / Shut Down only; `pmset sleepnow` is a display sleep, powerd goes to DarkWake, the kext never gets `setPowerState(0)`, and the screen does not come back (hub-task-344). `rdna4-sleeptest=1` simulates the driver's own quiesce and never power-gates the card. |
| WindowServer, multi-display, long-run stability | no desktop, one display, minutes-long sessions |
| Metal milestones M0/M1 (steps 5-6) | `docs/metal-spike.md`: **Recovery has no AMD Metal stack at all**; `AppleParavirtGPU.kext` and `AppleParavirtGPUMetal.bundle` exist only in a full install |
| Anything a user would call a driver | — |

So: a full Tahoe 26.6.x install on the Gigabyte B550M K / Ryzen 7 5700G / RX 9070 XT box is a
hard prerequisite for steps 4-6, and it has its own failure mode worth planning before burning a
day on it.

## 2. The install-time problem, stated precisely

The macOS installer runs **before** any of our configuration exists, on whatever framebuffer
macOS can drive by itself. On this card that is Apple's generic `IONDRVFramebuffer` +
`IOBootNDRV` over the scanout OpenCore's GOP programmed — the exact surface RDNA4FB adopts at
runtime. Two consequences:

1. **The installer's screen is the GOP's scanout and nothing else.** If it dies during macOS's
   PCI configuration, the install is blind (and this machine has no iGPU fallback to install on:
   the 5700G *does* have Cezanne graphics, which is an escape hatch worth keeping in mind —
   see §6).
2. **RDNA4FB will be loaded during the install** if it is in `EFI/OC/Kexts`, because OpenCore
   injects it into the boot kernel collection on every boot, Recovery and installer alike. It
   has never run against an installer environment.

1401 hit exactly (1) on NVIDIA and solved it with a rule it calls `nullmoth-installer-bar`:

> The macOS installer has no NVIDIA driver and draws on the firmware's screen, which only
> survives macOS's PCI setup with a small BAR. — `nullmoth/1401`, `docs/HOW-IT-WORKS.md` §3

Their values for the install medium are `ResizeAppleGpuBars = 0`, `ResizeGpuBars = -1`, and the
Mac app flips them to the full BAR (`ResizeGpuBars = 13`, `ResizeAppleGpuBars = -1`) only after
the driver is in place.

**For us this is a genuine tension, not a copy-paste**, and it is the first spike below:

- small BAR → the GOP scanout survives PCI setup → the installer draws, and RDNA4FB's display
  adoption (which reads the console geometry and maps BAR5) is in its proven configuration;
- large BAR → the compute runtime can place buffers in all of VRAM directly. Today, without
  DMA, buffers share a **96 MiB CPU-visible heap** limited by the BAR; with SDMA + the AGP
  aperture + bounce buffer we already get 7896 MiB of buffers and 4.8/3.8 GB/s
  (`docs/hw-logs/2026-09-28-recovery-dma-pass.txt`), i.e. **we may not need the big BAR at all**.

Note that BAR5 (registers) is not what is being resized here; `ResizeGpuBars` targets BAR0 (the
VRAM aperture). RDNA4FB's register MMIO is unaffected either way.

## 3. Proposed install procedure (to be executed, then corrected in place)

### 3.1 Before touching anything
- Full image or verified backup of the target disk. The installer step erases a volume.
- Keep the known-good `OPENCORE` stick exactly as it is; build the installer on a **second**
  stick so the bring-up loop is never at risk.
- Copy `backup-before-premetal/RDNA4FB.kext` and `config.plist.before-set-boot` as today.

### 3.2 Installer medium
- **RDNA4FB disabled for the install itself.** Either leave the kext out of the installer EFI,
  or inject it with `rdna4-off=1` in the installer's boot-args. Rationale: the plugin has never
  seen an installer environment, and a hook on `IONDRVFramebuffer::doDriverIO` that misbehaves
  there costs the whole install. Turn it on for the first post-install boot, not before.
  (`rdna4-off=1` is already the documented kill switch; `-liluoff` kills all Lilu plugins.)
- **No WhateverGreen.** 1401's `tahoe-weg-amd` rule removes WhateverGreen entirely on macOS 26
  with an AMD card because **it panics on 26**, and `tahoe-weg-args` strips its boot-args
  (`agdpmod=`, `shikigva=`, `-wegnoegpu`, …). Our `README.md` currently only says "disable other
  GPU-related kexts (WhateverGreen) so nothing fights over the device" — on Tahoe that is not a
  recommendation, it is a hard requirement, and leftover WEG boot-args must go too.
- **BAR sizing for the installer:** `ResizeAppleGpuBars = 0`, `ResizeGpuBars = -1` (1401's
  installer profile). Decide the post-install value from spike S1 below.
- **Above 4G Decoding on, CSM off** in BIOS (both projects require it; we need Above-4G for the
  card's apertures regardless).
- `SecureBootModel = Disabled` (Apple Secure Boot refuses kexts Apple did not sign).
- **Logs on the stick**, 1401's `boot-logs-on-stick`: have OpenCore write its own log, Apple's
  boot log and any panic to the stick (`Misc → Debug → Target` with the file bits,
  `AppleDebug`, `DisableWatchDog`). A machine that never reaches the desktop can then still
  explain itself — today a failed boot gives us only the NVRAM `rdna4-trail` breadcrumb.
- SMBIOS: keep whatever the current OPENCORE stick uses (it already boots Recovery on this
  board). 1401's `sip-minimal` principle applies: lower SIP only as far as the next feature
  needs, i.e. keep the install at the strictest value that still boots, and widen for the Metal
  bundle later (step 10).

### 3.3 First boot after install
1. Boot with RDNA4FB present but `rdna4-off=1`: proves macOS 26 reaches the desktop on this
   hardware at all, on the stock fallback framebuffer. **This is the baseline; if it fails, the
   problem is not ours.**
2. Boot with the plugin on and nothing else (`set-boot.sh 1` argument set): desktop, correct
   colours, EDID identity and the real pointer, as in Recovery.
3. Only then re-run `set-boot.sh 8 / 10 / 11s` in the full install, plus the first **real** sleep
   from the Apple menu — the one thing Recovery never allowed.

### 3.4 What to re-measure in the full install that Recovery could not show
- Resolution change in System Settings → Displays **on the card** (so far only the VM's Bochs
  backend has done a real switch; on hardware a non-boot mode is still refused until the
  DCN 4.1.0 mode-set engine lands).
- Second connector lit.
- Real S3: `power: quiesce complete` → `power: wake received` → SDMA resuming at the engine's
  own pointers (`sdma: ... the ring resumes at 0x17604`), then PSP `LOAD_IP_FW` + RLC autoload
  **on a live GPU after an actual power-gate** — the step the simulated cycle cannot prove.
- Idle 3-8 % / ~43 W sustained for an hour with a desktop up (Recovery's idle is not a desktop's).

## 4. Spikes this raises (each is cheap and decides a setting)

**S1 — do we want the resizable BAR at all?**
Measure `rdna4-run bench` with `ResizeGpuBars 13` vs the small-BAR installer profile, on the
same boot args otherwise. Hypothesis: since the DMA path (SDMA + AGP aperture + 16 MiB pinned
bounce buffer) already reaches 4.8 GB/s host→GPU and 3.8 GB/s GPU→host and places 7896 MiB of
buffers, the large BAR buys little and costs the GOP-scanout risk. If confirmed, our documented
profile is simply "small BAR, always", which is strictly safer than 1401's two-phase dance.
Evidence: a bench row per configuration plus whether the desktop still comes up.

**S2 — does RDNA4FB survive the installer environment?**
After a successful install, repeat one install (or an OS update) with the plugin *enabled* and
see whether the installer's framebuffer still works. Until that is known, §3.2's "disabled for
the install" stands. This also pre-empts the first macOS point update bricking a user's machine.

**S3 — is there a Cezanne iGPU escape hatch?**
The 5700G has integrated graphics. If it can be configured as the boot display for install and
recovery, every risk above drops to "inconvenient" — and a second display path makes the eventual
M4 pairing question (does macOS pair our Metal device with our IONDRV display?) testable with a
control. Costs one BIOS setting to find out; may well be blocked by macOS's own lack of Cezanne
support, which is why it is a spike and not a plan.

**S4 — SIP/AMFI floor.**
Record the strictest `csr-active-config` that still boots with the plugin. nullmoth needs
`0x0A43` (`430A0000`) plus `amfi_get_out_of_my_way=0x1 amfi=0x80` only because WindowServer must
load an unsigned bundle from `/Library/GPUBundles`; **we need none of that today** (OpenCore
injection involves no signing, SIP change or kext approval). That changes the moment step 10's
Metal bundle appears, so the floor should be measured now and re-measured then, not assumed.

## 5. What we should copy from 1401 later (step 10)

Recorded here so it is not re-derived: a single root script that backs up and lints
`config.plist` (`plutil`/`ocvalidate`) and restores the backup on any failure; a boot-picker
entry that sets the kill switch for the next boot plus a LaunchDaemon that completes the
removal; SHA256SUMS over the payload; a dry-run mode that lists every change and makes none;
refusing to touch any EFI that is not the one that actually started the machine (1401 reads
OpenCore's `boot-path` NVRAM variable for this). `set-boot.sh` already does the config-backup
and `plutil`-validate half of this, which is a good sign about the design.

## 6. Risk register for this step

| Risk | Mitigation |
|---|---|
| Installer screen dies during PCI setup | small-BAR installer profile (§3.2); S3 iGPU fallback |
| RDNA4FB misbehaves in the installer | ship the installer EFI without it, or `rdna4-off=1` |
| WhateverGreen panic on 26 | remove the kext *and* its boot-args (1401 `tahoe-weg-amd`) |
| A boot that never reaches a shell tells us nothing | OpenCore logs + Apple boot log + panic to the stick (1401 `boot-logs-on-stick`) |
| Losing the working Recovery bring-up loop | build on a second stick; never modify OPENCORE |
| Real S3 bricks the card (PSP/firmware reload on a live GPU is unproven) | do it last, after `rdna4-sleeptest=1` passes in the full install; power-cycle, keep the trail |
