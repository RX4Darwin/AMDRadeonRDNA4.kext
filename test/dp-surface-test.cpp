#include "desktoptrial.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>

using Registers = std::map<unsigned long long, uint32_t>;
static auto key(uint8_t seg, uint32_t reg) { return (static_cast<unsigned long long>(seg) << 32) | reg; }
static void execute(const ModeSet::Plan &p, size_t count, Registers &regs) {
	bool locked = false, checked = false, idle = false;
	for (size_t i = 0; i < count; ++i) {
		const auto &s = p.steps[i];
		if (s.seg == 2 && s.dword == 0x1b89) {
			if (s.op == ModeSet::Op::Update) {
				locked = (s.value & 1) != 0;
				checked = false;
			}
			if (s.op == ModeSet::Op::WaitSet) {
				assert(locked && !s.optional);
				checked = true;
			}
		}
		if (s.seg == 2 && (s.dword == 0x0607 || s.dword == 0x060b || s.dword == 0x060a))
			assert(locked && checked);
		if (s.op == ModeSet::Op::WaitClear && s.dword == 0x2122 && !s.optional)
			idle = true;
		if (s.dword == 0x2122 && s.op == ModeSet::Op::Update && (s.value & 1))
			idle = false;
		if (s.dword == 0x211f || s.dword == 0x2120)
			assert(idle);
		auto &v = regs[key(s.seg, s.dword)];
		if (s.op == ModeSet::Op::Update)
			v = (v & ~s.mask) | s.value;
		if (s.op == ModeSet::Op::Write)
			v = s.value;
	}
}

static void testDeepColorStream() {
	ModeSet::Plan off {}, rgb10 {}, rgb8 {};
	const uint32_t oldM = ModeSet::dpVidM(699500, 540000), newM = ModeSet::dpVidM(699500, 810000);
	assert(ModeSet::buildDpStreamOff(0, off) && ModeSet::buildDpStreamOn(0, 10, newM, rgb10) &&
	       ModeSet::buildDpStreamOn(0, 8, oldM, rgb8));
	const Registers baseline { {key(2, 0x2122), 0x201}, {key(2, 0x211f), 0x04000100},
	                           {key(2, 0x2120), 0x28001234}, {key(2, 0x2128), oldM},
	                           {key(2, 0x060a), 0x43800000}, {key(2, 0x0607), 2559},
	                           {key(2, 0x1b2f), 1558}, {key(2, 0x2162), 2720u << 16 | 1559} };
	auto regs = baseline;
	execute(off, off.count, regs);
	execute(rgb10, rgb10.count, regs);
	assert(regs.at(key(2, 0x211f)) == 0x04000200 && regs.at(key(2, 0x2120)) == 0x48001234 &&
	       regs.at(key(2, 0x2128)) == newM);
	for (size_t cut = 0; cut <= off.count + rgb10.count; ++cut) {
		regs = baseline;
		execute(off, cut < off.count ? cut : off.count, regs);
		if (cut > off.count)
			execute(rgb10, cut - off.count, regs);
		execute(off, off.count, regs);
		execute(rgb8, rgb8.count, regs);
		for (const auto &entry : baseline)
			assert(regs.at(entry.first) == entry.second);
	}
	printf("PASS %zu RGB10 stream interruption points restored RGB8, M/N and the native QHD surface\n",
	       off.count + rgb10.count + 1);
}

