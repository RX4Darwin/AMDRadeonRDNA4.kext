/*
 * AMD Radeon RX 9070 XT (Navi 48, RDNA 4) display-engine model.
 *
 * A development model for RDNA4FB: it emulates what the kext's bare-metal
 * path touches, so the unmodified kext runs its real code paths in a VM.
 *
 *  - PCI identity and BAR layout of the card: BAR0/1 VRAM aperture, BAR2/3
 *    doorbells (the MEC compute queue), BAR4 I/O (inert), BAR5 registers, expansion ROM
 *    (romfile=, normally the card's legacy AtomBIOS image + an EFI GOP driver).
 *  - VRAM: the aperture is RAM; the rest of VRAM is reachable through
 *    MM_INDEX/MM_DATA, with the top 64 MiB modelled (the IP discovery copy
 *    the PSP leaves at VRAM top - 64 KiB lives there).
 *  - Registers: a flat BAR5 image, loaded at reset from a register image in
 *    tools/linux-capture.sh's format (state=), i.e. the state the GOP leaves.
 *  - Live engines: OTG frame counter and vblank status, DC_I2C (EDID from a
 *    DDC slave on one line), DP AUX (no sink attached), RCC_CONFIG_MEMSIZE,
 *    the DMUB inbox1 ring (QUERY_FEATURE_CAPS answered; the VBIOS-family
 *    SET_PIXEL_CLOCK / DIG1_TRANSMITTER_CONTROL / DIGX_ENCODER_CONTROL
 *    commands drive the model's PHY PLLs and transmitters), the PSP (MP0)
 *    bootloader mailbox and GPCOM ring (sOS components, ring creation,
 *    LOAD_TOC, LOAD_IP_FW, fences; buffers are read through the MM hub's FB
 *    aperture) and the SMU (MP1) mailbox, which, as on the card, only
 *    answers once the SMU firmware came in through the PSP. After a complete
 *    AUTOLOAD_RLC the GC shows its booted state, the GC hub acknowledges TLB
 *    flushes, and SDMA0 queue 0 runs NOP/WRITE/COPY/FENCE/CONST_FILL packets
 *    through the GC hub (which must have been set up by the driver). A MEC
 *    compute queue, fed through the doorbell BAR, runs PM4 (SET_UCONFIG_REG,
 *    SET_SH_REG, WRITE_DATA, ACQUIRE/RELEASE_MEM, DISPATCH_DIRECT); a
 *    dispatch runs each work-item through a small GFX12 interpreter that
 *    knows the test kernel's instructions.
 *    OTG_MASTER_UPDATE_LOCK holds the double-buffered OTG timing and HUBP
 *    surface registers until it is released.
 *  - An attached monitor: the scanout only shows when the lit OTG's DIG
 *    drives an enabled transmitter at the OTG's pixel clock and the
 *    resulting timing is inside the EDID's range limits; otherwise the
 *    console says "no signal" / "out of range", as the monitor would.
 *  - Scanout: follows the lit OTG -> OPP -> MPCC -> HUBP surface registers
 *    into the QEMU console; the OPP pattern generator blanks it.
 *
 * Not a GPU: no GFX, SDMA or VCN, and no PSP/DMUB/PMFW firmware — only the
 * register protocols the display driver speaks to them.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <math.h>
#include <sys/mman.h>
#include "qemu/module.h"
#include "qemu/units.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "qemu/host-utils.h"
#include "qemu/error-report.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pcie.h"
#include "hw/pci/msi.h"
#include "qapi/visitor.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "ui/console.h"
#include "ui/qemu-pixman.h"
#include "qom/object.h"

/* SmuMetrics_t offsets from smu14_driver_if_v14_0.h; QEMU compiles this
 * source outside the repo, so keep the model's table ABI constants here. */
#define RDNA4_SMU_METRICS_AVG_GFXCLK_POST_DS 48u
#define RDNA4_SMU_METRICS_AVG_MEMCLK_POST_DS 56u
#define RDNA4_SMU_METRICS_AVG_SOCKET_POWER  136u
#define RDNA4_SMU_METRICS_AVG_TEMPERATURE   140u
#define RDNA4_SMU_METRICS_AVG_FAN_RPM       170u
#define RDNA4_SMU_METRICS_CURR_CLOCK          0u
#define RDNA4_SMU_METRICS_AVG_GFXCLK_PRE_DS  46u
#define RDNA4_SMU_METRICS_COUNTER           104u
#define RDNA4_SMU_METRICS_AVG_VOLTAGE       108u
#define RDNA4_SMU_METRICS_AVG_CURRENT       116u
#define RDNA4_SMU_METRICS_AVG_GFX_ACTIVITY  124u
#define RDNA4_SMU_METRICS_AVG_UCLK_ACTIVITY 126u
#define RDNA4_SMU_METRICS_THROTTLING_PCT    172u

#define TYPE_RDNA4 "rdna4"
OBJECT_DECLARE_SIMPLE_TYPE(RDNA4State, RDNA4)

#define RDNA4_MMIO_SIZE      (1 * MiB)
#define RDNA4_DOORBELL_SIZE  (2 * MiB)
#define RDNA4_IO_SIZE        256
#define RDNA4_VRAM_MB        16304          /* RCC_CONFIG_MEMSIZE on the card */
#define RDNA4_RESV_SIZE      (64 * MiB)     /* modelled top of VRAM */
#define RDNA4_DISCOVERY_TOP  (64 * KiB)     /* discovery at VRAM top - 64 KiB */

#define NUM_OTG  4
#define NUM_AUX  4
#define NUM_DDC  8
#define RDNA4_WORK_SLICE_NS 2000000ULL     /* hard upper bound from the W11 brief */
#define RDNA4_WORK_REARM_NS 500000ULL      /* let vCPU/I/O work run between slices */
/* ASSUMPTION, not a measurement: how long after a warm reset the firmware/GOP
 * re-enables PCI Bus Master.  Nothing in the logs, amdgpu or UEFI sources fixes
 * it; it is the device property warm-gop-delay-ms.  The default is chosen, not
 * known: a vblank IRQ whose IH write is refused while Bus Master is off is not
 * raised again until acknowledged (a dead guest never does), so a delay above
 * one vblank period (16.7 ms at 60 Hz) would count no post-reset DMA from a
 * card left running.  8 ms keeps that no-quiesce case visible. */
#define RDNA4_WARM_GOP_DELAY_MS_DEFAULT 8

/*
 * DMU (DCN 4.1.0) register segment bases in dwords, from the card's IP
 * discovery table (HWID 271). BAR5 byte offset = (base + dword) * 4.
 */
#define DMU_SEG1  0xc0
#define DMU_SEG2  0x34c0
#define DMU_SEG3  0x9000
#define SEG1(dw)  ((DMU_SEG1 + (dw)) * 4)
#define SEG2(dw)  ((DMU_SEG2 + (dw)) * 4)
#define SEG3(dw)  ((DMU_SEG3 + (dw)) * 4)

/* Fixed BIF/NBIF registers (byte offsets). */
#define REG_MM_INDEX         0x0
#define REG_MM_DATA          0x4
#define REG_MM_INDEX_HI      0x18
#define REG_CONFIG_MEMSIZE   0x378c

/* OTG / OPTC (seg2) */
#define OTG_STRIDE           0x80
#define OTG_H_TOTAL          0x1b2a
#define OTG_H_BLANK          0x1b2b     /* START [14:0], END [30:16] */
#define OTG_V_TOTAL          0x1b2f
#define OTG_V_BLANK          0x1b38
#define OTG_V_SYNC_A_CNTL    0x1b3a
#define OTG_CONTROL          0x1b43     /* MASTER_EN [0], CURRENT_MASTER_EN_STATE [16] */
#define OTG_STATUS           0x1b49     /* V_BLANK [0] */
#define OTG_FRAME_COUNT      0x1b4d     /* [23:0] */
#define OTG_GLOBAL_SYNC_STATUS 0x1b88  /* VSTARTUP interrupt */
#define OTG_STATUS_POSITION  0x1b4a     /* regOTG0_OTG_STATUS_POSITION, BASE_IDX 2 */
#define OTG_MASTER_UPDATE_LOCK 0x1b89   /* LOCK [0], UPDATE_LOCK_STATUS [8] */
#define OTG_CLOCK_CONTROL    0x1b84     /* EN [0], GATE_DIS [1], CLOCK_ON [8], BUSY [16] */
#define OPTC_INPUT_CLOCK     0x1ad0     /* GATE_DIS [0], EN [1], CLK_ON [2] */
#define ODM_STRIDE           0x10
#define OPTC_DATA_SOURCE     0x1acb     /* SEG0_SRC_SEL [19:16] = OPP */
/* OPP (seg2) */
#define OPP_STRIDE           0x5a
#define DPG_CONTROL          0x1854     /* DPG_EN [0] */
/* HUBP (seg2) */
#define HUBP_STRIDE          0xdc
#define HUBP_VIEWPORT_DIM    0x05eb
#define HUBP_SURFACE_PITCH   0x0607     /* [15:0] = pixels - 1 */
#define HUBP_SURFACE_ADDR    0x060a
#define HUBP_SURFACE_ADDR_HI 0x060b
#define HUBP_FLIP_INTERRUPT  0x0617     /* DCSURF_SURFACE_FLIP_INTERRUPT */
#define HUBP_EARLIEST_ADDR   0x061c     /* DCSURF_SURFACE_EARLIEST_INUSE */
#define HUBP_EARLIEST_ADDR_HI 0x061d   /* DCSURF_SURFACE_EARLIEST_INUSE_HIGH */
#define HUBP_DB_FIRST        0x05e5     /* DCSURF_SURFACE_CONFIG .. */
#define HUBP_FLIP_CONTROL    0x0613     /* regHUBPREQ0_DCSURF_FLIP_CONTROL, BASE_IDX 2 */
#define HUBP_FLIP_PENDING    (1u << 8)
#define HUBP_DB_LAST         0x0613     /* .. regHUBPREQ0_DCSURF_FLIP_CONTROL */
#define HUBP_CURSOR_SETTINGS 0x0653     /* regHUBPREQ0_CURSOR_SETTINGS, BASE_IDX 2 */
#define HUBP_CUR0_TTU_CNTL0  0x0627     /* regHUBPREQ0_DCN_CUR0_TTU_CNTL0, BASE_IDX 2; CNTL1 = +1 */
#define HUBP_CURSOR_CONTROL  0x0679     /* regCURSOR0_0_CURSOR_CONTROL, BASE_IDX 2 */
#define HUBP_CURSOR_ADDRESS  0x067a     /* regCURSOR0_0_CURSOR_SURFACE_ADDRESS, BASE_IDX 2 */
#define HUBP_CURSOR_ADDRESS_HI 0x067b   /* regCURSOR0_0_CURSOR_SURFACE_ADDRESS_HIGH, BASE_IDX 2 */
#define HUBP_CURSOR_SIZE     0x067c     /* regCURSOR0_0_CURSOR_SIZE, BASE_IDX 2 */
#define HUBP_CURSOR_POSITION 0x067d     /* regCURSOR0_0_CURSOR_POSITION, BASE_IDX 2 */
#define HUBP_CURSOR_HOT_SPOT 0x067e     /* regCURSOR0_0_CURSOR_HOT_SPOT, BASE_IDX 2 */
#define HUBP_CURSOR_DST_OFFSET 0x0680   /* regCURSOR0_0_CURSOR_DST_OFFSET, BASE_IDX 2 */
#define CURSOR_CM_CONTROL    0x0cf1     /* regCM_CUR0_CURSOR0_CONTROL, BASE_IDX 2 */
#define CURSOR_CM_SCALE_GY   0x0cf4     /* regCM_CUR0_CURSOR0_FP_SCALE_BIAS_G_Y, BASE_IDX 2 */
#define CURSOR_CM_SCALE_RB   0x0cf5     /* regCM_CUR0_CURSOR0_FP_SCALE_BIAS_RB_CRCB, BASE_IDX 2 */
#define CURSOR_REQ_MODE      (1u << 2)
#define CURSOR_MODE_SHIFT    8
#define CURSOR_PITCH_SHIFT   16
#define CURSOR_LINES_SHIFT   24
#define CURSOR_CM_ENABLE     1u
#define CURSOR_CM_MODE_SHIFT 4
#define CURSOR_FP16_ONE      0x3c00
#define DCN_VSTARTUP_ENABLE   (1u << 0)
#define DCN_VSTARTUP_OCCURRED (1u << 2)
#define DCN_VSTARTUP_STATUS   (1u << 3)
#define DCN_VSTARTUP_CLEAR    (1u << 4)
#define DCN_FLIP_ENABLE       (1u << 0)
#define DCN_FLIP_OCCURRED     (1u << 16)
#define DCN_FLIP_STATUS       (1u << 17)
#define DCN_FLIP_CLEAR        (1u << 8)
#define DCN_CLIENT             0x04
#define DCN_SRC_VBLANK         0x3c
#define DCN_SRC_PFLIP          0x4f
#define DCN_VM_FB_LOC_BASE   0x0475     /* [23:0] = MC address >> 24 */
/* DIG front-/back-ends (seg2) */
#define DIG_STRIDE           0x124
#define DIG_FE_CNTL          0x2093     /* SOURCE_SELECT [2:0] = OTG */
#define DIG_FE_CLK_CNTL      0x2094     /* FE_MODE [2:0], FE_CLK_EN [4] */
#define DIG_FE_EN_CNTL       0x2095     /* ENABLE [0] */
#define DIG_FIFO_CTRL0       0x209b     /* ENABLE [0], RESET [1], RESET_DONE [20] */
#define DIG_HDMI_GC          0x20a8     /* AVMUTE [0] */
#define DIG_BE_CLK_CNTL      0x20bb     /* BE_MODE [2:0], BE_CLK_EN [4] */
#define DIG_BE_CNTL          0x20bc     /* FE_SOURCE_SELECT [14:8] */
#define DIG_BE_EN_CNTL       0x20bd     /* BE_ENABLE [0] */
#define DIG_MODE_HDMI        3
#define DPP_STRIDE           0x16b
#define DSCL_RECOUT_SIZE     0x0d1f     /* WIDTH [13:0], HEIGHT [29:16] */
#define STREAM_MAPPER        0x1f0d     /* + dig: LINK_TARGET [2:0] */
#define NUM_DIG              4
#define NUM_PHY              8
/* MPC (seg3) */
#define MPCC_STRIDE          0x15
#define MPCC_TOP_SEL         0x0000     /* regMPCC0_MPCC_TOP_SEL, BASE_IDX 3 */
#define MPCC_OPP_ID          0x0002     /* regMPCC0_MPCC_OPP_ID, BASE_IDX 3; [3:0], 0xf = none */

/* DC_I2C (seg2) */
#define I2C_CONTROL          0x1e98
#define I2C_ARBITRATION      0x1e99
#define I2C_SW_STATUS        0x1e9b
#define I2C_TRANSACTION0     0x1eae     /* 4 transactions */
#define I2C_DATA             0x1eb2
#define DIO_MEM_PWR_STATUS   0x1edd
#define DIO_MEM_PWR_CTRL     0x1ede

#define I2C_GO               (1u << 0)
#define I2C_SOFT_RESET       (1u << 1)
#define I2C_SW_STATUS_RESET  (1u << 3)
#define I2C_ARB_SW_USE_REQ   (1u << 20)
#define I2C_ARB_SW_DONE      (1u << 21)
#define I2C_ST_DONE          (1u << 2)
#define I2C_ST_NACK          (1u << 8)
#define I2C_TXN_READ         (1u << 0)
#define I2C_TXN_START        (1u << 12)
#define I2C_DATA_READ        (1u << 0)
#define I2C_DATA_INDEX_WRITE (1u << 31)

/* DP AUX (seg2), engine n at AUX_BASE + n * AUX_STRIDE */
#define AUX_BASE             0x16b2
#define AUX_STRIDE           0x1c
#define AUX_SW_CONTROL       0x1
#define AUX_ARB_CONTROL      0x2
#define AUX_INT_CONTROL      0x3
#define AUX_SW_STATUS        0x4
#define AUX_SW_DATA          0x6

#define AUX_SW_GO            (1u << 0)
#define AUX_ARB_USE_REQ      (1u << 16)
#define AUX_ARB_DONE_USING   (1u << 17)
#define AUX_INT_DONE_ACK     (1u << 1)
#define AUX_ST_DONE          (1u << 0)
#define AUX_ST_HPD_DISCON    (1u << 9)

/* DMCUB (seg2): the DMUB firmware's inbox1 ring lives in VRAM at REGION4 */
#define DMCUB_REGION4_OFFSET    0x0196
#define DMCUB_REGION4_OFFSET_HI 0x0197
#define DMCUB_INBOX1_SIZE       0x01d5
#define DMCUB_INBOX1_WPTR       0x01d6
#define DMCUB_INBOX1_RPTR       0x01d7
#define DMUB_CMD_SIZE           64
#define DMUB_CMD_QUERY_FEATURE_CAPS 6
#define DMUB_CMD_VBIOS          128
#define VBIOS_DIGX_ENCODER_CONTROL     0
#define VBIOS_DIG1_TRANSMITTER_CONTROL 1
#define VBIOS_SET_PIXEL_CLOCK          2
#define TRANSMITTER_ACTION_DISABLE     0
#define TRANSMITTER_ACTION_ENABLE      1

/* SMU message mailbox: MP1 C2PMSG_66/82/90, MP1 segment 1 (dword 0x16200) */
#define MP1_SEG1             0x16200
#define REG_SMU_MSG          ((MP1_SEG1 + 0x0082) * 4)
#define REG_SMU_PARAM        ((MP1_SEG1 + 0x0092) * 4)
#define REG_SMU_RESP         ((MP1_SEG1 + 0x009a) * 4)
#define SMU_RESP_OK          0x01
#define SMU_RESP_UNKNOWN     0xfe
#define SMU_PMFW_VERSION     0x00685000 /* smu_14_0_3.bin's ucode_version */

/*
 * PSP (MP0 14.0.3) mailbox: MPASP_SMN_C2PMSG_n, MP0 segment 0 (dword 0x16000).
 * The bootloader takes components via C2PMSG_35/36; the sOS answers ring
 * control in C2PMSG_64 and takes GPCOM frames when C2PMSG_67 moves.
 */
#define MP0_SEG0             0x16000
#define REG_PSP(n)           ((MP0_SEG0 + (n)) * 4)
#define REG_PSP_BL_CMD       REG_PSP(0x63)   /* C2PMSG_35 */
#define REG_PSP_BL_BUF       REG_PSP(0x64)   /* C2PMSG_36: MC address >> 20 */
#define REG_PSP_RING_CTL     REG_PSP(0x80)   /* C2PMSG_64 */
#define REG_PSP_RING_WPTR    REG_PSP(0x83)   /* C2PMSG_67 (dwords) */
#define REG_PSP_RING_LO      REG_PSP(0x85)   /* C2PMSG_69 */
#define REG_PSP_RING_HI      REG_PSP(0x86)   /* C2PMSG_70 */
#define REG_PSP_RING_SIZE    REG_PSP(0x87)   /* C2PMSG_71 */
#define REG_PSP_SOS          REG_PSP(0x91)   /* C2PMSG_81: sOS sign of life */
#define PSP_READY            0x80000000u
#define PSP_BL_SYSDRV        0x10000
#define PSP_BL_SOSDRV        0x20000
#define PSP_SOS_VERSION      0x003a1214     /* psp_14_0_3_sos.bin's sOS */
#define PSP_FRAME_SIZE       64
#define PSP_CMD_ID           8              /* psp_gfx_cmd_resp.cmd_id */
#define PSP_CMD_ARGS         28             /* .cmd */
#define PSP_RESP_STATUS      864            /* .resp.status */
#define PSP_RESP_FW_LO       872
#define PSP_RESP_TMR_SIZE    880
#define PSP_CMD_LOAD_IP_FW   0x06
#define PSP_CMD_LOAD_TOC     0x20
#define PSP_CMD_AUTOLOAD_RLC 0x21
#define PSP_CMD_FB_RESERV    0x50
#define PSP_FW_TYPE_SMU      18
#define PSP_ERR_AUTOLOAD     0x9            /* model: GC firmware set incomplete */

/*
 * GC 12.0.1 registers the RLC autoload changes (GC seg0 dword 0x1260, seg1
 * 0xa000). The model does not run the firmware; after a complete
 * AUTOLOAD_RLC it shows the state amdgpu waits for.
 */
#define GC_SEG0(dw)          ((0x1260 + (dw)) * 4)
#define GC_SEG1(dw)          ((0xa000 + (dw)) * 4)
#define REG_CP_HQD_ACTIVE_EARLY GC_SEG0(0x1fab)
#define REG_CP_HQD_DEQUEUE_REQ  GC_SEG0(0x1fc1)
#define REG_SQ_CMD              GC_SEG0(0x111b)
#define OSSSYS_SEG0          0x10a0
#define OSSSYS(dw)           ((OSSSYS_SEG0 + (dw)) * 4)
#define REG_IH_RB_CNTL       OSSSYS(0x0080)
#define REG_IH_RB_RPTR       OSSSYS(0x0081)
#define REG_IH_RB_WPTR       OSSSYS(0x0082)
#define REG_IH_RB_BASE       OSSSYS(0x0083)
#define REG_IH_RB_BASE_HI    OSSSYS(0x0084)
#define REG_IH_WPTR_ADDR_HI  OSSSYS(0x0085)
#define REG_IH_WPTR_ADDR_LO  OSSSYS(0x0086)
#define REG_IH_DOORBELL      OSSSYS(0x0087)
#define REG_IH_RB_CNTL_RING1 OSSSYS(0x008c)
#define REG_IH_CHICKEN       OSSSYS(0x018a)
#define IH_RB_ENABLE         (1u << 0)
#define IH_WPTR_WRITEBACK    (1u << 8)
#define IH_WPTR_OVERFLOW_EN  (1u << 16)
#define IH_ENABLE_INTR       (1u << 17)
#define IH_MC_SPACE_SHIFT    28
#define IH_WPTR_OVERFLOW_CLR (1u << 31)
#define IH_WPTR_OVERFLOW     1u
#define IH_MC_SPACE_BUS      2u
#define REG_CP_ME1_PIPE0_INT_CNTL GC_SEG0(0x1e25)
#define CP_TIME_STAMP_INT_ENABLE  (1u << 26)
#define REG_GC_CP_STAT       GC_SEG0(0x0f40)
#define REG_GC_RLC_BOOTLOAD  GC_SEG1(0x4e7c)   /* BOOTLOAD_COMPLETE [31] */
#define REG_GC_RLC_CNTL      GC_SEG1(0x4c00)
#define REG_GC_IMU_CORE_CTRL GC_SEG1(0x40b6)   /* CRESET [0] */
#define REG_GC_IMU_GFX_RESET GC_SEG1(0x40bc)   /* 0x1f = domains released */
#define REG_GC_SDMA0_STATUS  GC_SEG0(0x0024)
#define REG_GC_SDMA1_STATUS  GC_SEG0(0x0624)
#define SDMA_STATUS_BOOTED   0x08000001u       /* UCODE_INIT_DONE | IDLE */

/* GC hub: FB aperture, L1 TLB, VMID0 context and the GART flush engine. */
#define REG_GCMC_FB_BASE     GC_SEG0(0x1614)   /* MC >> 24 */
#define REG_GCMC_FB_TOP      GC_SEG0(0x1615)
#define REG_GCMC_FB_OFFSET   GC_SEG0(0x15a7)   /* GPU physical FB offset >> 24 */
#define REG_GCMC_L1_TLB      GC_SEG0(0x161b)   /* ENABLE_L1_TLB [0] */
#define REG_GCVM_CTX0_CNTL   GC_SEG0(0x1624)   /* ENABLE_CONTEXT [0] */
#define REG_GCVM_CTX1_CNTL   GC_SEG0(0x1625)
#define REG_GCVM_CTX1_BASE_LO GC_SEG0(0x1691)
#define REG_GCVM_CTX1_BASE_HI GC_SEG0(0x1692)
#define REG_GCVM_CTX1_START_LO GC_SEG0(0x16b1)
#define REG_GCVM_CTX1_START_HI GC_SEG0(0x16b2)
#define REG_GCVM_CTX1_END_LO GC_SEG0(0x16d1)
#define REG_GCVM_CTX1_END_HI GC_SEG0(0x16d2)
#define REG_GCVM_FAULT_CNTL   GC_SEG0(0x15cc)
#define REG_GCVM_FAULT_STATUS GC_SEG0(0x15d0)
#define REG_GCVM_L2_CNTL GC_SEG0(0x15c4)
#define REG_GCVM_FAULT_ADDR_LO GC_SEG0(0x15d2)
#define REG_GCVM_FAULT_ADDR_HI GC_SEG0(0x15d3)
#define REG_GCVM_FAULT_DEFAULT_LO GC_SEG0(0x15d4)
#define REG_GCVM_FAULT_DEFAULT_HI GC_SEG0(0x15d5)
#define REG_GRBM_GFX_CNTL GC_SEG1(0x0900)
#define REG_GCVM_INV0_REQ    GC_SEG0(0x1647)
#define REG_GCVM_INV0_ACK    GC_SEG0(0x1659)
#define REG_GCVM_INV17_REQ   GC_SEG0(0x1647 + 17)
#define REG_GCVM_INV17_ACK   GC_SEG0(0x1659 + 17)

/* SDMA0 queue 0 (GC seg0) and MCU control (hypervisor range, seg1). */
#define REG_SDMA0_RB_CNTL    GC_SEG0(0x0080)   /* RB_ENABLE [0], RB_SIZE [5:1], RPTR_WB [12] */
#define REG_SDMA0_RB_BASE    GC_SEG0(0x0081)   /* MC >> 8 */
#define REG_SDMA0_RB_BASE_HI GC_SEG0(0x0082)   /* MC >> 40 */
#define REG_SDMA0_RB_RPTR    GC_SEG0(0x0083)   /* bytes */
#define REG_SDMA0_RB_WPTR    GC_SEG0(0x0085)   /* bytes */
#define REG_SDMA0_RPTR_LO    GC_SEG0(0x0087)   /* rptr writeback address */
#define REG_SDMA0_RPTR_HI    GC_SEG0(0x0088)
#define REG_SDMA0_CNTL       GC_SEG0(0x000d)   /* TRAP_ENABLE [0] */
#define REG_SDMA0_MCU_CNTL   GC_SEG1(0x588e)   /* HALT [0] */
#define PSP_ERR_UNKNOWN_CMD  0x100
#define PSP_TMR_SIZE         0x1400000      /* model's answer to LOAD_TOC */

/* GFX ring 0 (GC): gfx_v12_0_cp_gfx_resume / cp_gfx_start. */
#define REG_GFX_CP_RB0_RPTR       GC_SEG0(0x0f60)
#define REG_GFX_CP_RB_WPTR_DELAY  GC_SEG0(0x0f61)
#define REG_GFX_CP_RB0_BASE       GC_SEG0(0x1de0)
#define REG_GFX_CP_RB0_CNTL       GC_SEG0(0x1de1)
#define REG_GFX_CP_RB0_RPTR_ADDR  GC_SEG0(0x1de3)
#define REG_GFX_CP_RB0_RPTR_HI    GC_SEG0(0x1de4)
#define REG_GFX_CP_DEVICE_ID      GC_SEG0(0x1deb)
#define REG_GFX_CP_RB_VMID        GC_SEG0(0x1df1)
#define REG_GFX_CP_RB0_WPTR       GC_SEG0(0x1df4)
#define REG_GFX_CP_RB0_WPTR_HI    GC_SEG0(0x1df5)
#define REG_GFX_CP_RB_DB_LOWER    GC_SEG0(0x1dfa)
#define REG_GFX_CP_RB_DB_UPPER    GC_SEG0(0x1dfb)
#define REG_GFX_CP_MAX_CONTEXT    GC_SEG0(0x1e4e)
#define REG_GFX_CP_RB0_BASE_HI    GC_SEG0(0x1e51)
#define REG_GFX_CP_WPTR_POLL_LO   GC_SEG0(0x1e8b)
#define REG_GFX_CP_WPTR_POLL_HI   GC_SEG0(0x1e8c)
#define REG_GFX_CP_DB_CONTROL     GC_SEG0(0x1e8d)
#define REG_GFX_CP_RB_ACTIVE      GC_SEG0(0x1f40)
#define REG_GFX_CP_INT_CNTL_RING0 GC_SEG0(0x1e0a)
#define REG_GFX_CP_PFP_START      GC_SEG0(0x1e44)
#define REG_GFX_CP_ME_START       GC_SEG0(0x1e45)
#define REG_GFX_GRBM_GFX_CNTL     GC_SEG1(0x0900)
#define REG_GFX_CP_ME_CNTL        GC_SEG1(0x0803)
#define REG_GFX_RLC_CSIB_LO       GC_SEG1(0x0987)
#define REG_GFX_RLC_CSIB_HI       GC_SEG1(0x0988)
#define REG_GFX_RLC_CSIB_LENGTH   GC_SEG1(0x0989)
#define GFX_DOORBELL_DWORD        0x116
#define GFX_DOORBELL_RANGE_MASK   0x00000ffcu
#define GFX_DOORBELL_OFFSET_MASK  0x0ffffffcu
#define CP_PRIV_INSTR_INT_ENABLE  (1u << 22)
#define CP_PRIV_REG_INT_ENABLE    (1u << 23)
#define CP_OPCODE_ERROR_INT_ENABLE (1u << 24)
#define CP_GENERIC0_INT_ENABLE    (1u << 31)

/* gfx12 graphics registers use the absolute dword numbers from Mesa's
 * gfx12.json.  SH registers are in GC segment 0; context/UCONFIG registers
 * are in segment 1.  Keep the conversion explicit: a SET packet's offset is
 * relative to 0x2c00/0xa000/0xc000, while reg_get() takes a BAR byte offset. */
#define GFX12_SH_DW(dw)           GC_SEG0((dw) - 0x1260)
#define GFX12_CTX_DW(dw)          GC_SEG1((dw) - 0xa000)
#define GFX12_UCFG_DW(dw)         GC_SEG1((dw) - 0xa000)

#define REG_GFX_GB_ADDR_CONFIG    GC_SEG0(0x13de) /* gc_12_0_0: GB_ADDR_CONFIG */
#define REG_GFX_VGT_PRIMITIVE_TYPE GFX12_UCFG_DW(0xc242)
#define REG_GFX_VGT_GS_OUT_PRIM_TYPE GFX12_UCFG_DW(0xc266)
#define REG_GFX_GE_CNTL           GFX12_UCFG_DW(0xc25b)
#define REG_GFX_GE_MAX_VTX_INDX   GFX12_UCFG_DW(0xc259)
#define REG_GFX_GE_POS_RING_BASE  GFX12_UCFG_DW(0xc268)
#define REG_GFX_GE_POS_RING_SIZE  GFX12_UCFG_DW(0xc269)
#define REG_GFX_GE_PRIM_RING_BASE GFX12_UCFG_DW(0xc26a)
#define REG_GFX_GE_PRIM_RING_SIZE GFX12_UCFG_DW(0xc26b)

#define REG_GFX_SPI_SHADER_PGM_HI_ES GFX12_SH_DW(0x2c86)
#define REG_GFX_SPI_SHADER_PGM_LO_ES GFX12_SH_DW(0x2c89)
#define REG_GFX_SPI_SHADER_PGM_LO_GS GFX12_SH_DW(0x2c84)
#define REG_GFX_SPI_SHADER_PGM_HI_GS GFX12_SH_DW(0x2c85)
#define REG_GFX_SPI_SHADER_RSRC1_GS GFX12_SH_DW(0x2c8a)
#define REG_GFX_SPI_SHADER_RSRC2_GS GFX12_SH_DW(0x2c8b)
#define REG_GFX_SPI_SHADER_RSRC4_GS GFX12_SH_DW(0x2c88)
#define REG_GFX_SPI_SHADER_GS_OUT_CONFIG_PS GFX12_SH_DW(0x2c31)
#define REG_GFX_SPI_SHADER_PGM_RSRC4_PS GFX12_SH_DW(0x2c07)
#define REG_GFX_SPI_SHADER_PGM_LO_PS GFX12_SH_DW(0x2c08)
#define REG_GFX_SPI_SHADER_PGM_RSRC2_PS GFX12_SH_DW(0x2c0b)
#define REG_GFX_SPI_SHADER_USER_DATA_PS_0 GFX12_SH_DW(0x2c0c)
#define REG_GFX_SPI_SHADER_USER_DATA_PS_1 GFX12_SH_DW(0x2c0d)
/* amdgpu golden registers (gfx_v12_0.c:253-261): DB_MEM_CONFIG, CB_HW_CONTROL_1 (GC seg 0), GL2C_CTRL5 (seg 1). */
#define REG_GFX_DB_MEM_CONFIG     GC_SEG0(0x13d2)
#define REG_GFX_CB_HW_CONTROL_1   GC_SEG0(0x1425)
#define REG_GFX_GL2C_CTRL5        GC_SEG1(0x2e19)
#define REG_GFX_SPI_SHADER_PGM_HI_PS GFX12_SH_DW(0x2c09)
#define REG_GFX_SPI_SHADER_RSRC1_PS GFX12_SH_DW(0x2c0a)
#define REG_GFX_SPI_SHADER_RSRC2_PS GFX12_SH_DW(0x2c0b)

#define REG_GFX_VGT_SHADER_STAGES_EN GFX12_CTX_DW(0xa2a6)
#define REG_GFX_SPI_SHADER_POS_FORMAT GFX12_CTX_DW(0xa193)
#define REG_GFX_SPI_SHADER_COL_FORMAT GFX12_CTX_DW(0xa195)
#define REG_GFX_SPI_PS_INPUT_ENA    GFX12_CTX_DW(0xa197)
#define REG_GFX_SPI_PS_INPUT_ADDR   GFX12_CTX_DW(0xa198)
#define REG_GFX_SPI_PS_IN_CONTROL   GFX12_CTX_DW(0xa190)
#define REG_GFX_PA_CL_VTE_CNTL      GFX12_CTX_DW(0xa205)
#define REG_GFX_PA_SU_SC_MODE_CNTL  GFX12_CTX_DW(0xa207)
#define REG_GFX_PA_SC_EDGERULE      GFX12_CTX_DW(0xa08c)
#define REG_GFX_PA_SC_MODE_CNTL_0   GFX12_CTX_DW(0xa292)
#define REG_GFX_PA_SU_VTX_CNTL      GFX12_CTX_DW(0xa2f9)
#define REG_GFX_PA_SC_CLIPRECT_RULE GFX12_CTX_DW(0xa083)
#define REG_GFX_PA_SC_AA_CONFIG     GFX12_CTX_DW(0xa2f8)
#define REG_GFX_PA_SC_AA_MASK_0     GFX12_CTX_DW(0xa30e)
#define REG_GFX_PA_SC_AA_MASK_1     GFX12_CTX_DW(0xa30f)
#define REG_GFX_PA_SC_WINDOW_OFFSET GFX12_CTX_DW(0xa080)
#define REG_GFX_PA_SC_WINDOW_TL     GFX12_CTX_DW(0xa081)
#define REG_GFX_PA_SC_WINDOW_BR     GFX12_CTX_DW(0xa082)
#define REG_GFX_PA_SC_GENERIC_TL    GFX12_CTX_DW(0xa090)
#define REG_GFX_PA_SC_GENERIC_BR    GFX12_CTX_DW(0xa091)
#define REG_GFX_PA_SC_SCREEN_TL     GFX12_CTX_DW(0xa060)
#define REG_GFX_PA_SC_SCREEN_BR     GFX12_CTX_DW(0xa061)
#define REG_GFX_PA_SC_VPORT_TL      GFX12_CTX_DW(0xa094)
#define REG_GFX_PA_SC_VPORT_BR      GFX12_CTX_DW(0xa095)
#define REG_GFX_PA_CL_GB_VERT_CLIP  GFX12_CTX_DW(0xa10b)
#define REG_GFX_PA_CL_GB_HORZ_CLIP  GFX12_CTX_DW(0xa10d)
#define REG_GFX_PA_CL_VPORT_XSCALE  GFX12_CTX_DW(0xa10f)
#define REG_GFX_PA_CL_VPORT_XOFFSET GFX12_CTX_DW(0xa110)
#define REG_GFX_PA_CL_VPORT_YSCALE  GFX12_CTX_DW(0xa111)
#define REG_GFX_PA_CL_VPORT_YOFFSET GFX12_CTX_DW(0xa112)

#define REG_GFX_CB_COLOR_CONTROL    GFX12_CTX_DW(0xa216)
#define REG_GFX_CB_TARGET_MASK      GFX12_CTX_DW(0xa214)
#define REG_GFX_CB_SHADER_MASK      GFX12_CTX_DW(0xa215)
#define REG_GFX_CB_COLOR0_BASE      GFX12_CTX_DW(0xa318)
#define REG_GFX_CB_COLOR0_ATTRIB2   GFX12_CTX_DW(0xa31e)
#define REG_GFX_CB_COLOR0_ATTRIB3   GFX12_CTX_DW(0xa31f)
#define REG_GFX_CB_COLOR0_BASE_EXT  GFX12_CTX_DW(0xa390)
#define REG_GFX_CB_COLOR0_INFO      GFX12_CTX_DW(0xa3b0)
#define REG_GFX_CB_BLEND0_CONTROL   GFX12_CTX_DW(0xa1e0)
#define REG_GFX_DB_Z_INFO           GFX12_CTX_DW(0xa006)
#define REG_GFX_DB_STENCIL_INFO     GFX12_CTX_DW(0xa007)
#define REG_GFX_GE_RING_MIN_SE      4u

/* MM hub FB aperture (mmhub 4.1.0 segment 0, dword 0x1a000) */
#define REG_MMHUB_FB_BASE    ((0x1a000 + 0x0554) * 4)

#define ARB_STATUS_SHIFT     2          /* [3:2]: 0 idle, 1 SW, 2 HW/DMCU */
#define ARB_STATUS_MASK      (3u << ARB_STATUS_SHIFT)

/*
 * Lenovo G25-10 base block, read from the card's HDMI port over DC_I2C
 * (tools/atomdump.cpp fixture). The captured block announces one extension
 * that was never read; rdna4_load_edid() trims the count to what exists.
 */
static const uint8_t lenovo_g25_10_edid[128] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x30, 0xae, 0xfe, 0x65, 0x00, 0x00, 0x00, 0x00,
    0x32, 0x1e, 0x01, 0x03, 0x80, 0x36, 0x1e, 0x78, 0x2a, 0x90, 0x55, 0xa7, 0x55, 0x53, 0xa0, 0x28,
    0x13, 0x50, 0x54, 0xa1, 0x08, 0x00, 0xd1, 0xc0, 0xb3, 0x00, 0x81, 0xc0, 0x81, 0x80, 0x95, 0x00,
    0xa9, 0xc0, 0x01, 0x01, 0x01, 0x01, 0x02, 0x3a, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58, 0x2c,
    0x45, 0x00, 0x20, 0x2f, 0x21, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x30, 0x90, 0x1e,
    0xaa, 0x22, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x4c,
    0x45, 0x4e, 0x20, 0x47, 0x32, 0x35, 0x2d, 0x31, 0x30, 0x0a, 0x20, 0x20, 0x00, 0x00, 0x00, 0xff,
    0x00, 0x55, 0x34, 0x42, 0x34, 0x33, 0x30, 0x4e, 0x39, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x01, 0x71,
};

typedef struct RDNA4I2C {
    uint8_t  buf[1024];      /* engine data RAM, INDEX [25:16] */
    uint32_t index;
    bool     read_mode;
    uint8_t  edid_offset;    /* the DDC slave's word address */
} RDNA4I2C;

typedef struct RDNA4Pending {
    uint32_t addr, val;
} RDNA4Pending;

/* ---- page flip (W5) ---------------------------------------------------- */
typedef struct RDNA4Flip {
    bool pending;
    uint64_t request_frame;
    uint64_t latch_ns;       /* VUPDATE, a few scanlines before frame */
} RDNA4Flip;

typedef struct RDNA4Cursor {
    bool valid;
    uint64_t offset;
    uint32_t width, height;
    uint32_t pitch;
    int32_t x, y;
} RDNA4Cursor;

#define MAX_PENDING 256

typedef struct RDNA4Scanout {
    bool     active;         /* an OTG is running */
    int      otg;
    bool     blank;          /* pattern generator on, or nothing to fetch */
    const char *nosignal;    /* what the monitor would say, NULL = picture */
    uint32_t width, height, stride;
    uint64_t offset;         /* into the VRAM aperture */
    RDNA4Cursor cursor;
} RDNA4Scanout;

static void rdna4_ih_emit_vmid(RDNA4State *s, uint8_t client, uint8_t source,
                               uint8_t ring, uint32_t vmid, uint32_t data0);
static void rdna4_ih_emit(RDNA4State *s, uint8_t client, uint8_t source,
                          uint8_t ring, uint32_t data0);
static bool rdna4_get_cursor(RDNA4State *s, const RDNA4Scanout *so,
                             RDNA4Cursor *cursor);
static void rdna4_gfx_update(void *opaque);

static bool rdna4_is_cursor_register(uint32_t d2)
{
    for (uint32_t hubp = 0; hubp < NUM_OTG; hubp++) {
        const uint32_t hp = hubp * HUBP_STRIDE;
        if (d2 == HUBP_CURSOR_SETTINGS + hp ||
            d2 == HUBP_CUR0_TTU_CNTL0 + hp || d2 == HUBP_CUR0_TTU_CNTL0 + 1 + hp ||
            (d2 >= HUBP_CURSOR_CONTROL + hp && d2 <= HUBP_CURSOR_DST_OFFSET + hp))
            return true;
        const uint32_t dpp = hubp * DPP_STRIDE;
        if (d2 == CURSOR_CM_CONTROL + dpp ||
            d2 == CURSOR_CM_SCALE_GY + dpp || d2 == CURSOR_CM_SCALE_RB + dpp)
            return true;
    }
    return false;
}

typedef struct RDNA4Dispatch RDNA4Dispatch;

struct RDNA4State {
    PCIDevice parent_obj;

    QemuConsole  *con;
    MemoryRegion vram, doorbell, io, mmio;

    /* properties */
    uint64_t aperture;
    char     *state_file;
    char     *flash_file;
    char     *edid_file;
    uint32_t edid_line;
    bool     trace;
    bool     kiq_only;       /* model a MEC that runs only the RLC-named KIQ */
    bool     inv_noack;      /* model the card (2026-09-28): GC hub flushes never ack */
    bool     sdma_no_db;     /* model an SDMA that ignores its doorbell */
    bool     dma_broken;     /* model a system-memory path that faults */
    bool     flip_stuck;     /* leave a surface flip pending forever */
    bool     ih_dead;        /* IH writes the ring but never raises MSI */
    bool     dcn_irq_storm;  /* DCN vblank source runs at 10x */
    bool     hang_sticky;    /* queue dequeue never completes */
    bool     sleep_reset;    /* monitor-triggered compute power reset */
    bool     gfx_hang;       /* accept gfx kicks but leave the ring stopped */
    uint32_t smu_gfx_soft_max;   /* SetSoftMaxByFreq(GFXCLK) in MHz, 0 = automatic */
    uint32_t smu_workload_mask;  /* last SetWorkloadMask */
    bool     gfxoff_preset;      /* "gfxoff-preset": every reset leaves the GC block powered down (an earlier boot allowed GFXOFF and the ASIC kept it) */
    uint32_t gfxoff_arm_ms;      /* "gfxoff-arm-ms": ... from this many virtual ms after the reset, so the firmware phase (which a real card re-POSTs) is not hit */
    bool     gfxoff_pending;
    int64_t  gfxoff_arm_ns;
    uint32_t reset_count;
    bool     smu_preloaded;      /* "smu-preloaded": the PMFW the VBIOS loaded at POST answers the mailbox from reset on */
    uint32_t gpm_restoring_reads; /* RLC_GPM_STAT reports RESTORING_REGISTERS for this many reads after a wake */
    bool     gfxoff_active;      /* AllowGfxOff seen and no DisallowGfxOff since: the GC block is powered down */
    uint32_t gfxoff_violations;  /* GC register/doorbell accesses made while it was */
    uint32_t smu_refuse;         /* "smu-refuse": this SMU message answers CmdRejectedPrereq (0xfd) */
    uint32_t smu_refuse_skip;    /* "smu-refuse-skip": ... but only after this many earlier sends of it */
    uint32_t smu_refuse_seen;
    bool     smu_stale;      /* metrics transfer acks but the table is never rewritten */
    bool     warm_keep;      /* reset keeps live engines/queues, like warm card restart */
    bool     cursor_enabled; /* strict DCN cursor plane/compositor */
    bool     ctx_garbage;    /* context registers power up as garbage, not 0 (W37, rootcause-draw.md #1); ctx-garbage=off restores the old model */
    uint64_t pstat[14];      /* SAMPLE_PIPELINESTAT counters (si_query.c order: PS, C_PRIM, C_INV, VS, GS_INV, GS_PRIM, IA_PRIM, IA_VERT, ...) */
    bool     gop_dlg;        /* the GOP left the HUBP DLG/TTU registers programmed (DCN_SURF0_TTU_CNTL0 delivery non-zero) */
    bool     cursor_lock_stuck; /* the GOP left the MPC cursor lock (CUR_VUPDATE_LOCK_SET0) held */
    bool     cursor_reject_logged;
    bool     cursor_ttu_hypothesis; /* reject the cursor plane while DCN_CUR0_TTU_CNTL0's delivery is 0 (cursor-ttu-hypothesis) */
    uint32_t gfx_break;      /* corrupt one G3 MUST register at draw time */
    bool     gfx_golden_strict; /* refuse a draw when the amdgpu golden registers are unset (gfx-golden-strict) */
    bool     gfx_golden_warned;
    bool     gfx_trace;      /* trace G3 vertices and effective scissors */

    uint32_t *regs;           /* BAR5 image, RDNA4_MMIO_SIZE bytes */
    uint8_t  *resv;           /* top RDNA4_RESV_SIZE bytes of VRAM */
    uint8_t  *hidden;         /* VRAM past the aperture: reserved, touched lazily */
    uint64_t hidden_size;
    uint8_t  edid[256];
    uint32_t edid_len;
    uint8_t  *discovery;      /* IP discovery binary from the flash image */
    uint32_t discovery_len;

    RDNA4I2C     i2c;
    int64_t      otg_epoch[NUM_OTG];
    uint32_t     pclk_khz[NUM_OTG];     /* PHY PLL feeding each OTG */
    uint32_t     symclk_khz[NUM_PHY];   /* 0 = transmitter off */
    uint8_t      dig_mode[NUM_DIG];     /* encoder mode last set up */
    RDNA4Pending pending[MAX_PENDING];  /* double-buffered writes under lock */
    unsigned     npending;
    QEMUTimer   *dcn_timer;
    uint64_t     dcn_next_ns[NUM_OTG];
    RDNA4Flip    flip[NUM_OTG];
    RDNA4Scanout scanout;

    /* PSP and SMU firmware state */
    uint32_t     psp_bl_loaded;         /* bootloader commands taken, by bit */
    uint64_t     psp_ring_mc;
    uint32_t     psp_ring_size;
    uint32_t     psp_rptr;              /* dwords */
    bool         pmfw_loaded;           /* SMU firmware in: the mailbox answers */
    uint64_t     psp_fw_types[2];       /* LOAD_IP_FW types seen, by bit */
    uint64_t     smu_allowed;           /* SetAllowedFeaturesMask */
    uint64_t     smu_running;           /* features the PMFW runs */
    uint64_t     smu_table_mc;          /* driver metrics table address */
    bool         autoload_armed;        /* AUTOLOAD_RLC accepted, IMU not released */
    uint64_t     sdma_wptr;             /* SDMA0 queue 0's last wptr: 64-bit, monotonic */
    bool         mec_hung;              /* dispatch waves are live and its fence is absent */
    uint64_t     ih_ring_bus;
    uint64_t     ih_wptr_bus;
    uint32_t     ih_rptr;
    uint32_t     ih_wptr;
    bool         ih_overflow;
    bool         gfx_booted;            /* RLC autoload done */
    uint64_t     gfx_wptr;               /* GFX ring 0's last wptr: 64-bit, monotonic */
    uint64_t     gfx_rptr;               /* GFX ring 0's consumed dwords */
    bool         gfx_csb_loaded;        /* the first valid kick loaded the CSB */
    bool         gfx_reinit;            /* CP_ME halt/restart has reset RB0 */
    bool         gfx_trace_selfcheck_done;
    uint32_t     gfx_job_seq;           /* latest RELEASE_MEM fence value */
    bool         gfx_pending, gfx_active, gfx_pending_doorbell, gfx_budget_hit;
    uint64_t     gfx_pending_wptr;
    uint64_t     gfx_work_base, gfx_work_pos, gfx_work_end;
    uint32_t     gfx_work_ring_dw;
    bool         warm_dma_window;
    uint64_t     dma_after_reset_writes;
    bool         bus_master_before_reset;
    QEMUTimer   *warm_gop_timer;
    bool         warm_gop_restore;
    uint32_t     warm_gop_delay_ms;   /* property warm-gop-delay-ms (an assumption) */
    uint64_t     dma_after_reset_reads;

    /* Queue kicks are consumed by the QEMU main loop, never by an MMIO
     * handler.  The realtime timer re-arms the bottom half between slices. */
    QEMUBH      *work_bh;
    QEMUTimer   *work_timer;
    RDNA4Dispatch *dispatch;
    uint64_t     work_slices;
    uint64_t     work_slice_logs;

    struct {
        bool     active;
        uint32_t pipe, queue, vmid, size, rptr;
        bool     priv;
        uint64_t pq, wptr;
        uint32_t packet_len;
    } mec_work;
    struct {
        bool     active;
        uint64_t address;
        uint32_t dwords, pos, vmid, depth;
        uint32_t packet_len, outer_len;
    } mec_ib;
    struct {
        bool     active, pending;
        uint64_t wptr, pending_wptr;
        uint32_t size, rptr;
        uint64_t ring;
        bool     packet_active;
        uint32_t packet_len, packet_op;
        uint64_t copy_src, copy_dst;
        uint32_t copy_bytes, copy_done;
        uint32_t fill_value;
        uint8_t  fill_size;
    } sdma_work;
    uint32_t     gfx_num_instances;
    bool         gfx_draw_refused;

    /* CP_HQD_* and compute shader registers are banked by GRBM_GFX_CNTL. */
    struct {
        uint32_t q[0x100];
        uint32_t sh[0x100];
        bool used;
        bool pending;
        uint64_t pending_wptr;
        uint64_t mqd_stall_logged;   /* MQD address | VMID last reported stalled */
    } hqd[4][8];
    uint32_t selected_pipe, selected_queue, selected_vmid;
};

static void rdna4_work_schedule(RDNA4State *s);
static void rdna4_warm_gop_timer(void *opaque);

static bool rdna4_hqd_reg(uint32_t byte, uint32_t *off)
{
    uint32_t dword = byte / 4;
    if (dword < 0x1260 + 0x1fa0 || dword > 0x1260 + 0x1fe0)
        return false;
    *off = dword - (0x1260 + 0x1fa0);
    return true;
}

static bool rdna4_sh_reg(uint32_t byte, uint32_t *off)
{
    uint32_t dword = byte / 4;
    if (dword == 0xa000 + 0x09e4 || dword == 0xa000 + 0x09e3) {
        *off = dword == 0xa000 + 0x09e4 ? 0x80 : 0x81;   /* CONFIG, BASES (gc_12_0_0_offset.h) */
        return true;
    }
    if (dword < 0x1260 + 0x1ba0 || dword > 0x1260 + 0x1c20)
        return false;
    *off = dword - (0x1260 + 0x1ba0);
    return true;
}

static void rdna4_gfx_wptr(RDNA4State *s, uint64_t wptr, bool doorbell);

static inline uint32_t reg_get(RDNA4State *s, uint32_t byte)
{
    uint32_t off;
    if (rdna4_hqd_reg(byte, &off) && s->selected_pipe < 4 && s->selected_queue < 8 &&
        (s->selected_pipe || s->selected_queue || s->selected_vmid))
        return s->hqd[s->selected_pipe][s->selected_queue].q[off];
    if (rdna4_sh_reg(byte, &off) && s->selected_pipe < 4 && s->selected_queue < 8 &&
        (s->selected_pipe || s->selected_queue || s->selected_vmid))
        return s->hqd[s->selected_pipe][s->selected_queue].sh[off];
    return s->regs[byte / 4];
}

static inline void reg_set(RDNA4State *s, uint32_t byte, uint32_t val)
{
    uint32_t off;
    if (rdna4_hqd_reg(byte, &off) && s->selected_pipe < 4 && s->selected_queue < 8 &&
        (s->selected_pipe || s->selected_queue || s->selected_vmid)) {
        s->hqd[s->selected_pipe][s->selected_queue].q[off] = val;
        return;
    }
    if (rdna4_sh_reg(byte, &off) && s->selected_pipe < 4 && s->selected_queue < 8 &&
        (s->selected_pipe || s->selected_queue || s->selected_vmid)) {
        s->hqd[s->selected_pipe][s->selected_queue].sh[off] = val;
        return;
    }
    s->regs[byte / 4] = val;
}

static bool rdna4_bus_master_enabled(RDNA4State *s)
{
    PCIDevice *pci = PCI_DEVICE(s);
    return (pci_get_word(pci->config + PCI_COMMAND) & PCI_COMMAND_MASTER) != 0;
}

/* Track the last Command.BusMaster value.  QEMU resets children before their
 * bus (resettable_phase_hold, hw/core/resettable.c:154-155), so this device's
 * reset callback runs BEFORE pcibus_reset_hold clears Command
 * (hw/pci/pci.c:583-591): inside rdna4_reset() the bit still has its pre-reset
 * value and it is cleared right after.  The remembered value is what the
 * delayed GOP handoff (rdna4_warm_gop_timer) restores. */
static void rdna4_config_write(PCIDevice *dev, uint32_t address, uint32_t data,
                               int len)
{
    RDNA4State *s = RDNA4(dev);
    pci_default_write_config(dev, address, data, len);
    if (address < PCI_COMMAND + 2 && address + len > PCI_COMMAND)
        s->bus_master_before_reset = rdna4_bus_master_enabled(s);
}

/* All device accesses to guest/system memory use this gate. IH and writeback
 * DMA must stop when PCI Command.BusMaster is cleared; a register-only reset
 * must never let an old queue scribble into or fetch from new allocations. */
static MemTxResult rdna4_dma_write(RDNA4State *s, dma_addr_t address,
                                   const void *buf, dma_addr_t len)
{
    if (!rdna4_bus_master_enabled(s)) {
        if (s->trace)
            fprintf(stderr, "rdna4: DMA write refused while PCI bus master is off\n");
        return MEMTX_ERROR;
    }
    MemTxResult result = pci_dma_write(PCI_DEVICE(s), address, buf, len);
    if (result == MEMTX_OK && s->warm_dma_window) {
        s->dma_after_reset_writes++;
        if (s->dma_after_reset_writes <= 3) {
            fprintf(stderr, "rdna4: warm-keep: DMA write after reset #%" PRIu64
                    " at 0x%" PRIx64 " (%" PRIu64 " bytes)\n",
                    s->dma_after_reset_writes, (uint64_t)address, (uint64_t)len);
        }
    }
    return result;
}

static MemTxResult rdna4_dma_read(RDNA4State *s, dma_addr_t address,
                                  void *buf, dma_addr_t len)
{
    if (!rdna4_bus_master_enabled(s)) {
        if (s->trace)
            fprintf(stderr, "rdna4: DMA read refused while PCI bus master is off\n");
        return MEMTX_ERROR;
    }
    MemTxResult result = pci_dma_read(PCI_DEVICE(s), address, buf, len);
    if (result == MEMTX_OK && s->warm_dma_window) {
        s->dma_after_reset_reads++;
        if (s->dma_after_reset_reads <= 3) {
            fprintf(stderr, "rdna4: warm-keep: DMA read after reset #%" PRIu64
                    " at 0x%" PRIx64 " (%" PRIu64 " bytes)\n",
                    s->dma_after_reset_reads, (uint64_t)address, (uint64_t)len);
        }
    }
    return result;
}

static bool rdna4_engine_active(RDNA4State *s)
{
    if (s->dispatch || s->mec_work.active || s->mec_ib.active ||
        s->sdma_work.active || s->gfx_active || s->gfx_pending || s->gfx_booted)
        return true;
    for (uint32_t pipe = 0; pipe < 4; pipe++)
        for (uint32_t queue = 0; queue < 8; queue++)
            if (s->hqd[pipe][queue].used || s->hqd[pipe][queue].pending)
                return true;
    return (s->ih_ring_bus &&
            (reg_get(s, REG_IH_RB_CNTL) & (IH_RB_ENABLE | IH_ENABLE_INTR))) ||
           (reg_get(s, REG_SDMA0_RB_CNTL) & 1u);
}

/* ---- VRAM --------------------------------------------------------------- */

static uint64_t rdna4_vram_size(void)
{
    return (uint64_t)RDNA4_VRAM_MB * MiB;
}

/* Host pointer for 4 bytes of VRAM at `off`, or NULL if not modelled. */
static uint8_t *rdna4_vram_ptr(RDNA4State *s, uint64_t off)
{
    uint64_t resv_base = rdna4_vram_size() - RDNA4_RESV_SIZE;

    if (off + 4 <= s->aperture) {
        return (uint8_t *)memory_region_get_ram_ptr(&s->vram) + off;
    }
    if (s->hidden && off >= s->aperture && off + 4 <= s->aperture + s->hidden_size) {
        return s->hidden + (off - s->aperture);
    }
    if (off >= resv_base && off + 4 <= rdna4_vram_size()) {
        return s->resv + (off - resv_base);
    }
    return NULL;
}

/* Host pointer for `len` bytes of VRAM at `off`, or NULL. */
static uint8_t *rdna4_vram_span(RDNA4State *s, uint64_t off, uint64_t len)
{
    uint8_t *first = rdna4_vram_ptr(s, off);

    return first && rdna4_vram_ptr(s, off + len - 4) == first + len - 4 ? first : NULL;
}

static uint64_t rdna4_mm_offset(RDNA4State *s)
{
    return ((uint64_t)reg_get(s, REG_MM_INDEX_HI) << 31) |
           (reg_get(s, REG_MM_INDEX) & 0x7ffffffc);
}

static uint32_t rdna4_mm_read(RDNA4State *s)
{
    uint8_t *p = rdna4_vram_ptr(s, rdna4_mm_offset(s));

    return p ? ldl_le_p(p) : 0;
}

static void rdna4_mm_write(RDNA4State *s, uint32_t val)
{
    uint64_t off = rdna4_mm_offset(s);
    uint8_t *p = rdna4_vram_ptr(s, off);

    if (!p) {
        return;
    }
    stl_le_p(p, val);
    if (off < s->aperture) {
        memory_region_set_dirty(&s->vram, off, 4);
    }
}

/* ---- OTG timing --------------------------------------------------------- */

static uint32_t rdna4_otg_reg(RDNA4State *s, int otg, uint32_t dw)
{
    return reg_get(s, SEG2(dw + otg * OTG_STRIDE));
}

/*
 * Pixels scanned out since the OTG was enabled, at the pixel clock of the
 * PHY PLL feeding it (SET_PIXEL_CLOCK; at power-on the GOP's clock).
 */
static bool rdna4_otg_position(RDNA4State *s, int otg, uint64_t *frame,
                               uint32_t *line, uint32_t *horizontal)
{
    uint64_t htot = (rdna4_otg_reg(s, otg, OTG_H_TOTAL) & 0x7fff) + 1;
    uint64_t vtot = (rdna4_otg_reg(s, otg, OTG_V_TOTAL) & 0x7fff) + 1;
    uint64_t pclk = (uint64_t)s->pclk_khz[otg] * 1000;
    int64_t ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->otg_epoch[otg];
    uint64_t pixels;

    if (!(rdna4_otg_reg(s, otg, OTG_CONTROL) & 1) || ns < 0 || !pclk ||
        pclk > UINT32_MAX) {
        return false;
    }
    pixels = muldiv64(ns, (uint32_t)pclk, NANOSECONDS_PER_SECOND);
    *frame = pixels / (htot * vtot);
    *line = (pixels % (htot * vtot)) / htot;
    *horizontal = pixels % htot;
    return true;
}

static bool rdna4_update_locked(RDNA4State *s, int otg);
static void rdna4_latch_flips(RDNA4State *s, int otg, uint64_t frame);
static uint64_t rdna4_dcn_period_ns(RDNA4State *s, int otg);
static void rdna4_dcn_schedule(RDNA4State *s);

static void rdna4_init_earliest_inuse(RDNA4State *s)
{
    for (int hubp = 0; hubp < NUM_OTG; hubp++) {
        uint32_t primary = reg_get(s, SEG2(HUBP_SURFACE_ADDR + hubp * HUBP_STRIDE));
        uint32_t primary_hi = reg_get(s, SEG2(HUBP_SURFACE_ADDR_HI + hubp * HUBP_STRIDE));
        if (!reg_get(s, SEG2(HUBP_EARLIEST_ADDR + hubp * HUBP_STRIDE)) &&
            !reg_get(s, SEG2(HUBP_EARLIEST_ADDR_HI + hubp * HUBP_STRIDE))) {
            reg_set(s, SEG2(HUBP_EARLIEST_ADDR + hubp * HUBP_STRIDE), primary);
            reg_set(s, SEG2(HUBP_EARLIEST_ADDR_HI + hubp * HUBP_STRIDE), primary_hi);
        }
    }
}

static uint32_t rdna4_otg_read(RDNA4State *s, int otg, uint32_t dw)
{
    uint32_t val = rdna4_otg_reg(s, otg, dw);
    uint64_t frame;
    uint32_t line, horizontal, vb;
    const bool running = rdna4_otg_position(s, otg, &frame, &line, &horizontal);

    switch (dw) {
    case OTG_CONTROL:
        return (val & ~(1u << 16)) | ((val & 1) << 16);
    case OTG_MASTER_UPDATE_LOCK:
        return (val & ~(1u << 8)) | ((val & 1) << 8);
    case OTG_CLOCK_CONTROL:                        /* running when enabled, never busy */
        return (val & ~((1u << 8) | (1u << 16))) | ((val & 1) << 8);
    case OTG_STATUS_POSITION:
        if (running) {
            return (line << 16) | (horizontal & 0xffff);
        }
        return val;
    case OTG_FRAME_COUNT:
        if (running) {
            return (val & ~0xffffffu) | (frame & 0xffffff);
        }
        return val;
    case OTG_STATUS:
        if (running) {
            vb = rdna4_otg_reg(s, otg, OTG_V_BLANK);
            bool in_blank = line < ((vb >> 16) & 0x7fff) || line >= (vb & 0x7fff);
            return (val & ~1u) | in_blank;
        }
        return val;
    }
    return val;
}

/*
 * Double buffering: with OTG_MASTER_UPDATE_LOCK held on a running OTG,
 * writes to its timing registers (and to the HUBP of the same pipe) stay
 * pending and latch together when the lock is released. A stopped OTG or
 * an unlocked one takes them at once (the next VUPDATE, in hardware).
 */
static bool rdna4_update_locked(RDNA4State *s, int otg)
{
    return (rdna4_otg_reg(s, otg, OTG_CONTROL) & 1) &&
           (rdna4_otg_reg(s, otg, OTG_MASTER_UPDATE_LOCK) & 1);
}

/* ---- page flip (W5): an address requested under lock latches at vblank. */
static int rdna4_flip_hubp_for_address(uint32_t addr)
{
    for (int hubp = 0; hubp < NUM_OTG; hubp++) {
        if (addr == SEG2(HUBP_SURFACE_ADDR + hubp * HUBP_STRIDE) ||
            addr == SEG2(HUBP_SURFACE_ADDR_HI + hubp * HUBP_STRIDE)) {
            return hubp;
        }
    }
    return -1;
}

static int rdna4_hubp_for_otg(RDNA4State *s, int otg)
{
    uint32_t opp = (reg_get(s, SEG2(OPTC_DATA_SOURCE + otg * ODM_STRIDE)) >> 16) & 0xf;

    if (opp >= NUM_OTG) {
        return -1;
    }
    for (int mpcc = 0; mpcc < NUM_OTG; mpcc++) {
        if ((reg_get(s, SEG3(MPCC_OPP_ID + mpcc * MPCC_STRIDE)) & 0xf) == opp) {
            uint32_t top = reg_get(s, SEG3(MPCC_TOP_SEL + mpcc * MPCC_STRIDE)) & 0xf;
            return top < NUM_OTG ? (int)top : -1;
        }
    }
    return -1;
}

static int rdna4_otg_for_hubp(RDNA4State *s, int hubp)
{
    for (int otg = 0; otg < NUM_OTG; otg++) {
        if (rdna4_hubp_for_otg(s, otg) == hubp) {
            return otg;
        }
    }
    return -1;
}

static void rdna4_latch_flips(RDNA4State *s, int otg, uint64_t frame)
{
    RDNA4Flip *flip = &s->flip[otg];
    const int hubp = rdna4_hubp_for_otg(s, otg);

    if (hubp < 0 || !flip->pending || s->flip_stuck || rdna4_update_locked(s, otg) ||
        !flip->latch_ns) {
        return;
    }
    unsigned out = 0;
    bool applied = false;
    for (unsigned i = 0; i < s->npending; i++) {
        if (rdna4_flip_hubp_for_address(s->pending[i].addr) == hubp) {
            reg_set(s, s->pending[i].addr, s->pending[i].val);
            applied = true;
        } else {
            s->pending[out++] = s->pending[i];
        }
    }
    s->npending = out;
    if (applied) {
        uint32_t primary = reg_get(s, SEG2(HUBP_SURFACE_ADDR + hubp * HUBP_STRIDE));
        uint32_t primary_hi = reg_get(s, SEG2(HUBP_SURFACE_ADDR_HI + hubp * HUBP_STRIDE));
        reg_set(s, SEG2(HUBP_EARLIEST_ADDR + hubp * HUBP_STRIDE), primary);
        reg_set(s, SEG2(HUBP_EARLIEST_ADDR_HI + hubp * HUBP_STRIDE), primary_hi);
        flip->pending = false;
        flip->latch_ns = 0;
        uint32_t addr = SEG2(HUBP_FLIP_INTERRUPT + hubp * HUBP_STRIDE);
        uint32_t status = reg_get(s, addr);
        fprintf(stderr, "rdna4: dcn: HUBP%d flip latched at VUPDATE before frame %" PRIu64
                " (earliest 0x%08x:%08x)\n", hubp, frame, primary_hi, primary);
        if ((status & DCN_FLIP_ENABLE) && !(status & DCN_FLIP_STATUS)) {
            reg_set(s, addr, status | DCN_FLIP_OCCURRED | DCN_FLIP_STATUS);
            rdna4_ih_emit(s, DCN_CLIENT, DCN_SRC_PFLIP + hubp, 0, 0);
        }
        /* The surface address becomes visible at this vblank.  Refresh the
         * QEMU display after the latch so a screendump observes the new
         * scanout rather than the pre-flip desktop surface. */
        rdna4_gfx_update(s);
    }
}

static void rdna4_db_write(RDNA4State *s, int otg, uint32_t addr, uint32_t val)
{
    if (!rdna4_update_locked(s, otg)) {
        reg_set(s, addr, val);
        return;
    }
    bool queued = false;
    for (unsigned i = 0; i < s->npending; i++) {
        if (s->pending[i].addr == addr) {
            s->pending[i].val = val;
            queued = true;
            break;
        }
    }
    if (!queued) {
        if (s->npending < MAX_PENDING) {
            s->pending[s->npending++] = (RDNA4Pending){ addr, val };
            queued = true;
        } else {
            reg_set(s, addr, val);
        }
    }
    const int hubp = rdna4_flip_hubp_for_address(addr);
    const int owner = hubp < 0 ? -1 : rdna4_otg_for_hubp(s, hubp);
    if (queued && owner == otg) {
        uint64_t frame = 0;
        uint32_t line = 0, horizontal = 0;
        if (!rdna4_otg_position(s, otg, &frame, &line, &horizontal)) {
            frame = 0;
        }
        s->flip[otg].pending = true;
        s->flip[otg].request_frame = frame;
        const uint64_t period = rdna4_dcn_period_ns(s, otg);
        const uint64_t vtotal = (rdna4_otg_reg(s, otg, OTG_V_TOTAL) & 0x7fff) + 1;
        const uint64_t lead = MAX(period / MAX(vtotal, 1ull) * 4, 1ull);
        uint64_t boundary = s->otg_epoch[otg] + (frame + 1) * period;
        const uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        if (!period || boundary <= now + lead)
            boundary += period;
        s->flip[otg].latch_ns = period ? boundary - lead : 0;
        rdna4_dcn_schedule(s);
    }
}

static void rdna4_latch_pending(RDNA4State *s)
{
    for (unsigned i = 0; i < s->npending; i++) {
        if (rdna4_flip_hubp_for_address(s->pending[i].addr) < 0) {
            reg_set(s, s->pending[i].addr, s->pending[i].val);
        }
    }
    unsigned out = 0;
    for (unsigned i = 0; i < s->npending; i++) {
        if (rdna4_flip_hubp_for_address(s->pending[i].addr) >= 0) {
            s->pending[out++] = s->pending[i];
        }
    }
    s->npending = out;
}

static uint32_t rdna4_hubp_read(RDNA4State *s, int hubp, uint32_t dw)
{
    uint32_t val = reg_get(s, SEG2(dw + hubp * HUBP_STRIDE));

    if (dw == HUBP_FLIP_CONTROL) {
        const int otg = rdna4_otg_for_hubp(s, hubp);
        if (otg >= 0 && s->flip[otg].pending) {
            val |= HUBP_FLIP_PENDING;
        } else {
            val &= ~HUBP_FLIP_PENDING;
        }
    }
    return val;
}

static void rdna4_otg_write(RDNA4State *s, int otg, uint32_t dw, uint32_t val)
{
    uint32_t old = rdna4_otg_reg(s, otg, dw);
    uint32_t addr = SEG2(dw + otg * OTG_STRIDE);

    if (dw == OTG_GLOBAL_SYNC_STATUS) {
        uint32_t next = val;
        if (val & DCN_VSTARTUP_CLEAR) {
            next &= ~(DCN_VSTARTUP_OCCURRED | DCN_VSTARTUP_STATUS | DCN_VSTARTUP_CLEAR);
        }
        reg_set(s, addr, next);
        return;
    }

    if (dw >= OTG_H_TOTAL && dw <= OTG_V_SYNC_A_CNTL) {
        rdna4_db_write(s, otg, addr, val);
        return;
    }
    if (dw == OTG_CONTROL && (val & 1) && !(old & 1)) {
        s->otg_epoch[otg] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        s->dcn_next_ns[otg] = 0;
        if (s->dcn_timer)
            timer_mod_ns(s->dcn_timer, s->otg_epoch[otg] + 1000000);
    } else if (dw == OTG_CONTROL && !(val & 1)) {
        s->dcn_next_ns[otg] = 0;
    }
    reg_set(s, addr, val & (dw == OTG_MASTER_UPDATE_LOCK ? ~(1u << 8) : ~0u));
    if ((dw == OTG_MASTER_UPDATE_LOCK && (old & 1) && !(val & 1)) ||
        (dw == OTG_CONTROL && !(val & 1))) {
        rdna4_latch_pending(s);
    }
}

/* ---- DCN interrupt timing ------------------------------------------------ */

static uint64_t rdna4_dcn_period_ns(RDNA4State *s, int otg)
{
    uint64_t htot = (rdna4_otg_reg(s, otg, OTG_H_TOTAL) & 0x7fff) + 1;
    uint64_t vtot = (rdna4_otg_reg(s, otg, OTG_V_TOTAL) & 0x7fff) + 1;
    uint64_t pclk = (uint64_t)s->pclk_khz[otg] * 1000;
    return pclk ? htot * vtot * NANOSECONDS_PER_SECOND / pclk : 0;
}

static void rdna4_reset(DeviceState *dev);
static void rdna4_rlc_srm_apply(RDNA4State *s);

static void rdna4_sleep_reset_get(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    const Property *prop = opaque;
    bool *src = object_field_prop_ptr(obj, prop);
    visit_type_bool(v, name, src, errp);
}

static void rdna4_sleep_reset_default(ObjectProperty *op, const Property *prop)
{
    object_property_set_default_bool(op, prop->defval.u);
}

static void rdna4_sleep_reset_set(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    const Property *prop = opaque;
    RDNA4State *s = RDNA4(obj);
    bool *dst = object_field_prop_ptr(obj, prop);
    if (!visit_type_bool(v, name, dst, errp) || !*dst)
        return;
    /* Make the already-running DCN timer observe this one-shot monitor
     * request even when the next raster event has not been scheduled. */
    if (s->dcn_timer)
        timer_mod_ns(s->dcn_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

static const PropertyInfo rdna4_sleep_reset_prop = {
    .type = "bool",
    .description = "trigger a debug compute power reset",
    .get = rdna4_sleep_reset_get,
    .set = rdna4_sleep_reset_set,
    .set_default_value = rdna4_sleep_reset_default,
    .realized_set_allowed = true,
};

static void rdna4_dcn_vblank(RDNA4State *s, int otg, uint64_t frame)
{
    uint32_t addr = SEG2(OTG_GLOBAL_SYNC_STATUS + otg * OTG_STRIDE);
    uint32_t status = reg_get(s, addr);
    if (!(status & DCN_VSTARTUP_ENABLE) || (status & DCN_VSTARTUP_STATUS))
        return;
    reg_set(s, addr, status | DCN_VSTARTUP_OCCURRED | DCN_VSTARTUP_STATUS);
    rdna4_ih_emit(s, DCN_CLIENT, DCN_SRC_VBLANK + otg, 0, (uint32_t)frame);
}

static void rdna4_dcn_timer(void *opaque)
{
    RDNA4State *s = opaque;
    if (s->sleep_reset) {
        fprintf(stderr, "rdna4: sleep-reset: property observed by DCN timer\n");
        s->sleep_reset = false;
        fprintf(stderr, "rdna4: sleep-reset: compute power reset; DCN and VRAM retained\n");
        rdna4_reset(DEVICE(s));
        return;
    }
    const uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t next = 0;
    for (int otg = 0; otg < NUM_OTG; otg++) {
        if (!(rdna4_otg_reg(s, otg, OTG_CONTROL) & 1))
            continue;
        uint64_t period = rdna4_dcn_period_ns(s, otg);
        if (!period)
            continue;
        uint64_t interval = s->dcn_irq_storm ? MAX(period / 10, 1ull) : period;
        if (!s->dcn_next_ns[otg])
            s->dcn_next_ns[otg] = now + interval;
        RDNA4Flip *flip = &s->flip[otg];
        if (flip->pending && flip->latch_ns && flip->latch_ns <= now) {
            uint64_t frame = 0;
            uint32_t line = 0;
            uint32_t horizontal = 0;
            if (rdna4_otg_position(s, otg, &frame, &line, &horizontal))
                rdna4_latch_flips(s, otg, frame);
            if (flip->pending && flip->latch_ns <= now)
                flip->latch_ns += interval;
        }
        while (s->dcn_next_ns[otg] <= now) {
            uint64_t frame = 0;
            uint32_t line = 0;
            uint32_t horizontal = 0;
            if (rdna4_otg_position(s, otg, &frame, &line, &horizontal)) {
                rdna4_latch_flips(s, otg, frame);
                rdna4_dcn_vblank(s, otg, frame);
            }
            s->dcn_next_ns[otg] += interval;
            if (s->dcn_next_ns[otg] + interval < s->dcn_next_ns[otg]) {
                s->dcn_next_ns[otg] = now + interval;
                break;
            }
        }
        if (!next || s->dcn_next_ns[otg] < next)
            next = s->dcn_next_ns[otg];
        if (flip->pending && flip->latch_ns && flip->latch_ns < next)
            next = flip->latch_ns;
    }
    if (next && s->dcn_timer)
        timer_mod_ns(s->dcn_timer, next);
}

static void rdna4_dcn_schedule(RDNA4State *s)
{
    if (!s->dcn_timer)
        return;
    uint64_t next = 0;
    for (int otg = 0; otg < NUM_OTG; otg++) {
        if ((rdna4_otg_reg(s, otg, OTG_CONTROL) & 1) && s->dcn_next_ns[otg] &&
            (!next || s->dcn_next_ns[otg] < next))
            next = s->dcn_next_ns[otg];
    }
    if (next)
        timer_mod_ns(s->dcn_timer, next);
}

/* ---- DC_I2C ------------------------------------------------------------- */

static uint8_t rdna4_ddc_read(RDNA4State *s)
{
    uint8_t off = s->i2c.edid_offset++;

    return off < s->edid_len ? s->edid[off] : 0xff;
}

/* Run the queued transactions (DC_I2C_CONTROL.GO). */
static void rdna4_i2c_execute(RDNA4State *s)
{
    uint32_t ctl = reg_get(s, SEG2(I2C_CONTROL));
    uint32_t line = (ctl >> 8) & 7;
    uint32_t ntxn = ((ctl >> 20) & 3) + 1;
    uint32_t status = I2C_ST_DONE;
    bool sink = s->edid_len && line == s->edid_line;
    uint32_t p = 0;

    for (uint32_t t = 0; t < ntxn; t++) {
        uint32_t txn = reg_get(s, SEG2(I2C_TRANSACTION0 + t));
        uint32_t count = (txn >> 16) & 0x3ff;
        bool read = txn & I2C_TXN_READ;

        if (txn & I2C_TXN_START) {
            uint8_t addr = s->i2c.buf[p++ % sizeof(s->i2c.buf)];
            if (!sink || (addr >> 1) != 0x50 || (addr & 1) != read) {
                status |= I2C_ST_NACK;
                break;
            }
        }
        for (uint32_t i = 0; i < count; i++, p++) {
            if (read) {
                s->i2c.buf[p % sizeof(s->i2c.buf)] = rdna4_ddc_read(s);
            } else if (i == 0) {
                s->i2c.edid_offset = s->i2c.buf[p % sizeof(s->i2c.buf)];
            }
        }
    }
    reg_set(s, SEG2(I2C_SW_STATUS), status);
}

static uint32_t rdna4_i2c_read(RDNA4State *s, uint32_t dw)
{
    uint32_t val = reg_get(s, SEG2(dw));

    switch (dw) {
    case I2C_ARBITRATION:
        /* write-blocked and reading 0 while in soft reset (seen on hardware) */
        return (reg_get(s, SEG2(I2C_CONTROL)) & I2C_SOFT_RESET) ? 0 : val;
    case I2C_DATA:
        if (!s->i2c.read_mode) {
            return 0;
        }
        return (uint32_t)s->i2c.buf[s->i2c.index++ % sizeof(s->i2c.buf)] << 8;
    case DIO_MEM_PWR_STATUS:
        return reg_get(s, SEG2(DIO_MEM_PWR_CTRL)) & 1;
    }
    return val;
}

static void rdna4_i2c_write(RDNA4State *s, uint32_t dw, uint32_t val)
{
    uint32_t arb, owner;

    switch (dw) {
    case I2C_CONTROL:
        if (val & (I2C_SOFT_RESET | I2C_SW_STATUS_RESET)) {
            reg_set(s, SEG2(I2C_SW_STATUS), 0);
        }
        reg_set(s, SEG2(dw), val & ~I2C_GO);
        if ((val & I2C_GO) && !(val & I2C_SOFT_RESET)) {
            rdna4_i2c_execute(s);
        }
        return;
    case I2C_ARBITRATION:
        if (reg_get(s, SEG2(I2C_CONTROL)) & I2C_SOFT_RESET) {
            return;
        }
        arb = reg_get(s, SEG2(dw));
        owner = (arb & ARB_STATUS_MASK) >> ARB_STATUS_SHIFT;
        if ((val & I2C_ARB_SW_USE_REQ) && owner != 2) {
            owner = 1;
        }
        if (val & I2C_ARB_SW_DONE) {
            owner = 0;
            val &= ~(I2C_ARB_SW_USE_REQ | I2C_ARB_SW_DONE);
        }
        reg_set(s, SEG2(dw), (val & ~ARB_STATUS_MASK) | (owner << ARB_STATUS_SHIFT));
        return;
    case I2C_SW_STATUS:
    case DIO_MEM_PWR_STATUS:
        return;
    case I2C_DATA:
        if (val & I2C_DATA_INDEX_WRITE) {
            s->i2c.index = (val >> 16) & 0x3ff;
            s->i2c.read_mode = val & I2C_DATA_READ;
        }
        if (!s->i2c.read_mode) {
            s->i2c.buf[s->i2c.index++ % sizeof(s->i2c.buf)] = (val >> 8) & 0xff;
        }
        return;
    }
    reg_set(s, SEG2(dw), val);
}

static bool rdna4_is_i2c(uint32_t dw)
{
    return dw == I2C_CONTROL || dw == I2C_ARBITRATION || dw == I2C_SW_STATUS ||
           dw == I2C_DATA || dw == DIO_MEM_PWR_STATUS;
}

/* ---- DP AUX (no sink attached) ------------------------------------------ */

static void rdna4_aux_write(RDNA4State *s, uint32_t base, uint32_t reg,
                            uint32_t val)
{
    uint32_t arb, owner, sts;

    switch (reg) {
    case AUX_SW_CONTROL:
        reg_set(s, SEG2(base + reg), val & ~AUX_SW_GO);
        if (val & AUX_SW_GO) {
            /* nothing on the other end: the engine reports HPD disconnect */
            reg_set(s, SEG2(base + AUX_SW_STATUS), AUX_ST_DONE | AUX_ST_HPD_DISCON);
        }
        return;
    case AUX_ARB_CONTROL:
        arb = reg_get(s, SEG2(base + reg));
        owner = (arb & ARB_STATUS_MASK) >> ARB_STATUS_SHIFT;
        if ((val & AUX_ARB_USE_REQ) && owner != 2) {
            owner = 1;
        }
        if (val & AUX_ARB_DONE_USING) {
            owner = 0;
            val &= ~(AUX_ARB_USE_REQ | AUX_ARB_DONE_USING);
        }
        reg_set(s, SEG2(base + reg), (val & ~ARB_STATUS_MASK) | (owner << ARB_STATUS_SHIFT));
        return;
    case AUX_INT_CONTROL:
        if (val & AUX_INT_DONE_ACK) {
            sts = reg_get(s, SEG2(base + AUX_SW_STATUS));
            reg_set(s, SEG2(base + AUX_SW_STATUS), sts & ~AUX_ST_DONE);
        }
        reg_set(s, SEG2(base + reg), val & ~AUX_INT_DONE_ACK);
        return;
    case AUX_SW_STATUS:
        return;
    }
    reg_set(s, SEG2(base + reg), val);
}

static uint32_t rdna4_aux_read(RDNA4State *s, uint32_t base, uint32_t reg)
{
    if (reg == AUX_SW_DATA) {
        return 0;
    }
    return reg_get(s, SEG2(base + reg));
}

/* ---- DMUB inbox1 --------------------------------------------------------- */

/*
 * The display firmware's command ring. There is no DMUB firmware in the
 * model: a WPTR write consumes every queued command at once, answers the
 * ones with a defined reply and moves RPTR up to WPTR.
 */
static void rdna4_dmub_command(RDNA4State *s, uint8_t *cmd)
{
    uint8_t type = cmd[0], sub = cmd[1];

    if (s->trace) {
        fprintf(stderr, "rdna4: dmub cmd type %u sub %u: %08x %08x %08x %08x\n",
                type, sub, ldl_le_p(cmd), ldl_le_p(cmd + 4), ldl_le_p(cmd + 8),
                ldl_le_p(cmd + 12));
    }
    if (type == DMUB_CMD_QUERY_FEATURE_CAPS) {
        memset(cmd + 4, 0, DMUB_CMD_SIZE - 4);    /* no optional features */
        return;
    }
    if (type != DMUB_CMD_VBIOS) {
        return;
    }
    switch (sub) {
    case VBIOS_SET_PIXEL_CLOCK: {                  /* set_pixel_clock_parameter_v1_7 */
        uint32_t pixclk_100hz = ldl_le_p(cmd + 4);
        uint8_t crtc = cmd[12];
        if (crtc < NUM_OTG) {
            s->pclk_khz[crtc] = pixclk_100hz / 10;
        }
        break;
    }
    case VBIOS_DIG1_TRANSMITTER_CONTROL: {         /* dig_transmitter_control_data_v1_7 */
        uint8_t phy = cmd[4], action = cmd[5];
        uint32_t symclk_10khz = ldl_le_p(cmd + 8);
        if (phy < NUM_PHY && action == TRANSMITTER_ACTION_ENABLE) {
            s->symclk_khz[phy] = symclk_10khz * 10;
        } else if (phy < NUM_PHY && action == TRANSMITTER_ACTION_DISABLE) {
            s->symclk_khz[phy] = 0;
        }
        break;
    }
    case VBIOS_DIGX_ENCODER_CONTROL: {             /* dig_encoder_stream_setup_parameters_v1_5 */
        uint8_t dig = cmd[4];
        if (dig < NUM_DIG) {
            s->dig_mode[dig] = cmd[6];
        }
        break;
    }
    }
}

static void rdna4_dmub_wptr(RDNA4State *s, uint32_t wptr)
{
    uint64_t region4 = ((uint64_t)reg_get(s, SEG2(DMCUB_REGION4_OFFSET_HI)) << 32) |
                       reg_get(s, SEG2(DMCUB_REGION4_OFFSET));
    uint64_t fb = (uint64_t)(reg_get(s, SEG2(DCN_VM_FB_LOC_BASE)) & 0xffffff) << 24;
    uint32_t size = reg_get(s, SEG2(DMCUB_INBOX1_SIZE));
    uint32_t rptr = reg_get(s, SEG2(DMCUB_INBOX1_RPTR));

    reg_set(s, SEG2(DMCUB_INBOX1_WPTR), wptr);
    if (region4 <= fb || !size || size % DMUB_CMD_SIZE || wptr >= size ||
        wptr % DMUB_CMD_SIZE || rptr >= size || rptr % DMUB_CMD_SIZE) {
        return;
    }
    while (rptr != wptr) {
        uint8_t *cmd = rdna4_vram_span(s, region4 - fb + rptr, DMUB_CMD_SIZE);
        if (!cmd) {
            break;                                 /* ring outside modelled VRAM */
        }
        rdna4_dmub_command(s, cmd);
        rptr = (rptr + DMUB_CMD_SIZE) % size;
    }
    reg_set(s, SEG2(DMCUB_INBOX1_RPTR), rptr);
}

/* ---- SMU mailbox ---------------------------------------------------------- */

static uint8_t *rdna4_mc_span(RDNA4State *s, uint64_t mc, uint64_t len);

/*
 * PPSMC message protocol (smu_v14_0): the driver clears RESP, stages PARAM
 * and writes the message id; the PMFW answers in RESP (and PARAM). As on the
 * card, nothing answers until the SMU firmware has been loaded through the
 * PSP (LOAD_IP_FW, type SMU). Only the messages the kext sends are known.
 */
#define SMU_FEATURE_GFX_IMU  36             /* smu14_driver_if_v14_0.h */

/*
 * The RLC autoload, once armed by the PSP, completes when the PMFW has
 * GFX_IMU running: that is what releases the IMU on a dGPU (amdgpu reaches
 * the autoload wait only after smu_hw_init's EnableAllSmuFeatures).
 */
static void rdna4_gfx_autoload(RDNA4State *s)
{
    if (!s->autoload_armed || s->gfx_booted ||
        !(s->smu_running & (1ull << SMU_FEATURE_GFX_IMU))) {
        return;
    }
    s->gfx_booted = true;
    reg_set(s, REG_GC_CP_STAT, 0);
    reg_set(s, REG_GC_RLC_BOOTLOAD, 0x8000003f);
    reg_set(s, REG_GC_RLC_CNTL, 1);
    reg_set(s, REG_GC_IMU_CORE_CTRL, 0);
    reg_set(s, REG_GC_IMU_GFX_RESET, reg_get(s, REG_GC_IMU_GFX_RESET) | 0x1f);
    reg_set(s, REG_GC_SDMA0_STATUS, SDMA_STATUS_BOOTED);
    reg_set(s, REG_GC_SDMA1_STATUS, SDMA_STATUS_BOOTED);
    fprintf(stderr, "rdna4: gfx: IMU released by the PMFW, RLC autoload complete\n");
}

/* SmuMetrics_t power-management fields (smu14_driver_if_v14_0.h:1649-1727).
 * The real PMFW bumps MetricsCounter on its own tick (about 1 ms), so it is the
 * virtual clock in ms: two reads a second apart differ, and a table the SMU did
 * not rewrite keeps whatever the driver put there. The activity values are the
 * emulator's idle card (no queue is executing), not a claim about silicon. */
static void rdna4_smu_metrics_pm_fields(RDNA4State *s, uint8_t *table)
{
    stl_le_p(table + RDNA4_SMU_METRICS_COUNTER,
             (uint32_t)(qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) & 0x7fffffff));
    /* PPCLK_GFXCLK follows a SetSoftMaxByFreq below the idle clock; the soft
     * max message carries max + 1 (SMU_V14_SOFT_FREQ_ROUND). */
    uint32_t gfxclk = 2100;
    if (s->smu_gfx_soft_max && s->smu_gfx_soft_max - 1 < gfxclk)
        gfxclk = s->smu_gfx_soft_max - 1;
    stl_le_p(table + RDNA4_SMU_METRICS_CURR_CLOCK + 0 * 4, gfxclk);    /* PPCLK_GFXCLK */
    stw_le_p(table + RDNA4_SMU_METRICS_AVG_GFXCLK_PRE_DS, 2100);
    stw_le_p(table + RDNA4_SMU_METRICS_AVG_GFX_ACTIVITY, 3);
    stw_le_p(table + RDNA4_SMU_METRICS_AVG_UCLK_ACTIVITY, 1);
    stw_le_p(table + RDNA4_SMU_METRICS_AVG_VOLTAGE + 0 * 2, 850);      /* SVI_PLANE_VDD_GFX, mV */
    stw_le_p(table + RDNA4_SMU_METRICS_AVG_CURRENT + 0 * 2, 12);
    table[RDNA4_SMU_METRICS_THROTTLING_PCT + 14] = 0;                  /* THROTTLER_PPT0 */
}

static void rdna4_smu_msg(RDNA4State *s, uint32_t msg)
{
    uint32_t resp = SMU_RESP_OK, param = reg_get(s, REG_SMU_PARAM);

    reg_set(s, REG_SMU_MSG, msg);
    if (!s->pmfw_loaded) {
        return;                                    /* no PMFW: RESP stays 0 */
    }
    /* PPSMC_Result_CmdRejectedPrereq, as the card answers under SCPM
     * (SetAllowedMask on the real card, hw-logs): a fault option, off by default. */
    if (s->smu_refuse && msg == s->smu_refuse && s->smu_refuse_seen++ >= s->smu_refuse_skip) {
        fprintf(stderr, "rdna4: smu: message 0x%x refused with 0xfd (smu-refuse)\n", msg);
        reg_set(s, REG_SMU_RESP, 0xfd);
        return;
    }
    switch (msg) {
    case 0x1:                                      /* TestMessage */
        break;
    case 0x2:                                      /* GetSmuVersion */
        reg_set(s, REG_SMU_PARAM, SMU_PMFW_VERSION);
        break;
    case 0x3:                                      /* GetDriverIfVersion */
        reg_set(s, REG_SMU_PARAM, 0x2e);
        break;
    case 0x4:                                      /* SetAllowedFeaturesMaskLow */
        s->smu_allowed = (s->smu_allowed & ~0xffffffffull) | param;
        break;
    case 0x5:                                      /* SetAllowedFeaturesMaskHigh */
        s->smu_allowed = (s->smu_allowed & 0xffffffffull) | ((uint64_t)param << 32);
        break;
    case 0x6:                                      /* EnableAllSmuFeatures */
        if (param != 0) {
            resp = SMU_RESP_UNKNOWN;               /* 14.0.3: no per-domain enable */
            break;
        }
        s->smu_running |= s->smu_allowed;          /* the pptable runs everything */
        fprintf(stderr, "rdna4: smu: features running 0x%016" PRIx64 "\n", s->smu_running);
        rdna4_gfx_autoload(s);
        break;
    case 0xc:                                      /* GetRunningSmuFeaturesLow */
        reg_set(s, REG_SMU_PARAM, (uint32_t)s->smu_running);
        break;
    case 0xd:                                      /* GetRunningSmuFeaturesHigh */
        reg_set(s, REG_SMU_PARAM, (uint32_t)(s->smu_running >> 32));
        break;
    case 0xe:                                      /* SetDriverDramAddrHigh */
        s->smu_table_mc = (s->smu_table_mc & 0xffffffffull) | ((uint64_t)param << 32);
        break;
    case 0xf:                                      /* SetDriverDramAddrLow */
        s->smu_table_mc = (s->smu_table_mc & 0xffffffff00000000ull) | param;
        break;
	case 0x12: {                                   /* TransferTableSmu2Dram / GetMetricsTable */
		/* TABLE_SMU_METRICS is id 5 in smu14_driver_if_v14_0.h. */
		if (param != 5) {
			fprintf(stderr, "rdna4: smu: metrics request table %u rejected (want 5)\n", param);
			resp = SMU_RESP_UNKNOWN;
			break;
		}
		/* smu14_driver_if_v14_0.h: fixed values make emulator telemetry
         * obvious in logs and tests. */
        uint8_t *table = rdna4_mc_span(s, s->smu_table_mc, 4096);
        if (!table) {
            resp = SMU_RESP_UNKNOWN;
            break;
        }
        if (s->smu_stale) {
            /* "smu-stale": the SMU acks the transfer but the driver table is
             * left as the driver wrote it (a stale HDP/PCIe write path). */
            fprintf(stderr, "rdna4: smu: metrics transfer acked, table NOT rewritten (smu-stale)\n");
            break;
        }
        memset(table, 0, 4096);
        rdna4_smu_metrics_pm_fields(s, table);
        stw_le_p(table + RDNA4_SMU_METRICS_AVG_GFXCLK_POST_DS, 2100);
        stw_le_p(table + RDNA4_SMU_METRICS_AVG_MEMCLK_POST_DS, 1000);
        stw_le_p(table + RDNA4_SMU_METRICS_AVG_TEMPERATURE + 0 * 2, 42);
        stw_le_p(table + RDNA4_SMU_METRICS_AVG_TEMPERATURE + 1 * 2, 55);
        stw_le_p(table + RDNA4_SMU_METRICS_AVG_SOCKET_POWER, s->gfxoff_active ? 8 : 120);
        if (s->gfxoff_active) {
            stw_le_p(table + RDNA4_SMU_METRICS_AVG_GFX_ACTIVITY, 0);
        }
        stw_le_p(table + RDNA4_SMU_METRICS_AVG_FAN_RPM, 900);
        fprintf(stderr, "rdna4: smu: synthetic metrics 42C/55C, 2100/1000 MHz, 120 W, 900 RPM\n");
        break;
    }
    case 0x19:                                     /* SetSoftMinByFreq: accepted, no model */
        break;
    case 0x1a:                                     /* SetSoftMaxByFreq */
        /* param = (PPCLK_e << 16) | MHz; 0xffff is the automatic (unlimited) max. */
        if ((param >> 16) == 0)                    /* PPCLK_GFXCLK */
            s->smu_gfx_soft_max = (param & 0xffff) == 0xffff ? 0 : (param & 0xffff);
        break;
    case 0x24:                                     /* SetWorkloadMask */
        s->smu_workload_mask = param;
        fprintf(stderr, "rdna4: smu: SetWorkloadMask 0x%x\n", param);
        break;
    case 0x28:                                     /* AllowGfxOff: entered at once in the model */
        s->gfxoff_pending = false;
        s->gfxoff_active = true;
        fprintf(stderr, "rdna4: smu: AllowGfxOff, GC powered down\n");
        break;
    case 0x29:                                     /* DisallowGfxOff */
        s->gfxoff_pending = false;
        if (s->gfxoff_active) {
            /* Round 4 on the card: the directly programmed MMIO HQD does not survive the
             * power-down (CP_HQD_ACTIVE, PQ base, doorbell, rptr/wptr read 0 after the wake). */
            memset(s->hqd, 0, sizeof(s->hqd));
            reg_set(s, REG_CP_HQD_ACTIVE_EARLY, 0);
            /* Worst case for the registers the kext restores (the RLC save/restore list decides on
             * silicon): clock gating and the CP interrupt enables go back to reset values. */
            reg_set(s, GC_SEG1(0x4c49), 0x0001003c);   /* RLC_CGCG_CGLS_CTRL */
            reg_set(s, GC_SEG1(0x4c48), 0x000007ff);   /* RLC_CGTT_MGCG_OVERRIDE */
            reg_set(s, GC_SEG0(0x1e0a), 0);            /* CP_INT_CNTL_RING0 */
            reg_set(s, GC_SEG0(0x000d), reg_get(s, GC_SEG0(0x000d)) & ~1u);   /* SDMA0_CNTL.TRAP_ENABLE */
            s->gpm_restoring_reads = 2;
            fprintf(stderr, "rdna4: smu: DisallowGfxOff, GC powered up; HQD registers lost\n");
        }
        s->gfxoff_active = false;
        break;
    case 0x36:                                     /* RunDcBtc */
        break;
    default:
        resp = SMU_RESP_UNKNOWN;
        break;
    }
    reg_set(s, REG_SMU_RESP, resp);
}

/* ---- PSP ------------------------------------------------------------------ */

/*
 * The firmware side of the PSP, as far as the kext's stage 2 needs it. It
 * checks what a real PSP would reject outright (buffers that are empty or
 * outside VRAM, an sOS without its system driver, a ring before the sOS) and
 * otherwise accepts: it authenticates nothing and runs no firmware.
 */

/* VRAM offset of an MC address in the MM hub's FB aperture, or -1. */
static int64_t rdna4_mc_to_vram(RDNA4State *s, uint64_t mc)
{
    uint64_t base = (uint64_t)(reg_get(s, REG_MMHUB_FB_BASE) & 0xffffff) << 24;

    if (!base || mc < base || mc - base >= rdna4_vram_size()) {
        return -1;
    }
    return mc - base;
}

static uint8_t *rdna4_mc_span(RDNA4State *s, uint64_t mc, uint64_t len)
{
    int64_t off = rdna4_mc_to_vram(s, mc);

    return off < 0 ? NULL : rdna4_vram_span(s, off, len);
}

static uint32_t rdna4_mc_get(RDNA4State *s, uint64_t mc)
{
    uint8_t *p = rdna4_mc_span(s, mc, 4);

    return p ? ldl_le_p(p) : 0;
}

static void rdna4_mc_set(RDNA4State *s, uint64_t mc, uint32_t val)
{
    uint8_t *p = rdna4_mc_span(s, mc, 4);

    if (p) {
        stl_le_p(p, val);
    }
}

/* A buffer the driver claims to have filled: in VRAM and not all zero. */
static bool rdna4_psp_buffer_ok(RDNA4State *s, uint64_t mc, uint32_t len)
{
    uint8_t *p = rdna4_mc_span(s, mc, len);

    if (!p || !len) {
        return false;
    }
    for (uint32_t i = 0; i < MIN(len, 256u); i++) {
        if (p[i]) {
            return true;
        }
    }
    return false;
}

/*
 * Bit for a bootloader command in psp_bl_loaded: the driver-component
 * commands are 0x10000..0xF0000 (bits 1..15 by their [19:16]); SPL and
 * SPDM (0x10000000, 0x20000000) take bits 16 and 17.
 */
static uint32_t rdna4_psp_bl_bit(uint32_t cmd)
{
    return cmd >= 0x10000000 ? 16 + ctz32(cmd >> 28) : (cmd >> 16) & 15;
}

static void rdna4_psp_bootloader(RDNA4State *s, uint32_t cmd)
{
    uint64_t buf = (uint64_t)reg_get(s, REG_PSP_BL_BUF) << 20;

    reg_set(s, REG_PSP_BL_CMD, cmd);
    if (reg_get(s, REG_PSP_SOS)) {
        return;                         /* the bootloader is gone once the sOS runs */
    }
    if (!rdna4_psp_buffer_ok(s, buf, 1 * MiB)) {
        fprintf(stderr, "rdna4: psp: bootloader command 0x%x with an empty or unmapped "
                "buffer (MC 0x%" PRIx64 "), not answering\n", cmd, buf);
        return;                         /* stays busy: the driver times out */
    }
    if (cmd == PSP_BL_SOSDRV) {
        if (!(s->psp_bl_loaded & (1u << rdna4_psp_bl_bit(PSP_BL_SYSDRV)))) {
            fprintf(stderr, "rdna4: psp: sOS without its system driver, not starting\n");
            return;
        }
        reg_set(s, REG_PSP_SOS, PSP_SOS_VERSION);
        reg_set(s, REG_PSP_RING_CTL, PSP_READY);     /* TOS ready for a ring */
    }
    s->psp_bl_loaded |= 1u << rdna4_psp_bl_bit(cmd);
    fprintf(stderr, "rdna4: psp: bootloader took command 0x%x\n", cmd);
    reg_set(s, REG_PSP_BL_CMD, PSP_READY);
}

static void rdna4_psp_ring_ctl(RDNA4State *s, uint32_t val)
{
    reg_set(s, REG_PSP_RING_CTL, val);
    if (val != (2u << 16)) {            /* only KM (GPCOM) ring creation */
        return;
    }
    s->psp_ring_mc = reg_get(s, REG_PSP_RING_LO) |
                     ((uint64_t)reg_get(s, REG_PSP_RING_HI) << 32);
    s->psp_ring_size = reg_get(s, REG_PSP_RING_SIZE);
    if (!reg_get(s, REG_PSP_SOS) || s->psp_ring_size < PSP_FRAME_SIZE ||
        s->psp_ring_size % PSP_FRAME_SIZE ||
        !rdna4_mc_span(s, s->psp_ring_mc, s->psp_ring_size)) {
        fprintf(stderr, "rdna4: psp: ring create refused (MC 0x%" PRIx64 ", %u bytes)\n",
                 s->psp_ring_mc, s->psp_ring_size);
        s->psp_ring_size = 0;
        reg_set(s, REG_PSP_RING_CTL, PSP_READY | 0x1);   /* error status */
        return;
    }
    s->psp_rptr = 0;
    reg_set(s, REG_PSP_RING_WPTR, 0);
    reg_set(s, REG_PSP_RING_CTL, PSP_READY);
}

static void rdna4_psp_command(RDNA4State *s, uint64_t cmd)
{
    uint32_t id = rdna4_mc_get(s, cmd + PSP_CMD_ID), status = 0;

    switch (id) {
    case PSP_CMD_LOAD_TOC: {
        uint64_t toc = rdna4_mc_get(s, cmd + PSP_CMD_ARGS) |
                       ((uint64_t)rdna4_mc_get(s, cmd + PSP_CMD_ARGS + 4) << 32);
        uint32_t len = rdna4_mc_get(s, cmd + PSP_CMD_ARGS + 8);
        if (rdna4_psp_buffer_ok(s, toc, len)) {
            rdna4_mc_set(s, cmd + PSP_RESP_TMR_SIZE, PSP_TMR_SIZE);
        } else {
            status = 0xffff;
        }
        break;
    }
    case PSP_CMD_LOAD_IP_FW: {
        uint64_t fw = rdna4_mc_get(s, cmd + PSP_CMD_ARGS) |
                      ((uint64_t)rdna4_mc_get(s, cmd + PSP_CMD_ARGS + 4) << 32);
        uint32_t len = rdna4_mc_get(s, cmd + PSP_CMD_ARGS + 8);
        uint32_t type = rdna4_mc_get(s, cmd + PSP_CMD_ARGS + 12);
        if (!rdna4_psp_buffer_ok(s, fw, len)) {
            status = 0xffff;
            break;
        }
        if (type == PSP_FW_TYPE_SMU) {
            s->pmfw_loaded = true;
            reg_set(s, REG_SMU_RESP, SMU_RESP_OK);   /* booted PMFW: ready for messages */
        }
        if (type < 128) {
            s->psp_fw_types[type / 64] |= 1ull << (type % 64);
        }
        rdna4_mc_set(s, cmd + PSP_RESP_FW_LO, 0x1000 * type);  /* "TMR address" */
        fprintf(stderr, "rdna4: psp: LOAD_IP_FW type %u, %u bytes\n", type, len);
        break;
    }
    case PSP_CMD_AUTOLOAD_RLC: {
        /* RLC_G, IMU I/D, SDMA, RS64 PFP/ME/MEC and their stacks, MES */
        static const uint8_t need[] = { 8, 33, 34, 68, 69, 71, 87, 88, 89, 90, 92, 94, 95 };
        for (unsigned i = 0; i < ARRAY_SIZE(need); i++) {
            if (!(s->psp_fw_types[need[i] / 64] & (1ull << (need[i] % 64)))) {
                fprintf(stderr, "rdna4: psp: AUTOLOAD_RLC without firmware type %u\n",
                        need[i]);
                status = PSP_ERR_AUTOLOAD;
                break;
            }
        }
        if (status) {
            break;
        }
        /*
         * As on the card (2026-09-28): the command is accepted, but the IMU
         * stays in reset until the PMFW powers GFX up (feature GFX_IMU).
         */
        s->autoload_armed = true;
        fprintf(stderr, "rdna4: psp: AUTOLOAD_RLC: armed, waiting for the PMFW's GFX power-up\n");
        rdna4_gfx_autoload(s);
        break;
    }
    case PSP_CMD_FB_RESERV:
        break;
    default:
        status = PSP_ERR_UNKNOWN_CMD;
        break;
    }
    rdna4_mc_set(s, cmd + PSP_RESP_STATUS, status);
}

/* ---- GC hub + SDMA -------------------------------------------------------- */

/*
 * VRAM offset of an MC address as the GC hub translates it for VMID0: only
 * once the driver has set up the FB aperture, the L1 TLB and context 0 (the
 * card's GC hub is cold after the RLC autoload in this model). -1 = fault.
 */
static int64_t rdna4_gc_to_vram(RDNA4State *s, uint64_t mc)
{
    uint64_t base = (uint64_t)(reg_get(s, REG_GCMC_FB_BASE) & 0xffffff) << 24;
    uint64_t top = ((uint64_t)(reg_get(s, REG_GCMC_FB_TOP) & 0xffffff) << 24) | 0xffffff;

    if (!(reg_get(s, REG_GCMC_L1_TLB) & 1) || !(reg_get(s, REG_GCVM_CTX0_CNTL) & 1) ||
        !reg_get(s, REG_GCMC_FB_TOP) || mc < base || mc > top) {
        return -1;
    }
    return mc - base;
}

static uint8_t *rdna4_gc_span(RDNA4State *s, uint64_t mc, uint64_t len)
{
    int64_t off = rdna4_gc_to_vram(s, mc);

    return off < 0 ? NULL : rdna4_vram_span(s, off, len);
}

/* ---- IH v7: bus-addressed ring, writeback pointer and MSI model ---------- */

#define IH_RING_BYTES (256u << 10)
#define IH_ENTRY_BYTES 32u

static bool rdna4_ih_ready(RDNA4State *s)
{
    uint32_t cntl = reg_get(s, REG_IH_RB_CNTL);
    return (cntl & (IH_RB_ENABLE | IH_ENABLE_INTR)) ==
               (IH_RB_ENABLE | IH_ENABLE_INTR) &&
           ((cntl >> IH_MC_SPACE_SHIFT) & 7) == IH_MC_SPACE_BUS &&
           reg_get(s, REG_IH_RB_BASE) != 0 &&
           (reg_get(s, REG_IH_WPTR_ADDR_LO) & ~3u) != 0;
}

/* A GPU IH producer writes vectors into the system-memory ring and then
 * writebacks the producer pointer. The QEMU card has no IOMMU, so the bus
 * addresses programmed through MC_SPACE=2 are guest physical addresses. */
static void rdna4_ih_emit_vmid(RDNA4State *s, uint8_t client, uint8_t source,
                               uint8_t ring, uint32_t vmid, uint32_t data0)
{
    if (!rdna4_ih_ready(s)) {
        return;
    }
    PCIDevice *pci = PCI_DEVICE(s);
    uint32_t wptr = s->ih_wptr & (IH_RING_BYTES - 1);
    uint32_t next = (wptr + IH_ENTRY_BYTES) & (IH_RING_BYTES - 1);
    if (next == s->ih_rptr) {
        s->ih_overflow = true;
    }

    uint8_t entry[IH_ENTRY_BYTES] = { 0 };
    stl_le_p(entry + 0, (uint32_t)client | ((uint32_t)source << 8) |
                         ((uint32_t)ring << 16) | ((vmid & 0xf) << 24));
    uint64_t timestamp = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) & ((1ull << 48) - 1);
    stl_le_p(entry + 1 * 4, (uint32_t)timestamp);
    stl_le_p(entry + 2 * 4, (uint32_t)(timestamp >> 32) & 0xffff);
    stl_le_p(entry + 4 * 4, data0);
    uint64_t ring_bus = ((uint64_t)reg_get(s, REG_IH_RB_BASE) << 8) |
                        ((uint64_t)(reg_get(s, REG_IH_RB_BASE_HI) & 0xff) << 40);
    if (rdna4_dma_write(s, ring_bus + wptr, entry, sizeof(entry)) != MEMTX_OK) {
        fprintf(stderr, "rdna4: ih: ring DMA write at 0x%" PRIx64 " failed\n",
                ring_bus + wptr);
        return;
    }
    s->ih_wptr = next;
    uint32_t raw = next | (s->ih_overflow ? IH_WPTR_OVERFLOW : 0);
    reg_set(s, REG_IH_RB_WPTR, raw);
    if (reg_get(s, REG_IH_RB_CNTL) & IH_WPTR_WRITEBACK) {
        uint64_t wb = (reg_get(s, REG_IH_WPTR_ADDR_LO) & ~3u) |
                      ((uint64_t)(reg_get(s, REG_IH_WPTR_ADDR_HI) & 0xffff) << 32);
        uint8_t bytes[4];
        stl_le_p(bytes, raw);
        if (rdna4_dma_write(s, wb, bytes, sizeof(bytes)) != MEMTX_OK) {
            fprintf(stderr, "rdna4: ih: wptr writeback at 0x%" PRIx64 " failed\n", wb);
            return;
        }
    }
    if (!s->ih_dead) {
        msi_notify(pci, 0);
    }
}

static void rdna4_ih_emit(RDNA4State *s, uint8_t client, uint8_t source,
                          uint8_t ring, uint32_t data0)
{
    rdna4_ih_emit_vmid(s, client, source, ring, 0, data0);
}

static void rdna4_ih_reg_write(RDNA4State *s, uint32_t dw, uint32_t val)
{
    uint32_t byte = OSSSYS(dw);
    switch (dw) {
    case 0x0080: { /* RB_CNTL; overflow clear is a pulse */
        if (val & IH_WPTR_OVERFLOW_CLR) {
            s->ih_overflow = false;
        }
        reg_set(s, byte, val & ~IH_WPTR_OVERFLOW_CLR);
        break;
    }
    case 0x0081:
        s->ih_rptr = val & (IH_RING_BYTES - 1);
        reg_set(s, byte, s->ih_rptr);
        break;
    case 0x0082:
        s->ih_wptr = val & (IH_RING_BYTES - 1);
        s->ih_overflow = false;
        reg_set(s, byte, s->ih_wptr);
        break;
    default:
        reg_set(s, byte, val);
        break;
    }
}

/* VM table entries and VM physical addresses use the GC hub's GPU-physical
 * FB_OFFSET basis, rather than the MC address used by SDMA and VMID0. */
static int64_t rdna4_phys_to_vram(RDNA4State *s, uint64_t physical)
{
    uint64_t fb_offset = (uint64_t)(reg_get(s, REG_GCMC_FB_OFFSET) & 0xffffff) << 24;

    if (!(reg_get(s, REG_GCMC_L1_TLB) & 1) || !(reg_get(s, REG_GCVM_CTX0_CNTL) & 1) ||
        physical < fb_offset || physical - fb_offset >= rdna4_vram_size()) {
        return -1;
    }
    return physical - fb_offset;
}

static uint8_t *rdna4_phys_span(RDNA4State *s, uint64_t physical, uint64_t len)
{
    int64_t off = rdna4_phys_to_vram(s, physical);

    return off < 0 ? NULL : rdna4_vram_span(s, off, len);
}

/* gfx12 VM walk used by CP and shader accesses for runtime VMIDs. */
#define RDNA4_VM_VALID       (1ull << 0)
#define RDNA4_VM_SYSTEM     (1ull << 1)
#define RDNA4_VM_SNOOPED    (1ull << 2)
#define RDNA4_VM_EXECUTABLE  (1ull << 4)
#define RDNA4_VM_READABLE    (1ull << 5)
#define RDNA4_VM_WRITEABLE   (1ull << 6)
#define RDNA4_VM_PHYS_MASK   0x0000FFFFFFFFF000ull
#define RDNA4_VM_PDE_PTE     (1ull << 63)

static void rdna4_vm_fault(RDNA4State *s, uint32_t vmid, uint64_t va)
{
    /* gc_12_0_0_sh_mask.h GCVM_L2_PROTECTION_FAULT_STATUS_LO32: MORE_FAULTS
     * [0], PERMISSION_FAULTS [7:4] (bit 4: the valid bit), VMID [23:20].
     * The first fault latches until an invalidation clears it; later ones
     * only set MORE_FAULTS. FAULT_ADDR holds the logical page (VA >> 12:
     * LOGICAL_PAGE_ADDR_LO32, HI4). Retry is disabled by the kext. */
    uint32_t status = reg_get(s, REG_GCVM_FAULT_STATUS);
    if (status) {
        reg_set(s, REG_GCVM_FAULT_STATUS, status | 1u);
        return;
    }
    reg_set(s, REG_GCVM_FAULT_STATUS, (1u << 4) | ((vmid & 0xfu) << 20));
    reg_set(s, REG_GCVM_FAULT_ADDR_LO, (uint32_t)(va >> 12));
    reg_set(s, REG_GCVM_FAULT_ADDR_HI, (uint32_t)(va >> 44) & 0xfu);
}

static bool rdna4_vm_entry(RDNA4State *s, uint64_t address, uint64_t *entry)
{
    uint8_t *p = rdna4_phys_span(s, address, 8);
    if (!p)
        return false;
    *entry = ldq_le_p(p);
    return true;
}

static uint8_t *rdna4_gc_span_vmid(RDNA4State *s, uint64_t va, uint64_t len,
                                   uint32_t vmid, bool write, bool execute)
{
    if (!vmid)
        return rdna4_gc_span(s, va, len);
    uint32_t n = vmid - 1;
    uint32_t cntl = reg_get(s, REG_GCVM_CTX1_CNTL + n * 4);
    uint64_t base, start, end, table, entry;
    if (!(cntl & 1) || ((cntl >> 1) & 3) != 3 || va >= (1ull << 48))
        goto fault;
    start = reg_get(s, REG_GCVM_CTX1_START_LO + n * 8) |
            ((uint64_t)reg_get(s, REG_GCVM_CTX1_START_HI + n * 8) << 32);
    end = reg_get(s, REG_GCVM_CTX1_END_LO + n * 8) |
          ((uint64_t)reg_get(s, REG_GCVM_CTX1_END_HI + n * 8) << 32);
    if (va < start || va > end || len > (1ull << 48) - va || va + len - 1 > end)
        goto fault;
    base = reg_get(s, REG_GCVM_CTX1_BASE_LO + n * 8) |
           ((uint64_t)reg_get(s, REG_GCVM_CTX1_BASE_HI + n * 8) << 32);
    table = base & RDNA4_VM_PHYS_MASK;
    for (uint32_t level = 0; level < 4; level++) {
        uint32_t shift = 12 + (3 - level) * 9;
        uint32_t idx = (va >> shift) & 0x1ff;
        if (!rdna4_vm_entry(s, table + (uint64_t)idx * 8, &entry) ||
            !(entry & RDNA4_VM_VALID))
            goto fault;
        if (level < 3 && (entry & RDNA4_VM_PDE_PTE)) {
            uint32_t page_shift = 12 + (3 - level) * 9;
            uint64_t page_mask = (1ull << page_shift) - 1;
            if (!(entry & RDNA4_VM_READABLE) || (write && !(entry & RDNA4_VM_WRITEABLE)) ||
                (execute && !(entry & RDNA4_VM_EXECUTABLE)))
                goto fault;
            uint64_t physical = ((entry & RDNA4_VM_PHYS_MASK) & ~page_mask) |
                                (va & page_mask);
            int64_t off = rdna4_phys_to_vram(s, physical);
            if (off < 0)
                goto fault;
            return rdna4_vram_span(s, off, len);
        }
        if (level == 3) {
            if (!(entry & RDNA4_VM_READABLE) || (write && !(entry & RDNA4_VM_WRITEABLE)) ||
                (execute && !(entry & RDNA4_VM_EXECUTABLE)))
                goto fault;
            uint64_t physical = (entry & RDNA4_VM_PHYS_MASK) | (va & 0xfff);
            int64_t off = rdna4_phys_to_vram(s, physical);
            if (off < 0)
                goto fault;
            return rdna4_vram_span(s, off, len);
        }
        table = entry & RDNA4_VM_PHYS_MASK;
    }
fault:
    rdna4_vm_fault(s, vmid, va);
    /* Retry is off: serve the configured dummy page so the queue can drain.
     * GCVM_L2_CNTL.ENABLE_DEFAULT_PAGE_OUT_TO_SYSTEM_MEMORY [11] (amdgpu sets it,
     * gfxhub_v12_0.c:248) makes it a system-memory page: modelled as one shared zeroed
     * page whose writes are discarded, never a VRAM address. */
    if (reg_get(s, REG_GCVM_L2_CNTL) & (1u << 11)) {
        static uint8_t sys_dummy_page[0x1000];
        if (len <= 0x1000 && (va & 0xfff) + len <= 0x1000)
            return sys_dummy_page + (va & 0xfff);
        return NULL;
    }
    uint64_t dummy = ((uint64_t)reg_get(s, REG_GCVM_FAULT_DEFAULT_LO) |
                      ((uint64_t)reg_get(s, REG_GCVM_FAULT_DEFAULT_HI) << 32)) << 12;
    int64_t off = rdna4_phys_to_vram(s, dummy);
    if (off >= 0 && len <= 0x1000 && (va & 0xfff) + len <= 0x1000)
        return rdna4_vram_span(s, off + (va & 0xfff), len);
    return NULL;
}

typedef struct RDNA4VmTarget {
    bool system;
    uint64_t address;
} RDNA4VmTarget;

/* Resolve one page of a VMID mapping.  GPU-physical VRAM and PCI bus
 * addresses are deliberately kept distinct: an MC address in a page-table
 * entry cannot accidentally reach the VRAM array. */
static bool rdna4_vm_target(RDNA4State *s, uint64_t va, uint64_t len,
                            uint32_t vmid, bool write, bool execute,
                            RDNA4VmTarget *target)
{
    uint32_t n = vmid - 1;
    uint32_t cntl = reg_get(s, REG_GCVM_CTX1_CNTL + n * 4);
    uint64_t base, start, end, table, entry;

    if (!(cntl & 1) || ((cntl >> 1) & 3) != 3 || va >= (1ull << 48) ||
        !len || len > 0x1000 - (va & 0xfff))
        goto fault;
    start = reg_get(s, REG_GCVM_CTX1_START_LO + n * 8) |
            ((uint64_t)reg_get(s, REG_GCVM_CTX1_START_HI + n * 8) << 32);
    end = reg_get(s, REG_GCVM_CTX1_END_LO + n * 8) |
          ((uint64_t)reg_get(s, REG_GCVM_CTX1_END_HI + n * 8) << 32);
    if (va < start || va > end || len > (1ull << 48) - va || va + len - 1 > end)
        goto fault;
    base = reg_get(s, REG_GCVM_CTX1_BASE_LO + n * 8) |
           ((uint64_t)reg_get(s, REG_GCVM_CTX1_BASE_HI + n * 8) << 32);
    table = base & RDNA4_VM_PHYS_MASK;
    for (uint32_t level = 0; level < 4; level++) {
        uint32_t shift = 12 + (3 - level) * 9;
        uint32_t idx = (va >> shift) & 0x1ff;
        if (!rdna4_vm_entry(s, table + (uint64_t)idx * 8, &entry) ||
            !(entry & RDNA4_VM_VALID))
            goto fault;
        if (level < 3 && (entry & RDNA4_VM_PDE_PTE)) {
            uint32_t page_shift = 12 + (3 - level) * 9;
            uint64_t page_mask = (1ull << page_shift) - 1;
            if (entry & RDNA4_VM_SYSTEM || !(entry & RDNA4_VM_READABLE) ||
                (write && !(entry & RDNA4_VM_WRITEABLE)) ||
                (execute && !(entry & RDNA4_VM_EXECUTABLE)))
                goto fault;
            target->system = false;
            target->address = ((entry & RDNA4_VM_PHYS_MASK) & ~page_mask) |
                              (va & page_mask);
            if (rdna4_phys_to_vram(s, target->address) < 0)
                goto fault;
            return true;
        }
        if (level == 3) {
            if (!(entry & RDNA4_VM_READABLE) || (write && !(entry & RDNA4_VM_WRITEABLE)) ||
                (execute && !(entry & RDNA4_VM_EXECUTABLE)))
                goto fault;
            target->system = (entry & RDNA4_VM_SYSTEM) != 0;
            target->address = (entry & RDNA4_VM_PHYS_MASK) | (va & 0xfff);
            if (!target->system && rdna4_phys_to_vram(s, target->address) < 0)
                goto fault;
            return true;
        }
        table = entry & RDNA4_VM_PHYS_MASK;
    }
fault:
    rdna4_vm_fault(s, vmid, va);
    return false;
}

/* Shader global memory operations can target system PTEs.  Accesses are split
 * at page boundaries and use PCI DMA for system leaves, just like SDMA's AGP
 * path. */
static bool rdna4_vm_access(RDNA4State *s, uint64_t va, uint8_t *data, uint64_t len,
                            uint32_t vmid, bool write, bool execute)
{
    while (len) {
        uint64_t chunk = 0x1000 - (va & 0xfff);
        if (chunk > len)
            chunk = len;
        if (!vmid) {
            uint8_t *p = rdna4_gc_span(s, va, chunk);
            if (!p)
                return false;
            if (write)
                memcpy(p, data, chunk);
            else
                memcpy(data, p, chunk);
        } else {
            RDNA4VmTarget target;
            if (!rdna4_vm_target(s, va, chunk, vmid, write, execute, &target)) {
                /* Retry is disabled in the model; retain the existing dummy
                 * page behaviour so a freed VA faults and drains cleanly. */
                uint8_t *dummy = rdna4_gc_span_vmid(s, va, chunk, vmid, write, execute);
                if (!dummy)
                    return false;
                if (write)
                    memcpy(dummy, data, chunk);
                else
                    memcpy(data, dummy, chunk);
            } else if (target.system) {
                if (!rdna4_bus_master_enabled(s)) {
                    rdna4_vm_fault(s, vmid, va);
                    return false;
                }
                MemTxResult result = write ? rdna4_dma_write(s, target.address, data, chunk)
                                           : rdna4_dma_read(s, target.address, data, chunk);
                if (result != MEMTX_OK) {
                    rdna4_vm_fault(s, vmid, va);
                    return false;
                }
            } else {
                int64_t off = rdna4_phys_to_vram(s, target.address);
                uint8_t *p = off < 0 ? NULL : rdna4_vram_span(s, off, chunk);
                if (!p)
                    return false;
                if (write)
                    memcpy(p, data, chunk);
                else
                    memcpy(data, p, chunk);
            }
        }
        va += chunk;
        data += chunk;
        len -= chunk;
    }
    return true;
}

static bool rdna4_gfx_mem_mapped(RDNA4State *s, uint64_t va, uint64_t len,
                                 uint32_t vmid, bool write, bool execute)
{
    while (len) {
        uint64_t chunk = 0x1000 - (va & 0xfff);

        if (chunk > len)
            chunk = len;
        if (!vmid) {
            if (!rdna4_gc_span(s, va, chunk))
                return false;
        } else {
            RDNA4VmTarget target;
            if (!rdna4_vm_target(s, va, chunk, vmid, write, execute, &target))
                return false;
        }
        va += chunk;
        len -= chunk;
    }
    return true;
}

static bool rdna4_gfx_mem_read(RDNA4State *s, uint64_t va, void *data,
                               uint64_t len, uint32_t vmid, bool execute)
{
    if (!vmid) {
        uint8_t *p = rdna4_gc_span(s, va, len);

        if (!p)
            return false;
        memcpy(data, p, len);
        return true;
    }
    return rdna4_vm_access(s, va, data, len, vmid, false, execute);
}

static bool rdna4_gfx_mem_write(RDNA4State *s, uint64_t va, const void *data,
                                uint64_t len, uint32_t vmid)
{
    if (!vmid) {
        uint8_t *p = rdna4_gc_span(s, va, len);

        if (!p)
            return false;
        memcpy(p, data, len);
        return true;
    }
    return rdna4_vm_access(s, va, (uint8_t *)(uintptr_t)data, len, vmid, true, false);
}

#define REG_GCMC_AGP_TOP     GC_SEG0(0x1616)   /* MC >> 24 */
#define REG_GCMC_AGP_BOT     GC_SEG0(0x1617)
#define REG_GCMC_AGP_BASE    GC_SEG0(0x1618)   /* system address >> 24 */

/*
 * The system address of `len` bytes at MC `mc` in the GC hub's AGP
 * aperture (system = mc - BOT + BASE, amdgpu keeps BASE 0), or -1.
 */
static int64_t rdna4_gc_agp(RDNA4State *s, uint64_t mc, uint64_t len)
{
    uint64_t bot = (uint64_t)(reg_get(s, REG_GCMC_AGP_BOT) & 0xffffff) << 24;
    uint64_t top = ((uint64_t)(reg_get(s, REG_GCMC_AGP_TOP) & 0xffffff) << 24) | 0xffffff;
    uint64_t base = (uint64_t)(reg_get(s, REG_GCMC_AGP_BASE) & 0xffffff) << 24;

    if (!(reg_get(s, REG_GCMC_L1_TLB) & 1) || bot > top || mc < bot || mc + len - 1 > top) {
        return -1;
    }
    return mc - bot + base;
}

/*
 * SDMA COPY_LINEAR between VRAM and/or system memory. System memory is the
 * guest's, reached by bus-master DMA, which the device must have enabled.
 */
static bool rdna4_sdma_copy(RDNA4State *s, uint64_t src, uint64_t dst, uint32_t bytes)
{
    PCIDevice *pci = PCI_DEVICE(s);
    uint8_t *sp = rdna4_gc_span(s, src, bytes), *dp = rdna4_gc_span(s, dst, bytes);
    int64_t sa = sp ? -1 : rdna4_gc_agp(s, src, bytes), da = dp ? -1 : rdna4_gc_agp(s, dst, bytes);
    bool ok;

    if ((!sp && sa < 0) || (!dp && da < 0)) {
        fprintf(stderr, "rdna4: sdma: copy 0x%" PRIx64 " -> 0x%" PRIx64 " (%u bytes) outside "
                "VRAM and the AGP aperture\n", src, dst, bytes);
        return false;
    }
    if ((!sp || !dp) && !(pci_get_word(pci->config + PCI_COMMAND) & PCI_COMMAND_MASTER)) {
        fprintf(stderr, "rdna4: sdma: system-memory copy with bus mastering off\n");
        return false;
    }
    if (sp && dp) {
        memmove(dp, sp, bytes);
        return true;
    }
    if (s->dma_broken) {
        return false;                               /* the engine stops on the packet */
    }
    if (sp) {
        ok = rdna4_dma_write(s, da, sp, bytes) == MEMTX_OK;          /* VRAM -> system */
    } else if (dp) {
        ok = rdna4_dma_read(s, sa, dp, bytes) == MEMTX_OK;            /* system -> VRAM */
    } else {
        g_autofree uint8_t *tmp = g_malloc(bytes);
        ok = rdna4_dma_read(s, sa, tmp, bytes) == MEMTX_OK &&
             rdna4_dma_write(s, da, tmp, bytes) == MEMTX_OK;
    }
    if (!ok) {
        fprintf(stderr, "rdna4: sdma: bus-master DMA %s 0x%" PRIx64 " failed\n",
                sp ? "to" : "from", sp ? (uint64_t)da : (uint64_t)sa);
    }
    return ok;
}

/*
 * SDMA0 queue 0: run the packets between RPTR and the new WPTR. Knows the
 * packets the kext uses: NOP, WRITE (linear), COPY (linear), FENCE and
 * CONST_FILL. A fault or an unknown packet stops the engine where it is.
 *
 * As on the card (SDMA 7, 64-bit pointers), the wptr only grows: a value
 * below the last one is ignored and the engine waits, which is what a
 * driver that wraps its pointer at the ring's end gets (2026-09-28). Zero
 * starts over (the queue being programmed).
 */
static void rdna4_sdma_wptr(RDNA4State *s, uint64_t wptr64)
{
    uint32_t wptr = (uint32_t)wptr64;

    if (wptr64 && wptr64 < s->sdma_wptr) {
        fprintf(stderr, "rdna4: sdma: wptr went back from 0x%" PRIx64 " to 0x%" PRIx64
                ": ignored, the engine waits\n", s->sdma_wptr, wptr64);
        return;
    }
    s->sdma_wptr = wptr64;
    s->sdma_work.pending_wptr = wptr64;
    s->sdma_work.pending = true;
    reg_set(s, REG_SDMA0_RB_WPTR, wptr);
    rdna4_work_schedule(s);
}

static bool rdna4_sdma_process_slice(RDNA4State *s, uint64_t deadline)
{
    for (;;) {
        uint32_t cntl, op, sub, len;
        uint64_t a;
        uint32_t dw[8];
        uint8_t *p, *dst;

        if (!s->sdma_work.active) {
            if (!s->sdma_work.pending)
                return false;
            s->sdma_work.pending = false;
            cntl = reg_get(s, REG_SDMA0_RB_CNTL);
            s->sdma_work.size = 4u << ((cntl >> 1) & 0x1f);
            s->sdma_work.ring = ((uint64_t)reg_get(s, REG_SDMA0_RB_BASE) << 8) |
                                 ((uint64_t)reg_get(s, REG_SDMA0_RB_BASE_HI) << 40);
            s->sdma_work.rptr = reg_get(s, REG_SDMA0_RB_RPTR);
            s->sdma_work.wptr = s->sdma_work.pending_wptr & (s->sdma_work.size - 1);
            s->sdma_work.packet_active = false;
            if (!s->gfx_booted || !(cntl & 1) || (reg_get(s, REG_SDMA0_MCU_CNTL) & 1))
                continue;
            s->sdma_work.active = true;
        }
        cntl = reg_get(s, REG_SDMA0_RB_CNTL);
        if (!s->sdma_work.packet_active && s->sdma_work.rptr == s->sdma_work.wptr) {
            if (cntl & (1u << 12)) {
                uint64_t wb = (reg_get(s, REG_SDMA0_RPTR_LO) & ~3u) |
                              ((uint64_t)reg_get(s, REG_SDMA0_RPTR_HI) << 32);
                p = rdna4_gc_span(s, wb, 8);
                if (p)
                    stq_le_p(p, s->sdma_work.rptr);
            }
            s->sdma_work.active = false;
            continue;
        }
        if (!s->sdma_work.packet_active) {
            for (int i = 0; i < 8; i++) {
                p = rdna4_gc_span(s, s->sdma_work.ring +
                                  ((s->sdma_work.rptr + 4 * i) & (s->sdma_work.size - 1)), 4);
                if (!p) {
                    fprintf(stderr, "rdna4: sdma: ring MC 0x%" PRIx64
                            " not mapped by the GC hub\n", s->sdma_work.ring);
                    s->sdma_work.active = false;
                    return false;
                }
                dw[i] = ldl_le_p(p);
            }
            op = dw[0] & 0xff;
            sub = (dw[0] >> 8) & 0xff;
            len = 1;
            a = dw[1] | ((uint64_t)dw[2] << 32);
            switch (op) {
            case 0:
                len = 1 + ((dw[0] >> 16) & 0x3fff);
                s->sdma_work.packet_len = len;
                break;
            case 2:
                len = 5;
                if (sub || dw[3] != 0 || !(dst = rdna4_gc_span(s, a, 4)))
                    goto fault;
                stl_le_p(dst, dw[4]);
                s->sdma_work.packet_len = len;
                break;
            case 5:
                len = 4;
                if (!(dst = rdna4_gc_span(s, a & ~3ull, 4)))
                    goto fault;
                stl_le_p(dst, dw[3]);
                s->sdma_work.packet_len = len;
                break;
            case 6:
                len = 2;
                if (reg_get(s, REG_SDMA0_CNTL) & 1)
                    rdna4_ih_emit(s, 0x0a, 49, 0, dw[1]);
                s->sdma_work.packet_len = len;
                break;
            case 11:
                len = 5;
                s->sdma_work.packet_len = len;
                s->sdma_work.packet_op = op;
                s->sdma_work.copy_dst = a;
                s->sdma_work.copy_bytes = dw[4] + 1;
                s->sdma_work.copy_done = 0;
                s->sdma_work.fill_value = dw[3];
                s->sdma_work.fill_size = (dw[0] >> 30) == 2 ? 4 : 1;
                s->sdma_work.packet_active = true;
                break;
            case 1:
                len = 8;
                s->sdma_work.packet_len = len;
                s->sdma_work.packet_op = op;
                s->sdma_work.copy_bytes = dw[1] + 1;
                s->sdma_work.copy_done = 0;
                s->sdma_work.copy_src = dw[3] | ((uint64_t)dw[4] << 32);
                s->sdma_work.copy_dst = dw[5] | ((uint64_t)dw[6] << 32);
                if (sub)
                    goto fault;
                s->sdma_work.packet_active = true;
                break;
            default:
                fprintf(stderr, "rdna4: sdma: unknown packet 0x%08x at rptr 0x%x, stopping\n",
                        dw[0], s->sdma_work.rptr);
                s->sdma_work.active = false;
                return false;
            }
        }
        if (s->sdma_work.packet_active) {
            uint32_t left = s->sdma_work.copy_bytes - s->sdma_work.copy_done;
            uint32_t chunk = MIN(left, 1u << 20);
            if (s->sdma_work.packet_op == 1) {
                if (!rdna4_sdma_copy(s, s->sdma_work.copy_src + s->sdma_work.copy_done,
                                     s->sdma_work.copy_dst + s->sdma_work.copy_done, chunk))
                    goto fault_active;
            } else {
                dst = rdna4_gc_span(s, s->sdma_work.copy_dst + s->sdma_work.copy_done, chunk);
                if (!dst)
                    goto fault_active;
                if (s->sdma_work.fill_size == 4) {
                    for (uint32_t i = 0; i + 4 <= chunk; i += 4)
                        stl_le_p(dst + i, s->sdma_work.fill_value);
                } else {
                    memset(dst, s->sdma_work.fill_value & 0xff, chunk);
                }
            }
            s->sdma_work.copy_done += chunk;
            if (s->sdma_work.copy_done != s->sdma_work.copy_bytes) {
                /* Keep each DMA operation <= 1 MiB, but use the remainder of
                 * this bounded callback for the next chunk. */
                if (qemu_clock_get_ns(QEMU_CLOCK_REALTIME) >= deadline)
                    goto next_slice;
                continue;
            }
            s->sdma_work.packet_active = false;
        }
        s->sdma_work.rptr = (s->sdma_work.rptr + 4 * s->sdma_work.packet_len) &
                             (s->sdma_work.size - 1);
        reg_set(s, REG_SDMA0_RB_RPTR, s->sdma_work.rptr);
        if (qemu_clock_get_ns(QEMU_CLOCK_REALTIME) >= deadline)
            goto next_slice;
        continue;
fault:
        fprintf(stderr, "rdna4: sdma: packet at rptr 0x%x: address 0x%" PRIx64
                " not mapped by the GC hub, stopping\n", s->sdma_work.rptr, a);
        s->sdma_work.active = false;
        return false;
fault_active:
        fprintf(stderr, "rdna4: sdma: packet at rptr 0x%x: address not mapped, stopping\n",
                s->sdma_work.rptr);
        s->sdma_work.active = false;
        s->sdma_work.packet_active = false;
        return false;
next_slice:
        return s->sdma_work.active || s->sdma_work.pending;
    }
}

/* Consume GPCOM frames up to the new write pointer. */
static void rdna4_psp_wptr(RDNA4State *s, uint32_t wptr)
{
    uint32_t ring_dw = s->psp_ring_size / 4;

    reg_set(s, REG_PSP_RING_WPTR, wptr);
    if (!ring_dw || wptr >= ring_dw) {
        return;
    }
    while (s->psp_rptr != wptr) {
        uint64_t frame = s->psp_ring_mc + s->psp_rptr * 4ull;
        uint64_t cmd = rdna4_mc_get(s, frame) | ((uint64_t)rdna4_mc_get(s, frame + 4) << 32);
        uint64_t fence = rdna4_mc_get(s, frame + 12) |
                         ((uint64_t)rdna4_mc_get(s, frame + 16) << 32);

        rdna4_psp_command(s, cmd);
        rdna4_mc_set(s, fence, rdna4_mc_get(s, frame + 20));
        s->psp_rptr = (s->psp_rptr + PSP_FRAME_SIZE / 4) % ring_dw;
    }
}

/* ---- BAR5 --------------------------------------------------------------- */

/* GFXOFF hazard model (W27). The GC block, which holds the CP, RLC, SDMA, GCVM and
 * GRBM registers and the MEC/gfx/SDMA doorbells, is powered down while GFXOFF is
 * allowed. On a real card an MMIO or doorbell access to it can hang the bus (amdgpu
 * disallows GFXOFF around every direct GC access, amdgpu_gfx_off_ctrl). The emulator
 * cannot hang, so it makes the violation loud: reads return all ones, writes are
 * dropped, and every access is counted and logged. Register windows are this
 * emulator's GC segment 0 (0x1260) and 1 (0xa000). */
static bool rdna4_gc_dword(uint32_t dw)
{
    return (dw >= 0x1260 && dw < 0x1260 + 0x2200) || (dw >= 0xa000 && dw < 0xa000 + 0x5000);
}

static bool rdna4_gfxoff_hazard(RDNA4State *s, const char *what, hwaddr addr)
{
    if (s->gfxoff_pending && qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) >= s->gfxoff_arm_ns) {
        s->gfxoff_pending = false;
        s->gfxoff_active = true;
        fprintf(stderr, "rdna4: gfxoff-preset armed: GC powered down\n");
    }
    if (!s->gfxoff_active) {
        return false;
    }
    s->gfxoff_violations++;
    fprintf(stderr, "rdna4: GC %s 0x%05" HWADDR_PRIx " while GFXOFF is allowed (violation %u): "
            "a real card can hang here\n", what, addr, s->gfxoff_violations);
    return true;
}

static uint64_t rdna4_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    RDNA4State *s = opaque;
    uint32_t dw = addr / 4, val;

    addr &= ~3ull;
    if (rdna4_gc_dword(dw) && rdna4_gfxoff_hazard(s, "read", addr)) {
        return 0xffffffffu;
    }
    if (addr == GC_SEG1(0x4e6c)) {
        /* RLC_GPM_STAT: GFX_POWER_STATUS [1] while powered; RESTORING_REGISTERS [10] for a
         * couple of reads right after a GFXOFF wake, as the RLC restores its save/restore list. */
        val = 0x2;
        if (s->gpm_restoring_reads) {
            val |= 1u << 10;
            s->gpm_restoring_reads--;
        }
        return val;
    }
    if (addr == REG_MM_DATA) {
        val = rdna4_mm_read(s);
    } else if (addr == REG_CONFIG_MEMSIZE) {
        val = RDNA4_VRAM_MB;
    } else if (dw >= DMU_SEG2 && dw < DMU_SEG3) {
        uint32_t d2 = dw - DMU_SEG2;
        if (d2 >= OPTC_INPUT_CLOCK && d2 < OPTC_INPUT_CLOCK + NUM_OTG * ODM_STRIDE &&
            (d2 - OPTC_INPUT_CLOCK) % ODM_STRIDE == 0) {
            val = reg_get(s, addr);
            val = (val & ~(1u << 2)) | (((val >> 1) & 1) << 2);   /* CLK_ON follows EN */
        } else if (d2 >= DIG_FIFO_CTRL0 && d2 < DIG_FIFO_CTRL0 + NUM_DIG * DIG_STRIDE &&
                   (d2 - DIG_FIFO_CTRL0) % DIG_STRIDE == 0) {
            val = reg_get(s, addr);
            val = (val & ~(1u << 20)) | (((val >> 1) & 1) << 20); /* RESET_DONE follows RESET */
        } else if (d2 >= HUBP_DB_FIRST && d2 < HUBP_DB_FIRST + NUM_OTG * HUBP_STRIDE &&
                   (d2 - HUBP_DB_FIRST) % HUBP_STRIDE <= HUBP_DB_LAST - HUBP_DB_FIRST) {
            uint32_t hubp = (d2 - HUBP_DB_FIRST) / HUBP_STRIDE;
            val = rdna4_hubp_read(s, hubp, d2 - hubp * HUBP_STRIDE);
        } else if (d2 >= OTG_H_TOTAL && d2 < OTG_H_TOTAL + NUM_OTG * OTG_STRIDE) {
            uint32_t otg = (d2 - OTG_H_TOTAL) / OTG_STRIDE;
            val = rdna4_otg_read(s, otg, d2 - otg * OTG_STRIDE);
        } else if (rdna4_is_i2c(d2)) {
            val = rdna4_i2c_read(s, d2);
        } else if (d2 >= AUX_BASE && d2 < AUX_BASE + NUM_AUX * AUX_STRIDE) {
            uint32_t n = (d2 - AUX_BASE) / AUX_STRIDE;
            val = rdna4_aux_read(s, AUX_BASE + n * AUX_STRIDE,
                                 d2 - AUX_BASE - n * AUX_STRIDE);
        } else {
            val = reg_get(s, addr);
        }
    } else {
        val = reg_get(s, addr);
    }
    if (s->trace && addr != REG_MM_DATA) {
        fprintf(stderr, "rdna4: R 0x%05" HWADDR_PRIx " = 0x%08x\n", addr, val);
    }
    return val;
}

static void rdna4_mmio_write(void *opaque, hwaddr addr, uint64_t data,
                             unsigned size)
{
    RDNA4State *s = opaque;
    uint32_t dw = addr / 4, val = data;

    addr &= ~3ull;
    if (rdna4_gc_dword(dw) && rdna4_gfxoff_hazard(s, "write", addr)) {
        return;
    }
    if (addr == GC_SEG1(0x0980)) {
        /* RLC_SAFE_MODE: the RLC firmware acknowledges a request by clearing CMD (bit 0)
         * and putting the answer in RESPONSE [11:8] (gfx_v12_0_set_safe_mode waits for
         * CMD to clear). MESSAGE 1 = enter safe mode. */
        val = (val & 1) ? (((val >> 1) & 0xf) == 1 ? (1u << 8) : 0) : val;
    }
    if (s->trace && addr != REG_MM_DATA) {
        fprintf(stderr, "rdna4: W 0x%05" HWADDR_PRIx " = 0x%08x\n", addr, val);
    }
    if (addr == REG_MM_DATA) {
        rdna4_mm_write(s, val);
    } else if (addr == REG_CONFIG_MEMSIZE) {
        /* read-only */
    } else if (addr == REG_SMU_MSG) {
        rdna4_smu_msg(s, val);
    } else if (addr == REG_PSP_BL_CMD) {
        rdna4_psp_bootloader(s, val);
    } else if (addr == REG_PSP_RING_CTL) {
        rdna4_psp_ring_ctl(s, val);
    } else if (addr == REG_PSP_RING_WPTR) {
        rdna4_psp_wptr(s, val);
    } else if (dw >= OSSSYS_SEG0 && dw < OSSSYS_SEG0 + 0x300) {
        rdna4_ih_reg_write(s, dw - OSSSYS_SEG0, val);
    } else if (addr == REG_GFX_CP_ME_CNTL) {
        reg_set(s, addr, val);
        if (val & ((1u << 26) | (1u << 28))) {
            s->gfx_reinit = true;
            s->gfx_csb_loaded = false;
            s->gfx_pending = false;
            s->gfx_active = false;
            reg_set(s, REG_GC_CP_STAT, 0);
            fprintf(stderr, "rdna4: gfx: CP_ME_CNTL halted (0x%08x); RB0 recovery armed\n", val);
        } else if (s->gfx_reinit) {
            fprintf(stderr, "rdna4: gfx: CP_ME_CNTL unhalted (0x%08x); RB0 recovery may kick\n", val);
        }
    } else if (addr == REG_GFX_CP_RB0_WPTR) {
        rdna4_gfx_wptr(s, (uint64_t)val |
                        ((uint64_t)reg_get(s, REG_GFX_CP_RB0_WPTR_HI) << 32), false);
    } else if (addr == REG_SDMA0_RB_WPTR) {
        rdna4_sdma_wptr(s, val | ((uint64_t)reg_get(s, REG_SDMA0_RB_WPTR + 4) << 32));
    } else if (addr == REG_GCVM_INV17_REQ) {
        reg_set(s, addr, val);
        if (!s->inv_noack) {
            reg_set(s, REG_GCVM_INV17_ACK, val & 0xffff);   /* per-VMID ack */
        }
        /* The invalidation does not clear the fault status: amdgpu never
         * relies on CLEAR_PROTECTION_FAULT_STATUS_ADDR here (bit 24, left 0
         * by gfxhub_v12_0_get_invalidate_req); see REG_GCVM_FAULT_CNTL. */
    } else if (addr == REG_GCVM_FAULT_CNTL) {
        /* GCVM_L2_PROTECTION_FAULT_CNTL.CLEAR_PROTECTION_FAULT_STATUS_ADDR
         * (bit 0) clears the latched status and address, as amdgpu's fault
         * handler does (gmc_v12_0.c: WREG32_P(vm_l2_pro_fault_cntl, 1, ~1)).
         * Modelled as a trigger that reads back 0. */
        reg_set(s, addr, val & ~1u);
        if (val & 1u) {
            reg_set(s, REG_GCVM_FAULT_STATUS, 0);
            reg_set(s, REG_GCVM_FAULT_ADDR_LO, 0);
            reg_set(s, REG_GCVM_FAULT_ADDR_HI, 0);
        }
    } else if (addr == GC_SEG1(0x4c80)) {
        /* RLC_SRM_CNTL (gfx_v12_0_rlc_enable_srm sets SRM_ENABLE | AUTO_INCR_ADDR): the RLC then applies the
         * clear-state buffer it was given (RLC_CSIB_*) to the context registers. Model of the mechanism
         * premetal/rootcause-draw.md #1 names; it is not verified on the card. */
        reg_set(s, addr, val);
        if (val & 1u)
            rdna4_rlc_srm_apply(s);
    } else if (addr == REG_GRBM_GFX_CNTL) {
        reg_set(s, addr, val);
        /* MEC1 has 2 pipes x 4 queues on GC 12.0.x (gfx_v12_0.c:1415-1423); the missing
         * pipe/queue ID bits alias real slots (round 4 log: a queue on "pipe 2" read back the
         * kernel ring's HQD). */
        s->selected_pipe = val & 1;
        s->selected_vmid = (val >> 4) & 0xf;
        s->selected_queue = (val >> 8) & 3;
        if (s->selected_pipe || s->selected_queue || s->selected_vmid)
            s->hqd[s->selected_pipe][s->selected_queue].used = true;
    } else if (addr == REG_CP_HQD_DEQUEUE_REQ) {
        reg_set(s, addr, val);
        if (val & 3) {
            if (s->hang_sticky) {
                fprintf(stderr, "rdna4: mec: HQD dequeue/wave kill held by hang-sticky\n");
            } else {
                /* CP_HQD_DEQUEUE_REQUEST RESET_WAVES: drop pending waves and deactivate. */
                s->mec_hung = false;
                reg_set(s, REG_CP_HQD_ACTIVE_EARLY, 0);
                reg_set(s, GC_SEG0(0x1fb3), 0);
                reg_set(s, GC_SEG0(0x1fdf), 0);
                reg_set(s, GC_SEG0(0x1fe0), 0);
                fprintf(stderr, "rdna4: mec: HQD dequeue reset waves; queue inactive\n");
            }
        }
    } else if (addr == REG_SQ_CMD) {
        reg_set(s, addr, val);
        if ((val & 0xf) == 3)
            fprintf(stderr, "rdna4: mec: SQ_CMD killed VMID-selected waves (0x%08x)\n", val);
    } else if (dw >= DMU_SEG2 && dw < DMU_SEG3) {
        uint32_t d2 = dw - DMU_SEG2;
        const bool cursorWrite = rdna4_is_cursor_register(d2);
        if (d2 == DMCUB_INBOX1_WPTR) {
            rdna4_dmub_wptr(s, val);
        } else if (d2 >= HUBP_FLIP_INTERRUPT &&
                   d2 < HUBP_FLIP_INTERRUPT + NUM_OTG * HUBP_STRIDE &&
                   (d2 - HUBP_FLIP_INTERRUPT) % HUBP_STRIDE == 0) {
            uint32_t hubp = (d2 - HUBP_FLIP_INTERRUPT) / HUBP_STRIDE;
            uint32_t flip_addr = SEG2(HUBP_FLIP_INTERRUPT + hubp * HUBP_STRIDE);
            uint32_t next = val;
            if (val & DCN_FLIP_CLEAR)
                next &= ~(DCN_FLIP_OCCURRED | DCN_FLIP_STATUS | DCN_FLIP_CLEAR);
            reg_set(s, flip_addr, next);
        } else if (d2 >= HUBP_DB_FIRST && d2 < HUBP_DB_FIRST + NUM_OTG * HUBP_STRIDE &&
                   (d2 - HUBP_DB_FIRST) % HUBP_STRIDE <= HUBP_DB_LAST - HUBP_DB_FIRST) {
            uint32_t hubp = (d2 - HUBP_DB_FIRST) / HUBP_STRIDE;
            int otg = rdna4_otg_for_hubp(s, hubp);
            rdna4_db_write(s, otg >= 0 ? otg : hubp, addr, val);
        } else if (d2 >= OTG_H_TOTAL && d2 < OTG_H_TOTAL + NUM_OTG * OTG_STRIDE) {
            uint32_t otg = (d2 - OTG_H_TOTAL) / OTG_STRIDE;
            rdna4_otg_write(s, otg, d2 - otg * OTG_STRIDE, val);
        } else if (rdna4_is_i2c(d2)) {
            rdna4_i2c_write(s, d2, val);
        } else if (d2 >= AUX_BASE && d2 < AUX_BASE + NUM_AUX * AUX_STRIDE) {
            uint32_t n = (d2 - AUX_BASE) / AUX_STRIDE;
            rdna4_aux_write(s, AUX_BASE + n * AUX_STRIDE,
                            d2 - AUX_BASE - n * AUX_STRIDE, val);
        } else {
            reg_set(s, addr, val);
        }
        if (cursorWrite && s->cursor_enabled)
            rdna4_gfx_update(s);
    } else {
        reg_set(s, addr, val);
    }
}

static const MemoryRegionOps rdna4_mmio_ops = {
    .read = rdna4_mmio_read,
    .write = rdna4_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/* BAR4 I/O: present, inert. */
static uint64_t rdna4_inert_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

static void rdna4_inert_write(void *opaque, hwaddr addr, uint64_t data,
                              unsigned size)
{
}

static const MemoryRegionOps rdna4_inert_ops = {
    .read = rdna4_inert_read,
    .write = rdna4_inert_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* ---- MEC compute queue --------------------------------------------------- */

/*
 * One MEC queue, the one the driver programs (the model does not bank the
 * CP_HQD_* registers by GRBM_GFX_CNTL). It runs when its doorbell rings and
 * every piece of routing the driver owes is in place.
 */
#define REG_NBIF_DB_APER_EN  ((0xd20 + 0x00c0) * 4)   /* BIF_DOORBELL_APER_EN [0] */
#define REG_NBIF_S2A_ENTRY0  ((0xd20 + 0x01cb) * 4)
#define REG_CP_MEC_CNTL      GC_SEG1(0x2904)          /* HALT [30], PIPE0_ACTIVE [26] */
#define REG_CP_MEC_PC_START  GC_SEG1(0x2900)
#define REG_CP_PQ_STATUS     GC_SEG0(0x1e58)          /* DOORBELL_ENABLE [1] */
#define REG_CP_HQD_ACTIVE    GC_SEG0(0x1fab)
#define REG_CP_HQD_VMID      GC_SEG0(0x1fac)
#define REG_CP_HQD_PQ_BASE   GC_SEG0(0x1fb1)          /* MC >> 8 */
#define REG_CP_HQD_PQ_BASE_HI GC_SEG0(0x1fb2)
#define REG_CP_HQD_PQ_RPTR   GC_SEG0(0x1fb3)          /* dwords */
#define REG_CP_HQD_RPTR_REP  GC_SEG0(0x1fb4)
#define REG_CP_HQD_RPTR_REP_HI GC_SEG0(0x1fb5)
#define REG_CP_HQD_DOORBELL  GC_SEG0(0x1fb8)          /* OFFSET [27:2], EN [30] */
#define REG_CP_HQD_PQ_CNTL   GC_SEG0(0x1fba)          /* QUEUE_SIZE [5:0] */
#define REG_CP_HQD_WPTR_LO   GC_SEG0(0x1fdf)
#define REG_CP_HQD_WPTR_HI   GC_SEG0(0x1fe0)

/*
 * Compute dispatch. The model has no shader cores; it runs each work-item
 * of a DISPATCH_DIRECT through a small GFX12 interpreter that knows the
 * instructions of the kext's test kernels (shaders/probe.s, and clang's
 * code for shaders/vadd.cl and shaders/bench.cl): SOPP (endpgm, branches,
 * nop/clause/delay/wait/sendmsg, barrier wait), SOP1 s_mov_b32 /
 * s_barrier_signal, SOPC unsigned compares, SMEM s_load_b32..b512, SOP2
 * s_add_co / s_lshl_b32, VOP1 moves/conversions, VOP2 integer/f32 ops,
 * VOPC integer/f32 compares, VOP3 integer/f32 ops, VOPD (dual issue:
 * fmac/fmaak/fmamk/mul/add/sub/mov and the Y-only add/lshl/and), DS b32/b128 loads/stores incl. 2addr, VGLOBAL
 * global_load/store_b32..b128 and wb/inv, and the wave-wide
 * v_wmma_f32_16x16x16_f16/bf16. Work-groups share LDS and meet at
 * barriers. Anything else stops the work-item and is reported.
 */
#define REG_SH_MEM_CONFIG    GC_SEG1(0x09e4)
#define REG_CS_NUM_THREAD_X  GC_SEG0(0x1ba7)
#define REG_CS_PGM_LO        GC_SEG0(0x1bac)
#define REG_CS_PGM_HI        GC_SEG0(0x1bad)
#define REG_CS_RSRC2         GC_SEG0(0x1bb3)
#define REG_CS_THREAD_SE0    GC_SEG0(0x1bb6)
#define REG_CS_USER_DATA_0   GC_SEG0(0x1be0)

typedef struct RDNA4Lane {
    uint32_t s[128];
    uint32_t v[256];
    uint64_t pc;
    uint32_t steps;
    bool     done, waiting;         /* ended / parked at a work-group barrier */
    bool     at_wave;               /* parked at a wave-wide instruction (WMMA) at pc */
} RDNA4Lane;

enum { ISA_ERROR, ISA_DONE, ISA_BARRIER, ISA_WAVEOP };

/* The work-group's LDS, sized by COMPUTE_PGM_RSRC2.LDS_SIZE (512-byte units). */
typedef struct RDNA4Lds {
    uint8_t  *mem;
    uint32_t size;
} RDNA4Lds;

struct RDNA4Dispatch {
    uint32_t dim_x, dim_y, dim_z;
    uint32_t tx, ty, tz, items, nuser, rsrc2;
    uint32_t vmid, initiator;
    uint64_t pgm;
    uint32_t gx, gy, gz;
    uint64_t groups_done, groups_total, ran;
    uint32_t user[16];
    RDNA4Lane *lanes;
    RDNA4Lds lds;
};

static uint32_t rdna4_isa_src(const RDNA4Lane *l, uint32_t src, uint32_t literal, bool *used_lit)
{
    if (src <= 106 || (src >= 108 && src <= 123)) {     /* SGPRs, VCC_LO, TTMP0-15 */
        return l->s[src];
    }
    if (src == 126) {                                   /* EXEC_LO: this lane is on */
        return l->s[126];
    }
    if (src >= 128 && src <= 192) {
        return src - 128;
    }
    if (src >= 193 && src <= 208) {
        return (uint32_t)-(int32_t)(src - 192);
    }
    if (src == 255) {
        *used_lit = true;
        return literal;
    }
    if (src >= 256) {
        return l->v[(src - 256) & 255];
    }
    return 0;
}

static uint64_t rdna4_isa_v64(const RDNA4Lane *l, uint32_t src)
{
    uint32_t r = (src - 256) & 255;
    return l->v[r] | ((uint64_t)l->v[(r + 1) & 255] << 32);
}

static void rdna4_isa_set_v64(RDNA4Lane *l, uint32_t vdst, uint64_t v)
{
    l->v[vdst & 255] = (uint32_t)v;
    l->v[(vdst + 1) & 255] = (uint32_t)(v >> 32);
}

static float rdna4_f(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static uint32_t rdna4_u(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/* Inline constants are decoded by the ALU according to the operand type:
 * integer VALU ops see 0..64/-1..-16, while FP VALU ops see their IEEE-754
 * bit patterns (including the 0.5/1/2/4 constants used by mandelbrot). */
static uint32_t rdna4_isa_float_src(const RDNA4Lane *l, uint32_t src,
                                     uint32_t literal, bool *used_lit)
{
    static const float fc[8] = { 0.5f, -0.5f, 1.0f, -1.0f,
                                 2.0f, -2.0f, 4.0f, -4.0f };

    if (src >= 128 && src <= 192)
        return rdna4_u((float)(src - 128));
    if (src >= 193 && src <= 208)
        return rdna4_u((float)-(int32_t)(src - 192));
    if (src >= 240 && src <= 247)
        return rdna4_u(fc[src - 240]);
    return rdna4_isa_src(l, src, literal, used_lit);
}

/* The VOPD (dual-issue) operations clang uses, X and Y (Y-only from 16). */
static bool rdna4_vopd_op(uint32_t op, uint32_t a, uint32_t b, uint32_t d, uint32_t lit,
                          uint32_t *out)
{
    switch (op) {
    case 0:  *out = rdna4_u(fmaf(rdna4_f(a), rdna4_f(b), rdna4_f(d))); return true;  /* fmac_f32 */
    case 1:  *out = rdna4_u(fmaf(rdna4_f(a), rdna4_f(b), rdna4_f(lit))); return true; /* fmaak */
    case 2:  *out = rdna4_u(fmaf(rdna4_f(a), rdna4_f(lit), rdna4_f(b))); return true; /* fmamk */
    case 3:  *out = rdna4_u(rdna4_f(a) * rdna4_f(b)); return true;                   /* mul_f32 */
    case 4:  *out = rdna4_u(rdna4_f(a) + rdna4_f(b)); return true;                   /* add_f32 */
    case 5:  *out = rdna4_u(rdna4_f(a) - rdna4_f(b)); return true;                   /* sub_f32 */
    case 8:  *out = a; return true;                                                  /* mov_b32 */
    case 16: *out = a + b; return true;                                              /* add_nc_u32 */
    case 17: *out = b << (a & 31); return true;                                      /* lshlrev_b32 */
    case 18: *out = a & b; return true;                                              /* and_b32 */
    default: return false;
    }
}

/*
 * Run one work-item from l->pc until it ends (ISA_DONE), reaches a
 * work-group barrier (ISA_BARRIER, pc past the signal), or meets anything
 * unknown (ISA_ERROR, reported). Per work-item, so VCC is one bit
 * (lane-local carry) kept in s[106], and SCC is scalar state of the lane.
 * This lane-local EXEC/SCC/VCC model is for the per-work-item interpreter;
 * wave-wide mask operations are not modelled, while gfxemu uses lockstep.
 */
static int rdna4_isa_run(RDNA4State *s, RDNA4Lane *l, RDNA4Lds *lds, uint32_t vmid)
{
    uint64_t pc = l->pc;
    uint32_t scc = l->s[127];               /* s[127] is not an addressable SGPR here */

    for (; l->steps < (1u << 22); l->steps++) {
        uint8_t *p = rdna4_gc_span_vmid(s, pc, 12, vmid, false, true);
        if (!p) {
            fprintf(stderr, "rdna4: cs: instruction fetch at 0x%" PRIx64 " not mapped\n", pc);
            return ISA_ERROR;
        }
        uint32_t dw = ldl_le_p(p), dw1 = ldl_le_p(p + 4), dw2 = ldl_le_p(p + 8);
        bool lit = false;
        uint32_t n = 1;

        if ((dw >> 23) == 0x17f) {                          /* SOPP */
            uint32_t op = (dw >> 16) & 0x7f;
            int32_t simm = (int16_t)(dw & 0xffff);
            if (op == 48) {
                l->done = true;
                return ISA_DONE;                            /* s_endpgm */
            }
            if (op == 32 || (op == 33 && !scc) || (op == 34 && scc) ||
                (op == 35 && !(l->s[106] & 1)) || (op == 36 && (l->s[106] & 1)) ||
                (op == 37 && !l->s[126]) || (op == 38 && l->s[126])) {
                pc += 4 + 4ll * simm;           /* s_branch, s_cbranch_scc0/1, _vccz/nz */
                continue;
            }
            /* s_nop, s_clause, s_delay_alu, s_barrier_wait (the signal is
             * the rendezvous), s_code_end, s_sendmsg, s_wait_*: nothing to
             * do for one in-order work-item. */
            if (op != 0 && op != 5 && op != 7 && op != 20 && op != 0x1f && op != 0x36 &&
                (op < 33 || op > 38) && (op < 0x40 || op > 0x49)) {
                goto unknown;
            }
        } else if ((dw >> 23) == 0x17d) {                   /* SOP1 */
            uint32_t op = (dw >> 8) & 0xff, sdst = (dw >> 16) & 0x7f;
            uint32_t a = rdna4_isa_src(l, dw & 0xff, dw1, &lit);
            if (op == 0x4e) {                               /* s_barrier_signal */
                l->pc = pc + 4;
                l->s[127] = scc;
                return ISA_BARRIER;
            }
            if (op == 0x20) {                               /* s_and_saveexec_b32 */
                uint32_t old = l->s[126];
                if (sdst <= 127)
                    l->s[sdst] = old;
                l->s[126] = old & a;
                scc = l->s[126] != 0;
            } else if (op == 0x30) {                        /* s_and_not1_saveexec_b32 */
                uint32_t old = l->s[126];
                if (sdst <= 127)
                    l->s[sdst] = old;
                l->s[126] = a & ~old & 1u;                  /* EXEC stays lane-local */
                scc = l->s[126] != 0;
            } else if (op != 0 || sdst >= 106) {             /* s_mov_b32 */
                goto unknown;
            } else {
                l->s[sdst] = a;
            }
        } else if ((dw >> 23) == 0x17e) {                   /* SOPC */
            uint32_t op = (dw >> 16) & 0x7f;
            uint32_t a = rdna4_isa_src(l, dw & 0xff, dw1, &lit);
            uint32_t b = rdna4_isa_src(l, (dw >> 8) & 0xff, dw1, &lit);
            switch (op) {
            case 6:  scc = a == b; break;                   /* s_cmp_eq_u32 */
            case 7:  scc = a != b; break;                   /* s_cmp_lg_u32 */
            case 8:  scc = a > b; break;                    /* s_cmp_gt_u32 */
            case 9:  scc = a >= b; break;                   /* s_cmp_ge_u32 */
            case 10: scc = a < b; break;                    /* s_cmp_lt_u32 */
            case 11: scc = a <= b; break;                   /* s_cmp_le_u32 */
            default: goto unknown;
            }
        } else if ((dw & 0xfe000000u) == 0x7c000000u) {     /* VOPC E32 */
            uint32_t op = (dw >> 17) & 0xff;
            uint32_t a = rdna4_isa_src(l, dw & 0x1ff, dw1, &lit);
            uint32_t b = rdna4_isa_src(l, ((dw >> 9) & 0xff) + 256, dw1, &lit);
            bool result;

            if (l->s[126]) {
                switch (op) {
                case 0x4c: result = a > b; break;           /* v_cmp_gt_u32 */
                case 0xcc: result = a > b; break;           /* v_cmpx_gt_u32 */
                case 0x1e:
                case 0x9e:
                    a = rdna4_isa_float_src(l, dw & 0x1ff, dw1, &lit);
                    b = rdna4_isa_float_src(l, ((dw >> 9) & 0xff) + 256, dw1, &lit);
                    result = !(rdna4_f(a) < rdna4_f(b));
                    break;                                  /* v_cmp_nlt_f32 */
                default: goto unknown;
                }
                if (op & 0x80) {                            /* v_cmpx_*: gfx10+ writes EXEC only */
                    l->s[126] = result;
                } else {
                    l->s[106] = result;
                }
            } else {
                if (!(op & 0x80))                            /* inactive E32 VCC lane */
                    l->s[106] = 0;
            }
        } else if ((dw & 0xffff0000u) == 0xd44c0000u) {     /* v_cmp_gt_u32_e64 */
            uint32_t sdst = dw & 0xff;
            uint32_t a = rdna4_isa_src(l, dw1 & 0x1ff, dw2, &lit);
            uint32_t b = rdna4_isa_src(l, ((dw1 >> 9) & 0xff) + 256, dw2, &lit);
            if (sdst > 106)
                goto unknown;
            l->s[sdst] = l->s[126] ? (a > b) : 0;
            n = 2;
        } else if ((dw >> 26) == 0x3d) {                    /* SMEM loads */
            uint32_t op = (dw >> 13) & 0x3f, sbase = (dw & 0x3f) * 2, sdata = (dw >> 6) & 0x7f;
            uint32_t soff = dw1 >> 25, count = op <= 4 ? 1u << op : op == 5 ? 3 : 0;
            int64_t off = ((int32_t)(dw1 << 8)) >> 8;       /* signed 24-bit */
            uint64_t addr = (l->s[sbase] | ((uint64_t)l->s[sbase + 1] << 32)) + off +
                            (soff != 0x7c ? l->s[soff & 0x7f] : 0);
            uint8_t io[64];
            if (!count || sdata + count > 106 ||
                !rdna4_vm_access(s, addr, io, 4 * count, vmid, false, false)) {
                goto unknown;
            }
            for (uint32_t i = 0; i < count; i++) {
                l->s[sdata + i] = ldl_le_p(io + 4 * i);
            }
            n = 2;
        } else if ((dw >> 26) == 0x36) {                    /* DS (LDS) */
            uint32_t op = (dw >> 18) & 0xff, off0 = dw & 0xff, off1 = (dw >> 8) & 0xff;
            uint32_t addr = l->v[dw1 & 0xff], data0 = (dw1 >> 8) & 0xff;
            uint32_t data1 = (dw1 >> 16) & 0xff, vdst = dw1 >> 24;
            uint32_t at[2] = { addr + (off1 << 8 | off0), 0 }, count = 1;
            uint32_t bytes = op == 223 || op == 255 ? 16 : 4;
            if (op == 14 || op == 55) {                     /* 2addr: two dword slots */
                at[0] = addr + off0 * 4;
                at[1] = addr + off1 * 4;
                count = 2;
            }
            for (uint32_t i = 0; i < count; i++) {
                if (at[i] + bytes > lds->size) {
                    fprintf(stderr, "rdna4: cs: LDS access at 0x%x beyond the work-group's %u "
                            "bytes (COMPUTE_PGM_RSRC2.LDS_SIZE)\n", at[i], lds->size);
                    return ISA_ERROR;
                }
            }
            if (l->s[126]) switch (op) {
            case 13: stl_le_p(lds->mem + at[0], l->v[data0]); break;           /* ds_store_b32 */
            case 14:                                                           /* ds_store_2addr_b32 */
                stl_le_p(lds->mem + at[0], l->v[data0]);
                stl_le_p(lds->mem + at[1], l->v[data1]);
                break;
            case 54: l->v[vdst] = ldl_le_p(lds->mem + at[0]); break;           /* ds_load_b32 */
            case 55:                                                           /* ds_load_2addr_b32 */
                l->v[vdst] = ldl_le_p(lds->mem + at[0]);
                l->v[(vdst + 1) & 255] = ldl_le_p(lds->mem + at[1]);
                break;
            case 223:                                                          /* ds_store_b128 */
                for (uint32_t i = 0; i < 4; i++) {
                    stl_le_p(lds->mem + at[0] + 4 * i, l->v[(data0 + i) & 255]);
                }
                break;
            case 255:                                                          /* ds_load_b128 */
                for (uint32_t i = 0; i < 4; i++) {
                    l->v[(vdst + i) & 255] = ldl_le_p(lds->mem + at[0] + 4 * i);
                }
                break;
            default: goto unknown;
            }
            n = 2;
        } else if ((dw >> 24) == 0xee) {                    /* VGLOBAL */
            uint32_t op = (dw >> 14) & 0xff, saddr = dw & 0x7f;
            uint32_t vdst = dw1 & 0xff, data = (dw1 >> 23) & 0xff, vaddr = dw2 & 0xff;
            int64_t ioff = ((int32_t)(dw2 & 0xffffff00)) >> 8;
            uint32_t count = op >= 20 && op <= 23 ? op - 19 : op >= 26 && op <= 29 ? op - 25 : 0;
            uint64_t addr;
            uint8_t io[16];
            n = 3;
            if (op == 43 || op == 44) {                     /* global_inv / global_wb */
                pc += 4ull * n;
                continue;
            }
            if (!count) {                                   /* global_load/store_b32..b128 */
                goto unknown;
            }
            if (!l->s[126]) {
                pc += 4ull * n;
                continue;
            }
            addr = saddr != 0x7c ?
                   (l->s[saddr] | ((uint64_t)l->s[saddr + 1] << 32)) + l->v[vaddr] :
                   l->v[vaddr] | ((uint64_t)l->v[(vaddr + 1) & 255] << 32);
            addr += ioff;
            if (op >= 26) {
                for (uint32_t i = 0; i < count; i++)
                    stl_le_p(io + 4 * i, l->v[(data + i) & 255]);
            }
            if (!rdna4_vm_access(s, addr, io, 4 * count, vmid, op >= 26, false)) {
                fprintf(stderr, "rdna4: cs: global access to 0x%" PRIx64 " not mapped\n", addr);
                return ISA_ERROR;
            }
            if (op < 26) {
                for (uint32_t i = 0; i < count; i++)
                    l->v[(vdst + i) & 255] = ldl_le_p(io + 4 * i);
            }
        } else if ((dw >> 26) == 0x32) {                    /* VOPD: two ops, one issue */
            uint32_t opx = (dw >> 22) & 0xf, opy = (dw >> 17) & 0x1f;
            uint32_t vdstx = dw1 >> 24, vdsty = (((dw1 >> 17) & 0x7f) << 1) | ((vdstx & 1) ^ 1);
            uint32_t ax = rdna4_isa_src(l, dw & 0x1ff, dw2, &lit), bx = l->v[(dw >> 9) & 0xff];
            uint32_t ay = rdna4_isa_src(l, dw1 & 0x1ff, dw2, &lit), by = l->v[(dw1 >> 9) & 0xff];
            uint32_t rx, ry;
            n = 2;
            if (l->s[126]) {
                if (opx <= 5) {
                    ax = rdna4_isa_float_src(l, dw & 0x1ff, dw2, &lit);
                    bx = rdna4_isa_float_src(l, ((dw >> 9) & 0xff) + 256, dw2, &lit);
                }
                if (opy <= 5) {
                    ay = rdna4_isa_float_src(l, dw1 & 0x1ff, dw2, &lit);
                    by = rdna4_isa_float_src(l, ((dw1 >> 9) & 0xff) + 256, dw2, &lit);
                }
                /* Both read their sources before either writes (no X/Y overlap). */
                if (!rdna4_vopd_op(opx, ax, bx, l->v[vdstx], dw2, &rx) ||
                    !rdna4_vopd_op(opy, ay, by, l->v[vdsty & 255], dw2, &ry)) {
                    goto unknown;
                }
                if (opx == 1 || opx == 2 || opy == 1 || opy == 2) {
                    lit = true;                             /* fmaak/fmamk carry one */
                }
                l->v[vdstx] = rx;
                l->v[vdsty & 255] = ry;
            }
        } else if ((dw >> 23) == 0x198 && ((dw >> 16) & 0x7f) >= 64 &&
                   ((dw >> 16) & 0x7f) <= 65) {             /* VOP3P WMMA: wave-wide */
            l->pc = pc;
            l->s[127] = scc;
            return ISA_WAVEOP;
        } else if ((dw >> 26) == 0x35) {                    /* VOP3 / VOP3B */
            uint32_t op = (dw >> 16) & 0x3ff, vdst = dw & 0xff, sdst = (dw >> 8) & 0x7f;
            uint32_t s0 = dw1 & 0x1ff, s1 = (dw1 >> 9) & 0x1ff, s2 = (dw1 >> 18) & 0x1ff;
            uint32_t a = rdna4_isa_src(l, s0, dw2, &lit), b = rdna4_isa_src(l, s1, dw2, &lit);
            uint32_t c = rdna4_isa_src(l, s2, dw2, &lit);
            n = 2;
            /* No neg, abs, opsel or clamp in these kernels; VOP3B (the two
             * carry-out ops) keeps SDST where the others keep abs/opsel. */
            if (dw1 >> 29 || (op != 0x300 && op != 0x2fe && op != 0x120 && (dw & 0xff00))) {
                goto unknown;
            }
            if (l->s[126]) switch (op) {
            case 0x213:
                a = rdna4_isa_float_src(l, s0, dw2, &lit);
                b = rdna4_isa_float_src(l, s1, dw2, &lit);
                c = rdna4_isa_float_src(l, s2, dw2, &lit);
                l->v[vdst] = rdna4_u(fmaf(rdna4_f(a), rdna4_f(b), rdna4_f(c)));
                break;                                                        /* v_fma_f32 */
            case 0x256: l->v[vdst] = (a << (b & 31)) | c; break;              /* v_lshl_or_b32 */
            case 0x246: l->v[vdst] = (a << (b & 31)) + c; break;              /* v_lshl_add_u32 */
            case 0x255: l->v[vdst] = a + b + c; break;                        /* v_add3_u32 */
            case 0x247: l->v[vdst] = (a + b) << (c & 31); break;              /* v_add_lshl_u32 */
            case 0x258: l->v[vdst] = a | b | c; break;                        /* v_or3_b32 */
            case 0x210:                                                       /* v_bfe_u32 */
                l->v[vdst] = (c & 31) ? (a >> (b & 31)) & ((1u << (c & 31)) - 1) : 0;
                break;
            case 0x20b: l->v[vdst] = (a & 0xffffff) * (b & 0xffffff) + c; break; /* v_mad_u32_u24 */
            case 0x32c: l->v[vdst] = a * b; break;                            /* v_mul_lo_u32 */
            case 0x300: {                                                     /* v_add_co_u32 */
                uint64_t sum = (uint64_t)a + b;
                l->v[vdst] = (uint32_t)sum;
                if (sdst <= 106) {
                    l->s[sdst] = (uint32_t)(sum >> 32);
                }
                break;
            }
            case 0x120: {                                                     /* v_add_co_ci_u32 */
                uint64_t sum = (uint64_t)a + b + (c & 1);
                l->v[vdst] = (uint32_t)sum;
                if (sdst <= 106) {
                    l->s[sdst] = (uint32_t)(sum >> 32);
                }
                break;
            }
            case 0x2fe: {                                                     /* v_mad_co_u64_u32 */
                uint64_t c64 = s2 >= 256 ? rdna4_isa_v64(l, s2) : c;
                rdna4_isa_set_v64(l, vdst, (uint64_t)a * b + c64);
                break;
            }
            default:
                goto unknown;
            }
        } else if ((dw >> 25) == 0x3f) {                    /* VOP1 */
            uint32_t op = (dw >> 9) & 0xff, vdst = (dw >> 17) & 0xff;
            uint32_t a = rdna4_isa_src(l, dw & 0x1ff, dw1, &lit);
            if (!l->s[126]) {
                /* Vector instructions are masked by EXEC; scalar mask
                 * bookkeeping above still runs for an inactive lane. */
            } else if (op == 1) {                           /* v_mov_b32 */
                l->v[vdst] = a;
            } else if (op == 6) {                            /* v_cvt_f32_u32 */
                l->v[vdst] = rdna4_u((float)a);
            } else if (op == 7) {                            /* v_cvt_u32_f32 */
                float f = rdna4_f(a);
                l->v[vdst] = isnan(f) || f <= 0.0f ? 0 :
                              f >= 4294967295.0f ? UINT32_MAX : (uint32_t)f;
            } else {
                goto unknown;
            }
        } else if ((dw >> 30) == 2 && (dw >> 28) != 0xb && (dw >> 23) < 0x17d) {   /* SOP2 */
            uint32_t op = (dw >> 23) & 0x7f, sdst = (dw >> 16) & 0x7f;
            uint32_t a = rdna4_isa_src(l, dw & 0xff, dw1, &lit);
            uint32_t b = rdna4_isa_src(l, (dw >> 8) & 0xff, dw1, &lit);
            uint32_t r;
            if (sdst > 127) {
                goto unknown;
            }
            switch (op) {
            case 0: {                                                          /* s_add_co_u32 */
                uint64_t sum = (uint64_t)a + b;
                r = (uint32_t)sum;
                scc = sum >> 32;
                break;
            }
            case 2: {                                                          /* s_add_co_i32 */
                int64_t sum = (int64_t)(int32_t)a + (int32_t)b;
                r = (uint32_t)sum;
                scc = sum != (int32_t)r;
                break;
            }
            case 8: r = a << (b & 31); scc = r != 0; break;                    /* s_lshl_b32 */
            case 44: r = a * b; break;                                         /* s_mul_i32 */
            case 22: r = a & b; scc = r != 0; break;                           /* s_and_b32 */
            case 24: r = a | b; scc = r != 0; break;                           /* s_or_b32 */
            case 26: r = a ^ b; scc = r != 0; break;                           /* s_xor_b32 */
            case 34: r = a & ~b; scc = r != 0; break;                          /* s_and_not1_b32 */
            case 36: r = a | ~b; scc = r != 0; break;                          /* s_or_not1_b32 */
            case 21: r = a > b ? a : b; scc = a > b; break;                      /* s_max_u32 */
            case 48: r = scc ? a : b; break;                                   /* s_cselect_b32 */
            default: goto unknown;
            }
            /* Each interpreter lane represents one wave bit (bit 0).  Keep
             * EXEC's scalar mask lane-local when a SOP2 writes exec_lo;
             * preserving a complement such as 0xfffffffe would make a
             * zero-bit lane look active to the boolean checks below. */
            l->s[sdst] = sdst == 126 ? r & 1u : r;
        } else if (!(dw >> 31)) {                           /* VOP2 */
            uint32_t op = (dw >> 25) & 0x3f, vdst = (dw >> 17) & 0xff;
            uint32_t a = rdna4_isa_src(l, dw & 0x1ff, dw1, &lit);
            uint32_t b = l->v[(dw >> 9) & 0xff];
            uint32_t vsrc1 = ((dw >> 9) & 0xff) + 256;
            if (op == 3 || op == 4 || op == 8 || op == 43 || op == 45) {
                a = rdna4_isa_float_src(l, dw & 0x1ff, dw1, &lit);
                b = rdna4_isa_float_src(l, ((dw >> 9) & 0xff) + 256, dw1, &lit);
            }
            if (l->s[126]) switch (op) {
            case 3:  l->v[vdst] = rdna4_u(rdna4_f(a) + rdna4_f(b)); break;       /* v_add_f32 */
            case 4:  l->v[vdst] = rdna4_u(rdna4_f(a) - rdna4_f(b)); break;       /* v_sub_f32 */
            case 8:  l->v[vdst] = rdna4_u(rdna4_f(a) * rdna4_f(b)); break;       /* v_mul_f32 */
            case 37: l->v[vdst] = a + b; break;                                /* v_add_nc_u32 */
            case 24: l->v[vdst] = b << (a & 31); break;                        /* v_lshlrev_b32 */
            case 25: l->v[vdst] = b >> (a & 31); break;                        /* v_lshrrev_b32 */
            case 27: l->v[vdst] = a & b; break;                                /* v_and_b32 */
            case 28: l->v[vdst] = a | b; break;                                /* v_or_b32 */
            case 29: l->v[vdst] = a ^ b; break;                                /* v_xor_b32 */
            case 11: l->v[vdst] = (a & 0xffffff) * (b & 0xffffff); break;      /* v_mul_u32_u24 */
            case 43:                                                           /* v_fmac_f32 */
                l->v[vdst] = rdna4_u(fmaf(rdna4_f(a), rdna4_f(b), rdna4_f(l->v[vdst])));
                break;
            case 45:                                                           /* v_fmaak_f32 */
                l->v[vdst] = rdna4_u(fmaf(rdna4_f(a), rdna4_f(b), rdna4_f(dw1)));
                lit = true;
                break;
            case 31:                                                           /* v_lshlrev_b64 */
                rdna4_isa_set_v64(l, vdst, rdna4_isa_v64(l, vsrc1) << (a & 63));
                break;
            case 32: {                                                         /* v_add_co_ci_u32 */
                uint64_t sum = (uint64_t)a + b + (l->s[106] & 1);
                l->v[vdst] = (uint32_t)sum;
                l->s[106] = (uint32_t)(sum >> 32);
                break;
            }
            default: goto unknown;
            }
        } else {
            goto unknown;
        }
        pc += 4ull * (n + (lit ? 1 : 0));
        continue;
unknown:
        fprintf(stderr, "rdna4: cs: unsupported instruction 0x%08x 0x%08x at 0x%" PRIx64 "\n",
                dw, dw1, pc);
        return ISA_ERROR;
    }
    fprintf(stderr, "rdna4: cs: work-item ran past %u instructions\n", 1u << 22);
    return ISA_ERROR;
}

/* IEEE half to float (for WMMA's f16 inputs). */
static float rdna4_half(uint16_t h)
{
    uint32_t sign = (uint32_t)(h >> 15) << 31, exp = (h >> 10) & 0x1f, man = h & 0x3ff, u;

    if (exp == 0x1f) {
        u = sign | 0x7f800000u | (man << 13);                /* inf / nan */
    } else if (exp) {
        u = sign | ((exp + 112) << 23) | (man << 13);        /* normal */
    } else if (man) {                                        /* subnormal: renormalise */
        exp = 113;
        while (!(man & 0x400)) {
            man <<= 1;
            exp--;
        }
        u = sign | (exp << 23) | ((man & 0x3ff) << 13);
    } else {
        u = sign;
    }
    return rdna4_f(u);
}

/*
 * The gfx12 (RDNA4) wave32 layout of a 16x16x16 WMMA, element i (0..7) of
 * lane l (0..31): A holds row l % 16, k (l / 16) * 8 + i; B holds column
 * l % 16, the same k; C/D hold row (l / 16) * 8 + i, column l % 16 (RDNA4
 * ISA 7.12.2, GPUOpen's WMMA guide). A and B pack two 16-bit values per
 * VGPR, low half first. The ISA numbers k differently for 16-bit A/B, but
 * the same way for both, so the sums are the same.
 */
static void rdna4_wmma_ab(unsigned l, unsigned i, unsigned *rc, unsigned *k)
{
    *rc = l % 16;
    *k = (l / 16) * 8 + i;
}

static void rdna4_wmma_cd(unsigned l, unsigned i, unsigned *row, unsigned *col)
{
    *row = (l / 16) * 8 + i;
    *col = l % 16;
}

/* An inline constant as WMMA's C: the integers, and the float constants. */
static bool rdna4_wmma_const(uint32_t src, float *out)
{
    static const float fc[8] = { 0.5f, -0.5f, 1.0f, -1.0f, 2.0f, -2.0f, 4.0f, -4.0f };

    if (src >= 128 && src <= 208) {
        *out = rdna4_f(src <= 192 ? src - 128 : (uint32_t)-(int32_t)(src - 192));
        return true;
    }
    if (src >= 240 && src <= 247) {
        *out = fc[src - 240];
        return true;
    }
    return false;
}

/*
 * V_WMMA_F32_16X16X16_F16 / _BF16 (VOP3P op 64 / 65) for the 32 lanes of
 * one wave, all parked at the same pc: D = A x B + C, then each lane
 * resumes past it (EXEC does not apply to WMMA). NEG / NEG_HI negate the
 * low / high 16-bit values of A and B; on C they are NEG and ABS. C may be
 * an inline constant, the same for every element. false (and a message) on
 * anything unexpected.
 */
static bool rdna4_wave_wmma(RDNA4State *s, RDNA4Lane *w, unsigned n, uint32_t vmid)
{
    uint8_t *p = rdna4_gc_span_vmid(s, w[0].pc, 8, vmid, false, true);
    uint32_t dw, dw1, op, vdst, s0, s1, s2, neg, neg_hi;
    float a[16][16], b[16][16], c[16][16], kc = 0;

    if (!p || n != 32) {
        fprintf(stderr, "rdna4: cs: WMMA needs a full wave of 32 (have %u)\n", n);
        return false;
    }
    dw = ldl_le_p(p);
    dw1 = ldl_le_p(p + 4);
    op = (dw >> 16) & 0x7f;
    vdst = dw & 0xff;
    s0 = dw1 & 0x1ff;
    s1 = (dw1 >> 9) & 0x1ff;
    s2 = (dw1 >> 18) & 0x1ff;
    neg = dw1 >> 29;
    neg_hi = (dw >> 8) & 7;
    if (s0 < 256 || s1 < 256 || (s2 < 256 && !rdna4_wmma_const(s2, &kc)) || (dw & 0x8000)) {
        fprintf(stderr, "rdna4: cs: WMMA operands 0x%08x 0x%08x not modelled\n", dw, dw1);
        return false;
    }
    for (unsigned l = 0; l < 32; l++) {
        if (w[l].pc != w[0].pc) {
            fprintf(stderr, "rdna4: cs: WMMA lanes diverged\n");
            return false;
        }
        for (unsigned i = 0; i < 8; i++) {
            unsigned rc, k, row, col;
            uint32_t va = w[l].v[(s0 - 256 + i / 2) & 255], vb = w[l].v[(s1 - 256 + i / 2) & 255];
            uint16_t ha = (uint16_t)(va >> (16 * (i & 1))), hb = (uint16_t)(vb >> (16 * (i & 1)));
            uint32_t half_neg = (i & 1) ? neg_hi : neg;
            float cv = s2 < 256 ? kc : rdna4_f(w[l].v[(s2 - 256 + i) & 255]);
            rdna4_wmma_ab(l, i, &rc, &k);
            a[rc][k] = op == 64 ? rdna4_half(ha) : rdna4_f((uint32_t)ha << 16);
            b[k][rc] = op == 64 ? rdna4_half(hb) : rdna4_f((uint32_t)hb << 16);
            a[rc][k] = (half_neg & 1) ? -a[rc][k] : a[rc][k];
            b[k][rc] = (half_neg & 2) ? -b[k][rc] : b[k][rc];
            cv = (neg_hi & 4) ? fabsf(cv) : cv;
            rdna4_wmma_cd(l, i, &row, &col);
            c[row][col] = (neg & 4) ? -cv : cv;
        }
    }
    for (unsigned l = 0; l < 32; l++) {
        for (unsigned i = 0; i < 8; i++) {
            unsigned row, col;
            float d;
            rdna4_wmma_cd(l, i, &row, &col);
            d = c[row][col];
            for (unsigned k = 0; k < 16; k++) {
                d = fmaf(a[row][k], b[k][col], d);
            }
            w[l].v[(vdst + i) & 255] = rdna4_u(d);
        }
        w[l].pc += 8;
        w[l].at_wave = false;
    }
    return true;
}

enum { RDNA4_DISPATCH_DONE, RDNA4_DISPATCH_MORE, RDNA4_DISPATCH_HUNG };

static void rdna4_dispatch_free(RDNA4Dispatch *d)
{
    if (!d)
        return;
    g_free(d->lds.mem);
    g_free(d->lanes);
    g_free(d);
}

static bool rdna4_dispatch_begin(RDNA4State *s, RDNA4Dispatch *d,
                                 uint32_t dim_x, uint32_t dim_y, uint32_t dim_z,
                                 uint32_t initiator, uint32_t vmid)
{
    d->dim_x = dim_x;
    d->dim_y = dim_y;
    d->dim_z = dim_z;
    d->initiator = initiator;
    d->vmid = vmid;
    d->pgm = ((uint64_t)reg_get(s, REG_CS_PGM_LO) << 8) |
             ((uint64_t)reg_get(s, REG_CS_PGM_HI) << 40);
    d->rsrc2 = reg_get(s, REG_CS_RSRC2);
    d->nuser = (d->rsrc2 >> 1) & 0x1f;
    d->tx = reg_get(s, REG_CS_NUM_THREAD_X);
    d->ty = reg_get(s, REG_CS_NUM_THREAD_X + 4);
    d->tz = reg_get(s, REG_CS_NUM_THREAD_X + 8);
    d->items = d->tx * d->ty * d->tz;
    d->lds.size = ((d->rsrc2 >> 15) & 0x1ff) * 512;
    d->groups_total = (uint64_t)dim_x * dim_y * dim_z;

    if (!(initiator & 1) || !reg_get(s, REG_SH_MEM_CONFIG) ||
        !reg_get(s, REG_CS_THREAD_SE0) || !d->tx || !d->ty || !d->tz ||
        (uint64_t)d->items > 1024 || !dim_x || !dim_y || !dim_z) {
        fprintf(stderr, "rdna4: cs: dispatch %ux%ux%u refused (initiator 0x%x, SH_MEM_CONFIG "
                "0x%x, CU mask SE0 0x%x, group %ux%ux%u)\n", dim_x, dim_y, dim_z,
                initiator, reg_get(s, REG_SH_MEM_CONFIG), reg_get(s, REG_CS_THREAD_SE0),
                d->tx, d->ty, d->tz);
        d->groups_total = 0;
        return true;
    }
    for (uint32_t i = 0; i < d->nuser && i < ARRAY_SIZE(d->user); i++)
        d->user[i] = reg_get(s, REG_CS_USER_DATA_0 + 4 * i);
    d->lanes = g_new(RDNA4Lane, d->items);
    d->lds.mem = g_malloc0(d->lds.size ? d->lds.size : 4);
    return false;
}

/* Run one complete work-group. The caller checks the realtime slice budget
 * only between groups, so a group keeps its LDS and wave rendezvous atomic. */
static int rdna4_dispatch_group(RDNA4State *s, RDNA4Dispatch *d)
{
    uint32_t live = d->items;

    memset(d->lds.mem, 0, d->lds.size ? d->lds.size : 4);
    for (uint32_t t = 0; t < d->items; t++) {
        uint32_t x = t % d->tx, y = (t / d->tx) % d->ty, z = t / (d->tx * d->ty);
        RDNA4Lane *l = &d->lanes[t];
        memset(l, 0, sizeof(*l));
        l->s[126] = 1;                                /* EXEC_LO for this lane */
        for (uint32_t i = 0; i < d->nuser && i < ARRAY_SIZE(d->user); i++)
            l->s[i] = d->user[i];
        /* GFX12 architected SGPRs carry the work-group ids in TTMP9/7. */
        l->s[108 + 9] = (d->rsrc2 & (1u << 7)) ? d->gx : 0;
        l->s[108 + 7] = ((d->rsrc2 & (1u << 8)) ? (d->gy & 0xffff) : 0) |
                        ((d->rsrc2 & (1u << 9)) ? (d->gz << 16) : 0);
        l->v[0] = (x & 0x3ff) | ((y & 0x3ff) << 10) | ((z & 0x3ff) << 20);
        l->pc = d->pgm;
    }
    while (live) {
        bool stuck = true;
        for (uint32_t t = 0; t < d->items; t++) {
            RDNA4Lane *l = &d->lanes[t];
            if (l->done || l->waiting || l->at_wave)
                continue;
            stuck = false;
            switch (rdna4_isa_run(s, l, &d->lds, d->vmid)) {
            case ISA_DONE:
                live--;
                d->ran++;
                break;
            case ISA_BARRIER:
                l->waiting = true;
                break;
            case ISA_WAVEOP:
                l->at_wave = true;
                break;
            default:
                /* A faulted work-item leaves the queue busy, as a hang. */
                fprintf(stderr, "rdna4: cs: dispatch hung (%" PRIu64
                        " work-items ran; waves remain queued)\n", d->ran);
                return -1;
            }
        }
        for (uint32_t w0 = 0; w0 < d->items; w0 += 32) {
            uint32_t n = d->items - w0 < 32 ? d->items - w0 : 32, at = 0;
            for (uint32_t i = 0; i < n; i++)
                at += d->lanes[w0 + i].at_wave;
            if (at && at == n) {
                if (!rdna4_wave_wmma(s, &d->lanes[w0], n, d->vmid))
                    return 0;
                stuck = false;
            }
        }
        bool all = true;
        for (uint32_t t = 0; t < d->items && all; t++)
            all = d->lanes[t].done || d->lanes[t].waiting;
        if (all) {
            for (uint32_t t = 0; t < d->items; t++)
                d->lanes[t].waiting = false;
            stuck = false;
        }
        if (stuck && live) {
            fprintf(stderr, "rdna4: cs: work-group deadlocked (barrier vs. wave op)\n");
            return 0;
        }
    }
    return 1;
}

static int rdna4_dispatch_slice(RDNA4State *s, RDNA4Dispatch *d, uint64_t deadline)
{
    while (d->groups_done < d->groups_total) {
        int result = rdna4_dispatch_group(s, d);
        if (result < 0)
            return RDNA4_DISPATCH_HUNG;
        d->groups_done++;
        d->gx++;
        if (d->gx == d->dim_x) {
            d->gx = 0;
            d->gy++;
            if (d->gy == d->dim_y) {
                d->gy = 0;
                d->gz++;
            }
        }
        if (result == 0)
            return RDNA4_DISPATCH_DONE;
        if (d->groups_done < d->groups_total &&
            qemu_clock_get_ns(QEMU_CLOCK_REALTIME) >= deadline) {
            return RDNA4_DISPATCH_MORE;
        }
    }
    /* GPU stores into hidden scanout VRAM do not dirty a QEMU
     * MemoryRegion; publish a completed interpreter dispatch. */
    dpy_gfx_update_full(s->con);
    fprintf(stderr, "rdna4: cs: dispatch %ux%ux%u of %ux%ux%u ran %" PRIu64
            " work-items%s\n", d->dim_x, d->dim_y, d->dim_z, d->tx, d->ty, d->tz,
            d->ran, d->lds.size ? " (with LDS)" : "");
    return RDNA4_DISPATCH_DONE;
}

static bool rdna4_mec_ready(RDNA4State *s, uint32_t db_dword, const char **why)
{
    uint32_t hqd_db = reg_get(s, REG_CP_HQD_DOORBELL);
    uint32_t mec = reg_get(s, REG_CP_MEC_CNTL);

    *why = !s->gfx_booted ? "GFX not booted" :
           s->mec_hung ? "queue has a hung wave; waiting for HQD reset" :
           !(reg_get(s, REG_NBIF_DB_APER_EN) & 1) ? "NBIF doorbell aperture off" :
           reg_get(s, REG_NBIF_S2A_ENTRY0) != 0x30000007 ? "doorbells not routed to GC" :
           !(reg_get(s, REG_CP_PQ_STATUS) & 2) ? "CP_PQ_STATUS.DOORBELL_ENABLE off" :
           !(hqd_db & (1u << 30)) ? "HQD doorbell disabled" :
           ((hqd_db >> 2) & 0x3ffffff) != db_dword ? "doorbell not the queue's" :
           !(reg_get(s, REG_CP_HQD_ACTIVE) & 1) ? "HQD not active" :
           s->kiq_only && !(reg_get(s, GC_SEG1(0x098a)) & 0x80) ?
               "queue is not the KIQ (kiq-only model)" :
           (mec & (1u << 30)) || !(mec & (1u << 26)) ? "MEC halted / pipe 0 inactive" :
           !reg_get(s, REG_CP_MEC_PC_START) ? "MEC entry point not set" : NULL;
    return *why == NULL;
}

/* Process one packet from the client compute IB.  The IB has its own cursor,
 * so a long stream and a dispatch inside it yield through the same MEC
 * bottom-half state machine as packets in the client queue. */
static void rdna4_mec_eop_access(RDNA4State *s, uint32_t vmid);

static bool rdna4_mec_ib_packet(RDNA4State *s)
{
    uint32_t dw[16] = { 0 }, hdr, op, count, len;
    const uint64_t address = s->mec_ib.address;
    const uint32_t pos = s->mec_ib.pos;
    const uint32_t vmid = s->mec_ib.vmid;

    if (!rdna4_vm_access(s, address + 4ull * pos, (uint8_t *)&hdr,
                         sizeof(hdr), vmid, false, false)) {
        fprintf(stderr, "rdna4: mec: indirect buffer 0x%" PRIx64 " header fault\n",
                (uint64_t)(address + 4ull * pos));
        return false;
    }
    dw[0] = hdr;
    op = (hdr >> 8) & 0xff;
    count = (hdr >> 16) & 0x3fff;
    len = count + 2;
    if (count == 0x3fff && op == 0x10)
        len = 1;
    if ((hdr >> 30) != 3 || !len || len > 16 || len > s->mec_ib.dwords - pos) {
        fprintf(stderr, "rdna4: mec: invalid indirect packet 0x%08x at dword %u\n",
                hdr, pos);
        return false;
    }
    if (len > 1 && !rdna4_vm_access(s, address + 4ull * pos + 4,
                                    (uint8_t *)&dw[1], 4ull * (len - 1),
                                    vmid, false, false)) {
        fprintf(stderr, "rdna4: mec: indirect packet at dword %u is unmapped\n", pos);
        return false;
    }

    switch (op) {
    case 0x10:
        break;
    case 0x79:
    case 0x76:
        for (uint32_t i = 0; i < count; i++) {
            uint32_t base = op == 0x79 ? 0xc000 : 0x2c00;
            uint32_t byte = (base + dw[1] + i) * 4;
            if (byte + 4 <= RDNA4_MMIO_SIZE)
                reg_set(s, byte, dw[2 + i]);
        }
        break;
    case 0x58:
        break;
    case 0x15:
        s->dispatch = g_new0(RDNA4Dispatch, 1);
        if (rdna4_dispatch_begin(s, s->dispatch, dw[1], dw[2], dw[3], dw[4], vmid)) {
            rdna4_dispatch_free(s->dispatch);
            s->dispatch = NULL;
        } else {
            s->mec_ib.packet_len = len;
            return true;
        }
        break;
    case 0x37: {
        uint64_t a = (dw[2] & ~3u) | ((uint64_t)dw[3] << 32);
        uint32_t n = count >= 2 ? count - 2 : 0;
        if (count < 2 || n > 14) {
            fprintf(stderr, "rdna4: mec: malformed indirect WRITE_DATA\n");
            return false;
        }
        for (uint32_t i = 0; i < n; i++) {
            if (!rdna4_vm_access(s, a + 4ull * i, (uint8_t *)&dw[4 + i], 4,
                                 vmid, true, false)) {
                fprintf(stderr, "rdna4: mec: indirect WRITE_DATA to 0x%" PRIx64
                        " refused\n", (uint64_t)(a + 4ull * i));
                return false;
            }
        }
        break;
    }
    case 0x49: {
        uint64_t a = (dw[3] & ~3u) | ((uint64_t)dw[4] << 32);
        uint32_t sel = dw[2] >> 29;
        uint64_t value = dw[5] | ((uint64_t)dw[6] << 32);
        uint32_t bytes = sel == 2 ? 8 : 4;
        rdna4_mec_eop_access(s, s->mec_work.vmid);
        if (count < 6 || !rdna4_vm_access(s, a, (uint8_t *)&value, bytes,
                                          vmid, true, false)) {
            fprintf(stderr, "rdna4: mec: indirect RELEASE_MEM to 0x%" PRIx64
                    " refused\n", a);
            return false;
        }
        if (((dw[2] >> 24) & 7) == 2 && s->selected_pipe < 2 &&
            (reg_get(s, REG_CP_ME1_PIPE0_INT_CNTL + 4 * s->selected_pipe) &
             CP_TIME_STAMP_INT_ENABLE)) {
            rdna4_ih_emit(s, 0x14, 181,
                          (uint8_t)((s->selected_queue << 4) | (1u << 2) | s->selected_pipe),
                          dw[5]);
        }
        break;
    }
    case 0x3f:
        fprintf(stderr, "rdna4: mec: nested compute INDIRECT_BUFFER is not allowed\n");
        return false;
    default:
        fprintf(stderr, "rdna4: mec: unknown indirect PM4 op 0x%02x at dword %u\n",
                op, pos);
        return false;
    }

    s->mec_ib.pos += len;
    return true;
}

static bool rdna4_select_mec_queue(RDNA4State *s, uint32_t db_dword)
{
    uint32_t off;

    s->selected_pipe = s->selected_queue = s->selected_vmid = 0;
    if (rdna4_hqd_reg(REG_CP_HQD_DOORBELL, &off)) {
        for (uint32_t pipe = 0; pipe < 4; pipe++) {
            for (uint32_t queue = 0; queue < 8; queue++) {
                uint32_t doorbell = s->hqd[pipe][queue].q[off];
                if (s->hqd[pipe][queue].used && (doorbell & (1u << 30)) &&
                    ((doorbell >> 2) & 0x3ffffff) == db_dword) {
                    s->selected_pipe = pipe;
                    s->selected_queue = queue;
                    s->selected_vmid = s->hqd[pipe][queue].q[
                        (REG_CP_HQD_VMID / 4) - (0x1260 + 0x1fa0)] & 0xf;
                    return true;
                }
            }
        }
    }
    return ((reg_get(s, REG_CP_HQD_DOORBELL) & (1u << 30)) &&
            ((reg_get(s, REG_CP_HQD_DOORBELL) >> 2) & 0x3ffffff) == db_dword);
}

static void rdna4_mec_doorbell(RDNA4State *s, uint32_t db_dword, uint64_t wptr)
{
    if (!rdna4_select_mec_queue(s, db_dword)) {
        fprintf(stderr, "rdna4: mec: doorbell dword %u ignored: no queue\n", db_dword);
        return;
    }
    /* Pipe0/queue0/VMID0 is represented by the unbanked register image; keep
     * its mirror marked so the deferred worker can find the pending queue. */
    s->hqd[s->selected_pipe][s->selected_queue].used = true;
    s->hqd[s->selected_pipe][s->selected_queue].pending = true;
    s->hqd[s->selected_pipe][s->selected_queue].pending_wptr = wptr;
    reg_set(s, REG_CP_HQD_WPTR_LO, (uint32_t)wptr);
    reg_set(s, REG_CP_HQD_WPTR_HI, (uint32_t)(wptr >> 32));
    rdna4_work_schedule(s);
}

static void rdna4_mec_select_hqd(RDNA4State *s, uint32_t pipe, uint32_t queue)
{
    uint32_t off;

    s->selected_pipe = pipe;
    s->selected_queue = queue;
    s->selected_vmid = 0;
    if (rdna4_hqd_reg(REG_CP_HQD_VMID, &off))
        s->selected_vmid = s->hqd[pipe][queue].q[off] & 0xf;
}

/* ---- MEC queue memory: the MQD and the EOP buffer (W16) ----
 * The CP accesses a queue's MQD through CP_MQD_CONTROL.VMID, not through the
 * queue's CP_HQD_VMID. amdgpu always keeps it 0 with a VMID0 (MC/GART) MQD
 * address: gfx_v12_0_compute_mqd_init ("set MQD vmid to 0") and
 * kfd_mqd_manager_v12 init_mqd (cp_mqd_control = PRIV_STATE only). The EOP
 * buffer is in the queue's own VMID (kfd_mqd_manager_v12:
 * cp_hqd_eop_base_addr = eop_ring_buffer_address >> 8, a process VA).
 * On the real RX 9070 XT, a VMID8 queue whose MQD was an MC address under
 * CP_MQD_CONTROL.VMID 8 raised a GC UTCL2 fault (IH client 0x0a source 0)
 * and never executed its first packet: a faulting MQD access stalls the
 * queue here too. An EOP access that faults is recorded in the fault status
 * and, with retry off, goes to the fault-default page; the fence still
 * lands, as nothing on the card says otherwise. */
#define REG_CP_MQD_BASE_ADDR     GC_SEG0(0x1fa9)
#define REG_CP_MQD_BASE_ADDR_HI  GC_SEG0(0x1faa)
#define REG_CP_MQD_CONTROL       GC_SEG0(0x1fcb)       /* VMID [3:0] */
#define REG_CP_HQD_EOP_BASE      GC_SEG0(0x1fce)       /* address >> 8 */
#define REG_CP_HQD_EOP_BASE_HI   GC_SEG0(0x1fcf)

/* One CP access to queue memory in the given VMID; false when it faults
 * (the fault is recorded by the walk). */
static bool rdna4_mec_queue_mem_ok(RDNA4State *s, uint64_t address, uint32_t vmid)
{
    RDNA4VmTarget target;

    if (!vmid)      /* flat MC space; the existing VMID0 paths check their spans */
        return true;
    return rdna4_vm_target(s, address, 4, vmid, true, false, &target);
}

/* The selected HQD's MQD, as the CP reaches it when it takes up the queue. */
static bool rdna4_mec_mqd_ok(RDNA4State *s, uint32_t pipe, uint32_t queue)
{
    uint64_t mqd = (reg_get(s, REG_CP_MQD_BASE_ADDR) & ~3u) |
                   ((uint64_t)reg_get(s, REG_CP_MQD_BASE_ADDR_HI) << 32);
    uint32_t vmid = reg_get(s, REG_CP_MQD_CONTROL) & 0xf;

    uint64_t *logged = &s->hqd[pipe][queue].mqd_stall_logged;

    if (rdna4_mec_queue_mem_ok(s, mqd, vmid)) {
        *logged = 0;
        return true;
    }
    /* Once per HQD and MQD setup, not on every doorbell or slice. */
    if (*logged != (mqd | vmid | 1)) {
        *logged = mqd | vmid | 1;
        fprintf(stderr, "rdna4: mec: queue pipe %u queue %u stalled: MQD 0x%" PRIx64
                " faulted in CP_MQD_CONTROL VMID %u (amdgpu keeps it 0)\n",
                pipe, queue, mqd, vmid);
    }
    return false;
}

/* RELEASE_MEM's end-of-pipe event goes through the running queue's EOP
 * buffer. Its registers are read from that queue's bank, not through
 * GRBM_GFX_CNTL, which the driver may have moved since the queue started. */
static void rdna4_mec_eop_access(RDNA4State *s, uint32_t vmid)
{
    uint32_t lo, hi;

    if (!vmid || !rdna4_hqd_reg(REG_CP_HQD_EOP_BASE, &lo) ||
        !rdna4_hqd_reg(REG_CP_HQD_EOP_BASE_HI, &hi) ||
        s->mec_work.pipe >= 4 || s->mec_work.queue >= 8)
        return;
    uint64_t eop = ((uint64_t)s->hqd[s->mec_work.pipe][s->mec_work.queue].q[lo] << 8) |
                   ((uint64_t)s->hqd[s->mec_work.pipe][s->mec_work.queue].q[hi] << 40);

    if (!rdna4_mec_queue_mem_ok(s, eop, vmid))
        fprintf(stderr, "rdna4: mec: EOP buffer 0x%" PRIx64 " faulted in VMID %u\n",
                eop, vmid);
}

static bool rdna4_mec_process_slice(RDNA4State *s, uint64_t deadline)
{
    for (;;) {
        if (s->dispatch) {
            int result = rdna4_dispatch_slice(s, s->dispatch, deadline);
            if (result == RDNA4_DISPATCH_MORE) {
                if (s->work_slice_logs++ < 8 || !(s->work_slice_logs & 63)) {
                    fprintf(stderr, "rdna4: work: dispatch slice budget 2 ms; work-groups "
                            "done=%" PRIu64 "/%" PRIu64 "\n", s->dispatch->groups_done,
                            s->dispatch->groups_total);
                }
                return true;
            }
            if (result == RDNA4_DISPATCH_HUNG) {
                s->mec_hung = true;
                fprintf(stderr, "rdna4: mec: dispatch left queue busy; following fence is not written\n");
                rdna4_dispatch_free(s->dispatch);
                s->dispatch = NULL;
                s->mec_work.active = false;
                s->mec_ib.active = false;
                return false;
            }
            rdna4_dispatch_free(s->dispatch);
            s->dispatch = NULL;
            if (s->mec_ib.active) {
                s->mec_ib.pos += s->mec_ib.packet_len;
                s->mec_ib.packet_len = 0;
            } else {
                s->mec_work.rptr = (s->mec_work.rptr + s->mec_work.packet_len) %
                                    s->mec_work.size;
                reg_set(s, REG_CP_HQD_PQ_RPTR, s->mec_work.rptr);
                s->mec_work.packet_len = 0;
            }
            continue;
        }

        if (s->mec_ib.active) {
            if (s->mec_ib.pos == s->mec_ib.dwords) {
                s->mec_ib.active = false;
                s->mec_work.rptr = (s->mec_work.rptr + s->mec_ib.outer_len) %
                                    s->mec_work.size;
                reg_set(s, REG_CP_HQD_PQ_RPTR, s->mec_work.rptr);
                s->mec_ib.outer_len = 0;
                continue;
            }
            if (!rdna4_mec_ib_packet(s)) {
                s->mec_hung = true;
                s->mec_ib.active = false;
                s->mec_work.active = false;
                return false;
            }
            if (qemu_clock_get_ns(QEMU_CLOCK_REALTIME) >= deadline)
                return true;
            continue;
        }

        if (!s->mec_work.active) {
            bool found = false;
            for (uint32_t pipe = 0; pipe < 4 && !found; pipe++) {
                for (uint32_t queue = 0; queue < 8; queue++) {
                    if (s->hqd[pipe][queue].used && s->hqd[pipe][queue].pending) {
                        rdna4_mec_select_hqd(s, pipe, queue);
                        s->hqd[pipe][queue].pending = false;
                        s->mec_work.pipe = pipe;
                        s->mec_work.queue = queue;
                        s->mec_work.vmid = reg_get(s, REG_CP_HQD_VMID) & 0xf;
                        /* CP_HQD_PQ_CONTROL.PRIV_STATE [30]: kernel queues (amdgpu sets it
                         * for kernel compute rings, gfx_v12_0.c:3250) may fetch an IB in
                         * any VMID, the job's (gfx_v12_0_ring_emit_ib_compute). */
                        s->mec_work.priv = (reg_get(s, REG_CP_HQD_PQ_CNTL) >> 30) & 1;
                        s->mec_work.size = 2u << (reg_get(s, REG_CP_HQD_PQ_CNTL) & 0x3f);
                        s->mec_work.pq = ((uint64_t)reg_get(s, REG_CP_HQD_PQ_BASE) << 8) |
                                         ((uint64_t)reg_get(s, REG_CP_HQD_PQ_BASE_HI) << 40);
                        s->mec_work.rptr = reg_get(s, REG_CP_HQD_PQ_RPTR);
                        s->mec_work.wptr = s->hqd[pipe][queue].pending_wptr % s->mec_work.size;
                        {
                            const char *why;
                            if (!rdna4_mec_ready(s, (reg_get(s, REG_CP_HQD_DOORBELL) >> 2) &
                                                 0x3ffffff, &why)) {
                            fprintf(stderr, "rdna4: mec: queue pipe %u queue %u ignored: %s\n",
                                    pipe, queue, why);
                            continue;
                            }
                        }
                        if (!rdna4_mec_mqd_ok(s, pipe, queue))
                            continue;
                        s->mec_work.active = true;
                        found = true;
                        break;
                    }
                }
            }
            if (!found)
                return false;
        }

        if (s->mec_work.rptr == s->mec_work.wptr) {
            uint64_t rep = (reg_get(s, REG_CP_HQD_RPTR_REP) & ~3u) |
                           ((uint64_t)(reg_get(s, REG_CP_HQD_RPTR_REP_HI) & 0xffff) << 32);
            uint8_t *p = rdna4_gc_span_vmid(s, rep, 4, s->mec_work.vmid, true, false);
            if (p)
                stl_le_p(p, s->mec_work.rptr);
            s->mec_work.active = false;
            continue;
        }

        uint32_t dw[16];
        for (int i = 0; i < 16; i++) {
            uint8_t *p = rdna4_gc_span_vmid(s, s->mec_work.pq +
                                            4ull * ((s->mec_work.rptr + i) % s->mec_work.size),
                                            4, s->mec_work.vmid, false, false);
            if (!p) {
                fprintf(stderr, "rdna4: mec: queue MC 0x%" PRIx64 " not mapped\n",
                        s->mec_work.pq);
                s->mec_work.active = false;
                return false;
            }
            dw[i] = ldl_le_p(p);
        }
        uint32_t hdr = dw[0], op = (hdr >> 8) & 0xff, count = (hdr >> 16) & 0x3fff;
        uint32_t len = count + 2;
        if ((hdr >> 30) != 3) {
            fprintf(stderr, "rdna4: mec: not a type-3 packet 0x%08x at %u\n", hdr,
                    s->mec_work.rptr);
            s->mec_work.active = false;
            return false;
        }
        switch (op) {
        case 0x10:
            if (count == 0x3fff)
                len = 1;
            break;
        case 0x79:
        case 0x76:
            for (uint32_t i = 0; i < count && i < 14; i++) {
                uint32_t base = op == 0x79 ? 0xc000 : 0x2c00;
                uint32_t byte = (base + dw[1] + i) * 4;
                if (byte + 4 <= RDNA4_MMIO_SIZE)
                    reg_set(s, byte, dw[2 + i]);
            }
            break;
        case 0x58:
            break;
        case 0x15:
            s->dispatch = g_new0(RDNA4Dispatch, 1);
            if (rdna4_dispatch_begin(s, s->dispatch, dw[1], dw[2], dw[3], dw[4],
                                     s->mec_work.vmid)) {
                rdna4_dispatch_free(s->dispatch);
                s->dispatch = NULL;
            } else {
                s->mec_work.packet_len = len;
                continue;
            }
            break;
        case 0x3f: {                                 /* compute INDIRECT_BUFFER */
            uint32_t control = dw[3];
            uint32_t ib_dwords = control & 0xfffff;
            if (!(control & (1u << 23))) {
                fprintf(stderr, "rdna4: mec: indirect buffer missing VALID; queue stopped\n");
                s->mec_hung = true;
                return false;
            }
            if (control & ((1u << 20) | (1u << 21) | (1u << 31)) ||
                (!s->mec_work.priv && ((control >> 24) & 0xf) != s->mec_work.vmid)) {
                fprintf(stderr, "rdna4: mec: indirect buffer was privileged/chained or VMID mismatched; queue stopped\n");
                s->mec_hung = true;
                return false;
            }
            const uint32_t ib_vmid = s->mec_work.priv ? (control >> 24) & 0xf : s->mec_work.vmid;
            if (!ib_dwords || !rdna4_gc_span_vmid(s, (uint64_t)dw[1] | ((uint64_t)dw[2] << 32),
                                    (uint64_t)ib_dwords * 4,
                                    ib_vmid, false, false)) {
                fprintf(stderr, "rdna4: mec: invalid indirect buffer; queue stopped\n");
                s->mec_hung = true;
                return false;
            }
            s->mec_ib.active = true;
            s->mec_ib.address = (uint64_t)dw[1] | ((uint64_t)dw[2] << 32);
            s->mec_ib.dwords = ib_dwords;
            s->mec_ib.pos = 0;
            s->mec_ib.vmid = ib_vmid;
            s->mec_ib.depth = 0;
            s->mec_ib.packet_len = 0;
            s->mec_ib.outer_len = len;
            continue;
        }
        case 0x37: {
            uint64_t a = (dw[2] & ~3u) | ((uint64_t)dw[3] << 32);
            uint32_t n = count >= 2 ? count - 2 : 0;
            uint8_t *p = ((dw[1] >> 8) & 0xf) == 5 ?
                         rdna4_gc_span_vmid(s, a, 4ull * n, s->mec_work.vmid, true, false) : NULL;
            if (!p) {
                fprintf(stderr, "rdna4: mec: WRITE_DATA to 0x%" PRIx64 " refused\n", a);
                s->mec_work.active = false;
                return false;
            }
            for (uint32_t i = 0; i < n; i++)
                stl_le_p(p + 4 * i, dw[4 + i]);
            break;
        }
        case 0x49: {
            uint64_t a = (dw[3] & ~3u) | ((uint64_t)dw[4] << 32);
            uint32_t sel = dw[2] >> 29;
            rdna4_mec_eop_access(s, s->mec_work.vmid);
            uint8_t *p = rdna4_gc_span_vmid(s, a, sel == 2 ? 8 : 4,
                                            s->mec_work.vmid, true, false);
            if (!p) {
                fprintf(stderr, "rdna4: mec: RELEASE_MEM to 0x%" PRIx64 " refused\n", a);
                s->mec_work.active = false;
                return false;
            }
            if (sel == 2)
                stq_le_p(p, dw[5] | ((uint64_t)dw[6] << 32));
            else if (sel == 1)
                stl_le_p(p, dw[5]);
            if (((dw[2] >> 24) & 7) == 2 && s->selected_pipe < 2 &&
                (reg_get(s, REG_CP_ME1_PIPE0_INT_CNTL + 4 * s->selected_pipe) &
                 CP_TIME_STAMP_INT_ENABLE)) {
                rdna4_ih_emit(s, 0x14, 181,
                              (uint8_t)((s->selected_queue << 4) | (1u << 2) | s->selected_pipe),
                              dw[5]);
            }
            break;
        }
        default:
            fprintf(stderr, "rdna4: mec: unknown PM4 op 0x%02x at %u, stopping\n", op,
                    s->mec_work.rptr);
            s->mec_work.active = false;
            return false;
        }
        s->mec_work.rptr = (s->mec_work.rptr + len) % s->mec_work.size;
        reg_set(s, REG_CP_HQD_PQ_RPTR, s->mec_work.rptr);
        if (qemu_clock_get_ns(QEMU_CLOCK_REALTIME) >= deadline)
            return true;
    }
}

/* ---- GFX ring (W3) ------------------------------------------------------ */

/* A packet stream is either the circular ring or one linear indirect buffer. */
typedef struct RDNA4GfxStream {
    uint64_t base;
    uint64_t pos;
    uint64_t end;
    uint32_t ring_dw;                 /* zero for an indirect buffer */
    uint32_t vmid;                    /* VMID used for this packet stream */
    bool     priv;                    /* PRIV bit for an indirect buffer */
} RDNA4GfxStream;

static bool rdna4_gfx_stream_dw(RDNA4State *s, const RDNA4GfxStream *st,
                                uint64_t pos, uint32_t *out)
{
    uint64_t slot = st->ring_dw ? pos % st->ring_dw : pos;
    uint64_t addr;
    uint8_t bytes[4];

    if (pos >= st->end || slot > (UINT64_MAX - st->base) / 4) {
        return false;
    }
    addr = st->base + 4 * slot;
    if (!rdna4_gfx_mem_read(s, addr, bytes, sizeof(bytes), st->vmid, false)) {
        return false;
    }
    *out = ldl_le_p(bytes);
    return true;
}

/*
 * Load gfx12_cs_data exactly as the RLC does. The register indices in this
 * buffer are absolute dword numbers (0xa000 + context offset), not offsets
 * that should be added to the context segment a second time.
 */
static bool rdna4_gfx_csb(RDNA4State *s, const char **why)
{
    uint64_t addr = (uint64_t)reg_get(s, REG_GFX_RLC_CSIB_LO) & ~3ull;
    uint32_t len = reg_get(s, REG_GFX_RLC_CSIB_LENGTH);
    uint64_t bytes;
    uint8_t *p;
    uint32_t clusters, at;

    if (!len) {
        *why = "CSB length is zero";
        return false;
    }
    addr |= (uint64_t)reg_get(s, REG_GFX_RLC_CSIB_HI) << 32;
    bytes = (uint64_t)len * 4;
    if (bytes / 4 != len || !(p = rdna4_gc_span(s, addr, bytes))) {
        *why = "CSB is outside the GC-mapped VRAM";
        return false;
    }
    clusters = ldl_le_p(p);
    if (!clusters || clusters > len - 1) {
        *why = "CSB cluster count is invalid";
        return false;
    }
    at = 1;
    for (uint32_t c = 0; c < clusters; c++) {
        uint32_t count, index;

        if (at + 2 > len) {
            *why = "CSB cluster header crosses LENGTH";
            return false;
        }
        count = ldl_le_p(p + 4 * at);
        index = ldl_le_p(p + 4 * at + 4);
        at += 2;
        if (count > len - at) {
            *why = "CSB cluster values cross LENGTH";
            return false;
        }
        if (index < 0xa000 || (uint64_t)index + count > RDNA4_MMIO_SIZE / 4) {
            *why = "CSB cluster register index is outside GC segment 1";
            return false;
        }
        at += count;
    }
    if (at != len) {
        *why = "CSB LENGTH does not end after its clusters";
        return false;
    }

    /* ctx-garbage=on (W37): the CSB reaches the registers only through the RLC's SRM (rdna4_rlc_srm_apply, on the
     * RLC_SRM_CNTL write). Without SRM the buffer is validated and nothing lands, the round-4 hypothesis:
     * premetal/rootcause-draw.md #1. ctx-garbage=off keeps the old model (the CP loads it when the ring starts). */
    bool land = !s->ctx_garbage || (reg_get(s, GC_SEG1(0x4c80)) & 1u);

    at = 1;
    for (uint32_t c = 0; c < clusters; c++) {
        uint32_t count = ldl_le_p(p + 4 * at);
        uint32_t index = ldl_le_p(p + 4 * at + 4);
        at += 2;
        for (uint32_t i = 0; land && i < count; i++) {
            /* index is already absolute: dword index = 0xa000 + offset. */
            reg_set(s, (index + i) * 4, ldl_le_p(p + 4 * (at + i)));
        }
        at += count;
    }
    s->gfx_csb_loaded = true;
    fprintf(stderr, "rdna4: gfx: CSB %s (%u clusters, %u dwords at MC 0x%" PRIx64 ")\n",
            land ? "loaded" : "seen, not applied (SRM off)", clusters, len, addr);
    return true;
}

static bool rdna4_gfx_ready(RDNA4State *s, bool doorbell, uint32_t db_dword,
                            const char **why)
{
    uint32_t cntl = reg_get(s, REG_GFX_CP_RB0_CNTL);
    uint32_t bufsz = cntl & 0x3f;
    uint32_t me = reg_get(s, REG_GFX_CP_ME_CNTL);
    uint32_t db = reg_get(s, REG_GFX_CP_DB_CONTROL);
    uint64_t base = ((uint64_t)reg_get(s, REG_GFX_CP_RB0_BASE) << 8) |
                    ((uint64_t)reg_get(s, REG_GFX_CP_RB0_BASE_HI) << 40);

    *why = !s->gfx_booted ? "GFX not booted" :
           (reg_get(s, REG_GFX_GRBM_GFX_CNTL) & 3) ? "GRBM_GFX_CNTL PIPEID is not 0" :
           reg_get(s, REG_GFX_CP_RB_WPTR_DELAY) ? "CP_RB_WPTR_DELAY is not zero" :
           reg_get(s, REG_GFX_CP_RB_VMID) ? "CP_RB_VMID is not zero" :
           (me & (1u << 26)) ? "PFP halted" :
           (me & (1u << 28)) ? "ME halted" :
           !reg_get(s, REG_GFX_CP_PFP_START) ? "PFP program start not set" :
           !reg_get(s, REG_GFX_CP_ME_START) ? "ME program start not set" :
           !(reg_get(s, REG_GFX_CP_RB_ACTIVE) & 1) ? "CP_RB_ACTIVE is off" :
           !base ? "ring base is not set" :
           bufsz < 5 || bufsz > 20 ? "RB_BUFSZ is outside 5..20" :
           ((cntl >> 8) & 0x3f) != bufsz - 2 ? "RB_BLKSZ does not match RB_BUFSZ" : NULL;
    if (*why) {
        return false;
    }
    if (doorbell) {
        if (!(reg_get(s, REG_NBIF_DB_APER_EN) & 1)) {
            *why = "NBIF doorbell aperture off";
        } else if (reg_get(s, REG_NBIF_S2A_ENTRY0) != 0x30000007) {
            *why = "doorbell S2A entry 0 not routing to GC";
        } else if (!(db & (1u << 30))) {
            *why = "gfx doorbell disabled";
        } else if ((db & GFX_DOORBELL_OFFSET_MASK) != (GFX_DOORBELL_DWORD << 2)) {
            *why = "gfx doorbell offset is not ring 0";
        } else if ((reg_get(s, REG_GFX_CP_RB_DB_LOWER) & GFX_DOORBELL_RANGE_MASK) !=
                   ((GFX_DOORBELL_DWORD << 2) & GFX_DOORBELL_RANGE_MASK)) {
            *why = "gfx doorbell lower range is wrong";
        } else if ((reg_get(s, REG_GFX_CP_RB_DB_UPPER) & GFX_DOORBELL_RANGE_MASK) !=
                   GFX_DOORBELL_RANGE_MASK) {
            *why = "gfx doorbell upper range is wrong";
        } else if (db_dword != GFX_DOORBELL_DWORD) {
            *why = "doorbell is not gfx ring 0's";
        }
    } else if (db & (1u << 30)) {
        *why = "gfx doorbell mode enabled for an MMIO kick";
    }
    if (*why) {
        return false;
    }
    if (!s->gfx_csb_loaded && !rdna4_gfx_csb(s, why)) {
        return false;
    }
    return true;
}

/* ---- GFX12 wave32 and first-triangle pipeline -------------------------- */

/* Graphics waves are deliberately separate from RDNA4Lane.  A compute
 * work-item has a private EXEC/VCC view; NGG and PS need one lockstep EXEC
 * mask and one VGPR file shared by 32 lanes. */
typedef struct RDNA4GfxWave {
    uint32_t s[128];
    uint32_t v[256][32];
    uint32_t vcc, exec, m0;
    bool scc;
    uint64_t pc;
    uint32_t steps;
    uint32_t prim_export[32];
    bool prim_valid[32];
    float pos_export[32][4];
    bool pos_valid[32];
    uint32_t mrt_export[4][32];
    bool mrt_valid[32];
    uint32_t mrt_enable;
    unsigned alloc_count;
} RDNA4GfxWave;

typedef struct RDNA4GfxTriangle {
    float x[3], y[3], z[3];
    uint32_t primitive;
} RDNA4GfxTriangle;

static uint32_t rdna4_gfx_inline(uint32_t src, bool *ok)
{
    static const float fconst[] = { 0.5f, -0.5f, 1.0f, -1.0f,
                                    2.0f, -2.0f, 4.0f, -4.0f };

    *ok = true;
    if (src >= 128 && src <= 192) {
        return src - 128;
    }
    if (src >= 193 && src <= 208) {
        return (uint32_t)-(int32_t)(src - 192);
    }
    if (src >= 240 && src <= 247) {
        return rdna4_u(fconst[src - 240]);
    }
    *ok = false;
    return 0;
}

static uint32_t rdna4_gfx_sreg(const RDNA4GfxWave *w, uint32_t src)
{
    if (src < 106) {
        return w->s[src];
    }
    if (src == 106) {
        return w->vcc;
    }
    if (src == 126) {
        return w->exec;
    }
    if (src == 127) {
        return 0;
    }
    {
        bool ok;
        uint32_t v = rdna4_gfx_inline(src, &ok);
        return ok ? v : 0;
    }
}

static uint32_t rdna4_gfx_sreg_lit(const RDNA4GfxWave *w, uint32_t src,
                                   uint32_t literal)
{
    return src == 255 ? literal : rdna4_gfx_sreg(w, src);
}

static uint32_t rdna4_gfx_vsrc(const RDNA4GfxWave *w, uint32_t src,
                               unsigned lane, uint32_t literal)
{
    if (src < 106 || src == 106 || src == 126 || src == 127) {
        return rdna4_gfx_sreg(w, src);
    }
    if (src >= 256 && src < 512) {
        return w->v[src - 256][lane];
    }
    if (src == 255) {
        return literal;
    }
    {
        bool ok;
        uint32_t v = rdna4_gfx_inline(src, &ok);
        return ok ? v : 0;
    }
}

static bool rdna4_gfx_set_sreg(RDNA4GfxWave *w, uint32_t dst, uint32_t value)
{
    if (dst < 106) {
        w->s[dst] = value;
    } else if (dst == 106) {
        w->vcc = value;
    } else if (dst == 125) {
        w->m0 = value;
    } else if (dst == 126) {
        w->exec = value;
    } else if (dst == 127) {
        return false;
    } else {
        return false;
    }
    return true;
}

static bool rdna4_gfx_read_code(RDNA4State *s, uint64_t pc, uint32_t vmid,
                                uint32_t *dw, uint32_t *dw1)
{
    uint8_t bytes[8];

    if (!rdna4_gfx_mem_read(s, pc, bytes, sizeof(bytes), vmid, true)) {
        return false;
    }
    *dw = ldl_le_p(bytes);
    *dw1 = ldl_le_p(bytes + 4);
    return true;
}

/* Execute the exact subset emitted by shaders/ngg.s and shaders/psred.s.
 * SALU runs once, VALU runs only on EXEC lanes, and exports are captured as
 * the stage boundary.  This is intentionally not a native shader shortcut. */
static bool rdna4_gfx_wave_run(RDNA4State *s, RDNA4GfxWave *w, bool ngg,
                               bool passthru, uint32_t vmid)
{
    while (w->steps++ < 4096) {
        uint32_t dw, dw1, dw2 = 0, dw3 = 0;
        uint32_t n = 1;

        if (!rdna4_gfx_read_code(s, w->pc, vmid, &dw, &dw1)) {
            fprintf(stderr, "rdna4: gfx: %s shader fetch at 0x%" PRIx64 " refused\n",
                    ngg ? "NGG" : "PS", w->pc);
            return false;
        }
        if (!rdna4_gfx_read_code(s, w->pc + 8, vmid, &dw2, &dw3)) {
            /* The last s_endpgm needs no third dword; this fetch is only
             * required by the VOP3 forms below. */
            dw2 = 0;
        }

        if ((dw & 0xff800000u) == 0xbf800000u) {             /* SOPP */
            uint32_t op = (dw >> 16) & 0x7f;
            int32_t simm = (int16_t)(dw & 0xffff);

            if (op == 48) {                                  /* s_endpgm */
                return true;
            }
            if (op == 32 || (op == 33 && !w->scc) || (op == 34 && w->scc) ||
                (op == 35 && !(w->vcc & 1)) || (op == 36 && (w->vcc & 1)) ||
                (op == 37 && !w->exec)) {                   /* branches */
                w->pc += 4 + 4ll * simm;
                continue;
            }
            if (op == 54) {                                  /* s_sendmsg */
                if (ngg && (dw & 0xffff) == 9) {             /* GS_ALLOC_REQ */
                    if (passthru || w->alloc_count || w->m0 != 0x1003) {
                        fprintf(stderr, "rdna4: gfx: NGG GS_ALLOC_REQ is invalid\n");
                        return false;
                    }
                    w->alloc_count++;
                }
            } else if (op != 0 && op != 5 && op != 7 && op != 20 &&
                       op != 31 && op != 49 && (op < 33 || op > 37) &&
                       (op < 64 || op > 73)) {
                goto unknown;
            }
        } else if ((dw & 0xff800000u) == 0xbe800000u) {     /* SOP1 */
            uint32_t op = (dw >> 8) & 0xff;
            uint32_t dst = (dw >> 16) & 0x7f;
            uint32_t value = rdna4_gfx_sreg_lit(w, dw & 0xff, dw1);

            if (op == 0x20) {                                /* saveexec */
                uint32_t old = w->exec;
                if (!rdna4_gfx_set_sreg(w, dst, old))
                    goto unknown;
                w->exec = old & rdna4_gfx_sreg(w, dw & 0xff);
            } else if (op == 0) {                            /* s_mov_b32 */
                if (!rdna4_gfx_set_sreg(w, dst, value))
                    goto unknown;
                if ((dw & 0xffu) == 0xffu)                   /* a literal follows (shaders/nggstore.s) */
                    n = 2;
            } else {
                goto unknown;
            }
        } else if ((dw & 0xff800000u) == 0xbf000000u) {     /* SOPC */
            uint32_t op = (dw >> 16) & 0x7f;
            uint32_t a = rdna4_gfx_sreg(w, dw & 0xff);
            uint32_t b = rdna4_gfx_sreg(w, (dw >> 8) & 0xff);

            switch (op) {
            case 6: w->scc = a == b; break;                 /* eq */
            case 7: w->scc = a != b; break;                 /* lg */
            case 8: w->scc = a > b; break;
            case 9: w->scc = a >= b; break;
            case 10: w->scc = a < b; break;
            case 11: w->scc = a <= b; break;
            default: goto unknown;
            }
        } else if ((dw & 0xc0000000u) == 0x80000000u &&
                   ((dw >> 28) & 3) != 3 && ((dw >> 23) & 0x7f) < 0x7d) {
            /* SOP2: the NGG programs use BFE, BFM, AND, LSHL and OR. */
            uint32_t op = (dw >> 23) & 0x7f;
            uint32_t dst = (dw >> 16) & 0x7f;
            uint32_t a = rdna4_gfx_sreg_lit(w, dw & 0xff, dw1);
            uint32_t b = rdna4_gfx_sreg_lit(w, (dw >> 8) & 0xff, dw1);
            uint32_t value;

            switch (op) {
            case 0x26: {                                     /* s_bfe_u32 */
                uint32_t width = (b >> 16) & 0xff;
                uint32_t offset = b & 0xff;
                value = width ? (a >> offset) & ((1u << MIN(width, 32u)) - 1u) : 0;
                if (width == 32)
                    value = a >> offset;
                if (!rdna4_gfx_set_sreg(w, dst, value)) goto unknown;
                break;
            }
            case 0x2b: {                                     /* s_bfm_b64 */
                uint32_t count = a & 0x3f, shift = b & 0x3f;
                uint64_t mask = count >= 32 ? UINT64_MAX :
                                (((uint64_t)1 << count) - 1) << shift;
                if (dst >= 106 || dst + 1 >= 128) goto unknown;
                w->s[dst] = (uint32_t)mask;
                w->s[dst + 1] = (uint32_t)(mask >> 32);
                break;
            }
            case 8: value = a << (b & 31); if (!rdna4_gfx_set_sreg(w, dst, value)) goto unknown; break;
            case 22: value = a & b; if (!rdna4_gfx_set_sreg(w, dst, value)) goto unknown; break;
            case 24: value = a | b; if (!rdna4_gfx_set_sreg(w, dst, value)) goto unknown; break;
            default: goto unknown;
            }
            n = ((dw & 0xffu) == 0xffu || ((dw >> 8) & 0xffu) == 0xffu) ? 2 : 1;
        } else if ((dw & 0xfc000000u) == 0xd4000000u) {     /* VOP3 */
            uint32_t op = (dw >> 16) & 0x3ff;
            uint32_t dst = dw & 0xff;
            uint32_t s0 = dw1 & 0x1ff;
            uint32_t s1 = (dw1 >> 9) & 0x1ff;
            uint32_t s2 = (dw1 >> 18) & 0x1ff;

            if (op == 0x4a) {                                /* v_cmp_eq_u32_e64 */
                uint32_t mask = 0;
                for (unsigned lane = 0; lane < 32; lane++) {
                    if ((w->exec >> lane) & 1u &&
                        rdna4_gfx_vsrc(w, s0, lane, dw2) ==
                        rdna4_gfx_vsrc(w, s1, lane, dw2))
                        mask |= 1u << lane;
                }
                if (dst >= 106 || !rdna4_gfx_set_sreg(w, dst, mask))
                    goto unknown;
            } else if (op == 0x101) {                        /* v_cndmask_b32_e64 */
                uint32_t cond = rdna4_gfx_sreg(w, s2);
                for (unsigned lane = 0; lane < 32; lane++) {
                    if ((w->exec >> lane) & 1u)
                        w->v[dst][lane] = (cond >> lane) & 1u ?
                            rdna4_gfx_vsrc(w, s1, lane, dw2) :
                            rdna4_gfx_vsrc(w, s0, lane, dw2);
                }
            } else {
                goto unknown;
            }
            n = 2;
        } else if ((dw & 0xfe000000u) == 0x7e000000u) {     /* VOP1 */
            uint32_t op = (dw >> 9) & 0xff;
            uint32_t dst = (dw >> 17) & 0xff;

            if (op != 1) goto unknown;                       /* v_mov_b32 */
            for (unsigned lane = 0; lane < 32; lane++) {
                if ((w->exec >> lane) & 1u)
                    w->v[dst][lane] = rdna4_gfx_vsrc(w, dw & 0x1ff, lane, dw1);
            }
            if ((dw & 0x1ffu) == 0xffu)                      /* a literal follows */
                n = 2;
        } else if ((dw & 0xff000000u) == 0xee000000u && ((dw >> 14) & 0x7f) == 0x1a) {
            /* VGLOBAL global_store_b32 vaddr, vdata, saddr (96-bit form, encodings
             * taken from llvm-mc -mcpu=gfx1201): dw0 [6:0] saddr, dw1 [30:23] vdata,
             * dw2 [7:0] vaddr (32-bit offset added to the SGPR-pair base). The PS
             * diagnostic variant (shaders/psstore.s) stores a marker with it. */
            uint32_t saddr = dw & 0x7f, vdata = (dw1 >> 23) & 0xff, vaddr = dw2 & 0xff;
            uint64_t base;

            if (saddr >= 106 || (dw2 & 0x00ffff00u))
                goto unknown;
            base = (uint64_t)w->s[saddr] | ((uint64_t)w->s[saddr + 1] << 32);
            for (unsigned lane = 0; lane < 32; lane++) {
                if ((w->exec >> lane) & 1u) {
                    uint32_t value = w->v[vdata][lane];

                    if (!rdna4_gfx_mem_write(s, base + w->v[vaddr][lane], &value,
                                             sizeof(value), vmid)) {
                        fprintf(stderr, "rdna4: gfx: PS global_store to 0x%" PRIx64 " faulted\n",
                                base + w->v[vaddr][lane]);
                        return false;
                    }
                }
            }
            n = 3;
        } else if ((dw & 0xffff0000u) == 0xf8000000u) {     /* EXPORT */
            uint32_t target = (dw >> 4) & 0x3f;
            uint32_t enable = dw & 0xf;
            uint32_t src = dw1 & 0xff;

            /* gfx12's MRT0 encoding carries target 8 in the adjacent target
             * field form; primitive and position exports use [9:4]. */
            if (!target)
                target = (dw >> 8) & 0x3f;

            if (target == 20) {                              /* SQ_EXP_PRIM */
                if (!ngg || enable != 1) goto unknown;
                for (unsigned lane = 0; lane < 32; lane++) {
                    if ((w->exec >> lane) & 1u) {
                        uint32_t raw = w->v[src][lane];
                        /* The shader's packed byte lanes are converted at
                         * SQ_EXP_PRIM to the 9-bit primitive index fields. */
                        w->prim_export[lane] = (raw & 0xffu) |
                            (((raw >> 8) & 0xffu) << 9) |
                            (((raw >> 16) & 0xffu) << 18) |
                            (raw & (1u << 31));
                        w->prim_valid[lane] = true;
                    }
                }
            } else if (target == 12) {                       /* SQ_EXP_POS */
                if (!ngg || enable != 0xf) goto unknown;
                for (unsigned lane = 0; lane < 32; lane++) {
                    if ((w->exec >> lane) & 1u) {
                        for (unsigned c = 0; c < 4; c++)
                            w->pos_export[lane][c] = rdna4_f(w->v[src + c][lane]);
                        w->pos_valid[lane] = true;
                    }
                }
            } else if (target == 8) {                        /* SQ_EXP_MRT */
                if (ngg || (enable != 0xf && enable != 0x3)) goto unknown;
                w->mrt_enable = enable;
                for (unsigned lane = 0; lane < 32; lane++) {
                    if ((w->exec >> lane) & 1u) {
                        for (unsigned c = 0; c < (enable == 0xf ? 4 : 2); c++)
                            w->mrt_export[c][lane] = w->v[src + c][lane];
                        w->mrt_valid[lane] = true;
                    }
                }
            } else {
                goto unknown;
            }
            if ((dw >> 11) & 1u)
                w->steps += 0;
            n = 2;
        } else {
            goto unknown;
        }
        w->pc += 4ull * n;
    }
    fprintf(stderr, "rdna4: gfx: %s shader exceeded 4096 lockstep instructions\n",
            ngg ? "NGG" : "PS");
    return false;

unknown:
    {
        uint32_t bad_dw = 0;
        uint32_t ignored = 0;
        rdna4_gfx_read_code(s, w->pc, vmid, &bad_dw, &ignored);
    fprintf(stderr, "rdna4: gfx: %s shader unsupported instruction 0x%08x at pc 0x%" PRIx64 "\n",
            ngg ? "NGG" : "PS", bad_dw, w->pc);
    }
    return false;
}

/* Shader-engine count as amdgpu gets it: amdgpu_discovery_get_gc_info reads
 * gc_num_se from the IP discovery GC table (table_list[1]; gpu_info_header is
 * 12 bytes, gc_num_se follows).  GB_ADDR_CONFIG is not an SE count source:
 * the card's 0x08200545 has NUM_SHADER_ENGINES [22:19] = 4 (16 by a 1<<n
 * decode) on a 4-SE Navi 48.  0 when the table is absent. */
static uint32_t rdna4_gfx_discovery_num_se(RDNA4State *s)
{
    uint32_t list, off;

    if (!s->discovery || s->discovery_len < 32) {
        return 0;
    }
    list = lduw_le_p(s->discovery + 4) >= 2 ? 16 : 12;
    off = lduw_le_p(s->discovery + list + 8);
    if (!off || off + 16 > s->discovery_len) {
        return 0;
    }
    if (ldl_le_p(s->discovery + off) != 0x4347) { /* GC_TABLE_ID */
        return 0;
    }
    return ldl_le_p(s->discovery + off + 12);
}

static bool rdna4_gfx_draw_refuse(RDNA4State *s, const char *reg,
                                  const char *why)
{
    s->gfx_draw_refused = true;
    fprintf(stderr, "rdna4: gfx: draw refused: %s %s\n", reg, why);
    return false;
}

static void rdna4_gfx_break_must(RDNA4State *s)
{
    switch (s->gfx_break) {
    case 1: /* CB_COLOR_CONTROL.MODE = CB_DISABLE */
        reg_set(s, REG_GFX_CB_COLOR_CONTROL,
                reg_get(s, REG_GFX_CB_COLOR_CONTROL) & ~(7u << 4));
        break;
    case 2: /* PA_SC_CLIPRECT_RULE = reject-all */
        reg_set(s, REG_GFX_PA_SC_CLIPRECT_RULE, 0);
        break;
    case 3: /* no GE position ring */
        reg_set(s, REG_GFX_GE_POS_RING_BASE, 0);
        break;
    case 4: /* no PS barycentrics */
        reg_set(s, REG_GFX_SPI_PS_INPUT_ENA, 0);
        break;
    default:
        break;
    }
}

static uint32_t rdna4_gfx_ring_bytes(RDNA4State *s, uint32_t base_reg,
                                     uint32_t size_reg, uint32_t mask,
                                     uint32_t vmid)
{
    uint64_t base = (uint64_t)reg_get(s, base_reg) << 16;
    uint64_t bytes = (uint64_t)(reg_get(s, size_reg) & mask) << 5;

    return base && bytes && bytes <= UINT32_MAX &&
           rdna4_gfx_mem_mapped(s, base, bytes, vmid, false, false) ?
               (uint32_t)bytes : 0;
}

/* W37: context-register power-up garbage and the clear state (premetal/rootcause-draw.md #1).
 * On silicon the context registers are SRAM that powers up with garbage (round 4 read
 * VGT_SHADER_STAGES_EN=0xfd1ffe88 before the first draw, different each boot); amdgpu hands the RLC a
 * clear-state buffer (gfx12_cs_data, six extents) and enables SRM so the registers Mesa never writes end up 0.
 * The old model read every unwritten register as 0, which is why the emulator drew when the card did not.
 * ctx-garbage=on (default): the 1024 context registers (seg 1 dwords 0..0x3ff) start as a fixed non-zero pattern,
 * and a draw is refused while a clear-state register still holds it (a hypothesis model like
 * gfx-golden-strict: it only shows whether the kext clears them). */
static const struct { uint16_t index, count; } rdna4_csb_extents[] = {
    { 0x03e, 34 }, { 0x0cc, 2 }, { 0x0d8, 1 }, { 0x0db, 6 }, { 0x2e5, 11 }, { 0x3c0, 8 },
};

static uint32_t rdna4_ctx_garbage(uint32_t off)
{
    return (0x9e3779b1u * (off + 1)) | 0x00010001u;   /* never 0 */
}

static void rdna4_ctx_poison(RDNA4State *s)
{
    for (uint32_t off = 0; off < 0x400; off++) {
        reg_set(s, GC_SEG1(off), rdna4_ctx_garbage(off));
    }
}

static void rdna4_rlc_srm_apply(RDNA4State *s)
{
    uint64_t csib = reg_get(s, GC_SEG1(0x0987)) | ((uint64_t)reg_get(s, GC_SEG1(0x0988)) << 32);
    uint32_t len = reg_get(s, GC_SEG1(0x0989));
    uint32_t buf[256];

    if (!csib || !len || len > 256) {
        fprintf(stderr, "rdna4: rlc: SRM enabled without a usable clear-state buffer (0x%" PRIx64 ", %u dwords)\n",
                csib, len);
        return;
    }
    if (!rdna4_gfx_mem_read(s, csib, buf, len * 4, 0, false)) {
        fprintf(stderr, "rdna4: rlc: SRM cannot read the clear-state buffer at 0x%" PRIx64 "\n", csib);
        return;
    }
    uint32_t clusters = buf[0], at = 1, regs = 0;
    for (uint32_t c = 0; c < clusters && at + 2 <= len; c++) {
        uint32_t n = buf[at], first = buf[at + 1];
        at += 2;
        for (uint32_t i = 0; i < n && at < len; i++, at++) {
            reg_set(s, (first + i) * 4, buf[at]);
            regs++;
        }
    }
    fprintf(stderr, "rdna4: rlc: SRM applied the clear-state buffer: %u clusters, %u registers\n", clusters, regs);
}

/* First clear-state register still holding the power-up pattern, or -1; *count = how many. */
static int rdna4_csb_garbage(RDNA4State *s, uint32_t *count)
{
    int first = -1;

    *count = 0;
    for (unsigned e = 0; e < sizeof(rdna4_csb_extents) / sizeof(rdna4_csb_extents[0]); e++) {
        for (uint32_t i = 0; i < rdna4_csb_extents[e].count; i++) {
            uint32_t off = rdna4_csb_extents[e].index + i;

            if (reg_get(s, GC_SEG1(off)) == rdna4_ctx_garbage(off)) {
                if (first < 0) {
                    first = (int)off;
                }
                (*count)++;
            }
        }
    }
    return first;
}

static bool rdna4_gfx_check_draw(RDNA4State *s, uint32_t count, uint32_t vmid)
{
    uint32_t stages, ena, addr, col, info, attrib3, vte, cbcc, target, shader;
    uint32_t ses, pos_bytes, prim_bytes;

    /* amdgpu programs the 3D pipeline's golden registers before it starts the CP
     * (gfx_v12_0_init_golden_registers, gfx_v12_0.c:3671-3690): DB_MEM_CONFIG
     * bit 15 always, and for rev_id 0 DB_MEM_CONFIG[3:0]=0xf, CB_HW_CONTROL_1[25:24]=3,
     * GL2C_CTRL5[6:4]=2. rev_id is the NBIF strap (nbif_v6_3_1_get_rev_id,
     * RCC_STRAP0 at NBIF dword 0x1c, [27:24]; the model's strap reads 0). Whether the silicon draws
     * without them is not known: the round-3 card did not draw and had none, so the
     * model reports the omission; gfx-golden-strict=on refuses the draw. Strict mode
     * encodes that hypothesis: its result is NOT evidence about the card, it only
     * shows that the kext writes the registers. */
    {
        uint32_t rev_id = (reg_get(s, (0xd20 + 0x1c) * 4) >> 24) & 0xf;
        uint32_t db = reg_get(s, REG_GFX_DB_MEM_CONFIG);
        uint32_t cb = reg_get(s, REG_GFX_CB_HW_CONTROL_1);
        uint32_t gl2 = reg_get(s, REG_GFX_GL2C_CTRL5);
        bool golden = (db & 0x8000) &&
            (rev_id != 0 || ((db & 0xf) == 0xf && ((cb >> 24) & 3) == 3 && ((gl2 >> 4) & 7) == 2));

        if (!golden) {
            if (!s->gfx_golden_warned) {
                fprintf(stderr, "rdna4: gfx: draw without the amdgpu golden registers "
                        "(rev_id %u: DB_MEM_CONFIG 0x%08x CB_HW_CONTROL_1 0x%08x GL2C_CTRL5 0x%08x)\n",
                        rev_id, db, cb, gl2);
                s->gfx_golden_warned = true;
            }
            if (s->gfx_golden_strict)
                return rdna4_gfx_draw_refuse(s, "golden registers",
                                             "gfx_v12_0_init_golden_registers was not applied");
        }
    }
    if (s->ctx_garbage) {
        uint32_t nbad;
        int first = rdna4_csb_garbage(s, &nbad);

        if (first >= 0) {
            /* the primitive reaches the clipper and is lost before the pixel shader (pipeline statistics: C_PRIM > 0, PS = 0) */
            s->pstat[2] += 1;
            s->pstat[1] += 1;
            fprintf(stderr, "rdna4: gfx: draw lost: %u clear-state registers still hold power-up garbage "
                    "(first: context 0x%03x = 0x%08x); the RLC clear state (SRM) or a CSB replay is missing\n",
                    nbad, first, reg_get(s, GC_SEG1((uint32_t)first)));
            return rdna4_gfx_draw_refuse(s, "clear state", "context registers hold power-up garbage");
        }
    }
    if (count == 0 || count > 30)
        return rdna4_gfx_draw_refuse(s, "INDEX_COUNT", "count is not modelled (1..30)");
    if (s->gfx_num_instances != 1)
        return rdna4_gfx_draw_refuse(s, "NUM_INSTANCES", "must be 1");
    if ((reg_get(s, REG_GFX_VGT_PRIMITIVE_TYPE) & 0x3f) != 4)
        return rdna4_gfx_draw_refuse(s, "VGT_PRIMITIVE_TYPE", "must be TRILIST (4)");
    stages = reg_get(s, REG_GFX_VGT_SHADER_STAGES_EN);
    if (!(stages & (1u << 22)) || (stages & ((1u << 2) | (1u << 5))))
        return rdna4_gfx_draw_refuse(s, "VGT_SHADER_STAGES_EN", "requires GS_W32_EN and no HS/GS");
    if (!reg_get(s, REG_GFX_SPI_SHADER_PGM_LO_ES) &&
        !reg_get(s, REG_GFX_SPI_SHADER_PGM_HI_ES))
        return rdna4_gfx_draw_refuse(s, "SPI_SHADER_PGM_ES", "NGG address is zero");
    if (!reg_get(s, REG_GFX_SPI_SHADER_PGM_LO_PS) &&
        !reg_get(s, REG_GFX_SPI_SHADER_PGM_HI_PS))
        return rdna4_gfx_draw_refuse(s, "SPI_SHADER_PGM_PS", "PS address is zero");
    if ((reg_get(s, REG_GFX_SPI_SHADER_POS_FORMAT) & 0xf) != 4)
        return rdna4_gfx_draw_refuse(s, "SPI_SHADER_POS_FORMAT", "POS0 must be 4COMP");
    col = reg_get(s, REG_GFX_SPI_SHADER_COL_FORMAT) & 0xf;
    if (col != 9 && col != 4)
        return rdna4_gfx_draw_refuse(s, "SPI_SHADER_COL_FORMAT", "COL0 must be 32_ABGR or FP16_ABGR");
    ena = reg_get(s, REG_GFX_SPI_PS_INPUT_ENA);
    addr = reg_get(s, REG_GFX_SPI_PS_INPUT_ADDR);
    if (!(ena & ((1u << 0) | (1u << 1) | (1u << 2) |
                 (1u << 4) | (1u << 5) | (1u << 6))))
        return rdna4_gfx_draw_refuse(s, "SPI_PS_INPUT_ENA", "no PERSP/LINEAR barycentric");
    if ((addr & ena) != ena)
        return rdna4_gfx_draw_refuse(s, "SPI_PS_INPUT_ADDR", "does not cover SPI_PS_INPUT_ENA");
    if (!(reg_get(s, REG_GFX_SPI_PS_IN_CONTROL) & (1u << 15)))
        return rdna4_gfx_draw_refuse(s, "SPI_PS_IN_CONTROL", "PS_W32_EN is off");
    if (!(reg_get(s, REG_GFX_SPI_SHADER_GS_OUT_CONFIG_PS) & (1u << 10)))
        return rdna4_gfx_draw_refuse(s, "SPI_SHADER_GS_OUT_CONFIG_PS", "NO_PC_EXPORT is off");
    ses = rdna4_gfx_discovery_num_se(s);
    if (!ses || ses > REG_GFX_GE_RING_MIN_SE)
        return rdna4_gfx_draw_refuse(s, "gc_info gc_num_se", "shader-engine count is not modelled");
    pos_bytes = rdna4_gfx_ring_bytes(s, REG_GFX_GE_POS_RING_BASE,
                                     REG_GFX_GE_POS_RING_SIZE, 0x3fff, vmid);
    prim_bytes = rdna4_gfx_ring_bytes(s, REG_GFX_GE_PRIM_RING_BASE,
                                      REG_GFX_GE_PRIM_RING_SIZE, 0x7ff, vmid);
    if (!pos_bytes || pos_bytes < 0x40000)
        return rdna4_gfx_draw_refuse(s, "GE_POS_RING_BASE/SIZE", "position ring is unmapped or undersized");
    if (!prim_bytes || prim_bytes < 0xffc0)
        return rdna4_gfx_draw_refuse(s, "GE_PRIM_RING_BASE/SIZE", "primitive ring is unmapped or undersized");
    if (reg_get(s, REG_GFX_GE_MAX_VTX_INDX) < count - 1)
        return rdna4_gfx_draw_refuse(s, "GE_MAX_VTX_INDX", "auto index is clamped below INDEX_COUNT");
    vte = reg_get(s, REG_GFX_PA_CL_VTE_CNTL);
    if ((vte & 0x43f) != 0x43f)
        return rdna4_gfx_draw_refuse(s, "PA_CL_VTE_CNTL", "viewport enables and VTX_W0_FMT are required");
    if (!reg_get(s, REG_GFX_PA_SC_AA_MASK_0) || !reg_get(s, REG_GFX_PA_SC_AA_MASK_1))
        return rdna4_gfx_draw_refuse(s, "PA_SC_AA_MASK_*", "sample mask disables rasterization");
    if ((reg_get(s, REG_GFX_PA_SC_CLIPRECT_RULE) & 0xffff) != 0xffff)
        return rdna4_gfx_draw_refuse(s, "PA_SC_CLIPRECT_RULE", "must be 0xffff");
    if (reg_get(s, REG_GFX_PA_SC_AA_CONFIG) & 7)
        return rdna4_gfx_draw_refuse(s, "PA_SC_AA_CONFIG", "MSAA_NUM_SAMPLES must be zero");
    cbcc = reg_get(s, REG_GFX_CB_COLOR_CONTROL);
    if (((cbcc >> 4) & 7) != 1)
        return rdna4_gfx_draw_refuse(s, "CB_COLOR_CONTROL.MODE", "must be CB_NORMAL (1)");
    target = reg_get(s, REG_GFX_CB_TARGET_MASK) & 0xf;
    shader = reg_get(s, REG_GFX_CB_SHADER_MASK) & 0xf;
    if (!target)
        return rdna4_gfx_draw_refuse(s, "CB_TARGET_MASK", "MRT0 is disabled");
    if (!shader)
        return rdna4_gfx_draw_refuse(s, "CB_SHADER_MASK", "MRT0 is disabled");
    info = reg_get(s, REG_GFX_CB_COLOR0_INFO);
    if ((info & 0x1f) != 10 || ((info >> 8) & 7) != 0 || ((info >> 11) & 3) != 0)
        return rdna4_gfx_draw_refuse(s, "CB_COLOR0_INFO", "requires 8_8_8_8 UNORM standard component order");
    attrib3 = reg_get(s, REG_GFX_CB_COLOR0_ATTRIB3);
    if (((attrib3 >> 15) & 7) != 0 || ((attrib3 >> 24) & 3) != 1)
        return rdna4_gfx_draw_refuse(s, "CB_COLOR0_ATTRIB3", "requires linear 2D resource");
    if (reg_get(s, REG_GFX_CB_BLEND0_CONTROL) & 1)
        return rdna4_gfx_draw_refuse(s, "CB_BLEND0_CONTROL", "blending must be disabled");
    if ((reg_get(s, REG_GFX_DB_Z_INFO) & 0xff) != 0)
        return rdna4_gfx_draw_refuse(s, "DB_Z_INFO", "depth format must be invalid");
    if ((reg_get(s, REG_GFX_DB_STENCIL_INFO) & 0xff) != 0)
        return rdna4_gfx_draw_refuse(s, "DB_STENCIL_INFO", "stencil format must be invalid");
    if (!(reg_get(s, REG_GFX_PA_SU_VTX_CNTL) & 1))
        return rdna4_gfx_draw_refuse(s, "PA_SU_VTX_CNTL", "PIX_CENTER must be one");
    if (reg_get(s, REG_GFX_PA_SC_EDGERULE) != 0xaa959a6a)
        return rdna4_gfx_draw_refuse(s, "PA_SC_EDGERULE", "G3 requires the programmed top-left rule");
    return true;
}

static int64_t rdna4_gfx_edge(int64_t ax, int64_t ay, int64_t bx, int64_t by,
                              int64_t px, int64_t py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

static bool rdna4_gfx_top_left(int64_t ax, int64_t ay, int64_t bx, int64_t by)
{
    int64_t dx = bx - ax, dy = by - ay;
    return dy > 0 || (dy == 0 && dx < 0);
}

static uint32_t rdna4_gfx_unorm8(float value)
{
    float x = nearbyintf(CLAMP(value, 0.0f, 1.0f) * 255.0f);
    return (uint32_t)CLAMP(x, 0.0f, 255.0f);
}

static bool rdna4_gfx_ps_wave(RDNA4State *s, const RDNA4GfxTriangle *tri,
                              const int *px, const int *py, unsigned count,
                              uint8_t *target, uint32_t pitch, uint32_t chan_mask,
                              uint32_t col_format, uint32_t vmid,
                              uint32_t *written)
{
    RDNA4GfxWave w = { 0 };
    uint64_t pgm = ((uint64_t)reg_get(s, REG_GFX_SPI_SHADER_PGM_LO_PS) << 8) |
                   ((uint64_t)reg_get(s, REG_GFX_SPI_SHADER_PGM_HI_PS) << 40);
    uint32_t base = reg_get(s, REG_GFX_CB_COLOR0_BASE);
    uint32_t base_ext = reg_get(s, REG_GFX_CB_COLOR0_BASE_EXT);
    uint64_t cb = ((uint64_t)base << 8) | ((uint64_t)(base_ext & 0xff) << 40);

    (void)target;

    w.pc = pgm;
    /* The user SGPRs the driver asked for (RSRC2_PS.USER_SGPR [5:1]) arrive as
     * s0.. from SPI_SHADER_USER_DATA_PS_n: the diagnostic PS takes a memory
     * address there. */
    {
        uint32_t nuser = (reg_get(s, REG_GFX_SPI_SHADER_PGM_RSRC2_PS) >> 1) & 0x1f;

        if (nuser > 0)
            w.s[0] = reg_get(s, REG_GFX_SPI_SHADER_USER_DATA_PS_0);
        if (nuser > 1)
            w.s[1] = reg_get(s, REG_GFX_SPI_SHADER_USER_DATA_PS_1);
    }
    w.exec = count == 32 ? UINT32_MAX : ((1u << count) - 1u);
    for (unsigned lane = 0; lane < count; lane++) {
        float area = (float)rdna4_gfx_edge(
            (int64_t)nearbyintf(tri->x[0] * 256.0f), (int64_t)nearbyintf(tri->y[0] * 256.0f),
            (int64_t)nearbyintf(tri->x[1] * 256.0f), (int64_t)nearbyintf(tri->y[1] * 256.0f),
            (int64_t)nearbyintf(px[lane] * 256.0f + 128.0f),
            (int64_t)nearbyintf(py[lane] * 256.0f + 128.0f));
        float area2 = (float)rdna4_gfx_edge(
            (int64_t)nearbyintf(tri->x[0] * 256.0f), (int64_t)nearbyintf(tri->y[0] * 256.0f),
            (int64_t)nearbyintf(tri->x[2] * 256.0f), (int64_t)nearbyintf(tri->y[2] * 256.0f),
            (int64_t)nearbyintf(px[lane] * 256.0f + 128.0f),
            (int64_t)nearbyintf(py[lane] * 256.0f + 128.0f));
        float denom = (float)rdna4_gfx_edge(
            (int64_t)nearbyintf(tri->x[0] * 256.0f), (int64_t)nearbyintf(tri->y[0] * 256.0f),
            (int64_t)nearbyintf(tri->x[1] * 256.0f), (int64_t)nearbyintf(tri->y[1] * 256.0f),
            (int64_t)nearbyintf(tri->x[2] * 256.0f), (int64_t)nearbyintf(tri->y[2] * 256.0f));
        /* PERSP_CENTER inputs are real barycentrics even though ps_red ignores them. */
        w.v[0][lane] = rdna4_u(area / denom);
        w.v[1][lane] = rdna4_u(area2 / denom);
    }
    if (!rdna4_gfx_wave_run(s, &w, false, false, vmid))
        return false;
    if (w.mrt_enable != (col_format == 4 ? 3u : 15u)) {
        fprintf(stderr, "rdna4: gfx: PS export mask 0x%x does not match COL0 format %u\n",
                w.mrt_enable, col_format);
        return false;
    }
    for (unsigned lane = 0; lane < count; lane++) {
        float rgba[4];
        uint8_t pixel[4];
        uint64_t addr;

        if (!w.mrt_valid[lane])
            return false;
        if (col_format == 4) {
            rgba[0] = rdna4_half((uint16_t)w.mrt_export[0][lane]);
            rgba[1] = rdna4_half((uint16_t)(w.mrt_export[0][lane] >> 16));
            rgba[2] = rdna4_half((uint16_t)w.mrt_export[1][lane]);
            rgba[3] = rdna4_half((uint16_t)(w.mrt_export[1][lane] >> 16));
        } else {
            for (unsigned c = 0; c < 4; c++)
                rgba[c] = rdna4_f(w.mrt_export[c][lane]);
        }
        addr = cb + (uint64_t)py[lane] * pitch + (uint64_t)px[lane] * 4;
        if (!rdna4_gfx_mem_read(s, addr, pixel, sizeof(pixel), vmid, false))
            return false;
        for (unsigned c = 0; c < 4; c++) {
            if ((chan_mask >> c) & 1u)
                pixel[c] = (uint8_t)rdna4_gfx_unorm8(rgba[c]);
        }
        if (!rdna4_gfx_mem_write(s, addr, pixel, sizeof(pixel), vmid))
            return false;
        (*written)++;
    }
    return true;
}

static bool rdna4_gfx_draw(RDNA4State *s, uint32_t count, uint32_t vmid)
{
    RDNA4GfxWave ngg = { 0 };
    RDNA4GfxTriangle tri = { 0 };
    uint32_t stages, prim, indices[3], col_format, chan_mask;
    uint64_t pgm, start_ns;
    uint32_t pitch, height, attrib2;
    uint32_t written = 0;

    s->gfx_draw_refused = false;
    rdna4_gfx_break_must(s);
    s->pstat[7] += count;                              /* IA_VERTICES */
    s->pstat[6] += count / 3;                          /* IA_PRIMITIVES */
    s->pstat[3] += count;                              /* VS_INVOCATIONS (the NGG stage) */
    if (!rdna4_gfx_check_draw(s, count, vmid))
        return false;
    start_ns = qemu_clock_get_ns(QEMU_CLOCK_HOST);
    attrib2 = reg_get(s, REG_GFX_CB_COLOR0_ATTRIB2);
    pitch = ((attrib2 & 0xffff) + 1) * 4;
    height = ((attrib2 >> 16) & 0xffff) + 1;
    if (pitch != 1024 || height != 256)
        return rdna4_gfx_draw_refuse(s, "CB_COLOR0_ATTRIB2", "G3 model requires a 256x256 RGBA8 target");
    col_format = reg_get(s, REG_GFX_SPI_SHADER_COL_FORMAT) & 0xf;
    chan_mask = (reg_get(s, REG_GFX_CB_TARGET_MASK) &
                 reg_get(s, REG_GFX_CB_SHADER_MASK)) & 0xf;

    stages = reg_get(s, REG_GFX_VGT_SHADER_STAGES_EN);
    pgm = ((uint64_t)reg_get(s, REG_GFX_SPI_SHADER_PGM_LO_ES) << 8) |
          ((uint64_t)reg_get(s, REG_GFX_SPI_SHADER_PGM_HI_ES) << 40);
    ngg.exec = UINT32_MAX;
    ngg.pc = pgm;
    ngg.s[0] = reg_get(s, REG_GFX_SPI_SHADER_PGM_LO_GS);
    ngg.s[1] = reg_get(s, REG_GFX_SPI_SHADER_PGM_HI_GS);
    ngg.s[2] = 0x00403000;             /* gs_tg_info: 3 verts, 1 prim */
    ngg.s[3] = 0x10000103;             /* merged_wave_info: wave 0 of 1 */
    ngg.v[0][0] = 0x04020100;          /* primitive indices 0,1,2 + edge flags */
    for (unsigned lane = 0; lane < count; lane++)
        ngg.v[3][lane] = lane;         /* auto-index VertexID */
    if (!rdna4_gfx_wave_run(s, &ngg, true,
                            (stages & (1u << 26)) != 0, vmid))
        return false;
    if ((stages & (1u << 26)) != 0) {
        if (ngg.alloc_count)
            return rdna4_gfx_draw_refuse(s, "VGT_SHADER_STAGES_EN", "passthrough sent GS_ALLOC_REQ");
    } else if (ngg.alloc_count != 1) {
        return rdna4_gfx_draw_refuse(s, "GS_ALLOC_REQ", "exactly one wave-0 allocation is required");
    }
    if (!ngg.prim_valid[0])
        return rdna4_gfx_draw_refuse(s, "NGG primitive export", "wave 0 did not export a primitive");
    prim = ngg.prim_export[0];
    if (prim & (1u << 31)) {
        fprintf(stderr, "rdna4: gfx: draw produced a null primitive\n");
        return true;
    }
    for (unsigned i = 0; i < 3; i++) {
        indices[i] = (prim >> (9 * i)) & 0xff;
        if (indices[i] >= count || !ngg.pos_valid[indices[i]])
            return rdna4_gfx_draw_refuse(s, "NGG position export", "primitive references an unexported vertex");
        tri.x[i] = ngg.pos_export[indices[i]][0];
        tri.y[i] = ngg.pos_export[indices[i]][1];
        tri.z[i] = ngg.pos_export[indices[i]][2];
    }
    tri.primitive = prim;
    for (unsigned i = 0; i < 3; i++) {
        float w = ngg.pos_export[indices[i]][3];
        float ndcx, ndcy;
        if (!(w > 0.0f))
            return rdna4_gfx_draw_refuse(s, "NGG position.w", "must be positive");
        ndcx = tri.x[i] / w;
        ndcy = tri.y[i] / w;
        if (fabsf(ndcx) > rdna4_f(reg_get(s, REG_GFX_PA_CL_GB_HORZ_CLIP)) ||
            fabsf(ndcy) > rdna4_f(reg_get(s, REG_GFX_PA_CL_GB_VERT_CLIP)))
            return rdna4_gfx_draw_refuse(s, "PA_CL_GB_*", "vertex is outside the guard band");
        tri.x[i] = ndcx * rdna4_f(reg_get(s, REG_GFX_PA_CL_VPORT_XSCALE)) +
                   rdna4_f(reg_get(s, REG_GFX_PA_CL_VPORT_XOFFSET));
        tri.y[i] = ndcy * rdna4_f(reg_get(s, REG_GFX_PA_CL_VPORT_YSCALE)) +
                   rdna4_f(reg_get(s, REG_GFX_PA_CL_VPORT_YOFFSET));
        tri.x[i] += (int16_t)(reg_get(s, REG_GFX_PA_SC_WINDOW_OFFSET) & 0xffff);
        tri.y[i] += (int16_t)(reg_get(s, REG_GFX_PA_SC_WINDOW_OFFSET) >> 16);
        tri.x[i] = nearbyintf(tri.x[i] * 256.0f) / 256.0f;
        tri.y[i] = nearbyintf(tri.y[i] * 256.0f) / 256.0f;
    }

    {
        float area = (tri.x[1] - tri.x[0]) * (tri.y[2] - tri.y[0]) -
                     (tri.y[1] - tri.y[0]) * (tri.x[2] - tri.x[0]);
        uint32_t su = reg_get(s, REG_GFX_PA_SU_SC_MODE_CNTL);
        bool front = area > 0.0f;       /* FACE=CCW in the gfx12 stream */
        int64_t qx[3], qy[3];
        int minx, maxx, miny, maxy;
        int sc_tlx, sc_tly, sc_brx, sc_bry;
        int *px = g_new(int, 32), *py = g_new(int, 32);
        unsigned np = 0;
        bool top[3];

        if (area == 0.0f) {
            g_free(px); g_free(py);
            return rdna4_gfx_draw_refuse(s, "PA rasterizer", "triangle has zero area");
        }
        if ((front && (su & 1)) || (!front && (su & 2))) {
            g_free(px); g_free(py);
            return true;                /* cull is a valid no-pixel draw */
        }
        if (area < 0.0f) {
            float f;
            f = tri.x[1]; tri.x[1] = tri.x[2]; tri.x[2] = f;
            f = tri.y[1]; tri.y[1] = tri.y[2]; tri.y[2] = f;
            f = tri.z[1]; tri.z[1] = tri.z[2]; tri.z[2] = f;
        }
        for (unsigned i = 0; i < 3; i++) {
            qx[i] = (int64_t)nearbyintf(tri.x[i] * 256.0f);
            qy[i] = (int64_t)nearbyintf(tri.y[i] * 256.0f);
        }
        top[0] = rdna4_gfx_top_left(qx[0], qy[0], qx[1], qy[1]);
        top[1] = rdna4_gfx_top_left(qx[1], qy[1], qx[2], qy[2]);
        top[2] = rdna4_gfx_top_left(qx[2], qy[2], qx[0], qy[0]);
        minx = (int)ceilf(fminf(tri.x[0], fminf(tri.x[1], tri.x[2])) - 0.5f);
        maxx = (int)floorf(fmaxf(tri.x[0], fmaxf(tri.x[1], tri.x[2])) - 0.5f);
        miny = (int)ceilf(fminf(tri.y[0], fminf(tri.y[1], tri.y[2])) - 0.5f);
        maxy = (int)floorf(fmaxf(tri.y[0], fmaxf(tri.y[1], tri.y[2])) - 0.5f);
        sc_tlx = (int)(reg_get(s, REG_GFX_PA_SC_SCREEN_TL) & 0xffff);
        sc_tly = (int)(reg_get(s, REG_GFX_PA_SC_SCREEN_TL) >> 16);
        sc_brx = (int)(reg_get(s, REG_GFX_PA_SC_SCREEN_BR) & 0xffff);
        sc_bry = (int)(reg_get(s, REG_GFX_PA_SC_SCREEN_BR) >> 16);
        {
            uint32_t tl = reg_get(s, REG_GFX_PA_SC_WINDOW_TL);
            uint32_t br = reg_get(s, REG_GFX_PA_SC_WINDOW_BR);
            sc_tlx = MAX(sc_tlx, (int)(tl & 0xffff));
            sc_tly = MAX(sc_tly, (int)(tl >> 16));
            sc_brx = MIN(sc_brx, (int)(br & 0xffff));
            sc_bry = MIN(sc_bry, (int)(br >> 16));
            tl = reg_get(s, REG_GFX_PA_SC_GENERIC_TL);
            br = reg_get(s, REG_GFX_PA_SC_GENERIC_BR);
            sc_tlx = MAX(sc_tlx, (int)(tl & 0xffff));
            sc_tly = MAX(sc_tly, (int)(tl >> 16));
            sc_brx = MIN(sc_brx, (int)(br & 0xffff));
            sc_bry = MIN(sc_bry, (int)(br >> 16));
            if (reg_get(s, REG_GFX_PA_SC_MODE_CNTL_0) & (1u << 1)) {
                tl = reg_get(s, REG_GFX_PA_SC_VPORT_TL);
                br = reg_get(s, REG_GFX_PA_SC_VPORT_BR);
                sc_tlx = MAX(sc_tlx, (int)(tl & 0xffff));
                sc_tly = MAX(sc_tly, (int)(tl >> 16));
                sc_brx = MIN(sc_brx, (int)(br & 0xffff));
                sc_bry = MIN(sc_bry, (int)(br >> 16));
            }
        }
        minx = MAX(minx, MAX(sc_tlx, 0));
        miny = MAX(miny, MAX(sc_tly, 0));
        maxx = MIN(maxx, MIN(sc_brx, 255));
        maxy = MIN(maxy, MIN(sc_bry, 255));
        if (s->gfx_trace) {
            fprintf(stderr, "rdna4: gfx: draw vertices (%.3f,%.3f) (%.3f,%.3f) (%.3f,%.3f), "
                    "scissor %d,%d..%d,%d\n", tri.x[0], tri.y[0], tri.x[1], tri.y[1],
                    tri.x[2], tri.y[2], sc_tlx, sc_tly, sc_brx, sc_bry);
        }
        for (int y = miny; y <= maxy; y++) {
            for (int x = minx; x <= maxx; x++) {
                int64_t pxf = (int64_t)x * 256 + 128;
                int64_t pyf = (int64_t)y * 256 + 128;
                int64_t e0 = rdna4_gfx_edge(qx[0], qy[0], qx[1], qy[1], pxf, pyf);
                int64_t e1 = rdna4_gfx_edge(qx[1], qy[1], qx[2], qy[2], pxf, pyf);
                int64_t e2 = rdna4_gfx_edge(qx[2], qy[2], qx[0], qy[0], pxf, pyf);
                if ((e0 > 0 || (e0 == 0 && top[0])) &&
                    (e1 > 0 || (e1 == 0 && top[1])) &&
                    (e2 > 0 || (e2 == 0 && top[2]))) {
                    px[np] = x; py[np++] = y;
                    if (np == 32) {
                        uint8_t *dummy = NULL;
                        uint64_t cb = ((uint64_t)reg_get(s, REG_GFX_CB_COLOR0_BASE) << 8) |
                                      ((uint64_t)(reg_get(s, REG_GFX_CB_COLOR0_BASE_EXT) & 0xff) << 40);
                        if (!rdna4_gfx_ps_wave(s, &tri, px, py, np,
                                               dummy, pitch, chan_mask, col_format,
                                               vmid, &written)) {
                            g_free(px); g_free(py);
                            return false;
                        }
                        (void)cb;
                        np = 0;
                    }
                }
            }
        }
        if (np && !rdna4_gfx_ps_wave(s, &tri, px, py, np, NULL, pitch,
                                     chan_mask, col_format, vmid, &written)) {
            g_free(px); g_free(py);
            return false;
        }
        g_free(px); g_free(py);
    }
    s->pstat[2] += 1;                                  /* C_INVOCATIONS */
    s->pstat[1] += 1;                                  /* C_PRIMITIVES */
    s->pstat[0] += written;                            /* PS_INVOCATIONS */
    fprintf(stderr, "rdna4: gfx: draw: primitives=1 pixels shaded=%u time=%" PRIu64 " ns\n",
            written, qemu_clock_get_ns(QEMU_CLOCK_HOST) - start_ns);
    dpy_gfx_update_full(s->con);
    return true;
}

static bool rdna4_gfx_fault_enabled(RDNA4State *s, uint32_t source)
{
    uint32_t cntl = reg_get(s, REG_GFX_CP_INT_CNTL_RING0);

    switch (source) {
    case 183:
        return (cntl & CP_OPCODE_ERROR_INT_ENABLE) != 0;
    case 184:
        return (cntl & CP_PRIV_REG_INT_ENABLE) != 0;
    case 185:
        return (cntl & CP_PRIV_INSTR_INT_ENABLE) != 0;
    default:
        return false;
    }
}

static void rdna4_gfx_fault(RDNA4State *s, uint32_t vmid, uint32_t source,
                            const char *name, uint64_t at)
{
    fprintf(stderr, "rdna4: gfx: %s at packet dword 0x%" PRIx64
            " (VMID %u), stopping\n", name, at, vmid);
    if (rdna4_gfx_fault_enabled(s, source))
        rdna4_ih_emit_vmid(s, 0x14, source, 0, vmid, s->gfx_job_seq);
}

static bool rdna4_gfx_reg_allowed(uint64_t dword)
{
    return (dword >= 0x2c00 && dword < 0x3000) ||
           (dword >= 0xa000 && dword < 0xc000) ||
           (dword >= 0xc000 && dword <= 0xffff);
}

static bool rdna4_gfx_reg_range_valid(uint64_t dword, uint32_t count)
{
    /* PM4 register operands are SOC15 dword offsets; the BAR image is bytes. */
    return count && dword < RDNA4_MMIO_SIZE / 4 &&
           (uint64_t)count <= RDNA4_MMIO_SIZE / 4 - dword;
}

static bool rdna4_gfx_set_regs(RDNA4State *s, RDNA4GfxStream *st,
                               uint32_t op, uint32_t count)
{
    uint32_t start, base;

    if (!count || !rdna4_gfx_stream_dw(s, st, st->pos + 1, &start)) {
        fprintf(stderr, "rdna4: gfx: SET_* packet has no register offset, stopping\n");
        return false;
    }
    base = op == 0x79 ? 0xc000 : op == 0x69 ? 0xa000 : 0x2c00;
    if (!st->priv && op == 0x79 &&
        ((uint64_t)base + start < 0xc000 ||
         (uint64_t)base + start + count - 1 > 0xffff)) {
        fprintf(stderr, "rdna4: gfx: SET_UCONFIG_REG 0x%08x..0x%08x is privileged, stopping\n",
                base + start, base + start + count - 1);
        rdna4_gfx_fault(s, st->vmid, 184, "PRIV_REG", st->pos);
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t value;
        uint64_t dword = (uint64_t)base + start + i;

        if (!rdna4_gfx_stream_dw(s, st, st->pos + 2 + i, &value) ||
            dword > RDNA4_MMIO_SIZE / 4 - 1) {
            fprintf(stderr, "rdna4: gfx: SET_* register packet crosses unmapped MMIO, stopping\n");
            return false;
        }
        reg_set(s, (uint32_t)dword * 4, value);
    }
    return true;
}

static void rdna4_gfx_rptr_writeback(RDNA4State *s, uint32_t rptr);
static bool rdna4_gfx_packets(RDNA4State *s, RDNA4GfxStream *st, bool allow_ib,
                              uint64_t deadline_ns, bool ring);
static bool rdna4_gfx_trace_selfcheck(RDNA4State *s);

static bool rdna4_gfx_packets(RDNA4State *s, RDNA4GfxStream *st, bool allow_ib,
                              uint64_t deadline_ns, bool ring)
{
    while (st->pos < st->end) {
        uint64_t at = st->pos;
        uint32_t hdr, op, count, len;

        if (deadline_ns && qemu_clock_get_ns(QEMU_CLOCK_REALTIME) >= deadline_ns) {
            s->gfx_budget_hit = true;
            return true;
        }

        if (!rdna4_gfx_stream_dw(s, st, at, &hdr)) {
            fprintf(stderr, "rdna4: gfx: packet at dword %" PRIu64 " is not mapped, stopping\n", at);
            return false;
        }
        op = (hdr >> 8) & 0xff;
        count = (hdr >> 16) & 0x3fff;
        len = op == 0x10 && count == 0x3fff ? 1 : count + 2;
        if ((hdr >> 30) != 3) {
            fprintf(stderr, "rdna4: gfx: not a type-3 packet 0x%08x at dword %" PRIu64 ", stopping\n",
                    hdr, at);
            return false;
        }
        if (st->end - at < len) {
            fprintf(stderr, "rdna4: gfx: packet 0x%02x at dword %" PRIu64 " crosses the wptr, stopping\n",
                    op, at);
            return false;
        }
        switch (op) {
        case 0x10:                                           /* NOP */
            break;
        case 0x79:                                           /* SET_UCONFIG_REG */
        case 0x69:                                           /* SET_CONTEXT_REG */
        case 0x76:                                           /* SET_SH_REG */
            if (!rdna4_gfx_set_regs(s, st, op, count)) {
                return false;
            }
            break;
        case 0x28:                                           /* CONTEXT_CONTROL */
            if (count != 1 ||
                !rdna4_gfx_stream_dw(s, st, at + 1, &hdr) || hdr != 0x80000000 ||
                !rdna4_gfx_stream_dw(s, st, at + 2, &hdr) ||
                (hdr != 0 && hdr != 0x80000000)) {
                fprintf(stderr, "rdna4: gfx: CONTEXT_CONTROL length %u refused, stopping\n", len);
                return false;
            }
            break;
        case 0x46:                                           /* EVENT_WRITE */
            if (count != 0 && count != 2) {
                fprintf(stderr, "rdna4: gfx: EVENT_WRITE length %u refused, stopping\n", len);
                return false;
            }
            if (count == 2) {
                uint32_t ev, lo, hi;

                if (!rdna4_gfx_stream_dw(s, st, at + 1, &ev) || !rdna4_gfx_stream_dw(s, st, at + 2, &lo) ||
                    !rdna4_gfx_stream_dw(s, st, at + 3, &hi)) {
                    return false;
                }
                if ((ev & 0xff) == 0x1e && !rdna4_gfx_mem_write(s, ((uint64_t)hi << 32) | (lo & ~7u), s->pstat,
                                                                  sizeof(s->pstat), st->vmid)) {
                    fprintf(stderr, "rdna4: gfx: SAMPLE_PIPELINESTAT to 0x%" PRIx64 " refused, stopping\n",
                            ((uint64_t)hi << 32) | lo);
                    return false;
                }
            }
            break;
        case 0x58:                                           /* ACQUIRE_MEM */
            if (count != 6) {
                fprintf(stderr, "rdna4: gfx: ACQUIRE_MEM length %u refused, stopping\n", len);
                return false;
            }
            break;
        case 0x42:                                           /* PFP_SYNC_ME */
            if (count != 0) {
                fprintf(stderr, "rdna4: gfx: PFP_SYNC_ME length %u refused, stopping\n", len);
                return false;
            }
            break;
        case 0x3c: {                                         /* WAIT_REG_MEM */
            uint32_t ctl, lo, hi, ref, mask, poll, value = 0;
            uint64_t addr, reg_dword;
            uint32_t function, operation, mem_space;
            bool matched;

            if (count != 5 ||
                !rdna4_gfx_stream_dw(s, st, at + 1, &ctl) ||
                !rdna4_gfx_stream_dw(s, st, at + 2, &lo) ||
                !rdna4_gfx_stream_dw(s, st, at + 3, &hi) ||
                !rdna4_gfx_stream_dw(s, st, at + 4, &ref) ||
                !rdna4_gfx_stream_dw(s, st, at + 5, &mask) ||
                !rdna4_gfx_stream_dw(s, st, at + 6, &poll)) {
                fprintf(stderr, "rdna4: gfx: WAIT_REG_MEM length %u refused, stopping\n", len);
                return false;
            }
            function = ctl & 7;
            mem_space = (ctl >> 4) & 3;
            operation = (ctl >> 6) & 3;
            addr = (uint64_t)lo | ((uint64_t)hi << 32);
            reg_dword = addr;
            if (operation == 1) {
                if (function != 3 || mem_space != 0 ||
                    lo != REG_GCVM_INV0_REQ / 4 ||
                    hi != REG_GCVM_INV0_ACK / 4 ||
                    ref != ((1u << st->vmid) | 0x00f80000u) ||
                    mask != (1u << st->vmid)) {
                    fprintf(stderr, "rdna4: gfx: WAIT_REG_MEM invalidate form refused, stopping\n");
                    return false;
                }
                reg_set(s, REG_GCVM_INV0_REQ, ref);
                reg_set(s, REG_GCVM_INV0_ACK, mask);
                break;
            }
            if (operation != 0 || (function != 3 && function != 4 && function != 5) ||
                mem_space > 1) {
                fprintf(stderr, "rdna4: gfx: WAIT_REG_MEM operation/function refused, stopping\n");
                return false;
            }
            if (mem_space) {
                if (!rdna4_gfx_mem_read(s, addr, &value, sizeof(value), st->vmid, false)) {
                    fprintf(stderr, "rdna4: gfx: WAIT_REG_MEM address 0x%" PRIx64
                            " is unmapped, stopping\n", addr);
                    return false;
                }
            } else if (rdna4_gfx_reg_range_valid(reg_dword, 1)) {
                value = reg_get(s, (uint32_t)reg_dword * 4);
            } else {
                fprintf(stderr, "rdna4: gfx: WAIT_REG_MEM register dword 0x%" PRIx64
                        " is unmapped, stopping\n", addr);
                return false;
            }
            switch (function) {
            case 3: matched = (value & mask) == (ref & mask); break;
            case 4: matched = (value & mask) != (ref & mask); break;
            default: matched = (value & mask) >= (ref & mask); break;
            }
            if (!matched) {
                fprintf(stderr, "rdna4: gfx: WAIT_REG_MEM poll 0x%08x did not match, stopping\n",
                        value);
                return false;
            }
            (void)poll;
            break;
        }
        case 0x37: {                                         /* WRITE_DATA */
            uint32_t ctl, lo, hi, value, n;
            uint64_t addr, reg_dword;

            if (count < 3 || !rdna4_gfx_stream_dw(s, st, at + 1, &ctl) ||
                !rdna4_gfx_stream_dw(s, st, at + 2, &lo) ||
                !rdna4_gfx_stream_dw(s, st, at + 3, &hi)) {
                fprintf(stderr, "rdna4: gfx: WRITE_DATA length %u refused, stopping\n", len);
                return false;
            }
            n = count - 2;
            addr = (uint64_t)(lo & ~3u) | ((uint64_t)hi << 32);
            reg_dword = (uint64_t)lo | ((uint64_t)hi << 32);
            if (((ctl >> 8) & 0xf) == 5) {
                g_autofree uint8_t *bytes = g_malloc(4ull * n);

                if (!rdna4_gfx_mem_mapped(s, addr, 4ull * n, st->vmid, true, false)) {
                    fprintf(stderr, "rdna4: gfx: WRITE_DATA memory 0x%" PRIx64 " refused, stopping\n", addr);
                    return false;
                }
                for (uint32_t i = 0; i < n; i++) {
                    if (!rdna4_gfx_stream_dw(s, st, at + 4 + i, &value)) {
                        return false;
                    }
                    stl_le_p(bytes + 4 * i, value);
                }
                if (!rdna4_gfx_mem_write(s, addr, bytes, 4ull * n, st->vmid)) {
                    return false;
                }
            } else if (((ctl >> 8) & 0xf) == 0 &&
                       rdna4_gfx_reg_range_valid(reg_dword, n)) {
                if (!st->priv) {
                    for (uint32_t i = 0; i < n; i++) {
                        if (!rdna4_gfx_reg_allowed(reg_dword + i)) {
                            rdna4_gfx_fault(s, st->vmid, 184, "PRIV_REG", at);
                            return false;
                        }
                    }
                }
                for (uint32_t i = 0; i < n; i++) {
                    if (!rdna4_gfx_stream_dw(s, st, at + 4 + i, &value)) {
                        return false;
                    }
                    reg_set(s, (uint32_t)(reg_dword + i) * 4, value);
                }
            } else {
                fprintf(stderr, "rdna4: gfx: WRITE_DATA DST_SEL %u or register dword refused, stopping\n",
                        (ctl >> 8) & 0xf);
                return false;
            }
            break;
        }
        case 0x40: {                                         /* COPY_DATA */
            uint32_t ctl, src_lo, src_hi, dst_lo, dst_hi, src_sel, dst_sel;
            uint32_t value;
            uint64_t src, dst, src_dword, dst_dword;

            if (count != 4 ||
                !rdna4_gfx_stream_dw(s, st, at + 1, &ctl) ||
                !rdna4_gfx_stream_dw(s, st, at + 2, &src_lo) ||
                !rdna4_gfx_stream_dw(s, st, at + 3, &src_hi) ||
                !rdna4_gfx_stream_dw(s, st, at + 4, &dst_lo) ||
                !rdna4_gfx_stream_dw(s, st, at + 5, &dst_hi)) {
                fprintf(stderr, "rdna4: gfx: COPY_DATA length %u refused, stopping\n", len);
                return false;
            }
            src_sel = ctl & 0xf;
            dst_sel = (ctl >> 8) & 0xf;
            src_dword = (uint64_t)src_lo | ((uint64_t)src_hi << 32);
            dst_dword = (uint64_t)dst_lo | ((uint64_t)dst_hi << 32);
            if ((dst_sel == 0 || dst_sel == 4) && !st->priv &&
                (dst_sel == 4 || !rdna4_gfx_reg_range_valid(dst_dword, 1) ||
                 !rdna4_gfx_reg_allowed(dst_dword))) {
                rdna4_gfx_fault(s, st->vmid, 184, "PRIV_REG", at);
                return false;
            }
            src = (uint64_t)src_lo | ((uint64_t)src_hi << 32);
            dst = (uint64_t)dst_lo | ((uint64_t)dst_hi << 32);
            if (src_sel == 5) {
                value = src_lo;
            } else if (src_sel == 0 && rdna4_gfx_reg_range_valid(src_dword, 1)) {
                value = reg_get(s, (uint32_t)src_dword * 4);
            } else if (src_sel == 1 && rdna4_gfx_mem_read(s, src, &value, sizeof(value),
                                                            st->vmid, false)) {
                /* value loaded below */
            } else {
                fprintf(stderr, "rdna4: gfx: COPY_DATA source selector %u refused, stopping\n",
                        src_sel);
                return false;
            }
            if (dst_sel == 0 && rdna4_gfx_reg_range_valid(dst_dword, 1)) {
                reg_set(s, (uint32_t)dst_dword * 4, value);
            } else if (dst_sel == 5 && rdna4_gfx_mem_write(s, dst, &value, sizeof(value),
                                                            st->vmid)) {
                /* completed */
            } else {
                fprintf(stderr, "rdna4: gfx: COPY_DATA destination selector %u refused, stopping\n",
                        dst_sel);
                return false;
            }
            break;
        }
        case 0x49: {                                         /* RELEASE_MEM */
            uint32_t event_ctl, ctl, sel, int_sel, lo, hi, value;
            uint64_t addr;
            uint8_t bytes[8];

            if (count != 6 || !rdna4_gfx_stream_dw(s, st, at + 1, &event_ctl) ||
                !rdna4_gfx_stream_dw(s, st, at + 2, &ctl) ||
                !rdna4_gfx_stream_dw(s, st, at + 3, &lo) ||
                !rdna4_gfx_stream_dw(s, st, at + 4, &hi) ||
                !rdna4_gfx_stream_dw(s, st, at + 5, &value)) {
                fprintf(stderr, "rdna4: gfx: RELEASE_MEM length %u refused, stopping\n", len);
                return false;
            }
            sel = (ctl >> 29) & 3;
            int_sel = (ctl >> 24) & 3;
            if (sel == 0 && (event_ctl & (1u << 31))) {       /* PWS wait-for-idle */
                break;
            }
            if (sel != 1 && sel != 2) {
                fprintf(stderr, "rdna4: gfx: RELEASE_MEM DATA_SEL %u refused, stopping\n", sel);
                return false;
            }
            addr = (uint64_t)(lo & ~3u) | ((uint64_t)hi << 32);
            if (!rdna4_gfx_mem_mapped(s, addr, sel == 2 ? 8 : 4, st->vmid, true, false)) {
                fprintf(stderr, "rdna4: gfx: RELEASE_MEM address 0x%" PRIx64 " refused, stopping\n", addr);
                return false;
            }
            if (sel == 2) {
                uint32_t hi_value;
                if (!rdna4_gfx_stream_dw(s, st, at + 6, &hi_value)) {
                    return false;
                }
                stq_le_p(bytes, value | ((uint64_t)hi_value << 32));
            } else {
                stl_le_p(bytes, value);
            }
            if (!rdna4_gfx_mem_write(s, addr, bytes, sel == 2 ? 8 : 4, st->vmid)) {
                return false;
            }
            s->gfx_job_seq = value;
            if (int_sel && (reg_get(s, REG_GFX_CP_INT_CNTL_RING0) &
                            (CP_TIME_STAMP_INT_ENABLE | CP_GENERIC0_INT_ENABLE)) ==
                           (CP_TIME_STAMP_INT_ENABLE | CP_GENERIC0_INT_ENABLE)) {
                rdna4_ih_emit_vmid(s, 0x14, 181, 0, st->vmid, value);
            } else if (int_sel && s->trace) {
                fprintf(stderr, "rdna4: gfx: RELEASE_MEM INT_SEL %u masked by CP_INT_CNTL_RING0\n",
                        int_sel);
            }
            break;
        }
        case 0x3f: {                                         /* INDIRECT_BUFFER */
            uint32_t lo, hi, ctl, ib_len, vmid;
            uint64_t addr;
            RDNA4GfxStream ib;

            if (!allow_ib || count != 2 ||
                !rdna4_gfx_stream_dw(s, st, at + 1, &lo) ||
                !rdna4_gfx_stream_dw(s, st, at + 2, &hi) ||
                !rdna4_gfx_stream_dw(s, st, at + 3, &ctl)) {
                fprintf(stderr, "rdna4: gfx: INDIRECT_BUFFER packet is nested or has wrong length, stopping\n");
                return false;
            }
            addr = (uint64_t)(lo & ~3u) | ((uint64_t)hi << 32);
            ib_len = ctl & 0xfffff;
            vmid = (ctl >> 24) & 0xf;
            if (lo & 3) {
                fprintf(stderr, "rdna4: gfx: INDIRECT_BUFFER address is not dword aligned, stopping\n");
                return false;
            }
            if (ctl & ((1u << 20) | (1u << 21) | (1u << 23))) {
                fprintf(stderr, "rdna4: gfx: INDIRECT_BUFFER VALID/CHAIN/PRE_ENB form refused, stopping\n");
                return false;
            }
            if (!ib_len || (uint64_t)ib_len * 4 / 4 != ib_len ||
                !rdna4_gfx_mem_mapped(s, addr, (uint64_t)ib_len * 4, vmid, false, false)) {
                fprintf(stderr, "rdna4: gfx: INDIRECT_BUFFER 0x%" PRIx64 " crosses unmapped memory, stopping\n",
                        addr);
                return false;
            }
            ib.base = addr;
            ib.pos = 0;
            ib.end = ib_len;
            ib.ring_dw = 0;
            ib.vmid = vmid;
            ib.priv = (ctl >> 31) & 1;
            if (!rdna4_gfx_packets(s, &ib, false, 0, false)) {
                return false;
            }
            if (s->gfx_budget_hit) {
                return true;
            }
            break;
        }
        case 0x2f:                                           /* NUM_INSTANCES */
            if (count != 0 || !rdna4_gfx_stream_dw(s, st, at + 1,
                                                   &s->gfx_num_instances)) {
                fprintf(stderr, "rdna4: gfx: NUM_INSTANCES length %u refused, stopping\n", len);
                return false;
            }
            break;
        case 0x2d: {                                         /* DRAW_INDEX_AUTO */
            uint32_t index_count, initiator;
            bool draw_ok;

            if (count != 1 || !rdna4_gfx_stream_dw(s, st, at + 1, &index_count) ||
                !rdna4_gfx_stream_dw(s, st, at + 2, &initiator) ||
                (initiator & 3) != 2) {
                fprintf(stderr, "rdna4: gfx: DRAW_INDEX_AUTO SOURCE_SELECT or length refused, stopping\n");
                return false;
            }
            draw_ok = rdna4_gfx_draw(s, index_count, st->vmid);
            if (!draw_ok && !s->gfx_draw_refused)
                return false;
            if (draw_ok && st->vmid == 0 && s->gfx_trace && !s->gfx_trace_selfcheck_done &&
                !rdna4_gfx_trace_selfcheck(s)) {
                return false;
            }
            break;
        }
        default:
            rdna4_gfx_fault(s, st->vmid, 183, "OPCODE_ERROR", at);
            return false;
        }
        st->pos += len;
        if (ring) {
            s->gfx_rptr = st->pos;
            reg_set(s, REG_GFX_CP_RB0_RPTR, (uint32_t)(s->gfx_rptr % st->ring_dw));
            rdna4_gfx_rptr_writeback(s, (uint32_t)(s->gfx_rptr % st->ring_dw));
            if (deadline_ns && qemu_clock_get_ns(QEMU_CLOCK_REALTIME) >= deadline_ns) {
                s->gfx_budget_hit = true;
                return true;                                /* resumable boundary */
            }
        }
    }
    return true;
}

typedef struct RDNA4GfxTraceTable {
    uint64_t key;
    uint64_t off;
} RDNA4GfxTraceTable;

static uint64_t rdna4_gfx_trace_phys(RDNA4State *s, uint64_t off)
{
    return ((uint64_t)(reg_get(s, REG_GCMC_FB_OFFSET) & 0xffffff) << 24) + off;
}

static bool rdna4_gfx_trace_table_entry(RDNA4State *s, uint64_t table,
                                        uint32_t index, uint64_t value)
{
    uint8_t *p = rdna4_vram_span(s, table + (uint64_t)index * 8, 8);

    if (!p)
        return false;
    stq_le_p(p, value);
    return true;
}

static bool rdna4_gfx_trace_new_table(RDNA4State *s, uint64_t *cursor,
                                      uint64_t *table)
{
    uint8_t *p;

    *table = *cursor;
    *cursor += 0x1000;
    p = rdna4_vram_span(s, *table, 0x1000);
    if (!p)
        return false;
    memset(p, 0, 0x1000);
    return true;
}

static bool rdna4_gfx_trace_find_table(RDNA4State *s, uint64_t *cursor,
                                       RDNA4GfxTraceTable *tables,
                                       unsigned *count, unsigned max,
                                       uint64_t key, uint64_t *table)
{
    for (unsigned i = 0; i < *count; i++) {
        if (tables[i].key == key) {
            *table = tables[i].off;
            return true;
        }
    }
    if (*count == max || !rdna4_gfx_trace_new_table(s, cursor, table))
        return false;
    tables[*count].key = key;
    tables[*count].off = *table;
    (*count)++;
    return true;
}

static bool rdna4_gfx_trace_map_page(RDNA4State *s, uint64_t root,
                                     uint64_t *cursor,
                                     RDNA4GfxTraceTable *l1, unsigned *nl1,
                                     RDNA4GfxTraceTable *l2, unsigned *nl2,
                                     RDNA4GfxTraceTable *l3, unsigned *nl3,
                                     uint64_t va, uint64_t mc)
{
    int64_t target_off = rdna4_gc_to_vram(s, mc & ~0xfffull);
    uint32_t i0 = (va >> 39) & 0x1ff;
    uint32_t i1 = (va >> 30) & 0x1ff;
    uint32_t i2 = (va >> 21) & 0x1ff;
    uint32_t i3 = (va >> 12) & 0x1ff;
    uint64_t t1, t2, t3;

    if (target_off < 0 || (uint64_t)target_off >= rdna4_vram_size())
        return false;
    if (!rdna4_gfx_trace_find_table(s, cursor, l1, nl1, 32, i0, &t1) ||
        !rdna4_gfx_trace_table_entry(s, root, i0,
                                     rdna4_gfx_trace_phys(s, t1) | RDNA4_VM_VALID))
        return false;
    if (!rdna4_gfx_trace_find_table(s, cursor, l2, nl2, 64,
                                    ((uint64_t)i0 << 9) | i1, &t2) ||
        !rdna4_gfx_trace_table_entry(s, t1, i1,
                                     rdna4_gfx_trace_phys(s, t2) | RDNA4_VM_VALID))
        return false;
    if (!rdna4_gfx_trace_find_table(s, cursor, l3, nl3, 128,
                                    ((uint64_t)i0 << 18) | ((uint64_t)i1 << 9) | i2,
                                    &t3) ||
        !rdna4_gfx_trace_table_entry(s, t2, i2,
                                     rdna4_gfx_trace_phys(s, t3) | RDNA4_VM_VALID))
        return false;
    return rdna4_gfx_trace_table_entry(s, t3, i3,
        rdna4_gfx_trace_phys(s, (uint64_t)target_off) |
        RDNA4_VM_VALID | RDNA4_VM_READABLE | RDNA4_VM_WRITEABLE | RDNA4_VM_EXECUTABLE);
}

static bool rdna4_gfx_trace_map_range(RDNA4State *s, uint64_t root,
                                      uint64_t *cursor,
                                      RDNA4GfxTraceTable *l1, unsigned *nl1,
                                      RDNA4GfxTraceTable *l2, unsigned *nl2,
                                      RDNA4GfxTraceTable *l3, unsigned *nl3,
                                      uint64_t va, uint64_t mc, uint64_t len)
{
    uint64_t first_va = va & ~0xfffull;
    uint64_t first_mc = mc & ~0xfffull;
    uint64_t end = (va & 0xfffull) + len;

    if (!len || end < len)
        return false;
    for (uint64_t off = 0; off < end; off += 0x1000) {
        if (!rdna4_gfx_trace_map_page(s, root, cursor, l1, nl1, l2, nl2,
                                      l3, nl3, first_va + off, first_mc + off))
            return false;
    }
    return true;
}

static uint32_t rdna4_gfx_trace_ib_ctl(uint32_t length, uint32_t vmid)
{
    return (length & 0xfffff) | ((vmid & 0xf) << 24);
}

static bool rdna4_gfx_trace_selfcheck(RDNA4State *s)
{
    RDNA4GfxTraceTable l1[32] = { 0 }, l2[64] = { 0 }, l3[128] = { 0 };
    uint64_t old_base, root_off, cursor, packet_off, draw_mc, priv_mc, bad_mc;
    uint64_t root_mc, cb, pos, prim, es, ps;
    uint64_t end = rdna4_vram_size() - RDNA4_RESV_SIZE;
    uint32_t old_vm[8], old_int, old_seq;
    unsigned nl1 = 0, nl2 = 0, nl3 = 0;
    uint32_t attrib2, pitch, height, pos_bytes, prim_bytes;
    uint8_t *p;
    bool draw_ok = false, priv_ok = false, bad_ok = false, pass = false;
    bool ih_ready;

    s->gfx_trace_selfcheck_done = true;
    old_vm[0] = reg_get(s, REG_GCVM_CTX1_CNTL + 7 * 4);
    old_vm[1] = reg_get(s, REG_GCVM_CTX1_BASE_LO + 7 * 8);
    old_vm[2] = reg_get(s, REG_GCVM_CTX1_BASE_HI + 7 * 8);
    old_vm[3] = reg_get(s, REG_GCVM_CTX1_START_LO + 7 * 8);
    old_vm[4] = reg_get(s, REG_GCVM_CTX1_START_HI + 7 * 8);
    old_vm[5] = reg_get(s, REG_GCVM_CTX1_END_LO + 7 * 8);
    old_vm[6] = reg_get(s, REG_GCVM_CTX1_END_HI + 7 * 8);
    old_vm[7] = s->gfx_job_seq;
    old_int = reg_get(s, REG_GFX_CP_INT_CNTL_RING0);
    old_seq = s->gfx_job_seq;

    if (end < 0x100000 || !s->hidden || s->hidden_size < 0x100000) {
        fprintf(stderr, "rdna4: gfx: trace self-check unavailable (no scratch VRAM)\n");
        return false;
    }
    old_base = ((uint64_t)reg_get(s, REG_GCMC_FB_BASE) & 0xffffff) << 24;
    root_off = (end - 0x100000) & ~0xfffull;
    cursor = root_off + 0x1000;
    packet_off = root_off - 0x4000;
    root_mc = old_base + packet_off;
    draw_mc = root_mc + 0x1000;
    priv_mc = root_mc + 0x2000;
    bad_mc = root_mc + 0x3000;
    old_seq = s->gfx_job_seq;

    es = ((uint64_t)reg_get(s, REG_GFX_SPI_SHADER_PGM_LO_ES) << 8) |
         ((uint64_t)reg_get(s, REG_GFX_SPI_SHADER_PGM_HI_ES) << 40);
    ps = ((uint64_t)reg_get(s, REG_GFX_SPI_SHADER_PGM_LO_PS) << 8) |
         ((uint64_t)reg_get(s, REG_GFX_SPI_SHADER_PGM_HI_PS) << 40);
    cb = ((uint64_t)reg_get(s, REG_GFX_CB_COLOR0_BASE) << 8) |
         ((uint64_t)(reg_get(s, REG_GFX_CB_COLOR0_BASE_EXT) & 0xff) << 40);
    pos = (uint64_t)reg_get(s, REG_GFX_GE_POS_RING_BASE) << 16;
    prim = (uint64_t)reg_get(s, REG_GFX_GE_PRIM_RING_BASE) << 16;
    attrib2 = reg_get(s, REG_GFX_CB_COLOR0_ATTRIB2);
    pitch = ((attrib2 & 0xffff) + 1) * 4;
    height = ((attrib2 >> 16) & 0xffff) + 1;
    pos_bytes = (reg_get(s, REG_GFX_GE_POS_RING_SIZE) & 0x3fff) << 5;
    prim_bytes = (reg_get(s, REG_GFX_GE_PRIM_RING_SIZE) & 0x7ff) << 5;

    p = rdna4_vram_span(s, root_off, 0x1000);
    if (!p)
        goto restore_vm;
    memset(p, 0, 0x1000);
    if (!rdna4_gfx_trace_map_range(s, root_off, &cursor,
                                   l1, &nl1, l2, &nl2, l3, &nl3,
                                   root_mc, root_mc, 0x4000) ||
        !rdna4_gfx_trace_map_range(s, root_off, &cursor,
                                   l1, &nl1, l2, &nl2, l3, &nl3,
                                   es, es, 0x2000) ||
        !rdna4_gfx_trace_map_range(s, root_off, &cursor,
                                   l1, &nl1, l2, &nl2, l3, &nl3,
                                   ps, ps, 0x2000) ||
        !rdna4_gfx_trace_map_range(s, root_off, &cursor,
                                   l1, &nl1, l2, &nl2, l3, &nl3,
                                   pos, pos, pos_bytes) ||
        !rdna4_gfx_trace_map_range(s, root_off, &cursor,
                                   l1, &nl1, l2, &nl2, l3, &nl3,
                                   prim, prim, prim_bytes) ||
        !rdna4_gfx_trace_map_range(s, root_off, &cursor,
                                   l1, &nl1, l2, &nl2, l3, &nl3,
                                   cb, cb, (uint64_t)pitch * height)) {
        fprintf(stderr, "rdna4: gfx: trace self-check failed to build VMID8 map\n");
        return false;
    }
    if (cursor + 0x1000 > end) {
        fprintf(stderr, "rdna4: gfx: trace self-check page tables overflow scratch VRAM\n");
        return false;
    }
    reg_set(s, REG_GCVM_CTX1_CNTL + 7 * 4, 7);
    reg_set(s, REG_GCVM_CTX1_BASE_LO + 7 * 8, (uint32_t)rdna4_gfx_trace_phys(s, root_off));
    reg_set(s, REG_GCVM_CTX1_BASE_HI + 7 * 8, (uint32_t)(rdna4_gfx_trace_phys(s, root_off) >> 32));
    reg_set(s, REG_GCVM_CTX1_START_LO + 7 * 8, 0);
    reg_set(s, REG_GCVM_CTX1_START_HI + 7 * 8, 0);
    reg_set(s, REG_GCVM_CTX1_END_LO + 7 * 8, 0xffffffff);
    reg_set(s, REG_GCVM_CTX1_END_HI + 7 * 8, 0xffff);

    /* The root stream points at a VMID-8 draw IB, so this covers the same
     * packet path that a user submission takes rather than calling draw() by
     * itself. */
    p = rdna4_vram_span(s, packet_off, 0x4000);
    if (!p) {
        fprintf(stderr, "rdna4: gfx: trace self-check packet scratch is unmapped\n");
        goto restore_vm;
    }
    memset(p, 0, 0x4000);
    stl_le_p(p + 0, 0xc0023f00);
    stl_le_p(p + 4, (uint32_t)draw_mc);
    stl_le_p(p + 8, (uint32_t)(draw_mc >> 32));
    stl_le_p(p + 12, rdna4_gfx_trace_ib_ctl(3, 8));
    stl_le_p(p + 0x1000, 0xc0012d00);
    stl_le_p(p + 0x1004, 3);
    stl_le_p(p + 0x1008, 2);
    RDNA4GfxStream st = { root_mc, 0, 4, 0, 0, true };
    draw_ok = rdna4_gfx_packets(s, &st, true, 0, false) && st.pos == 4;
    fprintf(stderr, "rdna4: gfx: trace self-check VMID8 draw %s\n",
            draw_ok ? "PASS" : "FAIL");

    /* Each bad IB is reached through INDIRECT_BUFFER, and both failures are
     * before the parent packet can commit its cursor. */
    stl_le_p(p + 0x2000, 0xc0033700);
    stl_le_p(p + 0x2004, 0x00100000);
    stl_le_p(p + 0x2008, REG_GC_CP_STAT / 4);
    stl_le_p(p + 0x200c, 0);
    stl_le_p(p + 0x2010, 0x12345678);
    stl_le_p(p + 0, 0xc0023f00);
    stl_le_p(p + 4, (uint32_t)priv_mc);
    stl_le_p(p + 8, (uint32_t)(priv_mc >> 32));
    stl_le_p(p + 12, rdna4_gfx_trace_ib_ctl(5, 8));
    old_int = reg_get(s, REG_GFX_CP_INT_CNTL_RING0);
    reg_set(s, REG_GFX_CP_INT_CNTL_RING0, old_int | CP_PRIV_REG_INT_ENABLE |
            CP_OPCODE_ERROR_INT_ENABLE | CP_TIME_STAMP_INT_ENABLE | CP_GENERIC0_INT_ENABLE);
    ih_ready = rdna4_ih_ready(s);
    uint32_t old_ih = s->ih_wptr;
    st = (RDNA4GfxStream){ root_mc, 0, 4, 0, 0, true };
    priv_ok = !rdna4_gfx_packets(s, &st, true, 0, false) && st.pos == 0 &&
              (!ih_ready || s->ih_wptr == ((old_ih + IH_ENTRY_BYTES) &
                                             (IH_RING_BYTES - 1)));

    stl_le_p(p + 0x3000, 0xc000ff00);
    stl_le_p(p + 0x3004, 0);
    stl_le_p(p + 0, 0xc0023f00);
    stl_le_p(p + 4, (uint32_t)bad_mc);
    stl_le_p(p + 8, (uint32_t)(bad_mc >> 32));
    stl_le_p(p + 12, rdna4_gfx_trace_ib_ctl(2, 8));
    old_ih = s->ih_wptr;
    st = (RDNA4GfxStream){ root_mc, 0, 4, 0, 0, true };
    bad_ok = !rdna4_gfx_packets(s, &st, true, 0, false) && st.pos == 0 &&
             (!ih_ready || s->ih_wptr == ((old_ih + IH_ENTRY_BYTES) &
                                            (IH_RING_BYTES - 1)));
    fprintf(stderr, "rdna4: gfx: trace self-check PRIV_REG IH %s; OPCODE_ERROR IH %s\n",
            ih_ready ? (priv_ok ? "PASS" : "FAIL") : "SKIP (ring disabled)",
            ih_ready ? (bad_ok ? "PASS" : "FAIL") : "SKIP (ring disabled)");
    pass = draw_ok && priv_ok && bad_ok;

restore_vm:
    reg_set(s, REG_GCVM_CTX1_CNTL + 7 * 4, old_vm[0]);
    reg_set(s, REG_GCVM_CTX1_BASE_LO + 7 * 8, old_vm[1]);
    reg_set(s, REG_GCVM_CTX1_BASE_HI + 7 * 8, old_vm[2]);
    reg_set(s, REG_GCVM_CTX1_START_LO + 7 * 8, old_vm[3]);
    reg_set(s, REG_GCVM_CTX1_START_HI + 7 * 8, old_vm[4]);
    reg_set(s, REG_GCVM_CTX1_END_LO + 7 * 8, old_vm[5]);
    reg_set(s, REG_GCVM_CTX1_END_HI + 7 * 8, old_vm[6]);
    reg_set(s, REG_GFX_CP_INT_CNTL_RING0, old_int);
    s->gfx_job_seq = old_seq;
    return pass;
}

static void rdna4_gfx_rptr_writeback(RDNA4State *s, uint32_t rptr)
{
    uint64_t addr = (uint64_t)(reg_get(s, REG_GFX_CP_RB0_RPTR_ADDR) & ~3u) |
                    ((uint64_t)(reg_get(s, REG_GFX_CP_RB0_RPTR_HI) & 0xffff) << 32);
    uint8_t *p = rdna4_gc_span(s, addr, 4);

    if (p) {
        stl_le_p(p, rptr);
    } else if (addr) {
        fprintf(stderr, "rdna4: gfx: rptr writeback MC 0x%" PRIx64 " is not mapped\n", addr);
    }
}

/* G3's packet-boundary graphics engine, run by the W11 bottom half.  The
 * cursor is committed only after a complete PM4 packet, so a realtime slice
 * can yield without inventing a half-packet state machine. */
static bool rdna4_gfx_process(RDNA4State *s, uint64_t deadline_ns)
{
    const char *why;
    uint32_t cntl, ring_dw, rptr_slot;
    uint64_t base, available;
    RDNA4GfxStream st;

    if (!s->gfx_active) {
        if (!s->gfx_pending)
            return false;
        s->gfx_pending = false;
        if (!rdna4_gfx_ready(s, s->gfx_pending_doorbell,
                             s->gfx_pending_doorbell ? GFX_DOORBELL_DWORD : 0, &why)) {
            fprintf(stderr, "rdna4: gfx: %s kick ignored: %s\n",
                    s->gfx_pending_doorbell ? "doorbell" : "MMIO", why);
            return false;
        }
        cntl = reg_get(s, REG_GFX_CP_RB0_CNTL);
        ring_dw = 2u << (cntl & 0x3f);
        base = ((uint64_t)reg_get(s, REG_GFX_CP_RB0_BASE) << 8) |
               ((uint64_t)reg_get(s, REG_GFX_CP_RB0_BASE_HI) << 40);
        available = s->gfx_pending_wptr - s->gfx_rptr;
        if (!ring_dw || available > ring_dw) {
            fprintf(stderr, "rdna4: gfx: kick ignored: ring overrun (%" PRIu64
                    " dwords for %u)\n", available, ring_dw);
            return false;
        }
        if (s->gfx_hang) {
            reg_set(s, REG_GC_CP_STAT, 1);
            fprintf(stderr, "rdna4: gfx: gfx-hang: kick accepted at wptr 0x%" PRIx64
                    ", ring does not advance\n", s->gfx_pending_wptr);
            return false;
        }
        s->gfx_work_base = base;
        s->gfx_work_pos = s->gfx_rptr;
        s->gfx_work_end = s->gfx_pending_wptr;
        s->gfx_work_ring_dw = ring_dw;
        s->gfx_active = true;
        reg_set(s, REG_GC_CP_STAT, available ? 1 : 0);
    }

    st.base = s->gfx_work_base;
    st.pos = s->gfx_work_pos;
    st.end = s->gfx_work_end;
    st.ring_dw = s->gfx_work_ring_dw;
    st.vmid = 0;
    st.priv = true;
    s->gfx_budget_hit = false;
    if (!rdna4_gfx_packets(s, &st, true, deadline_ns, true)) {
        s->gfx_active = false;
        reg_set(s, REG_GC_CP_STAT, 0);
        return false;
    }
    s->gfx_work_pos = st.pos;
    if (s->gfx_budget_hit)
        return true;
    s->gfx_rptr = st.pos;
    rptr_slot = (uint32_t)(s->gfx_rptr % s->gfx_work_ring_dw);
    reg_set(s, REG_GFX_CP_RB0_RPTR, rptr_slot);
    rdna4_gfx_rptr_writeback(s, rptr_slot);
    reg_set(s, REG_GC_CP_STAT, 0);
    s->gfx_active = false;
    fprintf(stderr, "rdna4: gfx: ring advanced rptr to 0x%x (wptr 0x%" PRIx64 ")\n",
            rptr_slot, s->gfx_work_end);
    return s->gfx_pending;
}

static void rdna4_gfx_wptr(RDNA4State *s, uint64_t wptr, bool doorbell)
{
    if (s->gfx_reinit && wptr == 0) {
        s->gfx_wptr = 0;
        s->gfx_rptr = 0;
        s->gfx_csb_loaded = false;
        s->gfx_reinit = false;
        s->gfx_pending = false;
        s->gfx_active = false;
        s->gfx_pending_wptr = 0;
        s->gfx_work_base = 0;
        s->gfx_work_pos = 0;
        s->gfx_work_end = 0;
        s->gfx_work_ring_dw = 0;
        reg_set(s, REG_GFX_CP_RB0_RPTR, 0);
        reg_set(s, REG_GC_CP_STAT, 0);
        fprintf(stderr, "rdna4: gfx: CP_RB0 re-initialized at WPTR=0\n");
    }
    if (wptr < s->gfx_wptr) {
        fprintf(stderr, "rdna4: gfx: wptr went back from 0x%" PRIx64 " to 0x%" PRIx64
                ": ignored, the engine waits\n", s->gfx_wptr, wptr);
        return;
    }
    s->gfx_wptr = wptr;
    reg_set(s, REG_GFX_CP_RB0_WPTR, (uint32_t)wptr);
    reg_set(s, REG_GFX_CP_RB0_WPTR_HI, (uint32_t)(wptr >> 32));
    if (wptr == s->gfx_rptr) {
        /* amdgpu resume writes WPTR=0 before CP_RB_ACTIVE=1.  It is a
         * register initialization, not a queue kick. */
        return;
    }
    s->gfx_pending_wptr = wptr;
    s->gfx_pending_doorbell = doorbell;
    s->gfx_pending = true;
    rdna4_work_schedule(s);
}

static void rdna4_work_timer(void *opaque)
{
    RDNA4State *s = opaque;

    qemu_bh_schedule(s->work_bh);
}

static void rdna4_work_bh(void *opaque)
{
    RDNA4State *s = opaque;
    const uint64_t deadline = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + RDNA4_WORK_SLICE_NS;
    bool more;

    s->work_slices++;
    more = rdna4_mec_process_slice(s, deadline);
    if (qemu_clock_get_ns(QEMU_CLOCK_REALTIME) < deadline)
        more = rdna4_sdma_process_slice(s, deadline) || more;
    if (qemu_clock_get_ns(QEMU_CLOCK_REALTIME) < deadline)
        more = rdna4_gfx_process(s, deadline) || more;
    if (more)
        timer_mod_ns(s->work_timer, qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + RDNA4_WORK_REARM_NS);
}

static void rdna4_work_schedule(RDNA4State *s)
{
    if (s->work_bh)
        qemu_bh_schedule(s->work_bh);
}

/* A warm platform reset clears PCI Command.BusMaster.  The real platform's
 * firmware/GOP turns it back on while booting, after which preserved engines
 * resume their DMA.  Quiesce leaves bus mastering off, so it never arms this
 * path. */
static void rdna4_warm_gop_timer(void *opaque)
{
    RDNA4State *s = opaque;
    if (!s->warm_gop_restore)
        return;
    s->warm_gop_restore = false;

    PCIDevice *pci = PCI_DEVICE(s);
    if (!rdna4_bus_master_enabled(s)) {
        pci_set_word(pci->config + PCI_COMMAND,
                     pci_get_word(pci->config + PCI_COMMAND) | PCI_COMMAND_MASTER);
        fprintf(stderr, "rdna4: warm-keep: GOP re-enabled PCI bus master after reset\n");
    } else {
        fprintf(stderr, "rdna4: warm-keep: GOP observed PCI bus master already enabled\n");
    }
    rdna4_work_schedule(s);
}

/* BAR2: the doorbell aperture. 64-bit doorbells arrive whole (impl 8). */
static uint64_t rdna4_doorbell_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

#define REG_SDMA0_DOORBELL     GC_SEG0(0x008f)   /* ENABLE [28] */
#define REG_SDMA0_DOORBELL_OFF GC_SEG0(0x0091)   /* OFFSET [27:2]: dword index */
#define REG_NBIF_S2A_ENTRY2    ((0xd20 + 0x01cd) * 4)
#define SDMA0_DOORBELL_DWORD   0x200             /* sDMA_ENGINE0 << 1 */

/*
 * SDMA0's doorbell (sdma_v7_0 always uses one): it counts only if the
 * aperture is on, NBIF S2A entry 2 routes the SDMA range, and the queue has
 * its doorbell enabled at that index. The value is the wptr in bytes.
 */
static void rdna4_sdma_doorbell(RDNA4State *s, uint64_t wptr)
{
    uint32_t e2 = reg_get(s, REG_NBIF_S2A_ENTRY2), db = reg_get(s, REG_SDMA0_DOORBELL);
    const char *why =
        s->sdma_no_db ? "SDMA doorbells ignored (sdma-no-doorbell model)" :
        !(reg_get(s, REG_NBIF_DB_APER_EN) & 1) ? "NBIF doorbell aperture off" :
        !(e2 & 1) || ((e2 >> 7) & 0x3ff) != SDMA0_DOORBELL_DWORD ? "S2A entry 2 not routing SDMA" :
        !(db & (1u << 28)) ? "SDMA0 doorbell disabled" :
        ((reg_get(s, REG_SDMA0_DOORBELL_OFF) >> 2) & 0x3ffffff) != SDMA0_DOORBELL_DWORD ?
            "SDMA0 doorbell offset not its index" : NULL;
    if (why) {
        fprintf(stderr, "rdna4: sdma: doorbell ignored: %s\n", why);
        return;
    }
    rdna4_sdma_wptr(s, wptr);
}

static void rdna4_doorbell_write(void *opaque, hwaddr addr, uint64_t data, unsigned size)
{
    if (rdna4_gfxoff_hazard(opaque, "doorbell write", addr)) {
        return;
    }
    if (addr / 4 == GFX_DOORBELL_DWORD) {
        rdna4_gfx_wptr(opaque, data, true);
        return;
    }
    if (addr / 4 == SDMA0_DOORBELL_DWORD) {
        rdna4_sdma_doorbell(opaque, data);
        return;
    }
    rdna4_mec_doorbell(opaque, addr / 4, data);
}

static const MemoryRegionOps rdna4_doorbell_ops = {
    .read = rdna4_doorbell_read,
    .write = rdna4_doorbell_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 8 },
    .impl = { .min_access_size = 4, .max_access_size = 8 },
};

/* ---- scanout ------------------------------------------------------------ */

/* The EDID's display range limits descriptor (tag 0xfd), if any. */
static bool rdna4_edid_range(RDNA4State *s, uint32_t *min_v, uint32_t *max_v,
                             uint32_t *min_h_khz, uint32_t *max_h_khz,
                             uint32_t *max_pclk_khz)
{
    for (int d = 54; d + 18 <= 126; d += 18) {
        const uint8_t *p = s->edid + d;
        if (!p[0] && !p[1] && p[3] == 0xfd) {
            *min_v = p[5];
            *max_v = p[6];
            *min_h_khz = p[7];
            *max_h_khz = p[8];
            *max_pclk_khz = p[9] * 10000;
            return true;
        }
    }
    return false;
}

/*
 * What the attached monitor makes of OTG `otg`'s output: NULL if it shows
 * a picture, else its complaint. The signal needs an enabled DIG sourcing
 * the OTG, a transmitter on that DIG's link running at the OTG's pixel
 * clock (TMDS, 8 bpc: symbol clock = pixel clock), and a raster inside the
 * EDID's range limits.
 */
static const char *rdna4_monitor_check(RDNA4State *s, int otg, int *dig_out)
{
    uint32_t htot = (rdna4_otg_reg(s, otg, OTG_H_TOTAL) & 0x7fff) + 1;
    uint32_t vtot = (rdna4_otg_reg(s, otg, OTG_V_TOTAL) & 0x7fff) + 1;
    uint32_t min_v, max_v, min_h, max_h, max_pclk;
    uint64_t pclk_hz = (uint64_t)s->pclk_khz[otg] * 1000, vhz_milli, hkhz;
    int dig = -1, link;

    for (int d = 0; d < NUM_DIG && dig < 0; d++) {
        uint32_t o = d * DIG_STRIDE;
        if ((reg_get(s, SEG2(DIG_FE_EN_CNTL + o)) & 1) &&
            (reg_get(s, SEG2(DIG_FE_CNTL + o)) & 7) == (uint32_t)otg) {
            dig = d;
        }
    }
    if (dig < 0) {
        return "rdna4: no signal (no encoder on the OTG)";
    }
    *dig_out = dig;
    if (!(rdna4_otg_reg(s, otg, OTG_CLOCK_CONTROL) & 1) ||
        !(reg_get(s, SEG2(OPTC_INPUT_CLOCK + otg * ODM_STRIDE)) & 2)) {
        return "rdna4: no signal (OTG clock off)";
    }
    {
        uint32_t o = dig * DIG_STRIDE;
        uint32_t fe_clk = reg_get(s, SEG2(DIG_FE_CLK_CNTL + o));
        if ((fe_clk & 7) != DIG_MODE_HDMI || !(fe_clk & 0x10) ||
            !(reg_get(s, SEG2(DIG_FIFO_CTRL0 + o)) & 1)) {
            return "rdna4: no signal (stream encoder off)";
        }
    }
    link = reg_get(s, SEG2(STREAM_MAPPER + dig)) & 7;
    {
        uint32_t o = link * DIG_STRIDE;
        uint32_t be_clk = reg_get(s, SEG2(DIG_BE_CLK_CNTL + o));
        if ((be_clk & 7) != DIG_MODE_HDMI || !(be_clk & 0x10) ||
            !(reg_get(s, SEG2(DIG_BE_EN_CNTL + o)) & 1) ||
            !(reg_get(s, SEG2(DIG_BE_CNTL + o)) & (1u << (8 + dig)))) {
            return "rdna4: no signal (link encoder off)";
        }
    }
    if (!s->symclk_khz[link]) {
        return "rdna4: no signal (transmitter off)";
    }
    if (!pclk_hz || s->symclk_khz[link] != s->pclk_khz[otg]) {
        return "rdna4: out of range (TMDS clock != pixel clock)";
    }
    vhz_milli = pclk_hz * 1000 / ((uint64_t)htot * vtot);
    hkhz = pclk_hz / htot / 1000;
    if (rdna4_edid_range(s, &min_v, &max_v, &min_h, &max_h, &max_pclk) &&
        (vhz_milli < min_v * 1000ull || vhz_milli > max_v * 1000ull ||
         hkhz < min_h || hkhz > max_h || s->pclk_khz[otg] > max_pclk)) {
        return "rdna4: out of range";
    }
    return NULL;
}

/* What the display pipe would put on the wire right now. */
static void rdna4_get_scanout(RDNA4State *s, RDNA4Scanout *so)
{
    memset(so, 0, sizeof(*so));
    so->otg = -1;
    for (int otg = 0; otg < NUM_OTG; otg++) {
        uint32_t hb, vb, opp, pitch, hp;
        uint64_t addr, fb;
        int hubp = -1, dig = -1;

        if (!(rdna4_otg_reg(s, otg, OTG_CONTROL) & 1)) {
            continue;
        }
        hb = rdna4_otg_reg(s, otg, OTG_H_BLANK);
        vb = rdna4_otg_reg(s, otg, OTG_V_BLANK);
        so->active = true;
        so->otg = otg;
        so->width = (hb & 0x7fff) - ((hb >> 16) & 0x7fff);
        so->height = (vb & 0x7fff) - ((vb >> 16) & 0x7fff);
        so->blank = true;
        so->nosignal = rdna4_monitor_check(s, otg, &dig);
        if (so->nosignal) {
            return;
        }
        if (reg_get(s, SEG2(DIG_HDMI_GC + dig * DIG_STRIDE)) & 1) {
            return;                                /* AV mute: the sink shows black */
        }

        opp = (reg_get(s, SEG2(OPTC_DATA_SOURCE + otg * ODM_STRIDE)) >> 16) & 0xf;
        if (opp >= NUM_OTG || (reg_get(s, SEG2(DPG_CONTROL + opp * OPP_STRIDE)) & 1)) {
            return;
        }
        for (int m = 0; m < NUM_OTG && hubp < 0; m++) {
            if ((reg_get(s, SEG3(MPCC_OPP_ID + m * MPCC_STRIDE)) & 0xf) == opp) {
                uint32_t top = reg_get(s, SEG3(MPCC_TOP_SEL + m * MPCC_STRIDE)) & 0xf;
                if (top < NUM_OTG) {
                    hubp = (int)top;
                }
            }
        }
        if (hubp < 0) {
            return;
        }
        hp = hubp * HUBP_STRIDE;
        /* no scaler in the model: the fetched viewport and the scaler's
         * output rectangle have to be the raster's active size */
        if (reg_get(s, SEG2(HUBP_VIEWPORT_DIM + hp)) != ((so->width & 0x3fff) | (so->height << 16)) ||
            reg_get(s, SEG2(DSCL_RECOUT_SIZE + hubp * DPP_STRIDE)) !=
                ((so->width & 0x3fff) | (so->height << 16))) {
            so->nosignal = "rdna4: pipe misprogrammed (viewport/recout != timing)";
            return;
        }
        addr = ((uint64_t)(reg_get(s, SEG2(HUBP_SURFACE_ADDR_HI + hp)) & 0xffff) << 32) |
               reg_get(s, SEG2(HUBP_SURFACE_ADDR + hp));
        fb = (uint64_t)(reg_get(s, SEG2(DCN_VM_FB_LOC_BASE)) & 0xffffff) << 24;
        pitch = (reg_get(s, SEG2(HUBP_SURFACE_PITCH + hp)) & 0xffff) + 1;
        so->stride = pitch * 4;
        if (addr < fb || so->width < 64 || so->height < 64 || pitch < so->width ||
            addr - fb + (uint64_t)so->stride * so->height > rdna4_vram_size()) {
            return;
        }
        so->offset = addr - fb;
        so->blank = false;
        return;
    }
}

/* DCN cursor plane model. The kext must program the complete fetch contract:
 * prefetch REQ_MODE, 64-pixel pitch, premultiplied ARGB mode, a valid VRAM
 * address/size, and FP16 1.0 in both CM_CUR0 scale registers. A half-armed
 * cursor is rejected instead of being silently drawn, which keeps the VM
 * proof honest about the register programming. */
static bool rdna4_get_cursor(RDNA4State *s, const RDNA4Scanout *so, RDNA4Cursor *cursor)
{
    memset(cursor, 0, sizeof(*cursor));
    if (!s->cursor_enabled || !so->active || so->blank || so->nosignal)
        return false;
    const int hubp = rdna4_hubp_for_otg(s, so->otg);
    if (hubp < 0)
        return false;
    /* The MPC cursor lock (regCUR_VUPDATE_LOCK_SET<opp> at MPC dword 0x02c5 + 5 * opp, base idx 3;
     * amdgpu writes it around every cursor update: mpc1_cursor_lock, dc/mpc/dcn10/dcn10_mpc.c:458-463).
     * While it is 1 the cursor registers stay pending and the plane does not change: the model
     * shows no cursor until the driver has released it (cursor-lock-stuck=on starts it held,
     * as a GOP might leave it). */
    if (reg_get(s, SEG3(0x02c5 + 5 * so->otg)) & 1)
        return false;
    const uint32_t hp = hubp * HUBP_STRIDE;
    const uint32_t dpp = hubp * DPP_STRIDE;
    /* cursor-ttu-hypothesis: the rank-1 hypothesis of premetal/cursor-invisible-analysis.md (a cursor requestor whose
     * delivery rate DCN_CUR0_TTU_CNTL0.REFCYC_PER_REQ_DELIVERY [22:0] is 0 fetches nothing). It is NOT known
     * hardware behaviour: like gfx-golden-strict it only proves that the kext writes the register. */
    if (s->cursor_ttu_hypothesis &&
        !(reg_get(s, SEG2(HUBP_CUR0_TTU_CNTL0 + hp)) & 0x7fffff)) {
        if (!s->cursor_reject_logged || s->trace)
            fprintf(stderr, "rdna4: cursor: rejected, DCN_CUR0_TTU_CNTL0 delivery is 0 (cursor-ttu-hypothesis)\n");
        s->cursor_reject_logged = true;
        return false;
    }
    const uint32_t control = reg_get(s, SEG2(HUBP_CURSOR_CONTROL + hp));
    const uint32_t cm = reg_get(s, SEG2(CURSOR_CM_CONTROL + dpp));
    const uint32_t scaleGy = reg_get(s, SEG2(CURSOR_CM_SCALE_GY + dpp)) & 0xffff;
    const uint32_t scaleRb = reg_get(s, SEG2(CURSOR_CM_SCALE_RB + dpp)) & 0xffff;
    /* HUBP_CURSOR_CONTROL.CURSOR_PITCH follows the DCN card contract:
     * codes 0/1/2 select 64/128/256 pixels per fetched row. */
    const uint32_t pitchCode = (control >> CURSOR_PITCH_SHIFT) & 3;
    const uint32_t pitch = 64u << pitchCode;
    if (!(control & 1) || !(cm & CURSOR_CM_ENABLE))
        return false;
    if (!(control & CURSOR_REQ_MODE) ||
        ((control >> CURSOR_MODE_SHIFT) & 7) != 2 ||
        pitchCode > 2 ||
        scaleGy != CURSOR_FP16_ONE || scaleRb != CURSOR_FP16_ONE) {
        if (!s->cursor_reject_logged || s->trace)
            fprintf(stderr, "rdna4: cursor: rejected control=0x%08x cm=0x%08x scale=0x%04x/%04x\n",
                    control, cm, scaleGy, scaleRb);
        s->cursor_reject_logged = true;
        return false;
    }
    const uint32_t size = reg_get(s, SEG2(HUBP_CURSOR_SIZE + hp));
    const uint32_t width = (size >> 16) & 0x1ff;
    const uint32_t height = size & 0x1ff;
    const uint64_t address = reg_get(s, SEG2(HUBP_CURSOR_ADDRESS + hp)) |
        ((uint64_t)(reg_get(s, SEG2(HUBP_CURSOR_ADDRESS_HI + hp)) & 0xffff) << 32);
    const uint64_t fb = (uint64_t)(reg_get(s, SEG2(DCN_VM_FB_LOC_BASE)) & 0xffffff) << 24;
    if (!width || !height || width > 64 || height > 64 || address < fb ||
        address - fb + (uint64_t)pitch * height * 4 > rdna4_vram_size()) {
        if (!s->cursor_reject_logged || s->trace)
            fprintf(stderr, "rdna4: cursor: rejected size=%ux%u address=0x%" PRIu64
                    " fb=0x%" PRIu64 "\n", width, height, address, fb);
        s->cursor_reject_logged = true;
        return false;
    }
    const uint32_t position = reg_get(s, SEG2(HUBP_CURSOR_POSITION + hp));
    const uint32_t rawX = (position >> 15) & 0x7fff;
    const uint32_t rawY = position & 0x7fff;
    const int32_t x = (rawX & 0x4000) ? (int32_t)rawX - 0x8000 : (int32_t)rawX;
    const int32_t y = (rawY & 0x4000) ? (int32_t)rawY - 0x8000 : (int32_t)rawY;
    const uint32_t hot = reg_get(s, SEG2(HUBP_CURSOR_HOT_SPOT + hp));
    cursor->valid = true;
    cursor->offset = address - fb;
    cursor->width = width;
    cursor->height = height;
    cursor->pitch = pitch;
    cursor->x = x - (int32_t)((hot >> 16) & 0xff);
    cursor->y = y - (int32_t)(hot & 0xff);
    s->cursor_reject_logged = false;
    fprintf(stderr, "rdna4: cursor: plane HUBP%d addr=0x%" PRIu64
            " size=%ux%u pos=%d,%d\n", hubp, address, width, height,
            cursor->x, cursor->y);
    return true;
}

static void rdna4_blend_cursor(RDNA4State *s, const RDNA4Scanout *so,
                               DisplaySurface *surface)
{
    if (!so->cursor.valid)
        return;
    uint8_t *sprite = rdna4_vram_span(s, so->cursor.offset,
                                      (uint64_t)so->cursor.pitch * so->cursor.height * 4);
    if (!sprite)
        return;
    uint8_t *dst = surface_data(surface);
    const int dstStride = surface_stride(surface);
    for (uint32_t y = 0; y < so->cursor.height; y++) {
        int32_t dy = so->cursor.y + (int32_t)y;
        if (dy < 0 || dy >= (int32_t)so->height)
            continue;
        for (uint32_t x = 0; x < so->cursor.width; x++) {
            int32_t dx = so->cursor.x + (int32_t)x;
            if (dx < 0 || dx >= (int32_t)so->width)
                continue;
            uint32_t src = ldl_le_p(sprite + ((uint64_t)y * so->cursor.pitch + x) * 4);
            uint32_t alpha = src >> 24;
            if (!alpha)
                continue;
            uint32_t *out = (uint32_t *)(dst + (uint64_t)dy * dstStride + (uint64_t)dx * 4);
            uint32_t old = *out;
            uint32_t inv = 255 - alpha;
            uint32_t r = ((src >> 16) & 0xff) + (((old >> 16) & 0xff) * inv + 127) / 255;
            uint32_t g = ((src >> 8) & 0xff) + (((old >> 8) & 0xff) * inv + 127) / 255;
            uint32_t b = (src & 0xff) + ((old & 0xff) * inv + 127) / 255;
            *out = (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 |
                   (b > 255 ? 255 : b);
        }
    }
}

static void rdna4_gfx_update(void *opaque)
{
    RDNA4State *s = opaque;
    RDNA4Scanout so;
    DisplaySurface *ds;
    DirtyBitmapSnapshot *snap;
    int y, ys;

    rdna4_get_scanout(s, &so);
    rdna4_get_cursor(s, &so, &so.cursor);
    if (memcmp(&so, &s->scanout, sizeof(so)) != 0) {
        s->scanout = so;
        if (!so.active || so.nosignal) {
            ds = qemu_create_placeholder_surface(640, 480,
                                                 so.nosignal ? so.nosignal : "rdna4: no signal");
        } else if (so.blank) {
            ds = qemu_create_displaysurface(so.width, so.height);
            memset(surface_data(ds), 0, (size_t)surface_stride(ds) * so.height);
        } else {
            uint8_t *scanout = rdna4_vram_span(s, so.offset,
                                                (uint64_t)so.stride * so.height);
            if (!scanout) {
                ds = qemu_create_placeholder_surface(640, 480,
                                                     "rdna4: scanout surface not mapped");
            } else if (so.cursor.valid) {
                ds = qemu_create_displaysurface(so.width, so.height);
                for (int row = 0; row < (int)so.height; row++)
                    memcpy(surface_data(ds) + (uint64_t)row * surface_stride(ds),
                           scanout + (uint64_t)row * so.stride, so.width * 4);
                rdna4_blend_cursor(s, &so, ds);
            } else {
                ds = qemu_create_displaysurface_from(
                    so.width, so.height, PIXMAN_LE_x8r8g8b8, so.stride, scanout);
            }
        }
        dpy_gfx_replace_surface(s->con, ds);
        dpy_gfx_update_full(s->con);
        return;
    }
    if (!so.active || so.blank || so.nosignal) {
        return;
    }

    /* A copied surface is used while the cursor plane is armed so a cursor
     * move or a dirty framebuffer region cannot erase the composited sprite. */
    if (so.cursor.valid) {
        uint8_t *scanout = rdna4_vram_span(s, so.offset,
                                           (uint64_t)so.stride * so.height);
        if (!scanout)
            return;
        ds = qemu_create_displaysurface(so.width, so.height);
        for (int row = 0; row < (int)so.height; row++)
            memcpy(surface_data(ds) + (uint64_t)row * surface_stride(ds),
                   scanout + (uint64_t)row * so.stride, so.width * 4);
        rdna4_blend_cursor(s, &so, ds);
        dpy_gfx_replace_surface(s->con, ds);
        dpy_gfx_update_full(s->con);
        DirtyBitmapSnapshot *cursorSnap = memory_region_snapshot_and_clear_dirty(
            &s->vram, so.offset, (uint64_t)so.stride * so.height, DIRTY_MEMORY_VGA);
        g_free(cursorSnap);
        return;
    }

    snap = memory_region_snapshot_and_clear_dirty(&s->vram, so.offset,
                                                  (uint64_t)so.stride * so.height,
                                                  DIRTY_MEMORY_VGA);
    ys = -1;
    for (y = 0; y < so.height; y++) {
        bool dirty = memory_region_snapshot_get_dirty(&s->vram, snap,
                                                      so.offset + (uint64_t)so.stride * y,
                                                      so.stride);
        if (dirty && ys < 0) {
            ys = y;
        }
        if (!dirty && ys >= 0) {
            dpy_gfx_update(s->con, 0, ys, so.width, y - ys);
            ys = -1;
        }
    }
    if (ys >= 0) {
        dpy_gfx_update(s->con, 0, ys, so.width, y - ys);
    }
    g_free(snap);
}

static const GraphicHwOps rdna4_gfx_ops = {
    .gfx_update = rdna4_gfx_update,
};

/* ---- state and firmware images ------------------------------------------ */

/*
 * Register image lines, as tools/linux-capture.sh writes them:
 *   NAME base_idx dword_off byte_addr 0xVALUE
 * '#' starts a comment; SKIP:/ERR: values are ignored.
 */
static bool rdna4_load_state(RDNA4State *s, Error **errp)
{
    g_autofree char *text = NULL;
    g_auto(GStrv) lines = NULL;
    g_autoptr(GError) gerr = NULL;
    unsigned n = 0;

    if (!g_file_get_contents(s->state_file, &text, NULL, &gerr)) {
        error_setg(errp, "rdna4: cannot read state '%s': %s", s->state_file,
                   gerr->message);
        return false;
    }
    lines = g_strsplit(text, "\n", -1);
    for (char **l = lines; *l; l++) {
        char *hash = strchr(*l, '#');
        g_auto(GStrv) tok = NULL;
        guint64 byte, val;

        if (hash) {
            *hash = '\0';
        }
        tok = g_strsplit_set(g_strstrip(*l), " \t", -1);
        if (g_strv_length(tok) < 5) {
            continue;
        }
        /* tokens may be empty when separated by several blanks */
        char *f[5];
        int nf = 0;
        for (char **t = tok; *t && nf < 5; t++) {
            if (**t) {
                f[nf++] = *t;
            }
        }
        if (nf < 5 || !g_str_has_prefix(f[4], "0x")) {
            continue;
        }
        byte = g_ascii_strtoull(f[3], NULL, 16);
        val = g_ascii_strtoull(f[4], NULL, 16);
        if (byte + 4 > RDNA4_MMIO_SIZE || (byte & 3)) {
            warn_report("rdna4: state: %s at 0x%" PRIx64 " outside BAR5, ignored",
                        f[0], byte);
            continue;
        }
        reg_set(s, byte, val);
        n++;
    }
    if (!n) {
        error_setg(errp, "rdna4: state '%s' holds no registers", s->state_file);
        return false;
    }
    return true;
}

/* IP discovery binary out of the card's flash image (signature 0x28211407). */
static void rdna4_load_discovery(RDNA4State *s)
{
    g_autofree char *flash = NULL;
    gsize len = 0;

    if (!s->flash_file) {
        warn_report("rdna4: no flash= image, VRAM holds no IP discovery table");
        return;
    }
    if (!g_file_get_contents(s->flash_file, &flash, &len, NULL)) {
        warn_report("rdna4: cannot read flash image '%s'", s->flash_file);
        return;
    }
    for (gsize off = 0; off + 16 <= len; off += 4) {
        if (ldl_le_p(flash + off) == 0x28211407) {
            uint32_t size = lduw_le_p(flash + off + 10);
            if (size < 16 || off + size > len) {
                continue;
            }
            s->discovery = g_memdup2(flash + off, size);
            s->discovery_len = size;
            return;
        }
    }
    warn_report("rdna4: no IP discovery binary in '%s'", s->flash_file);
}

static void rdna4_load_edid(RDNA4State *s, Error **errp)
{
    g_autofree char *data = NULL;
    gsize len = 0;

    if (s->edid_file) {
        if (!g_file_get_contents(s->edid_file, &data, &len, NULL) ||
            len < 128 || len % 128 || len > sizeof(s->edid)) {
            error_setg(errp, "rdna4: edid '%s' must be 128 or 256 bytes", s->edid_file);
            return;
        }
        memcpy(s->edid, data, len);
        s->edid_len = len;
    } else {
        memcpy(s->edid, lenovo_g25_10_edid, sizeof(lenovo_g25_10_edid));
        s->edid_len = sizeof(lenovo_g25_10_edid);
    }
    /* announce only the extension blocks that exist */
    if (s->edid[126] > s->edid_len / 128 - 1) {
        uint8_t sum = 0;
        s->edid[126] = s->edid_len / 128 - 1;
        for (int i = 0; i < 127; i++) {
            sum += s->edid[i];
        }
        s->edid[127] = -sum;
    }
}

/* ---- device ------------------------------------------------------------- */

static void rdna4_reset(DeviceState *dev)
{
    RDNA4State *s = RDNA4(dev);
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    /* A warm platform reset clears PCI Command.BusMaster (QEMU does it in
     * pcibus_reset_hold, AFTER this callback: see rdna4_config_write).  The
     * firmware/GOP turns it back on while booting; the warm-keep model
     * schedules that delayed handoff (rdna4_warm_gop_timer), keeps the engines
     * and counts any DMA after it.  The engines themselves keep running
     * VRAM-only work all along; only DMA is gated by Bus Master. */
    if (s->warm_keep && rdna4_engine_active(s)) {
        const bool restoreBusMaster = rdna4_bus_master_enabled(s) ||
                                      s->bus_master_before_reset;
        fprintf(stderr, "rdna4: warm-keep: reset PCI bus master current=%d remembered=%d\n",
                rdna4_bus_master_enabled(s), s->bus_master_before_reset);
        s->warm_gop_restore = restoreBusMaster;
        if (s->warm_dma_window)
            fprintf(stderr, "rdna4: warm-keep: DMA-after-reset writes: %" PRIu64
                    " reads: %" PRIu64 "\n",
                    s->dma_after_reset_writes, s->dma_after_reset_reads);
        else if (!rdna4_bus_master_enabled(s))
            fprintf(stderr, "rdna4: warm-keep: DMA-after-reset writes: 0 "
                    "(bus master was off at reset)\n");
        s->warm_dma_window = true;
        s->dma_after_reset_writes = 0;
        s->dma_after_reset_reads = 0;
        fprintf(stderr, "rdna4: warm-keep: reset preserves IH/SDMA/MEC/GFX; "
                "firmware/GOP will re-enable DMA during boot\n");
        /* The generic reset may cancel a pending BH/timer although the queues
         * were kept: re-kick the live work, as the card's engines carry on
         * regardless of Bus Master (its DMA is refused until the handoff). */
        rdna4_work_schedule(s);
        if (s->warm_gop_timer && s->warm_gop_restore) {
            timer_mod_ns(s->warm_gop_timer,
                         qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                         (int64_t)s->warm_gop_delay_ms * SCALE_MS);
        } else if (s->warm_gop_timer) {
            timer_del(s->warm_gop_timer);
        }
        return;
    }

    if (s->warm_keep)
        fprintf(stderr, "rdna4: warm-keep: reset found no live engines (quiesced): "
                "nothing preserved, DMA-after-reset writes: 0 reads: 0 "
                "(bus master %d at reset)\n", rdna4_bus_master_enabled(s));
    if (s->warm_gop_timer)
        timer_del(s->warm_gop_timer);
    s->warm_gop_restore = false;
    if (s->work_timer)
        timer_del(s->work_timer);
    if (s->work_bh)
        qemu_bh_cancel(s->work_bh);
    rdna4_dispatch_free(s->dispatch);
    s->dispatch = NULL;
    memset(&s->mec_work, 0, sizeof(s->mec_work));
    memset(&s->sdma_work, 0, sizeof(s->sdma_work));
    s->sleep_reset = false;

    /* ACPI S3 removes power from the compute engines while DCN and VRAM
     * remain observable by the display. Save the DMU segments across the
     * power-on image restore; the rest of BAR5 is rebuilt from gop-state. */
    const size_t dcn_start = SEG2(0);
    const size_t dcn_bytes = SEG3(0) - dcn_start;
    uint8_t *dcn = s->regs ? g_memdup2(s->regs + dcn_start, dcn_bytes) : NULL;

    memset(s->regs, 0, RDNA4_MMIO_SIZE);
    memset(s->resv, 0, RDNA4_RESV_SIZE);
    memset(s->hqd, 0, sizeof(s->hqd));
    s->selected_pipe = s->selected_queue = s->selected_vmid = 0;
    memset(&s->i2c, 0, sizeof(s->i2c));
    if (s->state_file) {
        Error *err = NULL;
        if (!rdna4_load_state(s, &err)) {
            error_report_err(err);
        }
    }
    /* The real Navi 48 GB_ADDR_CONFIG, read on the card (premetal hw-logs,
     * round 2): 0x08200545.  Its NUM_SHADER_ENGINES field [22:19] reads 4, so
     * a 1<<n decode gives 16 SEs on a 4-SE chip: the SE count comes from the
     * IP discovery gc_info table, as in amdgpu_discovery_get_gc_info.  The
     * register is read-only in the model; always present the card's value. */
    reg_set(s, REG_GFX_GB_ADDR_CONFIG, 0x08200545u);
    if (s->ctx_garbage)
        rdna4_ctx_poison(s);
    if (s->cursor_lock_stuck)
        reg_set(s, SEG3(0x02c5), 1);
    if (s->gop_dlg)   /* regHUBPREQ0_DCN_SURF0_TTU_CNTL0 0x0623: a GOP that ran DML for its one surface */
        reg_set(s, SEG2(0x0623), 0x08005630u);
    if (dcn) {
        memcpy(s->regs + dcn_start, dcn, dcn_bytes);
        g_free(dcn);
        fprintf(stderr, "rdna4: power reset: compute state restored from gop-state; DCN and VRAM retained\n");
    }
    rdna4_init_earliest_inuse(s);
    if (s->discovery) {
        memcpy(s->resv + RDNA4_RESV_SIZE - RDNA4_DISCOVERY_TOP, s->discovery,
               MIN(s->discovery_len, RDNA4_DISCOVERY_TOP));
    }
    s->npending = 0;
    memset(s->flip, 0, sizeof(s->flip));
    s->psp_bl_loaded = 0;
    s->psp_ring_mc = 0;
    s->psp_ring_size = 0;
    s->psp_rptr = 0;
    s->pmfw_loaded = s->smu_preloaded;
    if (s->gfxoff_preset) {
        s->gfxoff_pending = true;
        s->gfxoff_arm_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + (int64_t)s->gfxoff_arm_ms * SCALE_MS;
        fprintf(stderr, "rdna4: reset: the GC block will be powered down in %u ms (gfxoff-preset), PMFW %s\n",
                s->gfxoff_arm_ms, s->pmfw_loaded ? "answering (smu-preloaded)" : "not loaded");
    }
    memset(s->psp_fw_types, 0, sizeof(s->psp_fw_types));
    s->smu_allowed = ~0ull;
    s->smu_running = 0;
    s->smu_table_mc = 0;
    s->autoload_armed = false;
    s->sdma_wptr = 0;
    s->mec_hung = false;
    s->ih_ring_bus = 0;
    s->ih_wptr_bus = 0;
    s->ih_rptr = 0;
    s->ih_wptr = 0;
    s->ih_overflow = false;
    s->gfx_booted = false;
    if (s->dcn_timer)
        timer_del(s->dcn_timer);
    memset(s->dcn_next_ns, 0, sizeof(s->dcn_next_ns));
    s->gfx_wptr = 0;
    s->gfx_rptr = 0;
    s->gfx_csb_loaded = false;
    s->gfx_reinit = false;
    s->gfx_trace_selfcheck_done = false;
    s->gfx_job_seq = 0;
    s->gfx_pending = false;
    s->gfx_active = false;
    s->gfx_pending_doorbell = false;
    s->gfx_budget_hit = false;
    s->gfx_pending_wptr = 0;
    s->gfx_work_base = 0;
    s->gfx_work_pos = 0;
    s->gfx_work_end = 0;
    s->gfx_work_ring_dw = 0;
    s->gfx_num_instances = 0;
    s->gfx_draw_refused = false;
    memset(s->symclk_khz, 0, sizeof(s->symclk_khz));
    memset(s->dig_mode, 0, sizeof(s->dig_mode));
    /*
     * The GOP left each running OTG's PLL at 60 Hz of its raster and the
     * transmitter of each enabled DIG's link on at the same clock.
     */
    for (int otg = 0; otg < NUM_OTG; otg++) {
        uint64_t htot = (rdna4_otg_reg(s, otg, OTG_H_TOTAL) & 0x7fff) + 1;
        uint64_t vtot = (rdna4_otg_reg(s, otg, OTG_V_TOTAL) & 0x7fff) + 1;
        s->otg_epoch[otg] = now;
        s->pclk_khz[otg] = (rdna4_otg_reg(s, otg, OTG_CONTROL) & 1) ?
                           htot * vtot * 60 / 1000 : 0;
        if (s->pclk_khz[otg])
            s->dcn_next_ns[otg] = now + rdna4_dcn_period_ns(s, otg);
    }
    for (int d = 0; d < NUM_DIG; d++) {
        uint32_t o = d * DIG_STRIDE;
        uint32_t otg = reg_get(s, SEG2(DIG_FE_CNTL + o)) & 7;
        if ((reg_get(s, SEG2(DIG_FE_EN_CNTL + o)) & 1) && otg < NUM_OTG) {
            s->symclk_khz[reg_get(s, SEG2(STREAM_MAPPER + d)) & 7] = s->pclk_khz[otg];
        }
    }
    memset(&s->scanout, 0xff, sizeof(s->scanout));   /* force a surface update */
    rdna4_dcn_schedule(s);
}

static void rdna4_realize(PCIDevice *dev, Error **errp)
{
    RDNA4State *s = RDNA4(dev);
    Object *obj = OBJECT(dev);
    Error *err = NULL;
    int ret;

    if (!s->state_file) {
        error_setg(errp, "rdna4: state=<register image> is required");
        return;
    }
    if (s->aperture < 16 * MiB || s->aperture > 1 * GiB || !is_power_of_2(s->aperture)) {
        error_setg(errp, "rdna4: vram aperture must be a power of two, 16 MiB..1 GiB");
        return;
    }
    if (s->edid_line >= NUM_DDC) {
        error_setg(errp, "rdna4: edid-line must be 0..%d", NUM_DDC - 1);
        return;
    }
    rdna4_load_edid(s, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }
    rdna4_load_discovery(s);
    s->regs = g_malloc0(RDNA4_MMIO_SIZE);
    s->resv = g_malloc0(RDNA4_RESV_SIZE);
    /*
     * The rest of VRAM, past the BAR the host sees: the kext's compute heap
     * lives there. NORESERVE, so only pages the guest's GPU work touches
     * take host memory.
     */
    s->hidden_size = rdna4_vram_size() - RDNA4_RESV_SIZE - s->aperture;
    s->hidden = mmap(NULL, s->hidden_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (s->hidden == MAP_FAILED) {
        warn_report("rdna4: no host reservation for VRAM past the aperture");
        s->hidden = NULL;
        s->hidden_size = 0;
    }
    if (!rdna4_load_state(s, errp)) {
        return;
    }
    rdna4_init_earliest_inuse(s);
    s->dcn_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rdna4_dcn_timer, s);
    s->warm_gop_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rdna4_warm_gop_timer, s);
    s->work_timer = timer_new_ns(QEMU_CLOCK_REALTIME, rdna4_work_timer, s);
    s->work_bh = qemu_bh_new(rdna4_work_bh, s);

    if (!memory_region_init_ram(&s->vram, obj, "rdna4.vram", s->aperture, errp)) {
        return;
    }
    memory_region_init_io(&s->mmio, obj, &rdna4_mmio_ops, s, "rdna4.mmio",
                          RDNA4_MMIO_SIZE);
    memory_region_init_io(&s->doorbell, obj, &rdna4_doorbell_ops, s, "rdna4.doorbell",
                          RDNA4_DOORBELL_SIZE);
    memory_region_init_io(&s->io, obj, &rdna4_inert_ops, s, "rdna4.io",
                          RDNA4_IO_SIZE);

    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_PREFETCH |
                     PCI_BASE_ADDRESS_MEM_TYPE_64, &s->vram);
    pci_register_bar(dev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_PREFETCH |
                     PCI_BASE_ADDRESS_MEM_TYPE_64, &s->doorbell);
    pci_register_bar(dev, 4, PCI_BASE_ADDRESS_SPACE_IO, &s->io);
    pci_register_bar(dev, 5, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);

    if (pci_bus_is_express(pci_get_bus(dev))) {
        ret = pcie_endpoint_cap_init(dev, 0x80);
        assert(ret > 0);
    } else {
        dev->cap_present &= ~QEMU_PCI_CAP_EXPRESS;
    }
    /* The card has MSI; nothing raises interrupts yet. */
    if (msi_init(dev, 0, 1, true, false, &err) < 0) {
        warn_report_err(err);
        err = NULL;
    }

    s->con = graphic_console_init(DEVICE(dev), 0, &rdna4_gfx_ops, s);
    memory_region_set_log(&s->vram, true, DIRTY_MEMORY_VGA);
}

static void rdna4_exit(PCIDevice *dev)
{
    RDNA4State *s = RDNA4(dev);

    if (s->warm_dma_window)
        fprintf(stderr, "rdna4: warm-keep: DMA-after-reset writes: %" PRIu64
                " reads: %" PRIu64 "\n",
                s->dma_after_reset_writes, s->dma_after_reset_reads);

    rdna4_dispatch_free(s->dispatch);
    s->dispatch = NULL;
    if (s->work_bh) {
        qemu_bh_cancel(s->work_bh);
        qemu_bh_delete(s->work_bh);
        s->work_bh = NULL;
    }
    if (s->work_timer) {
        timer_del(s->work_timer);
        timer_free(s->work_timer);
        s->work_timer = NULL;
    }
    if (s->dcn_timer) {
        timer_del(s->dcn_timer);
        timer_free(s->dcn_timer);
        s->dcn_timer = NULL;
    }
    if (s->warm_gop_timer) {
        timer_del(s->warm_gop_timer);
        timer_free(s->warm_gop_timer);
        s->warm_gop_timer = NULL;
    }
    graphic_console_close(s->con);
    msi_uninit(dev);
    g_free(s->regs);
    g_free(s->resv);
    g_free(s->discovery);
}

static bool rdna4_get_gfxoff_force(Object *obj, Error **errp)
{
    return RDNA4(obj)->gfxoff_active;
}

/* qom-set /machine/peripheral/rdna4 gfxoff-force true: the GC block is powered down as if
 * an earlier boot had allowed GFXOFF. gfxoff_active is not touched by a device reset, so it
 * survives a warm restart, as the real ASIC's state would. */
static void rdna4_set_gfxoff_force(Object *obj, bool value, Error **errp)
{
    RDNA4(obj)->gfxoff_active = value;
    fprintf(stderr, "rdna4: gfxoff-force %s\n", value ? "on: GC powered down" : "off");
}

static void rdna4_instance_init(Object *obj)
{
    PCI_DEVICE(obj)->cap_present |= QEMU_PCI_CAP_EXPRESS;
    object_property_add_bool(obj, "gfxoff-force", rdna4_get_gfxoff_force, rdna4_set_gfxoff_force);
}

static const Property rdna4_properties[] = {
    DEFINE_PROP_SIZE("vram", RDNA4State, aperture, 256 * MiB),
    DEFINE_PROP_STRING("state", RDNA4State, state_file),
    DEFINE_PROP_STRING("flash", RDNA4State, flash_file),
    DEFINE_PROP_STRING("edid", RDNA4State, edid_file),
    DEFINE_PROP_UINT32("edid-line", RDNA4State, edid_line, 2),
    DEFINE_PROP_BOOL("trace", RDNA4State, trace, false),
    DEFINE_PROP_BOOL("kiq-only", RDNA4State, kiq_only, false),
    DEFINE_PROP_BOOL("inv-noack", RDNA4State, inv_noack, false),
    DEFINE_PROP_BOOL("sdma-no-doorbell", RDNA4State, sdma_no_db, false),
    DEFINE_PROP_BOOL("dma-broken", RDNA4State, dma_broken, false),
    DEFINE_PROP_BOOL("flip-stuck", RDNA4State, flip_stuck, false),
    DEFINE_PROP_BOOL("ih-dead", RDNA4State, ih_dead, false),
    DEFINE_PROP_BOOL("dcn-irq-storm", RDNA4State, dcn_irq_storm, false),
    DEFINE_PROP_BOOL("cursor", RDNA4State, cursor_enabled, false),
    DEFINE_PROP_BOOL("cursor-lock-stuck", RDNA4State, cursor_lock_stuck, false),
    DEFINE_PROP_BOOL("gop-dlg", RDNA4State, gop_dlg, false),
    DEFINE_PROP_BOOL("ctx-garbage", RDNA4State, ctx_garbage, true),
    DEFINE_PROP_BOOL("cursor-ttu-hypothesis", RDNA4State, cursor_ttu_hypothesis, false),
    DEFINE_PROP_BOOL("hang-sticky", RDNA4State, hang_sticky, false),
    DEFINE_PROP("sleep-reset", RDNA4State, sleep_reset, rdna4_sleep_reset_prop,
                bool),
    DEFINE_PROP_BOOL("gfx-hang", RDNA4State, gfx_hang, false),
    DEFINE_PROP_BOOL("smu-stale", RDNA4State, smu_stale, false),
    DEFINE_PROP_BOOL("gfxoff-preset", RDNA4State, gfxoff_preset, false),
    DEFINE_PROP_UINT32("gfxoff-arm-ms", RDNA4State, gfxoff_arm_ms, 0),
    DEFINE_PROP_BOOL("smu-preloaded", RDNA4State, smu_preloaded, false),
    DEFINE_PROP_UINT32("smu-refuse", RDNA4State, smu_refuse, 0),
    DEFINE_PROP_UINT32("smu-refuse-skip", RDNA4State, smu_refuse_skip, 0),
    DEFINE_PROP_BOOL("warm-keep", RDNA4State, warm_keep, false),
    DEFINE_PROP_UINT32("warm-gop-delay-ms", RDNA4State, warm_gop_delay_ms,
                       RDNA4_WARM_GOP_DELAY_MS_DEFAULT),
    DEFINE_PROP_UINT32("gfx-break", RDNA4State, gfx_break, 0),
    DEFINE_PROP_BOOL("gfx-golden-strict", RDNA4State, gfx_golden_strict, false),
    DEFINE_PROP_BOOL("gfx-trace", RDNA4State, gfx_trace, false),
};

static void rdna4_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->vendor_id = 0x1002;               /* AMD */
    k->device_id = 0x7550;               /* Navi 48: RX 9070 / 9070 XT */
    k->revision = 0xc0;
    k->class_id = PCI_CLASS_DISPLAY_VGA;
    k->subsystem_vendor_id = 0x1da2;     /* Sapphire, from the card's ATOM header */
    k->subsystem_id = 0xe489;
    k->realize = rdna4_realize;
    k->exit = rdna4_exit;
    k->config_write = rdna4_config_write;
    device_class_set_legacy_reset(dc, rdna4_reset);
    device_class_set_props(dc, rdna4_properties);
    dc->desc = "AMD Radeon RX 9070 XT display-engine model (RDNA4FB development)";
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo rdna4_info = {
    .name          = TYPE_RDNA4,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(RDNA4State),
    .instance_init = rdna4_instance_init,
    .class_init    = rdna4_class_init,
    .interfaces    = (InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE },
        { },
    },
};

static void rdna4_register_types(void)
{
    type_register_static(&rdna4_info);
}

type_init(rdna4_register_types)
