# Window and animation latency experiment

This branch is an experimental build line for investigating slow window movement and UI stalls on macOS Tahoe.

## First-pass instrumentation

The driver reports time spent in the card-side present path, CPU scanout fallback, previous-frame fence waits, and global in-flight capacity checks. Existing GPU submit, fence, work-loop tick, display polling, and frame-gap counters remain available.

## Experimental behavior: nonblocking present

This pass changes the PVGPU present path to prefer dropping a frame over blocking the host work loop on a present-related fence:

- If a head's previous present still owns its scanout destination, the new present is skipped. The driver does not write to or reuse that destination until the previous fence completes.
- If the global in-flight queue remains full after checking completed work, the new present is skipped rather than waiting synchronously for the oldest queue fence.
- The next present event retries normally. Existing GPU fences, queue ordering, buffer ownership, and memory-safety checks are retained.
- A skipped frame is treated as handled by the card-side path, preventing the slower CPU fallback from immediately doing another blocking copy for that same present event.

This is a latency-versus-throughput experiment. It may make pointer/window movement feel less frozen under GPU back-pressure, but can increase visible frame drops or judder. It is not guaranteed to improve average frame rate, and it may make animation less smooth if the GPU is consistently overloaded.

The five-second report adds counts of frames skipped because a head was busy or because the global queue remained full, both as totals and broken down by display port. A rise on one port points toward that head's scanout/fence pressure; queue-full drops across several ports point toward shared GPU submission pressure. Compare these counts with the existing missed-frame, underflow, fence-wait, and work-loop timing statistics. The counters reset with each five-second reporting window, so compare equivalent windows rather than treating them as lifetime totals.

## Why required synchronization stays

Apple's Metal guidance recommends overlapping CPU and GPU work and avoiding unnecessary CPU waits, but also requires synchronization where resources are shared and accessed concurrently. This experiment does not remove the fences that protect scanout destinations or reuse in-flight command buffers. It skips work when those resources are still busy.

## Test procedure

Compare the same desktop, display mode, refresh rate, and window drag/minimize/restore sequence across the baseline and experimental builds. Record several five-second reports. Check whether the UI stalls become shorter and whether frame-drop counts or display underflows rise.

## Build and safety status

The separate experimental workflow builds with PVGPU=1 and publishes a prerelease ZIP under the tahoe-exp-v* tag family. The regular tahoe-v* release workflow remains unchanged.

CI compilation and host-side tests do not establish hardware compatibility. This branch has not been validated on an RX 9070 XT or macOS Tahoe 26.7.3. Keep a known-good boot entry and do not treat this as a production driver.