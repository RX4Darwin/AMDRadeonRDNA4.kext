//
//  gfxregs.hpp
//  RDNA4FB
//
//  Register map of the compute side of Navi 48 (GC 12.0.1, SDMA 7.0.1,
//  GC/MM hubs, MP0/MP1): dword offset + base segment, the same pairs as the
//  BASE_IDX / offset in Linux's asic_reg headers (gc_12_0_0_offset.h,
//  mmhub_4_1_0_offset.h, mp_14_0_2_offset.h). Absolute addresses come from
//  the card's IP discovery table at runtime (IpDiscovery::regByteOffset).
//
//  Hardware facts only; the code driving them is ours (compute.cpp).
//

#ifndef GfxRegs_hpp
#define GfxRegs_hpp

#include <stdint.h>

namespace GfxReg {

struct Reg {
	uint8_t  seg;
	uint32_t dword;
};

// --- GC: GRBM / CP / RLC / IMU ------------------------------------------------
constexpr Reg GrbmStatus          { 0, 0x0da4 };
constexpr Reg GrbmStatus2         { 0, 0x0da2 };
constexpr Reg CpStat              { 0, 0x0f40 };
constexpr Reg CpCpcStatus         { 0, 0x0e24 };
constexpr Reg CpCpfStatus         { 0, 0x0e27 };
constexpr Reg CpPfpInstrPntr      { 0, 0x0f45 };
constexpr Reg CpMeInstrPntr       { 0, 0x0f46 };
constexpr Reg CpMeCntl            { 1, 0x0803 };
constexpr Reg CpMecRs64Cntl       { 1, 0x2904 };
constexpr Reg CpMecRs64InstrPntr  { 1, 0x2908 };
constexpr Reg CpMesCntl           { 1, 0x2807 };
constexpr Reg RlcCntl             { 1, 0x4c00 };
constexpr Reg RlcStat             { 1, 0x4c04 };
constexpr Reg RlcGpmStat          { 1, 0x4e6c };
constexpr Reg RlcBootloadStatus   { 1, 0x4e7c };
constexpr Reg ImuCoreCtrl         { 1, 0x40b6 };
constexpr Reg ImuGfxResetCtrl     { 1, 0x40bc };

// GRBM_STATUS
constexpr uint32_t kGrbmCpBusy    = 1u << 29;
constexpr uint32_t kGrbmGuiActive = 1u << 31;
// GRBM_STATUS2
constexpr uint32_t kGrbm2SdmaBusy = 1u << 21;
constexpr uint32_t kGrbm2RlcBusy  = 1u << 26;
// CP_ME_CNTL
constexpr uint32_t kCpMePfpHalt   = 1u << 26;
constexpr uint32_t kCpMeMeHalt    = 1u << 28;
// CP_MEC_RS64_CNTL / CP_MES_CNTL
constexpr uint32_t kRs64PipeActiveShift = 26;   // PIPE0..3_ACTIVE = bits 26..29
constexpr uint32_t kRs64Halt      = 1u << 30;
// RLC_CNTL
constexpr uint32_t kRlcEnableF32  = 1u << 0;
// RLC_RLCS_BOOTLOAD_STATUS
constexpr uint32_t kRlcBootGfxInitDone = 1u << 1;
constexpr uint32_t kRlcBootGpmIramDone = 1u << 5;
constexpr uint32_t kRlcBootComplete    = 1u << 31;
// GFX_IMU_CORE_CTRL: CRESET = the IMU core is held in reset.
constexpr uint32_t kImuCoreReset  = 1u << 0;
// GFX_IMU_GFX_RESET_CTRL: all five GFX reset domains released.
constexpr uint32_t kImuGfxOutOfReset = 0x1f;

// --- SDMA 7 (inside GC) ---------------------------------------------------------
// Offsets are relative to the SDMA0 block. Registers in the hypervisor
// decode range [0x5880, 0x589a] sit in GC segment 1 and step 0x20 per
// instance; all others sit in segment 0 and step 0x600 (SDMA1).
constexpr uint32_t kSdmaHypStart  = 0x5880;
constexpr uint32_t kSdmaHypEnd    = 0x589a;
constexpr uint32_t kSdmaHypStride = 0x20;
constexpr uint32_t kSdmaStride    = 0x600;
constexpr uint32_t kSdmaInstances = 2;

constexpr uint32_t SdmaUcodeRev      = 0x0003;
constexpr uint32_t SdmaCntl          = 0x000d;
constexpr uint32_t SdmaStatusReg     = 0x0024;
constexpr uint32_t SdmaStatus1Reg    = 0x0025;
constexpr uint32_t SdmaQ0RbCntl      = 0x0080;
constexpr uint32_t SdmaQ0RbBase      = 0x0081;
constexpr uint32_t SdmaQ0RbBaseHi    = 0x0082;
constexpr uint32_t SdmaQ0RbRptr      = 0x0083;
constexpr uint32_t SdmaQ0RbWptr      = 0x0085;
constexpr uint32_t SdmaMcuCntl       = 0x588e;
constexpr uint32_t SdmaWatchdogCntl  = 0x002b;
constexpr uint32_t SdmaUtcl1Cntl     = 0x0035;
constexpr uint32_t SdmaQ0RbRptrHi    = 0x0084;
constexpr uint32_t SdmaQ0RbWptrHi    = 0x0086;
constexpr uint32_t SdmaQ0RptrAddrLo  = 0x0087;
constexpr uint32_t SdmaQ0RptrAddrHi  = 0x0088;
constexpr uint32_t SdmaQ0IbCntl      = 0x0089;
constexpr uint32_t SdmaQ0Doorbell    = 0x008f;
constexpr uint32_t SdmaQ0WptrPollLo  = 0x0098;
constexpr uint32_t SdmaQ0WptrPollHi  = 0x0099;
constexpr uint32_t SdmaQ0MinorPtrUpd = 0x009b;

// SDMA0_QUEUE0_RB_CNTL fields
constexpr uint32_t kSdmaRbSizeShift       = 1;          // [5:1] log2(dwords)
constexpr uint32_t kSdmaRbSizeMask        = 0x1fu << 1;
constexpr uint32_t kSdmaRbWptrPoll        = 1u << 8;
constexpr uint32_t kSdmaRbMcuWptrPoll     = 1u << 11;
constexpr uint32_t kSdmaRbRptrWriteback   = 1u << 12;
constexpr uint32_t kSdmaRbPriv            = 1u << 23;
// SDMA0_QUEUE0_IB_CNTL / DOORBELL / MCU_CNTL / UTCL1_CNTL / WATCHDOG_CNTL
constexpr uint32_t kSdmaIbEnable          = 1u << 0;
constexpr uint32_t kSdmaDoorbellEnable    = 1u << 28;
constexpr uint32_t kSdmaMcuReset          = 1u << 1;
constexpr uint32_t kSdmaUtcl1RedoMask     = 0x1fu;       // REDO_DELAY [4:0]
constexpr uint32_t kSdmaUtcl1RespMask     = 0x3u << 9;   // RESP_MODE [10:9]
constexpr uint32_t kSdmaWatchdogHangMask  = 0xffu;       // QUEUE_HANG_COUNT [7:0]

// SDMA0_STATUS_REG
constexpr uint32_t kSdmaIdle          = 1u << 0;
constexpr uint32_t kSdmaUcodeInitDone = 1u << 27;
// SDMA0_MCU_CNTL
constexpr uint32_t kSdmaMcuHalt       = 1u << 0;
// SDMA0_QUEUE0_RB_CNTL
constexpr uint32_t kSdmaRbEnable      = 1u << 0;

// Where an SDMA register of `instance` lives.
constexpr Reg sdma(uint32_t instance, uint32_t off) {
	return (off >= kSdmaHypStart && off <= kSdmaHypEnd)
	           ? Reg { 1, off + kSdmaHypStride * instance }
	           : Reg { 0, off + kSdmaStride * instance };
}

// --- GC hub (GCMC / GCVM): what GFX, compute and SDMA translate through --------
constexpr Reg GcFbLocationBase    { 0, 0x1614 };   // [23:0] MC address >> 24
constexpr Reg GcFbLocationTop     { 0, 0x1615 };
constexpr Reg GcAgpTop            { 0, 0x1616 };
constexpr Reg GcAgpBot            { 0, 0x1617 };
constexpr Reg GcAgpBase           { 0, 0x1618 };
constexpr Reg GcSysApertureLow    { 0, 0x1619 };   // MC address >> 18
constexpr Reg GcFbOffset          { 0, 0x15a7 };
constexpr Reg GcMxL1TlbCntl       { 0, 0x161b };
constexpr Reg GcL2Cntl            { 0, 0x15c4 };
constexpr Reg GcCtx0Cntl          { 0, 0x1624 };
constexpr Reg GcCtx0PtBaseLo      { 0, 0x168f };
constexpr Reg GcCtx0PtBaseHi      { 0, 0x1690 };
constexpr Reg GcCtx0PtStartLo     { 0, 0x16af };
constexpr Reg GcCtx0PtStartHi     { 0, 0x16b0 };
constexpr Reg GcCtx0PtEndLo       { 0, 0x16cf };
constexpr Reg GcCtx0PtEndHi       { 0, 0x16d0 };
constexpr Reg GcSysApertureHigh   { 0, 0x161a };   // MC address >> 18
constexpr Reg GcSysDefaultLsb     { 0, 0x15a8 };   // VRAM offset >> 12
constexpr Reg GcSysDefaultMsb     { 0, 0x15a9 };   // VRAM offset >> 44
constexpr Reg GcL2FaultDefaultLo  { 0, 0x15d4 };   // address >> 12
constexpr Reg GcL2FaultDefaultHi  { 0, 0x15d5 };   // address >> 44
constexpr Reg GcL2FaultCntl2      { 0, 0x15cd };
constexpr Reg GcL2Cntl2           { 0, 0x15c5 };
constexpr Reg GcL2Cntl3           { 0, 0x15c6 };
constexpr Reg GcL2Cntl4           { 0, 0x15dd };
constexpr Reg GcL2Cntl5           { 0, 0x15e3 };
constexpr Reg GcIdentLowLo        { 0, 0x15d7 };   // CONTEXT1 identity aperture
constexpr Reg GcIdentLowHi        { 0, 0x15d8 };
constexpr Reg GcIdentHighLo       { 0, 0x15d9 };
constexpr Reg GcIdentHighHi       { 0, 0x15da };
constexpr Reg GcIdentOffsetLo     { 0, 0x15db };
constexpr Reg GcIdentOffsetHi     { 0, 0x15dc };
constexpr Reg GcInvEng0Req        { 0, 0x1647 };   // + engine
constexpr Reg GcInvEng0Ack        { 0, 0x1659 };   // + engine
constexpr Reg GcInvEng0RangeLo    { 0, 0x166b };   // + 2 * engine
constexpr Reg GcInvEng0RangeHi    { 0, 0x166c };
constexpr uint32_t kGcInvEngines  = 18;
constexpr uint32_t kGcInvEngGart  = 17;            // amdgpu's engine for VMID0 flushes

// GCMC_VM_MX_L1_TLB_CNTL: L1 TLB on, system access mode 3 (unmapped ->
// system aperture), advanced driver model, MTYPE UC.
constexpr uint32_t kL1TlbEnable       = 1u << 0;
constexpr uint32_t kL1TlbSysAccess3   = 3u << 3;
constexpr uint32_t kL1TlbSysUnmapped  = 1u << 5;
constexpr uint32_t kL1TlbAdvDriver    = 1u << 6;
constexpr uint32_t kL1TlbEcoMask      = 0xfu << 7;
constexpr uint32_t kL1TlbMtypeMask    = 0x7u << 11;
constexpr uint32_t kL1TlbMtypeUc      = 3u << 11;
// GCVM_L2_CNTL
constexpr uint32_t kL2EnableCache         = 1u << 0;
constexpr uint32_t kL2FragmentProcessing  = 1u << 1;
constexpr uint32_t kL2Pde0TagGenMode      = 1u << 8;
constexpr uint32_t kL2DefaultPageToSys    = 1u << 11;
constexpr uint32_t kL2PdeFaultClassify    = 1u << 18;
constexpr uint32_t kL2Ctx1IdentityAccess  = 1u << 19;
constexpr uint32_t kL2IdentityFragMask    = 0x1fu << 21;
// GCVM_L2_CNTL2
constexpr uint32_t kL2InvalidateL1Tlbs    = 1u << 0;
constexpr uint32_t kL2InvalidateL2Cache   = 1u << 1;
// Reset values gfxhub_v12_0 starts from, and the fields it changes.
constexpr uint32_t kL2Cntl3Default        = 0x80120007;
constexpr uint32_t kL2Cntl4Default        = 0x000000c1;
constexpr uint32_t kL2Cntl5Default        = 0x00003fe0;
constexpr uint32_t kL2Cntl3BankMask       = 0x3fu;          // BANK_SELECT [5:0]
constexpr uint32_t kL2Cntl3BigKMask       = 0x1fu << 15;    // BIGK_FRAGMENT_SIZE [19:15]
constexpr uint32_t kL2Cntl4TapPhysMask    = 3u << 6;        // PDE/PTE_REQUEST_PHYSICAL
constexpr uint32_t kL2Cntl5SmallKMask     = 0x1fu;          // SMALLK_FRAGMENT_SIZE
constexpr uint32_t kL2FaultRetryRead      = 1u << 18;       // CNTL2 ACTIVE_PAGE_MIGRATION_PTE_READ_RETRY
// GCVM_CONTEXT0_CNTL
constexpr uint32_t kCtxDepthMask          = 3u << 1;
constexpr uint32_t kCtxRetryPermFault     = 1u << 8;
// GCVM_INVALIDATE_ENGx_REQ for VMID0, legacy flush: L2 PTEs, PDE0-2, L1 PTEs.
constexpr uint32_t kInvReqVmid0           = (1u << 0) | (1u << 19) | (1u << 20) |
                                            (1u << 21) | (1u << 22) | (1u << 23);

// --- MM hub (MMMC / MMVM): display, PSP and the rest of the SoC -----------------
constexpr Reg MmFbLocationBase    { 0, 0x0554 };
constexpr Reg MmFbLocationTop     { 0, 0x0555 };
constexpr Reg MmAgpTop            { 0, 0x0556 };
constexpr Reg MmAgpBot            { 0, 0x0557 };
constexpr Reg MmAgpBase           { 0, 0x0558 };
constexpr Reg MmSysApertureLow    { 0, 0x0559 };
constexpr Reg MmSysApertureHigh   { 0, 0x055a };
constexpr Reg MmFbOffset          { 0, 0x04c7 };
constexpr Reg MmL2Cntl            { 0, 0x04e4 };
constexpr Reg MmCtx0Cntl          { 0, 0x0564 };
constexpr Reg MmCtx0PtBaseLo      { 0, 0x05cf };
constexpr Reg MmCtx0PtBaseHi      { 0, 0x05d0 };

// VMx_CONTEXT0_CNTL
constexpr uint32_t kVmCtxEnable   = 1u << 0;
// VMx_L2_CNTL
constexpr uint32_t kVmL2Enable    = 1u << 0;

// --- NBIF 6.3.1: where a BAR5 write flushes the HDP (host data path) write
// cache, so CPU writes to VRAM through BAR0 become visible to the GPU. Holds a
// BAR5 byte offset; 0 = not set up.
constexpr Reg NbifRemapHdpMemFlush { 2, 0x012d };
constexpr Reg NbifRemapHdpRegFlush { 2, 0x012e };

// --- MP1 (SMU) mailbox, segment 1 (mp_14_0_2_offset.h; smu_v14_0_2) -------------
constexpr Reg SmuMsg              { 1, 0x0082 };   // C2PMSG_66
constexpr Reg SmuArg              { 1, 0x0092 };   // C2PMSG_82
constexpr Reg SmuResp             { 1, 0x009a };   // C2PMSG_90
constexpr uint32_t kSmuMsgTest       = 0x1;        // PPSMC_MSG_TestMessage
constexpr uint32_t kSmuMsgGetVersion = 0x2;        // PPSMC_MSG_GetSmuVersion

// --- MP0 (PSP) scratch mailbox, segment 0 ----------------------------------------
constexpr Reg PspBootStatus       { 0, 0x0063 };   // C2PMSG_35: bit31 bootloader ready
constexpr Reg PspRingStatus       { 0, 0x0080 };   // C2PMSG_64
constexpr Reg PspSosVersion       { 0, 0x0091 };   // C2PMSG_81: nonzero = sOS alive

} // namespace GfxReg

#endif /* GfxRegs_hpp */
