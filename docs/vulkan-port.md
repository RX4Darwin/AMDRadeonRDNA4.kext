# Vulkan on this kext: taking the RADV Darwin port

Written 2026-10-06. Sections 1 to 5 are from reading source and notes; sections 6 to 9 report what was built and
run since. On the card so far (2026-10-06): the memory half and its copy test (section 8), and the driver's first
work, two fills, through submit and wait (section 9). Rendering has not run there. The other project's claims are its own and unverified.

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

0. **Build the patched Mesa on a Mac and run its own tests.** Done 2026-10-06, section 6.
1. **The interface in this kext, memory half**: connection, buffers, mapping into the process, the 4-level table
   with caller-chosen addresses, contexts. The engine, which needs no card: done 2026-10-06, section 7. Its
   connection to the kext: section 8, **verified on the card 2026-10-06**: `build/n48nprobe` says "all ok", which
   includes a pattern the GPU's copy engine carried between three buffers at addresses the process chose.
2. **Submit and wait** on the graphics queue in the client's address space, section 9. **On the card 2026-10-06:
   `vkprobe` said "done: all ok"** for a fill by the command processor and one by a compute shader, each read
   back. Still to run there: `vkprobe`'s triangle, rendered into an image and read back
   (`docs/todo-vulkantest.md`). This was the step that met the open blocker.
3. **Present** through the flip path. Shows: a Vulkan program's picture on a display.
4. Then the Metal side of `docs/metal-phase-plan.md`, with Vulkan as the executor.

## 5. Open questions

- **One graphics stack, not two.** Their kext and this one both bring the card's graphics engines up; they cannot
  both run. Taking their approach as a reference means writing the interface here, on this repository's bring-up.
- **The service name.** Settled: the interface is a user client of type `'N48N'` on this kext's own service,
  `RDNA4ComputeService`, and a seventh patch makes Mesa look for that name before `Navi48Bringup`.
- **One client or many.** Their interface serves one process at a time, which is enough for a Vulkan test and for a
  single host process on top. Many clients is this repository's design; it can come later.
- **Whose work this touches.** The compute and graphics bring-up, the runtime and the address-space code are
  m1guer's. Steps 1 and 2 change them.
- **What a client may reach.** The kernel queue runs in address space 0 and can address all of VRAM. A client's
  command buffers have to be submitted unprivileged and in the client's own address space, as Linux submits them
  (`docs/w12k-gfx-submit.md`), or the interface gives a process the whole card's memory.

## 6. Step 0: the port builds and runs against its stand-in kext (2026-10-06)

On an Apple-silicon Mac (macOS 26.5). `vulkan/build-mesa.sh <directory>` does all of it from an empty directory;
that script was run end to end.

- Mesa at `f5cb8ee0` fetched; the five patches applied with `git am` without a conflict.
- The AMD Vulkan driver alone builds, for arm64 and cross-built for x86_64, the card's machines.
- **Two things in the build were tied to newer macOS, and neither is needed.** The port's own code opened its
  IOKit connection with a symbol that only exists from macOS 12 on: a driver built for macOS 11 would have read
  through a null address there. `vulkan/mesa-patches/0006` replaces it (one line). And Mesa's window-system layer
  for macOS brings in Apple's Metal and two more macOS 12 symbols: the driver builds without that layer
  (`-Dplatforms=`), and the port presents through its own calls anyway.
- The result, as the script builds it: `libvulkan_radeon.dylib`, x86_64, minimum macOS 11.0, no availability
  warning in the build, linked against IOKit, libSystem, libz and libc++ and nothing else. It was run on macOS
  26.5 under Rosetta only; **macOS 11 itself has not run it.**
- The patches' own test of the driver-to-kext marshalling (`acd_kext_test.c`): 95 checks, 0 failures.
- One defect left alone: with Mesa's tests switched on, two of its AMD tests fail to link, because the patches
  make `libamd_common` need IOKit and those tests do not ask for it. The driver is not affected; the script
  leaves the tests off.
