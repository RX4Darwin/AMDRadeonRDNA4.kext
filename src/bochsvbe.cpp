//
//  bochsvbe.cpp
//  RDNA4FB
//
//  See bochsvbe.hpp. Register numbers and the programming sequence follow
//  QEMU's hw/display/vga.c and OVMF's QemuVideoDxe (InitializeBochsGraphicsMode).
//

#include "bochsvbe.hpp"

namespace BochsVbe {

namespace {

constexpr uint16_t kIndexPort = 0x01ce;
constexpr uint16_t kDataPort  = 0x01cf;

enum : uint16_t {
	kRegId         = 0x0,
	kRegXRes       = 0x1,
	kRegYRes       = 0x2,
	kRegBpp        = 0x3,
	kRegEnable     = 0x4,
	kRegBank       = 0x5,
	kRegVirtWidth  = 0x6,
	kRegVirtHeight = 0x7,
	kRegXOffset    = 0x8,
	kRegYOffset    = 0x9,
};

constexpr uint16_t kIdMin       = 0xb0c0;
constexpr uint16_t kIdMax       = 0xb0c5;
constexpr uint16_t kEnabled     = 0x01;
constexpr uint16_t kLfbEnabled  = 0x40;
constexpr uint16_t kNoClearMem  = 0x80;

inline void outw(uint16_t port, uint16_t value) {
	__asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

inline uint16_t inw(uint16_t port) {
	uint16_t value;
	__asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

void write(uint16_t reg, uint16_t value) {
	outw(kIndexPort, reg);
	outw(kDataPort, value);
}

uint16_t read(uint16_t reg) {
	outw(kIndexPort, reg);
	return inw(kDataPort);
}

} // namespace

bool present() {
	uint16_t id = read(kRegId);
	return id >= kIdMin && id <= kIdMax;
}

bool setMode(uint16_t width, uint16_t height, uint16_t virtWidth) {
	if (!width || !height || virtWidth < width)
		return false;
	write(kRegEnable, 0);
	write(kRegBank, 0);
	write(kRegBpp, 32);
	write(kRegXRes, width);
	write(kRegYRes, height);
	write(kRegEnable, kEnabled | kLfbEnabled | kNoClearMem);
	// Enabling resets the virtual width to XRES and the offsets to 0
	// (QEMU vga.c), so the pitch goes in afterwards. QEMU derives the
	// virtual height from VRAM size itself.
	write(kRegVirtWidth, virtWidth);
	write(kRegXOffset, 0);
	write(kRegYOffset, 0);
	// QEMU clamps what it cannot show; read back to see what it took.
	return read(kRegXRes) == width && read(kRegYRes) == height &&
	       read(kRegVirtWidth) == virtWidth && read(kRegBpp) == 32 &&
	       (read(kRegEnable) & kEnabled);
}

} // namespace BochsVbe
