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

size_t count(Table table) { return table == Table::OtgOff ? countOf(kDcnOtgOffLinux) : countOf(kDcnInitLinux); }

bool steps(ModeSet::Step *out, size_t max, size_t &n, Table table) {
	const Gen *gen = table == Table::OtgOff ? kDcnOtgOffLinux : kDcnInitLinux;
	n = 0;
	if (max < count(table))
		return false;
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

} // namespace DcnInit