- `vulkan/vkprobe.c`, a small program that drives the driver through its entry point with no Vulkan loader, ran
  with `RADV_DARWIN_FAKE=1 RADV_DARWIN_MOCK=1` (Mesa's in-process stand-in for the kext, which checks arguments
  the way their kext does but runs nothing): the driver offers `AMD Radeon RX 9070 XT (RADV GFX1201)`, Vulkan
  1.4.363, heaps of 15448, 512 and 168 MiB (the stand-in's figures); device creation, a buffer with mapped
  memory, a command buffer, a submit and a fence wait all succeed. The fill it submits is not seen, as expected
  without a GPU.

What that run asked of the kext interface (`RADV_DARWIN_TRACE=1`), which is the whole list a first Vulkan program
needs:

| Call | Times | When |
|---|---|---|
| `Hello` (0) | 2 | once per connection: the driver opens one to look at the device and one for the device it creates |
| `QueryInfo` (1) | 3 | memory sizes, address ranges, limits |
| `ReadRegs` (2) | 2 | one register, `GB_ADDR_CONFIG` |
| `Ctx` (6) | 10 | contexts allocated and freed |
| `BoCreate` (3) / `BoFree` (4) | 10 / 10 | the driver's own buffers and the program's one |
| `GemVa` (5) | 20 | each buffer mapped at an address the driver chose, and unmapped |
| `Submit` (7) | 1 | two command buffers in one call, with a fence |
| `WaitSeq` (8) | 1 | |

Plus a CPU mapping of buffers (`IOConnectMapMemory64`), which the trace does not list. Nothing else: no present
calls, no host-memory import. So steps 1 and 2 of section 4 need exactly selectors 0 to 8 and the mapping. The
stand-in (`src/amd/common/darwin/ac_darwin_mock.c` in the patched tree, about 800 lines) states the rules each
call has to enforce, argument by argument; it is the closest thing to a specification of the kernel half.


## 7. Step 1, first part: the engine, with the real driver on top of it (2026-10-06)

- `src/vmtree.{hpp,cpp}`: a process's page table, four levels and 48 bits, pages mapped wherever the caller asks,
  directory and table pages taken one at a time from whoever owns the memory. It uses the entry encoding the
  runtime's own table uses (`src/gpuvm.cpp`).
- `src/n48n.{hpp,cpp}`: one client's side of the interface, selectors 0 to 6 (`Hello`, `QueryInfo`, `ReadRegs`,
  `BoCreate`, `BoFree`, `GemVa`, `Ctx`): the buffer table, the mappings, the contexts, and the rules each call
  enforces. The rules are the ones Mesa's stand-in for the kext states; Navi48-MacOS's kernel code was read as a
  reference, none of it is copied. `Submit` and `WaitSeq` answer "unsupported" until step 2. Memory and registers
  come through a small backend interface, so the same code runs in the kext, in the host test and under the
  driver. Both files are compiled into the kext already; nothing there calls them yet.
- `make test` (`testN48N` in `tools/atomdump.cpp`): what each call accepts and refuses (wrong sizes, a call before
  `Hello`, buffers that are malformed or too large, mappings below the first address, across the seam of the
  address space, not canonical, over another mapping, past a buffer's end), buffer placement (the pool the CPU can
  reach, the other one only for a buffer the CPU need not touch, system memory within its caps), and what a
  mapping leaves in the table, read back with the walk the runtime's tests use: the right physical page, the
  permissions asked for, system memory marked as such, uncached when the buffer says so. Freeing a buffer takes
  its mappings out of the table and flushes before the memory is given back; closing leaves nothing behind. Three
  planted faults were caught.
- **The real driver on this engine**: `tools/n48n-host/run.sh <build directory>` inserts a small library into
  `vulkan/vkprobe.c` that answers the driver's IOKit calls (find the service, open it, call a method, map a
  buffer) with the engine over ordinary memory. RADV's device creation, its ten buffers, eleven mappings, nine
  context calls and the CPU mapping of the program's buffer all go through `src/n48n.cpp` and succeed; the first
  connection closes with nothing left behind; `vkQueueSubmit` is refused at selector 7, as it must be for now.

What the kext still has to supply for this half, each a function of the backend interface (`N48N::Backend`):
VRAM from the pool the CPU can reach and from the one it cannot, system memory with the physical address of each
page, 4 KiB pages for the table with a CPU pointer to each, the registers of the client's address space pointed
at the table's root, a translation-cache flush, the four `GB_ADDR_CONFIG` registers, and a user client that
carries the calls and maps a buffer into the process. One connection at a time.

## 8. Step 1, second part: the engine connected to the kext (2026-10-06, verified on the card)

`src/n48nkext.cpp` is all of it; the runtime's own files change by a few lines (declarations in
`src/compute.hpp` and `src/userclient.hpp`, one call in the wake's cleanup in `src/runtime.cpp`, one packet in
`src/sdma.{hpp,cpp}`).

