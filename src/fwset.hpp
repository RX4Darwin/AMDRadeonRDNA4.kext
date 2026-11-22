//
//  fwset.hpp
//  AMDRadeonRDNA4
//
//  Which firmware set a card takes. The PSP checks each blob's signature, so a
//  chip only accepts the set made for it: Navi 48 (GC 12.0.1) takes
//  psp_14_0_3 / smu_14_0_3 / sdma_7_0_1 / gc_12_0_1_*, Navi 44 (GC 12.0.0)
//  takes psp_14_0_2 / smu_14_0_2 / sdma_7_0_0 / gc_12_0_0_*. The display
//  microcontroller's firmware (dcn_4_0_1_dmcub) is the same for both.
//
//  The set is chosen from the GC version in the card's IP discovery table, the
//  way amdgpu names the files it asks for (gc_%d_%d_%d), not from the PCI id.
//
//  Navi 44's versions were read from an RX 9060 XT under Linux (1002:7590,
//  revision 0xC0): GC 12.0.0, MP0 and MP1 14.0.2, SDMA 7.0.0, DMU 4.1.0.
//
//  Freestanding (constexpr only): the static_asserts below are its test.
//

#ifndef FwSet_hpp
#define FwSet_hpp

#include <stdint.h>

namespace FwSet {

enum class Family : uint8_t {
	None,     // a GC version there is no set for
	Navi48,   // RX 9070 XT, RX 9070, RX 9070 GRE, AI PRO R9700
	Navi44,   // RX 9060 XT, RX 9060
};

constexpr Family forGc(uint8_t major, uint8_t minor, uint8_t revision) {
	if (major != 12 || minor != 0)
		return Family::None;

	return revision == 1 ? Family::Navi48 : revision == 0 ? Family::Navi44 : Family::None;
}

constexpr const char *name(Family family) {
	return family == Family::Navi48 ? "Navi 48" : family == Family::Navi44 ? "Navi 44" : "unknown";
}

// The file names' stems, for the log.
constexpr const char *gcStem(Family family) {
	return family == Family::Navi48 ? "gc_12_0_1" : family == Family::Navi44 ? "gc_12_0_0" : "none";
}

constexpr const char *pspStem(Family family) {
	return family == Family::Navi48 ? "psp_14_0_3" : family == Family::Navi44 ? "psp_14_0_2" : "none";
}

static_assert(forGc(12, 0, 1) == Family::Navi48, "GC 12.0.1 is Navi 48");
static_assert(forGc(12, 0, 0) == Family::Navi44, "GC 12.0.0 is Navi 44");
static_assert(forGc(12, 0, 2) == Family::None, "an unknown GC 12 revision takes no set");
static_assert(forGc(11, 0, 0) == Family::None, "RDNA 3 takes no set");
static_assert(forGc(0, 0, 0) == Family::None, "an unread GC version takes no set");

} // namespace FwSet

#endif /* FwSet_hpp */
