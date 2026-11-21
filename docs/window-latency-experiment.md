# Window and animation latency experiment

This branch is an experimental build line for investigating slow window movement and UI stalls on macOS Tahoe.

## First-pass changes

This pass does not alter rendering, scanout, synchronization, fence ordering, or retry policy. It adds timing counters around the existing present path so a real workload can identify which wait is responsible before behavior is changed.

The five-second render report now separates:
- time spent attempting the card-side present path;
- time spent in the CPU scanout fallback;
- time waiting for the previous frame's per-head GPU fence;
- time spent at the global in-flight draw capacity check.

Existing GPU submit, fence, work-loop tick, display polling, and frame-gap counters remain available.

## Why measurement comes first

The current code serializes a head's next present behind its previous present fence before reusing that head's present command buffer. It also applies a global in-flight capacity limit. Those are plausible sources of visible stalls, but changing either without preserving destination-buffer and scanout ordering could create races or corrupted frames.

Apple's Metal guidance explains that unnecessary CPU/GPU synchronization stalls otherwise-overlapping work. That supports investigating waits, not removing required fences blindly.

## Test interpretation

Reproduce the same window-drag and minimize/restore sequence for several seconds, then collect the driver's five-second host: render / host: present report.

- High previous-frame fence wait time points to per-head GPU present back-pressure.
- High in-flight capacity check time points to the shared graphics queue reaching its in-flight limit.
- High CPU fallback time means the card-side present path is failing or unavailable and the CPU copy is expensive.
- High card-path time with low wait time points toward mapping, command construction, or submission overhead. Compare with the existing submit and tick details.

Compare several runs at the same display mode and workload. One run is not proof.

## Build and safety status

The separate experimental release workflow builds with PVGPU=1 and publishes a prerelease ZIP under the tahoe-exp-v* tag family. The regular tahoe-v* release workflow remains unchanged.

A successful CI build and host tests do not establish that this driver is safe or stable on an RX 9070 XT. This branch is not hardware-validated. Keep it separate from the known-good build; do not replace a working system driver with an untested kernel extension.