| What the engine asks for | What stands behind it |
|---|---|
| The connection | `RDNA4ComputeService::newUserClient`: type `'N48N'` gets an `RDNA4VulkanClient`, any other type the compute client as before. Root only. One at a time: a second open answers "exclusive access", which Mesa waits out |
| VRAM the CPU reaches | the runtime's pool heap behind BAR0 (about 96 MiB), zeroed by the CPU |
| VRAM it does not | the device heap past the BAR; a buffer that allows it goes there first, because the other pool is small |
| System memory | wired pages, zeroed, with the address the card reaches each page by (as the runtime's host buffers) |
| The page table | 4 KiB pages of the pool heap, read and written by the CPU through the BAR |
| The address space | number 8: the table's root, the range and the enable in that context's registers (`vmContextInit`), and after every change of the table an HDP flush and a flush of that address space's translation caches (`vmInvalidate`) |
| A buffer in the process | `clientMemoryForType`: the system pages, or that part of BAR0 |
| `GB_ADDR_CONFIG` | read from the card |

It needs the compute bring-up (`rdna4-compute=6` or `7`) with its copy engine and DMA working, and refuses to open
with `rdna4-vm=1`, whose clients use the same address-space numbers.

**The proof on the card** is a call of this kext's own, outside the Vulkan interface
(`include/rdna4vulkan.h`): the kernel's copy queue, which stays in address space 0, is given a command buffer to
run **in the client's address space** and copies between two ranges the client mapped. That is how Linux runs a
process's copy work, and it is the same idea step 2 needs on the graphics queue, tried first where a failure is
easy to read. The kext looks both ranges up in the client's table before it lets the card try.
`vulkan/n48nprobe.c` (`make n48nprobe`) uses it: a buffer of each kind, mapped at 4 GiB, at the top of the lower
half and in the upper half of the address space; a pattern written by the CPU, copied by the GPU from the first to
the second to the third and back, and compared.

Checked on a host: `make test` has the new packet against Linux's encoding; `tools/n48n-host/run.sh` runs
`n48nprobe` on the engine with the CPU standing in for the copy engine (all ok; a planted fault in the copy is
caught) and then the real driver with the seventh patch, as before.

**On the card, 2026-10-06** (Big Sur 11.6.6, kext `F8A4B018`, both displays lit, log
`rdna4fb-diag-20261006-210519`): the bring-up finished at stage 7 with the firmware in the build, and `n48nprobe`
ended in "all ok". Address space 8 with its table at physical 0xa001000; a buffer behind the BAR, one past it and
one in system memory at 0x100000000, 0x7ffe00000000 and 0xffff800000200000; three copies of 256 KiB by the copy
engine in that address space, each pattern checked by the CPU; everything given back at the close. So on this
card, without the firmware scheduler: a four-level table written through the BAR, the context's registers set
over MMIO, system pages reached through their table entries, and a kernel queue running a command buffer in
another address space all work. A first try the same day reached none of this: its kext had been built without
the firmware (`docs/todo-vulkantest.md`).

**Known limits**

- A process keeps its CPU mapping of a buffer after freeing it, and the table's pages come from the same pool. For
  root that is nothing new; it has to be closed before anyone else may open the interface.
- Sleep: with `rdna4-pm=1` the wake's cleanup drops the client and its calls answer "not ready"; without it the
  runtime does not survive sleep at all, as before.
- A copy the card cannot finish leaves the copy engine stopped until the next boot (the runtime has no reset for
  it). The display does not depend on it.
- The table is read and written in place through the uncached BAR; mapping a large buffer costs tens of
  milliseconds. VRAM past the BAR is not zeroed.

## 9. Step 2: submitting work and waiting for it (2026-10-06, fills verified on the card)

**The engine** (`src/n48n.cpp`, selectors 7 and 8) decides what a submission may be, with the rules of Mesa's
stand-in: 1 to 64 command buffers for the graphics queue, each dword-aligned, at most 20 bits of dwords long, and
lying end to end in mappings the client made executable (a command buffer the card cannot read would stop the queue
for everyone); a context the client allocated; an optional fence, eight bytes in one of its buffers. Each accepted
submission gets the next sequence number. `WaitSeq` waits for a sequence, at most 2 s at a time, and says whether
it is still busy.

- **The fence** is written by the CPU when the kext sees the sequence finished, not by the card. RADV does not
  read it (it waits through `WaitSeq`), and it must therefore be in a buffer the CPU reaches. The stand-in accepts
  any buffer; this is the one rule that is stricter here.
- **Lost work.** Ten seconds without the card finishing anything (Linux gives its graphics queue the same) and
  the work is lost: the call that notices answers "timeout", every later submit and wait "aborted", `QueryInfo`
  carries the flag. Freeing and closing still work. Mesa turns both answers into a lost device.
- **Freeing under work in flight.** `BoFree`, an unmap and the close first wait for everything on the queue. At
  most 16 submissions are on the queue at a time; the 17th waits for the oldest.

**On the card** (`src/n48nkext.cpp`) the work goes onto the kernel's graphics ring the way the runtime puts its
own clients' there (`gfxClientEmit`, `docs/w12k-gfx-submit.md`): the ring stays in address space 0; a
`CONTEXT_CONTROL`, one `INDIRECT_BUFFER` packet per command buffer naming address space 8, and a `RELEASE_MEM`
that writes the sequence to a dword of the kext's. No page-table packets on the ring: the address space is fixed
and set up over MMIO (section 8). It needs the graphics ring up, `rdna4-gfx=2`; without it a submit answers
"unsupported". Lost work halts the ring's two microengines (`gfxClientWedge`) until the next boot. The compute
queues and the display are not involved.

**Checked on a host.** `make test`: nineteen malformed submissions refused and none of them reaching the queue; a
good one across two mappings handed over as given; the fence appearing only when the work has finished; a wait
that gives up after exactly its timeout; a full queue and a ring with no room waited out; a buffer freed under
work in flight; lost work noticed after 10 s, in the fifth 2 s wait, and what follows it. Four planted faults
were caught. `tools/n48n-host/run.sh`: the real driver now gets through `vkQueueSubmit`, `vkWaitForFences`,
`vkDeviceWaitIdle` and its teardown on the engine (the stand-in queue reports everything finished at once), and
closes with nothing left behind. `vulkan/vkprobe.c` checks two fills, which only a GPU can make true.

**On the card, 2026-10-06** (the boot of section 8; `vkprobe.txt`, no kernel log taken after it): the driver
opened the interface, created its device and submitted twice; a 1024-byte fill by the command processor's own
copy and a 61440-byte fill by a compute shader both came out right, the bytes between them untouched. So this
kext's graphics ring, programmed directly and without the firmware scheduler, runs the driver's command buffers
in another address space, a shader the driver compiled included. That is what `docs/metal-readiness.md` section 4
names as the open blocker, met by another route than the one that failed there: the kernel's own ring with the
address space in the packet, not a queue per client.

**What it had not met when written, and what the comparison showed.** Whether the ring runs RADV's command
buffers in another address space could only be answered on the card (it does, above). Three facts framed that,
found by comparing with Navi48-MacOS's native
path (`amd/native_s1b.cpp`, `amd/native_s1c_pure.h`, `amd/amdgpu_init.cpp`) and with this repository's own notes:

- **The packets are the same.** Their submit is `CONTEXT_CONTROL`, one `INDIRECT_BUFFER` per command buffer with
  the address space in bits 27:24 and no VALID bit, then `RELEASE_MEM`: what the runtime's `gfxClientEmit`
  emits and what this backend emits. Their `CONTEXT_CONTROL` carries the load enable and a zero shadow word; the
  backend now does the same (the runtime's own uses 0x80000000 for both). Their address space is set up as here:
  depth 3, block size 0, no retry on faults, the whole 48-bit range, over MMIO.
- **Their graphics queue is mapped by the firmware scheduler (MES), as Linux's is; this kext's is not.** They ask
  the scheduler to map the queue (`ADD_QUEUE` with `map_legacy_kq`) and keep address space 8 outside the
  scheduler's own range. This kext programs the ring directly and starts no scheduler. For the two fills it did
  not matter; rendering has not been tried.
- **This kext's graphics ring has run on a card, in address space 0 only.** `docs/HANDOFF-linux.md` records its
  ring test and a draw whose shader ran (the picture was wrong for a reason found later under Linux); on
  2026-10-06 the ring came up on this rig and the kext's own triangle was right. No card log
  has a command buffer in another address space on it: the runtime's clients do that in the emulator only, and
  its first design, a compute queue per client, failed on the card for a reason that was never found
  (`docs/metal-readiness.md` section 4).

The copy test of section 8 is there to tell a broken address space from a broken graphics queue.

**The card boots, in order** (each needs the firmware in the build, `tools/fetch-firmware.sh`):

1. `rdna4-compute=7 rdna4-trace=1`: the bring-up's own results first (`compute:` lines, `runtime: PASS service
   ready`), then `sudo build/n48nprobe`.
2. The same plus `rdna4-gfx=2`: `sudo ./vkprobe ./libvulkan_radeon.dylib`, with the library
   from `vulkan/build-mesa.sh` copied over. `docs/real-card-plan.md` ran its graphics-ring boots with
   `rdna4-ih=2 rdna4-hang=1` as well.

The guide for both boots, with what to look for and what failure looks like: `docs/todo-vulkantest.md`.
