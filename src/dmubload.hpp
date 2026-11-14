//
//  dmubload.hpp
//  AMDRadeonRDNA4
//
//  The display microcontroller (DMCUB) started on a firmware of the driver's own: what amdgpu does at load
//  (amdgpu_dm_dmub.c dm_dmub_hw_init) once the PSP has loaded dcn_4_0_1_dmcub.bin into it. The GOP's firmware
//  is stopped, the controller held in reset, its windows onto memory pointed at a block the driver owns
//  (VBIOS copy, mailboxes, trace buffer, firmware state, shared state), and released; the firmware reports
//  itself up. The steps are Linux's dmub_srv_hw_reset, dmub_srv_hw_init and dmub_srv_wait_for_auto_load for
//  DCN 4.01 (dmub_dcn401.c); tools/gen/pipegen/dmubgen.sh runs Linux's own code and test/dmubload-test.cpp
//  holds these against it, read by read and write by write (test/ref/dmub_start_linux.inc).
//
//  The first part of lighting the displays when nothing set them up (after a power loss): with this the
//  driver no longer depends on the GOP's firmware being alive.
//
//  Freestanding: registers and delays come through Io. Shared by the kext and the host test.
//

#ifndef DmubLoad_hpp
#define DmubLoad_hpp

#include <stdint.h>

namespace DmubLoad {

struct Io {
	void *ctx;
	uint32_t (*read)(void *ctx, uint32_t reg);           // DCN register: dword offset from segment 2's base
	void (*write)(void *ctx, uint32_t reg, uint32_t value);
	void (*delayUs)(void *ctx, uint32_t us);
};

// The block's windows for dcn_4_0_1_dmcub.bin 0x0a005000 and a 128 KiB VBIOS (dm_dmub_sw_init's layout, as
// dmubgen prints it): offsets from the block's start. The instruction window is the PSP's, not in the block.
enum Window { InstConst, Stack, BssData, Vbios, Mailbox, TraceBuff, FwState, ScratchMem, IbMem, SharedState,
              LsdmaBuffer, CursorOffload, kWindows };
struct Span { uint32_t offset, size; };
extern const Span kLayout[kWindows];
constexpr uint32_t kFwVersion = 0x0a005000;
constexpr uint32_t kBlockBytes = 0x15d000;
constexpr uint32_t kVbiosBytes = 0x20000;

// dmub_dcn401_reset: a running firmware asked to stop (GPINT STOP_FW, up to 1 s for its answer and its wait
// mode), the controller held in reset and disabled, windows 2-7 off, the mailbox pointers and boot status
// cleared. False when the firmware did not stop in time (Linux goes on regardless).
bool reset(const Io &io);

// dmub_srv_hw_init without the backdoor load, then dmub_srv_wait_for_auto_load (100 ms): the controller runs
// the firmware the PSP loaded, its windows in the block at MC address `block` (filled before: the VBIOS
// copied in, the mailbox, trace, state and shared state windows zeroed). False when the firmware did not
// report itself up (dal_fw and mailbox_rdy in SCRATCH0); `status` is that register.
bool start(const Io &io, uint64_t block, uint32_t &status);

} // namespace DmubLoad

#endif
