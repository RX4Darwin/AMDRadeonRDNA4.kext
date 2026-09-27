//
//  psp.hpp
//  RDNA4FB
//
//  The PSP (AMD security processor, MP0 14.0.3 on Navi 48) as a driver sees
//  it: a bootloader mailbox that accepts the secure-OS components, then a
//  kernel-mode command ring (GPCOM) that takes 1 KiB command buffers and
//  answers through a fence. Every firmware for the GPU's engines goes in
//  through it (LOAD_IP_FW); the PSP authenticates AMD's signed blobs itself.
//
//  Freestanding: register access, delays and the HDP flush come in through
//  a Bus, memory is a caller-provided window of VRAM (CPU pointer + the MC
//  address the PSP sees). The kernel binds it to BAR5/BAR0; the host test
//  binds it to a simulated PSP. Protocol per amdgpu psp_v14_0.c and
//  amdgpu_psp.c; register/record layouts per psp_gfx_if.h.
//

#ifndef Psp_hpp
#define Psp_hpp

#include <stddef.h>
#include <stdint.h>

#include "amdfw.hpp"

namespace Psp {

// MPASP_SMN_C2PMSG_n, MP0 segment 0 (mp_14_0_2_offset.h).
namespace Reg {
constexpr uint32_t kC2p35 = 0x0063;   // bootloader command / bit31 = ready
constexpr uint32_t kC2p36 = 0x0064;   // bootloader buffer, MC address >> 20
constexpr uint32_t kC2p64 = 0x0080;   // ring control / TOS response
constexpr uint32_t kC2p67 = 0x0083;   // KM ring write pointer (dwords)
constexpr uint32_t kC2p69 = 0x0085;   // KM ring MC address, low
constexpr uint32_t kC2p70 = 0x0086;   // KM ring MC address, high
constexpr uint32_t kC2p71 = 0x0087;   // KM ring size (bytes)
constexpr uint32_t kC2p81 = 0x0091;   // sOS sign of life
} // namespace Reg

// Bootloader commands (psp_bootloader_cmd).
enum BlCmd : uint32_t {
	BlSysDrv    = 0x10000,
	BlSosDrv    = 0x20000,
	BlKdb       = 0x80000,
	BlSocDrv    = 0xB0000,
	BlHadDrv    = 0xC0000,   // DBG_DRV, renamed HAD on PSP 14
	BlIntfDrv   = 0xD0000,
	BlRasDrv    = 0xE0000,
	BlIpKeyMgr  = 0xF0000,
	BlSplTable  = 0x10000000,
};

// GPCOM command ids (psp_gfx_cmd_id) and LOAD_IP_FW types used here.
enum Cmd : uint32_t {
	CmdLoadIpFw          = 0x06,
	CmdLoadToc           = 0x20,
	CmdAutoloadRlc       = 0x21,
	CmdFbFwReservAddr    = 0x50,
};
enum FwType : uint32_t {
	FwSmu = 18,
};

// Window layout inside the caller's VRAM (offsets from its start). The
// bootloader wants its buffer 1 MiB aligned (it takes the MC address >> 20),
// so the window's MC address must be 1 MiB aligned.
constexpr uint32_t kFwPriOffset  = 0;
constexpr uint32_t kFwPriSize    = 1u << 20;
constexpr uint32_t kRingOffset   = 1u << 20;
constexpr uint32_t kRingSize     = 0x1000;           // 64 frames
constexpr uint32_t kCmdOffset    = kRingOffset + 0x1000;
constexpr uint32_t kCmdSize      = 0x1000;
constexpr uint32_t kFenceOffset  = kRingOffset + 0x2000;
constexpr uint32_t kStageOffset  = 2u << 20;         // LOAD_IP_FW payloads
constexpr uint32_t kMinWindow    = 4u << 20;

// psp_gfx_rb_frame (64 bytes) and psp_gfx_cmd_resp (1 KiB) field offsets.
namespace Layout {
constexpr uint32_t kFrameSize       = 64;
constexpr uint32_t kFrameCmdLo      = 0;
constexpr uint32_t kFrameCmdHi      = 4;
constexpr uint32_t kFrameFenceLo    = 12;
constexpr uint32_t kFrameFenceHi    = 16;
constexpr uint32_t kFrameFenceValue = 20;
constexpr uint32_t kCmdId           = 8;
constexpr uint32_t kCmdArgs         = 28;    // union psp_gfx_commands
constexpr uint32_t kResp            = 864;   // struct psp_gfx_resp
constexpr uint32_t kRespStatus      = kResp + 0;
constexpr uint32_t kRespFwAddrLo    = kResp + 8;
constexpr uint32_t kRespFwAddrHi    = kResp + 12;
constexpr uint32_t kRespTmrSize     = kResp + 16;
constexpr uint32_t kRespUresp       = kResp + 64;
} // namespace Layout

struct Bus {
	void *ctx;
	uint32_t (*read)(void *ctx, uint32_t dword);            // MP0 seg 0
	void     (*write)(void *ctx, uint32_t dword, uint32_t value);
	void     (*delayUs)(void *ctx, uint32_t us);
	void     (*flushHdp)(void *ctx);   // CPU writes to the window -> visible to the GPU
};

struct Window {
	uint8_t *cpu;
	uint64_t mc;
	uint32_t size;
};

// Outcome of one step, for the log.
struct Result {
	bool        ok;
	const char *what;       // the step, or where it stopped
	uint32_t    value;      // register value / PSP status at the stop
};

struct Response {
	uint32_t status;
	uint32_t fwAddrLo, fwAddrHi;
	uint32_t tmrSize;
	uint32_t uresp[8];
};

class Driver {
public:
	bool init(const Bus &bus, const Window &win);

