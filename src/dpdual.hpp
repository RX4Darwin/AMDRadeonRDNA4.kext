//
//  dpdual.hpp
//  AMDRadeonRDNA4
//
//  What a passive DP++ (DP-to-HDMI) adapter can carry. The adapter answers
//  on the connector's DDC pads at I2C address 0x40 with an ID block (VESA
//  DisplayPort Dual-Mode Standard): the string "DP-HDMI ADAPTOR\x04" at 0x00,
//  the adapter ID at 0x10 (0xA0: type 2) and, on a type 2 adapter, its
//  maximum TMDS clock at 0x1D in 2.5 MHz units. A type 1 adapter, or one that
//  does not answer at all, is good for 165 MHz.
//
//  The limits are amdgpu's (link_ddc.c / link_detection.c,
//  query_dp_dual_mode_adaptor): a type 2 clock outside 25-600 MHz is taken
//  as type 1.
//
//  The ID block comes from the cable, i.e. it is untrusted input. Freestanding
//  (no kernel or libc dependencies), so the host test can include it as well.
//

#ifndef DpDual_hpp
#define DpDual_hpp

#include <stdint.h>
#include <stddef.h>

namespace DpDual {

constexpr uint8_t  kI2cAddress   = 0x40;
constexpr size_t   kIdBytes      = 0x20;        // all a type 2 adapter defines
constexpr size_t   kRegId        = 0x10;
constexpr size_t   kRegMaxTmds   = 0x1D;
constexpr uint8_t  kType2Id      = 0xA0;
constexpr uint32_t kSafeMaxKHz   = 165000;      // type 1, or no ID block
constexpr uint32_t kType2MinKHz  = 25000;
constexpr uint32_t kType2MaxKHz  = 600000;
constexpr uint32_t kTmdsStepKHz  = 2500;

constexpr char kSignature[] = "DP-HDMI ADAPTOR\x04";   // 16 bytes, no NUL on the wire

struct Adaptor {
	bool     type2;
	uint32_t maxTmdsKHz;
	char     deviceId[7];   // 0x14-0x19 on a type 2 adapter, NUL-terminated
};

// The adapter behind an ID block of `len` bytes; `len` 0 means it did not
// answer.
inline Adaptor parse(const uint8_t *id, size_t len) {
	Adaptor a { false, kSafeMaxKHz, {} };

	if (!id || len < kIdBytes)
		return a;

	for (size_t i = 0; i < sizeof(kSignature) - 1; i++) {
		if (id[i] != static_cast<uint8_t>(kSignature[i]))
			return a;
	}

	if ((id[kRegId] & 0xF0) != kType2Id)
		return a;

	const uint32_t maxKHz = id[kRegMaxTmds] * kTmdsStepKHz;
	if (maxKHz < kType2MinKHz || maxKHz > kType2MaxKHz)
		return a;

	a.type2 = true;
	a.maxTmdsKHz = maxKHz;

	for (size_t i = 0; i < sizeof(a.deviceId) - 1; i++) {
		const uint8_t c = id[0x14 + i];
		a.deviceId[i] = c >= 0x20 && c < 0x7F ? static_cast<char>(c) : 0;
	}

	return a;
}

} // namespace DpDual

#endif /* DpDual_hpp */
