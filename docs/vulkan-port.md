# Vulkan on this kext: taking the RADV Darwin port

Written 2026-10-06. **Everything here is from reading source and notes. Nothing was built or run**, here or on
the card; the other project's claims are its own and unverified.

## 1. The decision

Decided by Sunneva on 2026-10-06, after looking at https://github.com/Almosst-DEV/Navi48-MacOS (MIT; its bring-up
kext BSD 3-Clause; commit `69695975`):

- **Take** its Vulkan driver port: five patches that make RADV, Mesa's Vulkan driver, run on macOS against this
  card (`vulkan/`).
- **Use as a reference** its kernel-side way of giving a process its own GPU address space.
- **Leave** its Metal bundle, its helper accelerator kext and its hooks into Apple's Radeon driver.

The reason for the split is version bias. The Vulkan driver runs in user space and reaches the kext through an
ordinary IOKit connection: nothing in it depends on a macOS version. The Metal bundle subclasses Apple's private
Metal classes and the helper kext overrides kernel functions by slot number, both taken from one build (Tahoe
26.6.2). This kext's test rig runs Big Sur 11.6.6, and the driver should not be tied to one release.

Vulkan alone does not accelerate the desktop: macOS draws with Metal. It is the lower half of the route
`docs/metal-phase-plan.md` already describes (Apple's own virtual-GPU Metal driver on top, this kext as its host),
where a shader compiler was the riskiest milestone. With RADV underneath, the host can draw through Vulkan and
Mesa compiles the shaders.

## 2. What the port needs from a kext

The patched RADV opens an IOKit user client (service `Navi48Bringup`, type `'N48N'`) and uses the calls in
`vulkan/navi48_native_abi.h`, a close copy of the Linux amdgpu ioctls RADV uses.

| Need | Their kext | This repository today | Gap |
|---|---|---|---|
| Card brought up for graphics: firmware loaded, graphics and copy engines running | yes; it also starts the firmware scheduler (whether its graphics queue depends on that was not checked) | `rdna4-compute=7` loads the same firmware and runs the engines (`src/compute.cpp`); on the card per `docs/metal-readiness.md` | none known |
| The connection itself: `Hello`, `QueryInfo` (memory sizes, address ranges, chip configuration) | selectors 0, 1 | `RDNA4ComputeClient` (`src/userclient.cpp`), another interface: `Info`, root only | a second client type speaking this interface, or the patches changed to speak ours |
| Buffers: in VRAM the CPU can reach, in VRAM it cannot, in system memory; freed on request and on close | `BoCreate`, `BoFree`, 4,095 handles | `Alloc`/`Free` from a VRAM heap, `AllocHost` for system memory | handles and the three placements behind one call |
| A buffer mapped into the process | `IOConnectMapMemory64` with the handle | none: `Write`/`Read` copy | new |
| The process's own GPU address space: a buffer mapped at an address **the process chooses**, anywhere in 48 bits, with page flags; unmapped again | `GemVa`; one fresh 4-level table per client, in VRAM | per-client tables exist (`src/gpuvmtable.cpp`, `src/ptpages.cpp`): 3 levels, about 4 GiB, addresses chosen by the kext | a 4-level table and caller-chosen addresses |
| Contexts | `Ctx` alloc, free, reset query | none as such | small |
| Submitting work: up to 64 command buffers at the process's addresses, run in its address space; a fence written into one of its buffers; a sequence number to wait on | `Submit`, `WaitSeq`; through the kernel's own graphics queue, the address space named in each command-buffer packet | `SubmitGfxIb`/`WaitGfxFence` (`docs/w12k-gfx-submit.md`): written, passes in the emulator, **not run on the card** | the unproven piece, see section 3 |
| Showing a picture: take the plane, register up to three buffers, present one at a vertical blank, give the console back | `Scan*`, selectors 9 to 14 | page flips and `Present`/`PresentAsync` (`src/flip.cpp`), on the card; since then the Lilu plugin owns two displays | adapt, and decide what "present" means with two heads |
| Importing the caller's own memory as a buffer | `BoImportHost` (selector 21) | `AllocHost` is close | small |

Not needed for Vulkan: their selectors for display-clock experiments, mode trials and the Metal nub (15 to 20).

## 3. A process's own GPU address space: their way and ours

This is the blocker `docs/metal-readiness.md` section 4 names: on the card every client job in its own address
space fails here, and the cause was not found (`docs/vm-client-rootcause.md`).

| | Their kext (read in `amd/native_s1b.cpp`, `amd/native_s1c.cpp`) | This repository |
|---|---|---|
| Who runs the work | the kernel's own graphics queue, which stays in address space 0; the packet that starts a command buffer names the client's address space | first design: a compute queue of its own per client, set to the client's address space. **Fails on the card.** Second design (`rdna4-vmshared=1`, `docs/w13-vmid.md`): two shared kernel compute queues, the address space named in the packet. Emulator only |
| Clients | one at a time | many (a pool of address-space numbers, `src/vmid.cpp`) |
| Address-space number | 8, fixed | 8 to 15, from the pool |
| Page table | 4 levels, 48 bits, built fresh per client in VRAM | 3 levels, about 4 GiB, demand-allocated |
| What is written | the table, the registers of that one address space (base, start, end), and translation-cache flushes | the same registers, plus the queue's own setup in the first design |
| Proof | a self-test at boot (a write through the new address space, checked from the CPU); they report it passing on their card | boot self-test passes on the card; it runs no shader |

So their way is this repository's second design, on the graphics queue and for one client. Linux does it the same
way on this card (`docs/w12k-gfx-submit.md` has the sequence it wraps around an application's command buffer,
captured here), and `docs/metal-readiness.md` notes that a kernel queue taking the address space from the packet
ran our code correctly under Linux. What nobody has shown is this repository's code doing it under macOS on the
card.

## 4. Order of work

Each step has something that shows it is done. Steps 0 and 1 need no card.

0. **Build the patched Mesa on a Mac and run its own tests.** The patches include an in-process stand-in for the
   kext (`RADV_DARWIN_MOCK=1`) and a test program for the interface. Shows: the port builds from a clean Mesa
   checkout plus the five patches, and what the driver asks of a kext, call by call. Needs Mesa downloaded.
1. **The interface in this kext, memory half**: connection, buffers, mapping into the process, the 4-level table
   with caller-chosen addresses, contexts. The table code can be host-tested like `gpuvmtable.cpp` is. Shows on
   the card: Mesa's device creation succeeds and a buffer written by the CPU reads back through the GPU's copy
   engine at the address the process chose.
2. **Submit and wait** on the graphics queue in the client's address space, wrapped as Linux wraps it. Shows on the
   card: a Vulkan program that renders offscreen and reads back the right pixels. This is the step that meets the
   open blocker.
3. **Present** through the flip path. Shows: a Vulkan program's picture on a display.
4. Then the Metal side of `docs/metal-phase-plan.md`, with Vulkan as the executor.

## 5. Open questions

- **One graphics stack, not two.** Their kext and this one both bring the card's graphics engines up; they cannot
  both run. Taking their approach as a reference means writing the interface here, on this repository's bring-up.
- **The service name.** The patches look for `Navi48Bringup`. Either this kext answers to that name and type, which
  keeps the patches unchanged and easy to update, or a sixth patch renames it.
- **One client or many.** Their interface serves one process at a time, which is enough for a Vulkan test and for a
  single host process on top. Many clients is this repository's design; it can come later.
- **Whose work this touches.** The compute and graphics bring-up, the runtime and the address-space code are
  m1guer's. Steps 1 and 2 change them.
- **What a client may reach.** The kernel queue runs in address space 0 and can address all of VRAM. A client's
  command buffers have to be submitted unprivileged and in the client's own address space, as Linux submits them
  (`docs/w12k-gfx-submit.md`), or the interface gives a process the whole card's memory.
