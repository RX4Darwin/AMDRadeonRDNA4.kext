//
//  drr.cpp
//  AMDRadeonRDNA4
//

#include "drr.hpp"

#include "drr_linux.inc"

namespace Drr {

namespace {

struct Steps {
	const Step *steps;
	size_t      count;
};

#define DRR_PIPE(p) { { kPipe##p##On, sizeof(kPipe##p##On) / sizeof(Step) }, \
                      { kPipe##p##Off, sizeof(kPipe##p##Off) / sizeof(Step) }, \
                      { kPipe##p##Trigger, sizeof(kPipe##p##Trigger) / sizeof(Step) } }
const Steps kTable[][3] = { DRR_PIPE(0), DRR_PIPE(1), DRR_PIPE(2), DRR_PIPE(3) };
#undef DRR_PIPE
constexpr size_t kPipes = sizeof(kTable) / sizeof(kTable[0]);

bool gActive[kPipes];

} // namespace

size_t build(uint8_t pipe, Phase phase, uint32_t vTotalMin, uint32_t vTotalMax, ModeSet::Step *out, size_t max) {
	if (pipe >= kPipes)
		return 0;
	const Steps &s = kTable[pipe][static_cast<int>(phase)];
	if (s.count > max || (phase == Phase::On && (!vTotalMin || vTotalMax < vTotalMin)))
		return 0;
	for (size_t i = 0; i < s.count; i++) {
		const Step &d = s.steps[i];
		const uint32_t value = d.kind == kVMin ? vTotalMin - 1 : d.kind == kVMax ? vTotalMax - 1 : d.value;
		out[i] = ModeSet::Step { d.mask == ~0u ? ModeSet::Op::Write : ModeSet::Op::Update, d.seg, false, d.dword, d.mask,
		                         value & d.mask, 0, "drr" };
	}
	return s.count;
}

void setActive(uint8_t pipe, bool on) {
	if (pipe < kPipes)
		__atomic_store_n(&gActive[pipe], on, __ATOMIC_RELEASE);
}

bool active(uint8_t pipe) {
	return pipe < kPipes && __atomic_load_n(&gActive[pipe], __ATOMIC_ACQUIRE);
}

} // namespace Drr
