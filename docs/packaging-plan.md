# Packaging and installation plan

Status: proposal (2026-11-21); migration steps 1 and 2 built on 2026-11-22 (`make metal-driver`: `AMDRadeonRDNA4MTLDriver.bundle` with
the framework, RADV and the helpers inside, the kext's new `MetalPluginName`; card-verified), the rest not yet. The open points are decided provisionally at the end. It replaces the per-component `install.sh` scripts with one layout and one
installer, modelled on how Apple ships its AMD drivers.

## What ships today

| Component | Built by | Installed to | Installed by |
|-----------|----------|--------------|--------------|
| `AMDRadeonRDNA4.kext` | `make kext` | `EFI/OC/Kexts` (OpenCore injects it) | by hand, config.plist `Kernel → Add` |
| Metal driver `AMDRadeonRDNA4Metal.bundle` (links `librdna4.c` statically, carries its own `air2amdgpu` in `Resources/`) | `tools/metal/native/build.sh` | `/Library/GPUBundles` | `tools/metal/native/install.sh` |
| Shader translator `air2amdgpu` (root daemon, the paravirtual host's) | `tools/metal/m3/build.sh helper` | `/usr/local/libexec/rdna4-air2amdgpu` + LaunchDaemon | `tools/metal/m3/install-helper.sh` |
| `rdna4-iop` (sets CoreDisplay up each boot) | `make userspace` | `/usr/local/libexec/rdna4-iop` + `/Library/LaunchDaemons/org.rdna4.iop.plist` | `userspace/rdna4-iop/install.sh` |
| `rdna4-logd` (each boot's kernel log) | `make userspace` | `/usr/local/libexec/rdna4-logd` + LaunchDaemon | `userspace/rdna4-logd/install.sh` |
| `rdna4-run` (runtime client, tests, benchmarks; links `librdna4.c` statically) | `make userspace` | wherever the user copies it | none |
| RADV `libvulkan_radeon.dylib` + ICD JSON | `make mesa` | `Build/.../bin/vulkan/icd.d` | none (`build-app.sh` points apps at it) |
| MoltenVK environment LaunchAgent | none | `~/Library/LaunchAgents/org.rdna4.moltenvk.plist` | `userspace/moltenvk/install.sh` (per user) |

Five scripts, three install prefixes, two private copies of `librdna4`, and nothing that checks the pieces belong to the same release.

## How Apple ships its AMD driver

`/System/Library/Extensions` holds the kexts (`AMDRadeonX6000.kext` the accelerator, `AMDRadeonX6000Framebuffer.kext`,
`AMDRadeonX6000HWServices.kext`, `AMDSupport.kext`) and the user-space drivers as bundles beside them: `AMDRadeonX6000MTLDriver.bundle`,
`AMDRadeonX6000GLDriver.bundle`, `AMDRadeonX6000VADriver.bundle`, with shared code in another private bundle (`AMDRadeonX6000Shared.bundle`).
Each user-space driver is found by name: the accelerator's IOKit properties (`MetalPluginName`, `IOGLBundleName`, ...) name the bundle,
and the framework that loads it looks for that name in fixed directories. Apple ships no public framework for them: nothing outside
Apple links against that code.

## Constraints for a driver outside the sealed system volume

1. **Metal driver: `/Library/GPUBundles` only.** Metal's loader (`gpu_bundle_find_trusted`) looks for a third-party bundle there and
   nowhere else, and loads it only if `gpu_bundle_is_path_trusted` accepts it: the rootless storage class `KernelExtensionManagement`
   (which only kext management can set) or SIP's filesystem protection off (csr-active-config bit 1). It cannot live in
   `/Library/Extensions` or inside another bundle.
2. **OpenGL: no driver bundle of ours.** OpenGL.framework loads a renderer only from `/System/Library/Extensions` (absolute or relative
   `IOGLBundleName` values fail, 2026-11-12). We keep using Apple's GL-on-Metal renderer (`AppleMetalOpenGLRenderer`), named on the
   accelerator, running on our Metal device. (On our own accelerator that is not finished: see `docs/native-metal-spike.md`.)
3. **The kext stays injected by OpenCore.** As a Lilu plugin it must route IONDRVSupport before the framebuffers start (the 20 ms
   kext-list poll, `src/plugin.cpp lateNdrvCheck`); loaded from `/Library/Extensions` it would come up in the auxiliary kext collection,
   too late.
4. **The Metal driver links nothing from `/Library/Frameworks`.** It is loaded into WindowServer and into sandboxed apps, and
   WindowServer's sandbox refuses to map a library there ("file system sandbox blocked mmap()" of the framework's dylib): with the driver
   linked against the framework the desktop did not come up (2026-11-22). A library inside the driver's own bundle is mapped:
   the framework is embedded in the bundle's `Contents/Frameworks`.
5. **Code signing.** The Metal bundle is loaded into every Metal process. Hardened processes with library validation refuse a plug-in not
   signed by Apple or their own team, so the rig runs `amfi_get_out_of_my_way=0x1`. A Developer ID signature and notarization make the
   installer and the framework trusted, but do not lift library validation for Apple's own apps. This is a stated requirement of the
   driver, not something packaging solves.

## The layout

```
/Library/GPUBundles/AMDRadeonRDNA4MTLDriver.bundle/Contents/      the whole user space of the driver (built: `make metal-driver`)
    MacOS/AMDRadeonRDNA4MTLDriver           the Metal driver, named after Apple's convention; links the framework below (@rpath)
    Frameworks/AMDRadeonRDNA4.framework/    librdna4 (install name @rpath/AMDRadeonRDNA4.framework/Versions/A/AMDRadeonRDNA4),
                                            Headers/ (AMDRadeonRDNA4.h umbrella, librdna4.h, rdna4compute.h, rdna4vulkan.h),
                                            Resources/Info.plist (version, RDNA4ComputeABI)
    Frameworks/libvulkan_radeon.dylib       RADV
    Helpers/                                air2amdgpu, rdna4-run, rdna4-iop, rdna4-logd
    Resources/                              rdna4.metallib (the driver's own functions), radeon_icd.json (RADV's manifest, absolute
                                            path into the bundle); later: the LaunchDaemon plists, uninstall.sh
/Library/LaunchDaemons/org.rdna4.air2amdgpu.plist      -> .../Helpers/air2amdgpu --serve          (not yet: still /usr/local/libexec)
/Library/LaunchDaemons/org.rdna4.iop.plist             -> .../Helpers/rdna4-iop                     (not yet)
/Library/LaunchDaemons/org.rdna4.logd.plist            -> .../Helpers/rdna4-logd (on by default)    (not yet)
/usr/local/share/vulkan/icd.d/radeon_icd.json          -> the bundle's manifest (the Khronos loader's search path) (not yet)
/usr/local/bin/rdna4-run                               symlink -> .../Helpers/rdna4-run             (not yet)
EFI/OC/Kexts/AMDRadeonRDNA4.kext                       by the user, from the release zip
```

Why one bundle: the driver is loaded into WindowServer and sandboxed apps, which map libraries from `/Library/GPUBundles` but
not from `/Library/Frameworks` (constraint 4); the framework lives inside the bundle as an embedded framework instead. One
directory to install and remove, one copy of `librdna4` for the driver, and the framework still gives its headers and version.
`rdna4-run` keeps its own copy of `librdna4` (statically linked) so it also runs where the bundle is not installed (Recovery).
`rdna4mtlc.h` (the kext ↔ translator protocol) and the kext-internal headers stay private.

The MoltenVK LaunchAgent (`MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0`) exists for Apple's paravirtual Metal driver, which aborts in
`newIndirectArgumentEncoderWithLayout:`. The driver's own Metal device has argument buffers (2026-11-21): the agent ships as an
optional component for the paravirtual configuration only.

Not chosen: a framework in `/Library/Frameworks` (tried 2026-11-22: WindowServer could not load the driver, no desktop), and one
`/Library/Extensions/AMDRadeonRDNA4.bundle` holding everything (the Metal driver cannot live there, constraint 1, and
`/Library/Extensions` is the directory kext management scans).

## Versions

- One version for everything, the Makefile's `VERSION` (0.4.0 now): the kext's `CFBundleShortVersionString`, the framework's, the Metal
  bundle's, the package's.
- The framework's `Info.plist` names the kext ABI it was built for (`RDNA4_COMPUTE_ABI`, 4 now). `rdna4_open` already reads the kext's ABI
  (`rdna4_info`); it refuses a mismatch with an error that names both versions and says to install the matching kext. A kext from one
  release with user space from another is the most likely support problem, and today it fails in obscure ways.
- `rdna4-run info` prints the framework's version, the kext's version and ABI, and the Metal bundle's version.

## Building

- `make package` builds everything for one release into `Build/AMDRadeonRDNA4-<version>-<CONFIG>/pkgroot/`, in the layout above, then:
  - `pkgbuild` per component (core: framework + Metal bundle + iop daemon; translator daemon; logd; Vulkan; MoltenVK agent),
  - `productbuild` with a distribution file: core required, the translator daemon, logd and Vulkan chosen by default, the MoltenVK
    agent offered but not chosen, a macOS version check (Tahoe 26.x),
  - signed with a Developer ID Installer identity when one is configured (`SIGN_INSTALLER=...`), unsigned otherwise; notarized in the
    release workflow.
- The Metal driver links the embedded framework (`-F`/`-framework AMDRadeonRDNA4`, rpath `@loader_path/../Frameworks`).
- The release zip (today's GitHub release assets) holds the `.pkg`, `AMDRadeonRDNA4.kext` (RELEASE and DEBUG), and `INSTALL.md`.

## Installing

1. Kext: copy `AMDRadeonRDNA4.kext` into `EFI/OC/Kexts` and add its `Kernel → Add` entry after Lilu. Since the built-in defaults (next
   18db9b8) no boot-args are needed. An optional `rdna4-setup` helper (in `Helpers/`) checks the mounted EFI's config.plist for the entry,
   the kext's version against the framework's, and the SIP bits constraint 1 needs, and prints what is missing; it does not edit
   config.plist unless asked, and keeps a backup when it does.
2. The `.pkg`. Its postinstall loads the LaunchDaemons (`launchctl bootstrap system ...`) and writes the ICD JSON; the MoltenVK agent, if
   chosen, is installed for the user who runs the installer.
3. Reboot.

Upgrades: the `.pkg` replaces everything under the framework and the bundle (old daemons booted out first); the kext is replaced by hand
(the installer warns when the running kext's version differs from the package's).

## Uninstalling

`/Library/Frameworks/AMDRadeonRDNA4.framework/Resources/uninstall.sh` (sudo): boots out and removes the LaunchDaemons and the agent,
removes the Metal bundle, the ICD JSON, the `/usr/local/bin` links and the framework, forgets the package receipts (`pkgutil --forget`),
and reminds the user to remove the kext and its config.plist entry from the EFI.

## Migration from today's scripts

1. Framework: build `librdna4` as `AMDRadeonRDNA4.framework`, embedded in the driver bundle (done: `make metal-driver`,
   `tools/metal/native/build.sh`). `rdna4-run` keeps its own copy, so it runs where the bundle is not installed (Recovery).
2. Paths: daemons and their plists point into the framework; the Metal bundle is renamed `AMDRadeonRDNA4MTLDriver` (done: the
   accelerator's `MetalPluginName` in `src/rdna4accel.cpp` and `src/accelcensus.cpp` changed with it; the driver runs its
   `Contents/Helpers/air2amdgpu`; `tools/metal/native/install.sh <bundle>` installs it and removes 0.3's bundle). The postinstall removes the old `/usr/local/libexec/rdna4-*` files and `AMDRadeonRDNA4Metal.bundle`.
3. ABI check in `rdna4_open`, versions in `rdna4-run info`.
4. `make package`, the distribution file, the uninstaller.
5. Docs: `INSTALL.md` for users; `docs/install-tahoe.md` keeps the rig-specific history.
6. The per-component `install.sh` scripts stay as developer tools (installing one component from a build), documented as such.

## Decisions (provisional, 2026-11-21)

Sunneva had no settled view on these; taken so the work can start, each to be revisited:

- **The shader translator** stays both: the root daemon for the paravirtual host (the kext asks it) and the Metal driver's own run of it
  in each process. Both run the one binary in `Helpers/`.
- **The MoltenVK agent** is optional and off by default until vkQuake is tried on our own Metal device; it exists for Apple's
  paravirtual driver's abort.
- **No Developer ID for now.** Packages are unsigned (ad-hoc signed binaries) while the driver needs AMFI relaxed anyway; signing and
  notarization are revisited when that changes.
- **`rdna4-logd` is on by default** until the driver is more complete (Sunneva): each boot's kernel log is there for bug reports.
