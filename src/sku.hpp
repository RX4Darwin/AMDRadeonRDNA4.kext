//
//  sku.hpp
//  AMDRadeonRDNA4
//
//  Which Navi 48 card this is. The RX 9070 XT, the RX 9070 and the RX 9070 GRE
//  are all device 0x7550 and load the same firmware; the PCI revision tells
//  them apart. The Radeon AI PRO R9700 is device 0x7551. Compute units are from
//  the product specifications, not read from the card.
//
//  The GRE's revision 0xC2 is from a driver database's hardware IDs
//  (PCI\VEN_1002&DEV_7550&...&REV_C2), not from a card: not seen here yet.
//
//  The RX 9070 seen so far is a Sapphire 1DA2:E489 (VBIOS "2E489 Navi48 XT")
//  with the four display paths of the XT's 1E489 board: DP on HPD1/2, HDMI on
//  HPD3/4.
//
//  Freestanding (constexpr only): the static_asserts below are its test.
//

#ifndef Sku_hpp
#define Sku_hpp

#include <stdint.h>

namespace Sku {

constexpr uint16_t kNavi48      = 0x7550;   // RX 9070 XT, RX 9070, RX 9070 GRE
constexpr uint16_t kNavi48Pro   = 0x7551;   // Radeon AI PRO R9700
constexpr uint8_t  kRevRx9070Xt = 0xC0;
constexpr uint8_t  kRevRx9070   = 0xC3;
constexpr uint8_t  kRevRx9070Gre = 0xC2;   // 48 compute units, 12 GB on a 192-bit bus

constexpr bool isRx9070(uint16_t device, uint8_t revision) {
	return device == kNavi48 && revision == kRevRx9070;
}

constexpr bool isRx9070Gre(uint16_t device, uint8_t revision) {
	return device == kNavi48 && revision == kRevRx9070Gre;
}

// What System Report and ioreg show ("model", "GPU,Variant").
constexpr const char *modelName(uint16_t device, uint8_t revision) {
	if (device == kNavi48Pro)
		return "AMD Radeon AI PRO R9700";

	if (isRx9070(device, revision))
		return "AMD Radeon RX 9070";

	if (isRx9070Gre(device, revision))
		return "AMD Radeon RX 9070 GRE";

	return "AMD Radeon RX 9070 XT";
}

// RX 9070 XT and R9700: 64 compute units; RX 9070: 56; RX 9070 GRE: 48.
constexpr uint32_t computeUnits(uint16_t device, uint8_t revision) {
	if (isRx9070(device, revision))
		return 56;

	return isRx9070Gre(device, revision) ? 48 : 64;
}

static_assert(computeUnits(kNavi48, kRevRx9070Xt) == 64, "RX 9070 XT: 64 compute units");
static_assert(computeUnits(kNavi48, kRevRx9070) == 56, "RX 9070: 56 compute units");
static_assert(computeUnits(kNavi48, kRevRx9070Gre) == 48, "RX 9070 GRE: 48 compute units");
static_assert(computeUnits(kNavi48Pro, kRevRx9070Gre) == 64, "R9700: 64 compute units, whatever its revision");
static_assert(computeUnits(kNavi48Pro, kRevRx9070) == 64, "R9700: 64 compute units, whatever its revision");

} // namespace Sku

#endif /* Sku_hpp */
