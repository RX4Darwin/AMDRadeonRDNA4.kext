//
//  dcninit.cpp
//  AMDRadeonRDNA4
//
//  See dcninit.hpp.
//

#include "dcninit.hpp"

namespace DcnInit {

#include "dcn_init_linux.inc"

template <size_t N> constexpr size_t countOf(const Gen (&)[N]) { return N; }

size_t count(Table table) { return table == Table::OtgOff ? countOf(kDcnOtgOffLinux[0]) : countOf(kDcnInitLinux); }

bool steps(ModeSet::Step *out, size_t max, size_t &n, Table table, uint32_t otg) {
	n = 0;
	if (max < count(table) || otg >= kOtgs)
		return false;
	const Gen *gen = table == Table::OtgOff ? kDcnOtgOffLinux[otg] : kDcnInitLinux;
	for (size_t i = 0; i < count(table); i++) {
		const Gen &g = gen[i];
		ModeSet::Step &s = out[n++];
		s = ModeSet::Step {};
		s.seg = g.seg;
		s.dword = g.dword;
		s.mask = g.mask;
		s.value = g.value;
		s.arg = g.arg;
		s.what = g.what;
		switch (g.op) {
		case 'W': s.op = ModeSet::Op::Write; break;
		case 'U': s.op = ModeSet::Op::Update; break;
		case 'T': s.op = ModeSet::Op::WaitValue; s.optional = true; break;   // Linux's REG_WAIT logs a timeout and goes on
		case 'C': s.op = ModeSet::Op::Copy; break;
		case 'D': s.op = ModeSet::Op::Delay; break;
		default: return false;
		}
	}
	return true;
}

uint32_t timerRefDiv(uint32_t refKHz) {
	for (uint32_t div = 1; div <= 2; div++)
		if (refKHz / div >= 40000 && refKHz / div <= 60000)
			return div;
	return 0;
}

bool timerSteps(uint32_t refKHz, ModeSet::Step *out, size_t max, size_t &n) {
	const uint32_t div = timerRefDiv(refKHz);
	n = 0;
	if (!div || max < 2)
		return false;
	out[0] = out[1] = ModeSet::Step {};
	out[0].op = out[1].op = ModeSet::Op::Update;
	out[0].seg = out[1].seg = 2;
	out[0].dword = out[1].dword = kTimerCntl;
	out[0].mask = kTimerRefDivMask;
	out[0].value = div;
	out[0].what = "dcn10_hubbub_global_timer_enable DCHUBBUB_GLOBAL_TIMER_CNTL.REFDIV";
	out[1].mask = out[1].value = kTimerEnable;
	out[1].what = "dcn10_hubbub_global_timer_enable DCHUBBUB_GLOBAL_TIMER_CNTL.ENABLE";
	n = 2;
	return true;
}

} // namespace DcnInit
