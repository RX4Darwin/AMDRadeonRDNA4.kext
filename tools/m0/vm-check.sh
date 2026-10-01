#!/bin/bash
# M0 check, to run INSIDE the full-install guest after a boot with rdna4-pvgpu=1 (docs/m0-pvgpu.md, "Tests B"). Read-only.
#   ssh -p 10122 user@127.0.0.1 'bash -s' < tools/m0/vm-check.sh > m0-vm-check.txt
# Sections: our nub in the registry, Apple's classes, loaded kexts, Apple's own log (subsystem com.apple.gpusw.AppleParavirtGPU), the accelerator.
echo "== uname / vm"; uname -a; sysctl kern.hv_vmm_present 2>&1
echo "== our nub (ioreg -n PVGPU)"; ioreg -l -w0 -n PVGPU 2>&1 | head -60
echo "== Apple's control driver and framebuffer (AppleParavirtGPUControl, AppleParavirtFramebuffer)"
ioreg -l -w0 -c AppleParavirtGPUControl 2>&1 | head -80
ioreg -l -w0 -c AppleParavirtFramebuffer 2>&1 | head -40
echo "== accelerator (IOAccelerator, AppleParavirtAccelerator)"
ioreg -l -w0 -c IOAccelerator 2>&1 | head -80
echo "== loaded kexts"; kmutil showloaded 2>/dev/null | grep -i "paravirt\|RDNA4\|IOAccel\|IOGraphics" || kextstat | grep -i "paravirt\|RDNA4\|IOAccel"
echo "== Apple's own log (last 10 minutes)"
log show --last 10m --style compact --predicate 'subsystem == "com.apple.gpusw.AppleParavirtGPU" OR eventMessage CONTAINS[c] "paravirt"' 2>&1 | tail -150
echo "== display adapters seen by the system"; system_profiler SPDisplaysDataType 2>&1 | head -40
echo "== Metal devices"; swift -e 'import Metal; print(MTLCopyAllDevices().map{$0.name})' 2>&1 | tail -3