	bool sosAlive() const;
	uint32_t sosVersion() const;

	// Hand the package's bootloader-stage components and the sOS to the
	// bootloader, in amdgpu's order. No-op if the sOS already runs.
	// `loaded` counts the components that went in.
	Result loadSos(const AmdFw::PspPackage &pkg, uint32_t &loaded);

	// Create the kernel-mode (GPCOM) ring.
	Result createRing();

	// One command: copy it to the command buffer, post a frame, wait for
	// the fence. `args` fill psp_gfx_commands (dwords).
	Result submit(uint32_t cmdId, const uint32_t *args, uint32_t nargs, Response &resp,
	              uint32_t timeoutMs = 2000);

	// LOAD_TOC: returns the TMR size the PSP wants for this firmware set.
	Result loadToc(const AmdFw::Blob &toc, uint32_t &tmrSize);

	// LOAD_IP_FW: copy `ucode` to the staging area and have the PSP load it.
	Result loadIpFw(const AmdFw::Blob &ucode, uint32_t fwType, Response &resp);

	// AUTOLOAD_RLC: all GC firmware is in the TMR, let the RLC boot GFX.
	Result autoloadRlc();

	uint32_t fenceValue() const { return fence; }

private:
	Bus      bus {};
	Window   win {};
	uint32_t fence { 0 };

	uint32_t rd(uint32_t r) const { return bus.read(bus.ctx, r); }
	void     wr(uint32_t r, uint32_t v) { bus.write(bus.ctx, r, v); }
	bool     waitMask(uint32_t reg, uint32_t mask, uint32_t want, uint32_t timeoutUs,
	                  uint32_t &last);
	bool     waitChange(uint32_t reg, uint32_t from, uint32_t timeoutUs, uint32_t &last);
	void     put32(uint32_t off, uint32_t v);
	uint32_t get32(uint32_t off) const;
	void     zero(uint32_t off, uint32_t len);
	void     copy(uint32_t off, const uint8_t *src, uint32_t len);
	Result   loadComponent(const AmdFw::Blob &part, uint32_t cmd, const char *name);
};

} // namespace Psp

#endif /* Psp_hpp */
