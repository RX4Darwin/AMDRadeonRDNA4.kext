# Who may open the GPU? Client access policy for many clients (research + proposal)

Written 2026-10-01 at the user's request ("I don't know much about these parts, let's research and build a design"), as an input to the
Metal-phase item the lead recorded. **Nothing here is implemented**, and it does not change the lead's decision that the user client stays
root-only for pre-Metal. Tags: **[SOURCED]** from a page I read (links at the end), **[READ]** in this repo, **[MEMORY]** from my general
knowledge and *not verified here*, **[INFER]**, **[UNKNOWN]**.

## 1. The question in plain words

Today only the administrator account (root) can talk to the GPU driver. Metal will need every app, and the window system, to talk to it.
Two things decide whether that is allowed and safe:

1. **Who is let in** — the driver's own check, plus macOS's sandbox around the app.
2. **What a client can do once it is in** — whether one client can read or break another's data, hang the GPU for everyone, or eat all memory.

W13 (per-client GPU address spaces, per-client job submission, hang containment) is what makes point 2 manageable; this note is about point 1.

## 2. What the code does today [READ]

`RDNA4ComputeClient::initWithTask` (`src/userclient.cpp:47-52`) calls `clientHasPrivilege(securityToken, kIOClientPrivilegeAdministrator)`:
only root can open. The comment gives the reason: *"A kernel runs in VMID0 and can reach all of VRAM"*. That reason stops being true for
a client that runs only inside its own address space (VMID ≠ 0 tables, unprivileged IBs), which is exactly what W12k/W13 build, **but**:

- the per-client isolation has only been shown in the emulator; on the real card every client path currently fails (`docs/w13-vmid.md` s.1.1);
- without the VM (`rdna4-vm=0`) a client's kernel runs in VMID 0 with access to all VRAM, so the root-only check is still the only protection there;
- the same user client also exposes sensors, a sleep test, a quiesce call, display present and raw copies (`userclient.cpp:75-96`): not things to hand to every process.

## 3. What macOS offers for deciding who gets in

| Mechanism | What it is | Fit |
|---|---|---|
| `clientHasPrivilege(token, name)` with `kIOClientPrivilegeAdministrator` ("root"), `kIOClientPrivilegeLocalUser` ("local"), `kIOClientPrivilegeForeground` ("foreground") | The kernel-side check in `IOUserClient.h`. [SOURCED: xnu `IOUserClient.h`] | Root is what we use. *Local user* = a user logged in at the console, *foreground* = the foreground console session. They say nothing about which program it is. |
| `IOUserClient::copyClientEntitlement(task, "key")` | The driver reads an entitlement from the connecting program's code signature and decides. [SOURCED: same header; example of a kext checking `com.apple.private.iokit.nvram-csr`] | Lets the driver say "only programs signed with entitlement X". Needs the program (or the Metal stack that opens the client on its behalf) to carry that entitlement, and the system to honour a custom one: on a Hackintosh that depends on the AMFI/SIP settings [UNKNOWN for this machine]. |
| App Sandbox `iokit-open` rules | A sandboxed process can only open IOKit user-client classes its profile names. The default profile allows a short hard-coded list; a GPU class must be added explicitly, e.g. `(iokit-open (iokit-user-client-class "AGXDeviceUserClient"))` on Apple silicon. [SOURCED: two issue threads about Metal failing inside a sandbox] | A sandboxed app cannot open **our** class unless the sandbox profile allows its name. Apple's profiles name Apple's classes. [INFER] Which names Tahoe's profiles allow for the AMD accelerator stack is [UNKNOWN] and is the first thing to find out (section 6). |
| DriverKit entitlements (`com.apple.developer.driverkit.userclient-access`, `allow-any-userclient-access`) | How a *driver extension* (dext) controls who may connect. [SOURCED: Apple developer forums] | Not applicable: this driver is a kext (Lilu plugin), and a GPU accelerator is not something DriverKit offers. Listed so nobody wonders. |

Two more facts that matter:

