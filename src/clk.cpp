/*
 * clk.cpp
 * AMDRadeonRDNA4
 *
 * The display clocks, through the power-management firmware's display
 * mailbox (DALSMC), the way amdgpu's DCN 4.01 clock manager talks to it
 * (dcn401_clk_mgr_smu_msg.c), or its main mailbox if the display one is
 * silent. Read-only so far: what the firmware lets
 * DISPCLK, DPPCLK, DPREFCLK and DCFCLK run at, and where DISPCLK is now.
 * A 4K pipe needs more DISPCLK than the firmware set for its boot mode.
 */

#include "device.hpp"
#include "klog.hpp"
#include "trial.hpp"

#include <IOKit/IOLib.h>

#define KLOG(fmt, ...) \
	rdna4Log("AMDRadeonRDNA4: clk: " fmt "\n", ##__VA_ARGS__)

/*
 * A mailbox of the power-management firmware: its C2PMSG registers (MP1,
 * base index 1) and the ids of the messages the probe sends through it.
 */
struct SmuMailbox {
	const char *name;
	uint32_t msg, arg, resp;
	uint32_t getVersion, getDpmFreq, getDcModeMax, setHardMin;
	uint32_t hardMinStatus;   /* 0: the mailbox has no such message */
};

namespace
{

/* Firmware responses (dalsmc.h, smu_v14_0_2_ppsmc.h). */
constexpr uint32_t kSmuResultOk = 0x1;

/* amdgpu waits up to 2 s for an answer (200000 polls of 10 us). */
constexpr int kSmuWaitMs = 2000;

/* And up to 1 s for a hard minimum to be met. */
constexpr int kHardMinWaitMs = 1000;

/*
 * The display mailbox amdgpu's DCN 4.01 clock manager uses: mmDAL_MSG_REG,
 * mmDAL_ARG_REG and mmDAL_RESP_REG (0x1628A, 0x16273 and 0x16274 with
 * MP1's base 1 at 0x16200), and DALSMC message ids.
 */
constexpr SmuMailbox kDalMailbox = {
	"display", 0x008a, 0x0073, 0x0074, 0x2, 0xb, 0xc, 0x9, 0x15,
};

/*
 * The main mailbox amdgpu's power management uses: C2PMSG_66, 82 and 90,
 * and PPSMC message ids.
 */
constexpr SmuMailbox kMainMailbox = {
	"main", 0x0082, 0x0092, 0x009a, 0x2, 0x1f, 0x27, 0x1b, 0,
};

/* GetDpmFreqByIndex with level 0xFF: the number of levels, bit 31 fine-grained. */
constexpr uint8_t kDpmLevelCount = 0xff;
constexpr uint32_t kDpmFineGrained = 1u << 31;
constexpr uint32_t kMaxDpmLevels = 16;

/* PPCLK_e, dcn401_smu14_driver_if.h */
constexpr uint32_t kPpclkUclk = 2;
constexpr uint32_t kPpclkFclk = 3;
constexpr uint32_t kPpclkDispclk = 6;
constexpr uint32_t kPpclkDppclk = 7;
constexpr uint32_t kPpclkDprefclk = 8;
constexpr uint32_t kPpclkDcfclk = 9;

struct Clock {
	uint32_t id;
	const char *name;
};

constexpr Clock kClocks[] = {
	{ kPpclkUclk, "UCLK" },
	{ kPpclkFclk, "FCLK" },
	{ kPpclkDispclk, "DISPCLK" },
	{ kPpclkDppclk, "DPPCLK" },
	{ kPpclkDprefclk, "DPREFCLK" },
	{ kPpclkDcfclk, "DCFCLK" },
};

/*
 * The hard minimums the display asks the firmware for once it runs. DISPCLK,
 * DPPCLK and DCFCLK: above what Linux's DML asks for one 3840x2160@60 pipe
 * (621, 598 and 228 MHz); they are required for 4K. UCLK at its top level:
 * the firmware idles memory at 97 MHz, and two 4K planes underflowed there
 * (card, 2026-11-04); the display does not take part in memory clock
 * switches, so like Linux in that case it holds the memory clock. FCLK is
 * the firmware's. Each has an option to set it instead (0 for DISPCLK:
 * leave the clocks alone, and no 4K).
 */
constexpr uint32_t kTopLevel = ~0u;

struct HardMin {
	uint32_t id;
	const char *name;
	const char *option;
	uint32_t mhz;        /* 0: not asked for, kTopLevel: the top DPM level */
	bool required;
};

constexpr HardMin kRaised[] = {
	{ kPpclkDispclk, "DISPCLK", "rdna4-dispclk", 650, true },
	{ kPpclkDppclk, "DPPCLK", "rdna4-dispclk", 650, true },
	{ kPpclkDcfclk, "DCFCLK", "rdna4-dcfclk", 600, true },
	{ kPpclkUclk, "UCLK", "rdna4-uclk", kTopLevel, false },
	{ kPpclkFclk, "FCLK", "rdna4-fclk", 0, false },
};

/* DENTIST_DISPCLK_CNTL (base index 1): the divider DISPCLK runs on. */
constexpr uint32_t kDentistDispclkCntl = 0x0064;
constexpr uint32_t kDentistWdividerMask = 0x7f;

/*
 * The divider behind a DENTIST divider id, in quarters (amdgpu's
 * dentist_get_divider_from_did).
 */
uint32_t dentistDivider(uint32_t did)
{
	if (did < 0x08)
		did = 0x08;

	if (did > 0x7f)
		did = 0x7f;

	if (did < 0x40)
		return 8 + (did - 0x08);

	if (did < 0x60)
		return 64 + 2 * (did - 0x40);

	if (did < 0x7e)
		return 128 + 4 * (did - 0x60);

	return 248 + 264 * (did - 0x7e);
}

} /* namespace */

bool RDNA4Device::smuRegs(const SmuMailbox &mb, uint32_t &msg, uint32_t &arg,
			  uint32_t &resp) const
{
	if (!ipDiscovery.isValid() || !rmmio)
		return false;

	if (!ipDiscovery.regByteOffset(IpDiscovery::HwMp1, 0, 1, mb.msg, msg) ||
	    !ipDiscovery.regByteOffset(IpDiscovery::HwMp1, 0, 1, mb.arg, arg) ||
	    !ipDiscovery.regByteOffset(IpDiscovery::HwMp1, 0, 1, mb.resp, resp))
		return false;

	return msg + 4 <= rmmioSize && arg + 4 <= rmmioSize &&
	       resp + 4 <= rmmioSize;
}

/* The response register once it is not 0, or 0 after kSmuWaitMs. */
uint32_t RDNA4Device::smuWait(uint32_t resp) const
{
	uint32_t r = 0;

	for (int i = 0; i < kSmuWaitMs && !r; i++) {
		r = rmmio[resp / 4];
		if (!r)
			IOSleep(1);
	}

	return r;
}

bool RDNA4Device::smuSend(const SmuMailbox &mb, uint32_t id, uint32_t param,
			  uint32_t *out)
{
	uint32_t msg, arg, resp, r;

	if (!smuRegs(mb, msg, arg, resp))
		return false;

	/* Like amdgpu: wait for the last answer, then send regardless. */
	if (!smuWait(resp))
		KLOG("%s mailbox: no answer pending before message 0x%x",
		     mb.name, id);

	rmmio[resp / 4] = 0;
	rmmio[arg / 4] = param;
	rmmio[msg / 4] = id;

	r = smuWait(resp);
	if (r != kSmuResultOk) {
		KLOG("%s mailbox: message 0x%x (0x%08x): response 0x%x",
		     mb.name, id, param, r);
		return false;
	}

	if (out)
		*out = rmmio[arg / 4];

	return true;
}

/* Whether the firmware answers on this mailbox: its version if it does. */
bool RDNA4Device::smuProbe(const SmuMailbox &mb)
{
	uint32_t msg, arg, resp, version = 0;

	if (!smuRegs(mb, msg, arg, resp)) {
		KLOG("%s mailbox not in the discovery table", mb.name);
		return false;
	}

	KLOG("%s mailbox msg@0x%x arg@0x%x resp@0x%x, resp 0x%x before",
	     mb.name, msg, arg, resp, rmmio[resp / 4]);

	if (!smuSend(mb, mb.getVersion, 0, &version))
		return false;

	KLOG("%s mailbox: SMU firmware %u.%u.%u", mb.name, version >> 16 & 0xff,
	     version >> 8 & 0xff, version & 0xff);

	return true;
}

/* The number of DPM levels of a clock; a fine-grained one has two ends. */
uint32_t RDNA4Device::dpmLevelCount(const SmuMailbox &mb, uint32_t clk)
{
	uint32_t count;

	if (!smuSend(mb, mb.getDpmFreq, clk << 16 | kDpmLevelCount, &count))
		return 0;

	if (count & kDpmFineGrained)
		return 2;

	count &= 0xff;

	return count > kMaxDpmLevels ? kMaxDpmLevels : count;
}

/* A clock's highest DPM level in MHz, 0 if the firmware does not say. */
uint32_t RDNA4Device::topDpmLevel(uint32_t clk)
{
	const uint32_t count = dpmLevelCount(*smu, clk);
	uint32_t freq = 0;

	if (!count || !smuSend(*smu, smu->getDpmFreq, clk << 16 | (count - 1), &freq))
		return 0;

	return freq & 0xffff;
}

void RDNA4Device::logDpmLevels(const SmuMailbox &mb, uint32_t clk,
			       const char *name)
{
	const uint32_t count = dpmLevelCount(mb, clk);
	uint32_t freq, max = 0;
	char line[160];
	size_t len = 0;
	int n;

	line[0] = 0;

	for (uint32_t i = 0; i < count && len < sizeof(line) - 8; i++) {
		if (!smuSend(mb, mb.getDpmFreq, clk << 16 | i, &freq))
			break;

		n = snprintf(line + len, sizeof(line) - len, " %u",
			     freq & 0xffff);
		if (n < 0)
			break;

		len += static_cast<size_t>(n);
	}

	smuSend(mb, mb.getDcModeMax, clk << 16, &max);

	KLOG("%s: %u level(s) MHz:%s; DC-mode max %u MHz", name, count, line,
	     max & 0xffff);
}

void RDNA4Device::logDispclkDivider()
{
	const uint32_t cntl = regReadDmu(1, kDentistDispclkCntl);
	const uint32_t did = cntl & kDentistWdividerMask;
	const uint32_t div = dentistDivider(did);

	/* amdgpu's default VCO when its PLL register reads 0: 4.5 GHz. */
	KLOG("DENTIST_DISPCLK_CNTL 0x%08x: divider id 0x%02x = %u/4, %u MHz at "
	     "a 4500 MHz VCO", cntl, did, div, 4 * 4500 / div);
}

/* The mailbox the firmware answers on, or nullptr. */
const SmuMailbox *RDNA4Device::smuFind()
{
	if (smuProbe(kDalMailbox))
		return &kDalMailbox;

	if (smuProbe(kMainMailbox))
		return &kMainMailbox;

	KLOG("power-management firmware silent: is it loaded (rdna4-compute)?");

	return nullptr;
}

/* Whether the firmware reports the clock's hard minimum met. */
bool RDNA4Device::smuHardMinMet(uint32_t clk)
{
	uint32_t status = 0;

	for (int i = 0; i < kHardMinWaitMs; i++) {
		if (!smuSend(*smu, smu->hardMinStatus, 0, &status))
			return false;

		if (status & 1u << clk)
			return true;

		IOSleep(1);
	}

	return false;
}

/*
 * Keep a clock at or above `mhz`, as amdgpu's DCN 4.01 clock manager does:
 * whether the firmware took it (and, where it says, met it).
 */
bool RDNA4Device::smuSetHardMin(uint32_t clk, const char *name, uint32_t mhz)
{
	if (!smuSend(*smu, smu->setHardMin, clk << 16 | (mhz & 0xffff),
		     nullptr)) {
		KLOG("%s: hard minimum %u MHz refused", name, mhz);
		return false;
	}

	if (!smu->hardMinStatus) {
		KLOG("%s: hard minimum %u MHz accepted", name, mhz);
		return true;
	}

	if (!smuHardMinMet(clk)) {
		KLOG("%s: hard minimum %u MHz not met after 1 s", name, mhz);
		return false;
	}

	KLOG("%s: hard minimum %u MHz met", name, mhz);

	return true;
}

/* Whether every clock a 3840x2160 pipe needs is held high enough. */
bool RDNA4Device::raiseDisplayClocks()
{
	uint32_t dispclk = 0;
	bool met = true;

	if (Trial::option("rdna4-dispclk", dispclk) && !dispclk)
		return false;

	for (const HardMin &c : kRaised) {
		uint32_t mhz = c.mhz;

		(void)Trial::option(c.option, mhz);
		if (mhz == kTopLevel)
			mhz = topDpmLevel(c.id);

		if (mhz && !smuSetHardMin(c.id, c.name, mhz) && c.required)
			met = false;
	}

	logDispclkDivider();

	return met;
}

void RDNA4Device::logDisplayClocks()
{
	uint32_t on = 0;

	if (!Trial::option("rdna4-clocks", on) || !on)
		return;

	logDispclkDivider();

	for (const Clock &c : kClocks)
		logDpmLevels(*smu, c.id, c.name);
}

/*
 * The display mailbox is the one amdgpu's display code uses; the main one
 * is its power management's. Its other user here is the compute bring-up,
 * on whose thread this runs, so the two never send at once.
 */
void RDNA4Device::smuStarted()
{
	smu = smuFind();
	if (!smu)
		return;

	logDisplayClocks();
	uhdClocks = raiseDisplayClocks();
}
