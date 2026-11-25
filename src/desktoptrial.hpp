#ifndef RDNA4_DESKTOP_TRIAL_HPP
#define RDNA4_DESKTOP_TRIAL_HPP
#include "pipe2.hpp"

namespace DesktopTrial {
struct Work {
	ModeSet::Step change[Pipe2::kMaxModeSteps + 3], saved[Pipe2::kMaxModeSteps + 12];
};
// Both plans are self-contained after build. Capture every overwritten plane field,
// including DML's masked updates, before either plan can reach the register executor.
template <class Read>
bool build(ModeSet::Target target, ModeSet::Target undo, uint64_t mc, uint32_t pitch, uint64_t bytes, Work &work, size_t n,
           Read read, ModeSet::Plan &forward, ModeSet::Plan &back) {
	auto &change = work.change;
	auto &saved = work.saved;
	// The register snapshot below is for pipe zero. Never apply it to a second head.
	if (!target.dp || !undo.dp || target.otg || target.opp || target.hubp || target.dig || undo.otg || undo.opp ||
	    undo.hubp || undo.dig || !target.to.vActive || !pitch || pitch > 65536 || pitch < target.to.hActive ||
	    bytes < static_cast<uint64_t>(pitch) * 4 * target.to.vActive || !n || n >= Pipe2::kMaxModeSteps || !mc ||
	    (mc & 255) || (mc >> 48) || bytes > (1ull << 48) - mc)
		return false;
	auto op = [](uint32_t reg, uint32_t mask, uint32_t val) {
		return ModeSet::Step{ModeSet::Op::Update, 2, false, reg, mask, val & mask, 0, "desktop trial plane"};
	};
	change[n++] = op(0x0607, 0xffff, pitch - 1);
	change[n++] = op(0x060b, 0xffff, static_cast<uint32_t>(mc >> 32));
	change[n++] = op(0x060a, 0xffffff00, static_cast<uint32_t>(mc));
	size_t ns = 0;
	for (size_t i = 0; i < n; ++i) {
		const auto &s = change[i];
		const uint32_t old = read(s.seg, s.dword);
		if (s.op != ModeSet::Op::Update || old == 0xffffffff)
			return false;
		saved[ns] = s;
		saved[ns++].value = old & s.mask;
	}
	const uint32_t geometry[] = {0x05e9, 0x05eb, 0x063b, 0x0d19, 0x0d0a, 0x0d08, 0x0d1e, 0x0d1f, 0x0d20};
	for (uint32_t reg : geometry) {
		const uint32_t old = read(2, reg);
		if (old == 0xffffffff)
			return false;
		saved[ns++] = op(reg, 0xffffffff, old);
	}
	target.extra = change;
	target.nextra = n;
	undo.extra = saved;
	undo.nextra = ns;
	if (!ModeSet::build(undo, back, nullptr) || !ModeSet::build(target, forward, nullptr))
		return false;
	ModeSet::Plan *plans[] = {&back, &forward};
	for (auto *plan : plans) {
		for (size_t i = 0; i < plan->count; ++i)
			if (plan->steps[i].op == ModeSet::Op::WaitSet && plan->steps[i].dword == 0x1b89)
				plan->steps[i].optional = false;
	}
	return true;
}
} // namespace DesktopTrial
#endif
