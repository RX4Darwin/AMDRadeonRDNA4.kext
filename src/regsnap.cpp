/*
 * regsnap.cpp
 * AMDRadeonRDNA4
 *
 * See regsnap.hpp.
 */

#include "regsnap.hpp"

namespace
{

bool writes(ModeSet::Op op)
{
	return op == ModeSet::Op::Write || op == ModeSet::Op::Update || op == ModeSet::Op::Copy;
}

bool known(const RegSnap::Snap &snap, uint8_t seg, uint32_t dword)
{
	for (size_t i = 0; i < snap.count; i++) {
		if (snap.regs[i].seg == seg && snap.regs[i].dword == dword)
			return true;
	}

	return false;
}

} /* namespace */

void RegSnap::take(Snap &snap, const ModeSet::Step *steps, size_t count, ReadFn read, void *ctx)
{
	snap.count = 0;

	for (size_t i = 0; i < count && snap.count < kMaxRegs; i++) {
		const ModeSet::Step &s = steps[i];

		if (!writes(s.op) || known(snap, s.seg, s.dword))
			continue;

		snap.regs[snap.count++] = { s.seg, s.dword, read(ctx, s.seg, s.dword), s.what };
	}
}

size_t RegSnap::compare(const Snap &snap, ReadFn read, DiffFn diff, void *ctx)
{
	size_t differ = 0;

	for (size_t i = 0; i < snap.count; i++) {
		const Reg &r = snap.regs[i];
		const uint32_t now = read(ctx, r.seg, r.dword);

		if (now == r.value)
			continue;

		differ++;
		diff(ctx, r, now);
	}

	return differ;
}