- Metal apps do not open the GPU driver's client themselves; Apple's Metal stack does, inside the app's process, through Apple's IOAccelerator classes [MEMORY, the
  search did not return a source]. So the "client" that must be let in is usually **Apple's Metal code running as the app's user, inside the app's sandbox**. That Metal
  stack is what needs the class names and properties to match [UNKNOWN in detail; the lead's Metal-readiness doc owns this].
- WindowServer runs as its own unprivileged user, not root [MEMORY]. `ps -o user,pid,command -p $(pgrep WindowServer)` on the test Mac settles it. If true, a root-only check
  would already block the window system from using the accelerator.

## 4. The security side (why not just "allow everyone")

Graphics drivers reachable from sandboxed processes are a classic way out of the sandbox: published attack chains go from a sandboxed Safari renderer through a graphics
kext's user client to the kernel. [SOURCED: ret2 "Exploiting Intel Graphics Kernel Extensions on macOS"; Black Hat 2016 "Subverting Apple Graphics"]. A hobby kext that opens
its client to every process enlarges that surface. On a single-user personal machine this is an accepted-risk decision, not a blocker, but it should be made on purpose.

Conditions I would require **before** widening access (each is a W13-family item or a checklist line, not new research):

1. Isolation proven on the card for at least two clients (U1, U2, final boot plan in `docs/w13-vmid.md` s.8.1) and the client-path failure understood.
2. A client can only reach the selectors a graphics client needs: no sensors/quiesce/sleep-test/raw VRAM copy; `present` only for the window-server client.
3. Per-client quotas enforced in the kernel: buffers, programs, host-pinned bytes (already per-client and total caps exist, `compute.hpp:679-681`, but `kMaxBuffers`/`kMaxPrograms` are global), queue depth (`kMaxIbOutstanding`), VA.
4. Every user pointer handled through the task-checked copy paths (already the pattern, `userCopy`), IB sizes and VA ranges validated (done for `rtSubmitIb`).
5. One client's hang or fault never stops another (W13 per-queue wedge, kill-by-VMID recovery).
6. An easy global off switch: a boot-arg that returns the client class to root-only.

## 5. Proposal

Two user-client classes instead of one:

- **Runtime client** (today's `RDNA4ComputeClient`): stays root-only, keeps every selector, used by `rdna4-run`, the bench and the diagnostics.
- **Accelerator client** (Metal phase): a second class with the restricted selector set (point 2 above) and the per-client quotas, opened by the Metal stack.
  Access policy in three steps, each independently reversible:
  1. *Start*: `kIOClientPrivilegeLocalUser` (any logged-in local user, including the window server), no entitlement; a boot-arg `rdna4-client-policy=root|local` (default `root` until the
     isolation is proven on the card).
  2. *Then*, if the sandbox turns out to block the Metal stack, add the class name to whatever allow-list Tahoe's profile reads, or use the route the existing AMD stack uses (section 6 finds it).
  3. *Optionally*, an entitlement requirement (`copyClientEntitlement`) for the display-owning client only, if AMFI on this machine allows a custom one.
  Identify each client by its task at `initWithTask` (process id, audit token), record it in the per-client object (W13 s.5.1), log it, and apply the quotas there.

Why not a root helper that proxies everything: the window system and every Metal app would pay an extra process hop per command buffer and the helper becomes a single point of failure; I
would only use it if the sandbox cannot be satisfied any other way.

## 6. What to find out on the test Mac (read-only, about an hour, no risk) [UNKNOWN until done]

1. `ps -o user,pid,command -p $(pgrep WindowServer)`: which user runs it.
2. `ioreg -l -w0 -c IOAccelerator` and `ioreg -l -w0 | grep -i "UserClient"` on a Mac with a working AMD GPU (or read the Info.plist of Tahoe's `AMDRadeonX6000.kext`/`AMDRadeonX6000HWServices.kext` if present): which user-client
   classes Apple's AMD stack registers, and the `IOUserClientClass` values.
3. `log show --last 5m --predicate 'eventMessage CONTAINS "iokit-open"'` after launching a Metal app: which classes the sandbox allows or denies for it.
4. `nvram boot-args` / OpenCore `csr-active-config`, `amfi_get_out_of_my_way`: whether custom entitlements can work here at all.
5. Whether the kernel's built-in sandbox policy can be read or extended on this setup (macOS no longer ships the profiles as plain files) [MEMORY].

## 7. What I need from the user

Only one real decision, in plain terms: **should any program on the machine be able to use the GPU accelerator, or only ones you approve?** My recommendation for a
personal machine is the first, behind the `root|local` boot-arg, because Metal apps cannot work otherwise, plus the quota and isolation conditions in section 4. Nothing needs deciding until the
isolation is proven on the card; the default stays `root`.

## Sources

- xnu `IOUserClient.h` (privilege names, `clientHasPrivilege`, `copyClientEntitlement`): <https://fergofrog.com/code/codebrowser/xnu/iokit/IOKit/IOUserClient.h.html>
- A kext checking an entitlement (Pike R. Alpha): <https://pikeralpha.wordpress.com/2015/09/16/adding-an-entitlement-check-to-filenvram-kext/>
- Apple developer forums, which entitlements an IOUserClient connection needs: <https://developer.apple.com/forums/thread/774297>
- Sandbox: default profile's hard-coded `iokit-open` classes, Metal needing GPU classes: <https://github.com/anthropics/sandbox-runtime/issues/560>, <https://github.com/openai/codex/issues/17644>
- Graphics kexts as a sandbox-escape surface: <https://blog.ret2.io/2022/06/29/pwn2own-2021-safari-sandbox-intel-graphics-exploit/>, <https://blackhat.com/docs/us-16/materials/us-16-Chen-Subverting-Apple-Graphics-Practical-Approaches-To-Remotely-Gaining-Root.pdf>
