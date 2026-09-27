//
//  bochsvbe.hpp
//  RDNA4FB
//
//  VM test builds only: mode switching on QEMU's vmware-svga through the
//  Bochs VBE "DISPI" interface (I/O ports 0x1ce/0x1cf) that QEMU's VGA core
//  exposes on it. OVMF's GOP drives the card the same way, so the boot
//  framebuffer macOS inherits is a DISPI mode at BAR1 offset 0. The SVGA II
//  register interface is not used: QEMU scans out an SVGA mode only once the
//  FIFO is configured, and then redraws only on FIFO update commands, which
//  IONDRVFramebuffer never sends. A DISPI mode is redrawn from VRAM dirty
//  tracking, like the GOP mode.
//
//  Modes keep the boot surface's pitch: the virtual width stays the boot
//  width, only the visible size changes — the same rule the AMD path
//  follows (see device.hpp).
//

#ifndef BochsVbe_hpp
#define BochsVbe_hpp

#include <stdint.h>

namespace BochsVbe {

// True if the DISPI interface answers with a known ID.
bool present();

// Show `width` x `height` at 32 bpp from VRAM offset 0, with lines
// `virtWidth` pixels apart. VRAM is not cleared. Returns false if the device
// did not take the mode exactly (the caller then restores a known mode).
bool setMode(uint16_t width, uint16_t height, uint16_t virtWidth);

} // namespace BochsVbe

#endif /* BochsVbe_hpp */
