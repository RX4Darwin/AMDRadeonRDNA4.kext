//
//  ihdecode.cpp
//  RDNA4FB
//
//  Host-testable IH 7.0 entry decoding.
//

#include "ih.hpp"

namespace Ih {

bool missEligible(bool slept, bool completed, bool sourceAdvanced, bool recheckElapsed) {
	return slept && completed && recheckElapsed && !sourceAdvanced;
}

void decode(const uint32_t dw[kEntryDwords], Entry &out) {
	out.clientId = static_cast<uint8_t>(dw[0] & 0xff);
	out.srcId = static_cast<uint8_t>((dw[0] >> 8) & 0xff);
	out.ringId = static_cast<uint8_t>((dw[0] >> 16) & 0xff);
	out.vmid = static_cast<uint8_t>((dw[0] >> 24) & 0xf);
	out.vmidSrc = (dw[0] >> 31) != 0;
	out.timestamp = static_cast<uint64_t>(dw[1]) |
	                (static_cast<uint64_t>(dw[2] & 0xffff) << 32);
	out.pasid = static_cast<uint16_t>(dw[3] & 0xffff);
	out.vmidSrcNode = static_cast<uint8_t>((dw[3] >> 16) & 0xff);
	for (uint32_t i = 0; i < 4; i++)
		out.srcData[i] = dw[4 + i];
}

} // namespace Ih