int main() {
	testDeepColorStream();
	ModeSet::Target to{};
	to.dp = true;
	to.hpd = 1;
	to.dpMaxKHz = 241505;
	to.dto = {0, 241500, 720000};
	to.vidM = 9770;
	to.to = {241500, 2560, 160, 48, 32, 1440, 41, 3, 5, true, false, false};
	assert(Edid::vicTiming(16, to.from) && Pipe2::use(1, false, 0, true));
	const uint64_t mc = 0x843800000ull;
	const auto hd = to.from;
	const auto qhd60 = to.to;
	for (int scenario : {0, 1, 2, 3, 4, 5}) {
		bool stretched = scenario == 1;
		bool highRefresh = scenario >= 2;
		to.from = highRefresh ? qhd60 : hd;
		to.to = qhd60;
		if (highRefresh) {
			to.to = {699500, 2560, 160, 48, 32, 1440, 119, 3, 5, true, false, false};
			assert(to.to.refreshMilliHz() == 164957);
			assert(ModeSet::scaleDpDto({0, 241500000, 720000000}, 241500, 699500, to.dto));
			to.vidM = ModeSet::dpVidM(699500, 540000);
			to.dpMaxKHz = 720000;
		}
		if (stretched)
			to.from = to.to;
		to.scalerOff = stretched;
		auto back = to;
		back.from = to.to;
		back.to = to.from;
		if (highRefresh) {
			back.dto = {0, 241500000, 720000000};
			back.vidM = ModeSet::dpVidM(241500, 540000);
			to.dpBpc = 8;
			back.dpBpc = 10;
		}
		DesktopTrial::Work work{};
		size_t n = highRefresh ? 0 : Pipe2::modeSteps(to.to, work.change, Pipe2::kMaxModeSteps);
		assert(highRefresh || n == 21);
		if (highRefresh) {
			const ModeSet::Step exact[] = {
#include "refresh_dml.inc"
			    {ModeSet::Op::Update, 1, false, 0x99, 0xffffffff, 0x00ff00ff, 0, "DPP0 DTO"},
			};
			memcpy(work.change, exact, sizeof exact);
			n = sizeof(exact) / sizeof(exact[0]);
			unsigned segments[] = {4, 6, 8, 11};
			for (unsigned j = 0; j < n; ++j)
				if (work.change[j].seg == 2 && work.change[j].dword == 0x04bb)
					work.change[j].value = segments[scenario - 2];
		}
		Registers original;
		auto read = [&](uint8_t seg, uint32_t reg) {
			auto k = key(seg, reg);
			return original.emplace(k, 0x15910000u | reg).first->second;
		};
		if (highRefresh)
			original[key(2, 0x530)] = 0x85910530; // active VTG; checked by setDesktopRefresh
		original[key(2, 0x0607)] = highRefresh ? 2559 : 1919;
		original[key(2, 0x060a)] = 0x2000000;
		original[key(2, 0x060b)] = 8;
		ModeSet::Plan forward{}, reverse{};
		assert(DesktopTrial::build(to, back, mc, 2560, 2560ull * 4 * 1440, work, n, read, forward, reverse));
		if (highRefresh) {
			auto limited = to;
			limited.dpMaxKHz = 650000;
			ModeSet::Plan rejected{};
			assert(!DesktopTrial::build(limited, back, mc, 2560, 2560ull * 4 * 1440, work, n, read, rejected,
			                            reverse));
			assert(DesktopTrial::build(to, back, mc, 2560, 2560ull * 4 * 1440, work, n, read, forward, reverse));
			// Include the live timing registers of the 60 Hz mode in every rollback check.
			auto baseline = original;
			execute(reverse, reverse.count, baseline);
			for (const auto &where :
			     {key(1, 0x0081), key(1, 0x0082), key(1, 0x006f), key(2, 0x2128), key(2, 0x1b2f)})
				original[where] = baseline.at(where);
		}
		original[key(2, 0x211f)] = 0x04000200; // unrelated bits must survive
		original[key(2, 0x2120)] = 0x48001234;
		auto regs = original;
		execute(forward, forward.count, regs);
		assert((regs[key(2, 0x0607)] & 0xffff) == 2559 && regs[key(2, 0x060a)] == uint32_t(mc) &&
		       regs[key(2, 0x060b)] == mc >> 32);
		if (highRefresh) {
			assert(regs.at(key(2, 0x211f)) == 0x04000100 && regs.at(key(2, 0x2120)) == 0x28001234);
			for (size_t cut = 0; cut <= reverse.count; ++cut) {
				auto interrupted = regs;
				execute(reverse, cut, interrupted);
				execute(reverse, reverse.count, interrupted);
				assert(interrupted.at(key(2, 0x211f)) == original.at(key(2, 0x211f)));
				assert(interrupted.at(key(2, 0x2120)) == original.at(key(2, 0x2120)));
			}
		} else {
			assert(regs.at(key(2, 0x211f)) == original.at(key(2, 0x211f)) &&
			       regs.at(key(2, 0x2120)) == original.at(key(2, 0x2120)));
		}
		for (size_t cut = 0; cut <= forward.count; ++cut) {
			regs = original;
			execute(forward, cut, regs);
			execute(reverse, reverse.count, regs);
			for (const auto &entry : original)
				if (regs[entry.first] != entry.second) {
					printf("cut=%zu reg=%llx got=%x want=%x\n", cut, entry.first, regs[entry.first],
					       entry.second);
					return 1;
				}
		}
		printf("PASS %zu interruption points, %zu saved plane/geometry/DML registers, %s baseline\n", forward.count + 1,
		       original.size(),
		       highRefresh ? "1440p 60 Hz -> 164.957 Hz -> 60 Hz"
		       : stretched ? "stretched"
		                   : "1080p");
		assert(!DesktopTrial::build(to, back, mc, 2560, 1920ull * 4 * 1080, work, n, read, forward, reverse));
		assert(!DesktopTrial::build(to, back, mc, 1920, 2560ull * 4 * 1440, work, n, read, forward, reverse));
		auto wrongPipe = to;
		wrongPipe.hubp = 1;
		assert(!DesktopTrial::build(wrongPipe, back, mc, 2560, 2560ull * 4 * 1440, work, n, read, forward, reverse));
		assert(!DesktopTrial::build(to, back, mc + 1, 2560, 2560ull * 4 * 1440, work, n, read, forward, reverse));
		assert(!DesktopTrial::build(
		    to, back, mc, 2560, 2560ull * 4 * 1440, work, n, [](uint8_t, uint32_t) { return 0xffffffffu; }, forward,
		    reverse));
	}
}
