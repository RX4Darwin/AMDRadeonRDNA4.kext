//
//  dmubload.cpp
//  AMDRadeonRDNA4
//
//  See dmubload.hpp. Linux's dmub_dcn401.c and dmub_srv.c, step for step; register names are Linux's
//  (dcn_4_1_0_offset.h, segment 2), the bits its dcn_4_1_0_sh_mask.h.
//

#include "dmubload.hpp"

namespace DmubLoad {

const Span kLayout[kWindows] = {
	{ 0x000000, 0x059bc0 }, { 0x059c00, 0x0a0000 }, { 0x0f9c00, 0x000000 }, { 0x0f9c00, 0x020000 },
	{ 0x119c00, 0x004000 }, { 0x11dc00, 0x010040 }, { 0x12dd00, 0x01a240 }, { 0x148000, 0x000400 },
	{ 0x148400, 0x000c80 }, { 0x149100, 0x000600 }, { 0x149700, 0x010000 }, { 0x159700, 0x0037c0 },
};

namespace {

enum Reg : uint32_t {
	Region5Offset = 0x198, Region5OffsetHigh, Region6Offset, Region6OffsetHigh,
	Region5Top = 0x1a2, Region6Top,
	Cw3Base = 0x1a8, Cw4Base, Cw5Base, Cw6Base,
	Cw2Top = 0x1af,                        // Cw2Top .. Cw7Top
	Cw3Offset = 0x1bb, Cw3OffsetHigh, Cw4Offset, Cw4OffsetHigh, Cw5Offset, Cw5OffsetHigh, Cw6Offset, Cw6OffsetHigh,
	Inbox1Base = 0x1d4, Inbox1Size, Inbox1Wptr, Inbox1Rptr, Outbox0Base, Outbox0Size, Outbox0Wptr, Outbox0Rptr,
	Outbox1Base, Outbox1Size, Outbox1Wptr, Outbox1Rptr,
	Scratch0 = 0x1e3, Scratch7 = 0x1ea, Scratch14 = 0x1f1, Scratch15,
	Cntl = 0x1f6, GpintDataIn1 = 0x1f8, Cntl2 = 0x200,
	HostInterruptCsr = 0x222, MmhubbubSoftReset = 0x341,
};

constexpr uint32_t kEnable = 0x00010000, kTraceportEn = 0x00080000, kPwaitModeStatus = 0x00100000;   // DMCUB_CNTL
constexpr uint32_t kSoftReset = 0x1;                                                                  // DMCUB_CNTL2
constexpr uint32_t kWindowEnable = 0x80000000, kTopMask = 0x1fffffff;                                 // *_TOP_ADDRESS
constexpr uint32_t kInbox0RspIntEn = 0x1, kOutbox0RdyIntEn = 0x2;                                     // HOST_INTERRUPT_CSR
constexpr uint32_t kDmuifSoftReset = 0x100;                                                           // MMHUBBUB_SOFT_RESET
constexpr uint32_t kSkipPanelSequence = 1u << 5;   // dmub_fw_boot_options.skip_phy_init_panel_sequence

// dmub_gpint_data_register: param [15:0], command_code [27:16], status [31:28]
constexpr uint32_t kGpintStopFw = 1u << 28 | 2u << 16, kStopFwResponse = 0xdeaddead;

// The firmware's view of the windows (dmub_srv.c DMUB_CWn_BASE, DMUB_REGION5_BASE), and the mailboxes' size
// (DMUB_RB_SIZE: 64-byte commands, 128 of them) and the trace buffer's header (TRACE_BUFFER_ENTRY_OFFSET).
constexpr uint32_t kCw3 = 0x63000000, kCw4 = 0x64000000, kCw5 = 0x65000000, kCw6 = 0x66000000;
constexpr uint32_t kRegion5 = 0xa0000000, kRingBytes = 0x2000, kTraceHeader = 16;

uint32_t rd(const Io &io, uint32_t r) { return io.read(io.ctx, r); }
void wr(const Io &io, uint32_t r, uint32_t v) { io.write(io.ctx, r, v); }
void update(const Io &io, uint32_t r, uint32_t mask, uint32_t value) { wr(io, r, (rd(io, r) & ~mask) | (value & mask)); }

// A window the controller sees at `base` onto `size` bytes of the block (REG_SET_2 of its TOP_ADDRESS: the
// top is base + size, as dmub_srv_hw_init has it for windows 3 to 6).
void window(const Io &io, uint32_t offset, uint32_t baseReg, uint32_t topReg, uint64_t at, uint32_t base, uint32_t size) {
	wr(io, offset, static_cast<uint32_t>(at));
	wr(io, offset + 1, static_cast<uint32_t>(at >> 32));
	wr(io, baseReg, base);
	wr(io, topReg, ((base + size) & kTopMask) | kWindowEnable);
}

} // namespace

bool reset(const Io &io) {
	const uint32_t timeoutUs = 1000000;
	uint32_t i = 0;
	const bool enabled = rd(io, Cntl) & kEnable, inReset = rd(io, Cntl2) & kSoftReset;

	if (enabled && !inReset) {
		wr(io, GpintDataIn1, kGpintStopFw);
		for (; i < timeoutUs; i++) {
			if (rd(io, Scratch7) == kStopFwResponse)
				break;
			io.delayUs(io.ctx, 1);
		}
		for (; i < timeoutUs; i++) {
			if (rd(io, Cntl) & kPwaitModeStatus)
				break;
			io.delayUs(io.ctx, 1);
		}
	}

	if (enabled) {
		update(io, Cntl2, kSoftReset, kSoftReset);
		io.delayUs(io.ctx, 1);
		update(io, Cntl, kEnable, 0);
	}

	for (uint32_t top = Cw2Top; top < Cw2Top + 6; top++)
		update(io, top, kWindowEnable, 0);

	static const uint32_t kCleared[] = { Inbox1Rptr, Inbox1Wptr, Outbox1Rptr, Outbox1Wptr, Outbox0Rptr, Outbox0Wptr, Scratch0 };
	for (uint32_t r : kCleared)
		wr(io, r, 0);
	wr(io, GpintDataIn1, 0);   // the GPINT command cleared, so that the next start does not stop it again

	return i < timeoutUs;
}

bool start(const Io &io, uint64_t block, uint32_t &status) {
	const Span &vbios = kLayout[Vbios], &mail = kLayout[Mailbox], &trace = kLayout[TraceBuff];
	const Span &state = kLayout[FwState], &shared = kLayout[SharedState];

	reset(io);

	// dmub_dcn401_setup_windows: windows 3 (VBIOS), 4 (mailboxes), 5 (trace) and its region 5, 6 (firmware
	// state), region 6 (shared state)
	window(io, Cw3Offset, Cw3Base, Cw2Top + 1, block + vbios.offset, kCw3, vbios.size);
	window(io, Cw4Offset, Cw4Base, Cw2Top + 2, block + mail.offset, kCw4, mail.size);
	window(io, Cw5Offset, Cw5Base, Cw2Top + 3, block + trace.offset, kCw5, trace.size);
	wr(io, Region5Offset, static_cast<uint32_t>(block + trace.offset));
	wr(io, Region5OffsetHigh, static_cast<uint32_t>((block + trace.offset) >> 32));
	wr(io, Region5Top, ((trace.size - 1) & kTopMask) | kWindowEnable);
	window(io, Cw6Offset, Cw6Base, Cw2Top + 4, block + state.offset, kCw6, state.size);
	wr(io, Region6Offset, static_cast<uint32_t>(block + shared.offset));
	wr(io, Region6OffsetHigh, static_cast<uint32_t>((block + shared.offset) >> 32));
	wr(io, Region6Top, ((shared.size - 1) & kTopMask) | kWindowEnable);

	// outbox 0 (the trace buffer past its header), inbox 1 and outbox 1 (the two halves of window 4)
	wr(io, Outbox0Base, kRegion5 + kTraceHeader);
	wr(io, Outbox0Size, trace.size - kTraceHeader);
	wr(io, Inbox1Base, kCw4);
	wr(io, Inbox1Size, kRingBytes);
	wr(io, Outbox1Base, kCw4 + kRingBytes);
	wr(io, Outbox1Size, kRingBytes);
	update(io, HostInterruptCsr, kInbox0RspIntEn, kInbox0RspIntEn);
	update(io, HostInterruptCsr, kOutbox0RdyIntEn, kOutbox0RdyIntEn);

	// boot options (none), the panel sequence not skipped; reset_release: the DMUIF out of reset, no PSP
	// version (DCN 4.01 has no DMCU), the controller enabled with its trace port, out of reset
	wr(io, Scratch14, 0);
	update(io, Scratch14, kSkipPanelSequence, 0);
	update(io, MmhubbubSoftReset, kDmuifSoftReset, 0);
	wr(io, Scratch15, 0);
	update(io, Cntl, kEnable | kTraceportEn, kEnable | kTraceportEn);
	update(io, Cntl2, kSoftReset, 0);

	// dmub_srv_wait_for_auto_load: dal_fw and mailbox_rdy, 100 us apart for 100 ms
	for (uint32_t us = 0; us <= 100000; us += 100) {
		status = rd(io, Scratch0);
		if ((status & 3) == 3)
			return true;
		io.delayUs(io.ctx, 100);
	}
	return false;
}

} // namespace DmubLoad
