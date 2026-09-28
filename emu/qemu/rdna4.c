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
#include "qemu/host-utils.h"
#include "qemu/error-report.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pcie.h"
#include "hw/pci/msi.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "ui/console.h"
#include "ui/qemu-pixman.h"
#include "qom/object.h"

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
#define OTG_STATUS_POSITION  0x1b4a     /* regOTG0_OTG_STATUS_POSITION, BASE_IDX 2 */
#define OTG_FRAME_COUNT      0x1b4d     /* regOTG0_OTG_STATUS_FRAME_COUNT, BASE_IDX 2 */
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
#define HUBP_SURFACE_ADDR    0x060a     /* regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, BASE_IDX 2 */
#define HUBP_SURFACE_ADDR_HI 0x060b     /* regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, BASE_IDX 2 */
#define HUBP_FLIP_CONTROL    0x0613     /* regHUBPREQ0_DCSURF_FLIP_CONTROL, BASE_IDX 2 */
#define HUBP_FLIP_PENDING    (1u << 8)
#define HUBP_DB_FIRST        0x05e5     /* DCSURF_SURFACE_CONFIG .. */
#define HUBP_DB_LAST         0x0613     /* .. regHUBPREQ0_DCSURF_FLIP_CONTROL */
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
#define REG_GCMC_L1_TLB      GC_SEG0(0x161b)   /* ENABLE_L1_TLB [0] */
#define REG_GCVM_CTX0_CNTL   GC_SEG0(0x1624)   /* ENABLE_CONTEXT [0] */
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
#define REG_SDMA0_MCU_CNTL   GC_SEG1(0x588e)   /* HALT [0] */
#define PSP_ERR_UNKNOWN_CMD  0x100
#define PSP_TMR_SIZE         0x1400000      /* model's answer to LOAD_TOC */

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
} RDNA4Flip;

#define MAX_PENDING 256

typedef struct RDNA4Scanout {
    bool     active;         /* an OTG is running */
    bool     blank;          /* pattern generator on, or nothing to fetch */
    const char *nosignal;    /* what the monitor would say, NULL = picture */
    uint32_t width, height, stride;
    uint64_t offset;         /* into the VRAM aperture */
} RDNA4Scanout;

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
    bool         autoload_armed;        /* AUTOLOAD_RLC accepted, IMU not released */
    uint64_t     sdma_wptr;             /* SDMA0 queue 0's last wptr: 64-bit, monotonic */
    bool         gfx_booted;            /* RLC autoload done */
};

static inline uint32_t reg_get(RDNA4State *s, uint32_t byte)
{
    return s->regs[byte / 4];
}

static inline void reg_set(RDNA4State *s, uint32_t byte, uint32_t val)
{
    s->regs[byte / 4] = val;
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

static uint32_t rdna4_otg_read(RDNA4State *s, int otg, uint32_t dw)
{
    uint32_t val = rdna4_otg_reg(s, otg, dw);
    uint64_t frame;
    uint32_t line, horizontal, vb;
    const bool running = rdna4_otg_position(s, otg, &frame, &line, &horizontal);

    if (running) {
        rdna4_latch_flips(s, otg, frame);
    }

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
        frame == flip->request_frame) {
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
        flip->pending = false;
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
        uint64_t frame = 0;
        uint32_t line = 0, horizontal = 0;
        if (otg >= 0 && rdna4_otg_position(s, otg, &frame, &line, &horizontal)) {
            rdna4_latch_flips(s, otg, frame);
        }
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

    if (dw >= OTG_H_TOTAL && dw <= OTG_V_SYNC_A_CNTL) {
        rdna4_db_write(s, otg, addr, val);
        return;
    }
    if (dw == OTG_CONTROL && (val & 1) && !(old & 1)) {
        s->otg_epoch[otg] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    reg_set(s, addr, val & (dw == OTG_MASTER_UPDATE_LOCK ? ~(1u << 8) : ~0u));
    if ((dw == OTG_MASTER_UPDATE_LOCK && (old & 1) && !(val & 1)) ||
        (dw == OTG_CONTROL && !(val & 1))) {
        rdna4_latch_pending(s);
    }
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

static void rdna4_smu_msg(RDNA4State *s, uint32_t msg)
{
    uint32_t resp = SMU_RESP_OK, param = reg_get(s, REG_SMU_PARAM);

    reg_set(s, REG_SMU_MSG, msg);
    if (!s->pmfw_loaded) {
        return;                                    /* no PMFW: RESP stays 0 */
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
    case 0xf:                                      /* SetDriverDramAddrLow */
    case 0x29:                                     /* DisallowGfxOff */
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
        ok = pci_dma_write(pci, da, sp, bytes) == MEMTX_OK;          /* VRAM -> system */
    } else if (dp) {
        ok = pci_dma_read(pci, sa, dp, bytes) == MEMTX_OK;           /* system -> VRAM */
    } else {
        g_autofree uint8_t *tmp = g_malloc(bytes);
        ok = pci_dma_read(pci, sa, tmp, bytes) == MEMTX_OK &&
             pci_dma_write(pci, da, tmp, bytes) == MEMTX_OK;
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

    uint32_t cntl = reg_get(s, REG_SDMA0_RB_CNTL);
    uint32_t size = 4u << ((cntl >> 1) & 0x1f);          /* bytes */
    uint64_t ring = ((uint64_t)reg_get(s, REG_SDMA0_RB_BASE) << 8) |
                    ((uint64_t)reg_get(s, REG_SDMA0_RB_BASE_HI) << 40);
    uint32_t rptr = reg_get(s, REG_SDMA0_RB_RPTR);

    reg_set(s, REG_SDMA0_RB_WPTR, wptr);
    if (!s->gfx_booted || !(cntl & 1) || (reg_get(s, REG_SDMA0_MCU_CNTL) & 1)) {
        return;                         /* no firmware, queue off, or halted */
    }
    wptr &= size - 1;
    while (rptr != wptr) {
        uint32_t dw[8];
        for (int i = 0; i < 8; i++) {
            uint8_t *p = rdna4_gc_span(s, ring + ((rptr + 4 * i) & (size - 1)), 4);
            if (!p) {
                fprintf(stderr, "rdna4: sdma: ring MC 0x%" PRIx64 " not mapped by the GC hub\n",
                        ring);
                return;
            }
            dw[i] = ldl_le_p(p);
        }
        uint32_t op = dw[0] & 0xff, sub = (dw[0] >> 8) & 0xff, len = 1;
        uint64_t a = dw[1] | ((uint64_t)dw[2] << 32);
        uint8_t *dst;
        switch (op) {
        case 0:                                         /* NOP */
            len = 1 + ((dw[0] >> 16) & 0x3fff);
            break;
        case 2: {                                       /* WRITE linear, 1 dword */
            len = 5;
            if (sub || dw[3] != 0 || !(dst = rdna4_gc_span(s, a, 4))) {
                goto fault;
            }
            stl_le_p(dst, dw[4]);
            break;
        }
        case 5:                                         /* FENCE */
            len = 4;
            if (!(dst = rdna4_gc_span(s, a & ~3ull, 4))) {
                goto fault;
            }
            stl_le_p(dst, dw[3]);
            break;
        case 11: {                                      /* CONST_FILL */
            uint32_t bytes = dw[4] + 1, fsize = dw[0] >> 30;
            len = 5;
            if (!(dst = rdna4_gc_span(s, a, bytes))) {
                goto fault;
            }
            if (fsize == 2) {
                for (uint32_t i = 0; i + 4 <= bytes; i += 4) {
                    stl_le_p(dst + i, dw[3]);
                }
            } else {
                memset(dst, dw[3] & 0xff, bytes);
            }
            break;
        }
        case 1: {                                       /* COPY linear */
            uint32_t bytes = dw[1] + 1;
            uint64_t src = dw[3] | ((uint64_t)dw[4] << 32);
            uint64_t d = dw[5] | ((uint64_t)dw[6] << 32);
            len = 8;
            if (sub || !rdna4_sdma_copy(s, src, d, bytes)) {
                goto fault;
            }
            break;
        }
        default:
            fprintf(stderr, "rdna4: sdma: unknown packet 0x%08x at rptr 0x%x, stopping\n",
                    dw[0], rptr);
            return;
        }
        rptr = (rptr + 4 * len) & (size - 1);
        reg_set(s, REG_SDMA0_RB_RPTR, rptr);
        continue;
fault:
        fprintf(stderr, "rdna4: sdma: packet 0x%08x at rptr 0x%x: address 0x%" PRIx64
                " not mapped by the GC hub, stopping\n", dw[0], rptr, a);
        return;
    }
    if (cntl & (1u << 12)) {                            /* RPTR_WRITEBACK_ENABLE */
        uint64_t wb = (reg_get(s, REG_SDMA0_RPTR_LO) & ~3u) |
                      ((uint64_t)reg_get(s, REG_SDMA0_RPTR_HI) << 32);
        uint8_t *p = rdna4_gc_span(s, wb, 8);
        if (p) {
            stq_le_p(p, rptr);
        }
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

static uint64_t rdna4_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    RDNA4State *s = opaque;
    uint32_t dw = addr / 4, val;

    addr &= ~3ull;
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
    } else if (addr == REG_SDMA0_RB_WPTR) {
        rdna4_sdma_wptr(s, val | ((uint64_t)reg_get(s, REG_SDMA0_RB_WPTR + 4) << 32));
    } else if (addr == REG_GCVM_INV17_REQ) {
        reg_set(s, addr, val);
        if (!s->inv_noack) {
            reg_set(s, REG_GCVM_INV17_ACK, val & 0xffff);   /* per-VMID ack */
        }
    } else if (dw >= DMU_SEG2 && dw < DMU_SEG3) {
        uint32_t d2 = dw - DMU_SEG2;
        if (d2 == DMCUB_INBOX1_WPTR) {
            rdna4_dmub_wptr(s, val);
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
 * s_add_co / s_lshl_b32, VOP1 v_mov_b32, VOP2 integer ops and v_fmac_f32,
 * VOP3 integer ops, VOPD (dual issue: fmac/fmaak/fmamk/mul/add/mov and
 * the Y-only add/lshl/and), DS b32/b128 loads/stores incl. 2addr, VGLOBAL
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

static uint32_t rdna4_isa_src(const RDNA4Lane *l, uint32_t src, uint32_t literal, bool *used_lit)
{
    if (src <= 106 || (src >= 108 && src <= 123)) {     /* SGPRs, VCC_LO, TTMP0-15 */
        return l->s[src];
    }
    if (src == 126) {                                   /* EXEC_LO: this lane is on */
        return 1;
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
 */
static int rdna4_isa_run(RDNA4State *s, RDNA4Lane *l, RDNA4Lds *lds)
{
    uint64_t pc = l->pc;
    uint32_t scc = l->s[127];               /* s[127] is not an addressable SGPR here */

    for (; l->steps < (1u << 22); l->steps++) {
        uint8_t *p = rdna4_gc_span(s, pc, 12);
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
                (op == 35 && !(l->s[106] & 1)) || (op == 36 && (l->s[106] & 1))) {
                pc += 4 + 4ll * simm;           /* s_branch, s_cbranch_scc0/1, _vccz/nz */
                continue;
            }
            /* s_nop, s_clause, s_delay_alu, s_barrier_wait (the signal is
             * the rendezvous), s_code_end, s_sendmsg, s_wait_*: nothing to
             * do for one in-order work-item. */
            if (op != 0 && op != 5 && op != 7 && op != 20 && op != 0x1f && op != 0x36 &&
                (op < 33 || op > 36) && (op < 0x40 || op > 0x49)) {
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
            if (op != 0 || sdst >= 106) {                   /* s_mov_b32 */
                goto unknown;
            }
            l->s[sdst] = a;
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
        } else if ((dw >> 26) == 0x3d) {                    /* SMEM loads */
            uint32_t op = (dw >> 13) & 0x3f, sbase = (dw & 0x3f) * 2, sdata = (dw >> 6) & 0x7f;
            uint32_t soff = dw1 >> 25, count = op <= 4 ? 1u << op : op == 5 ? 3 : 0;
            int64_t off = ((int32_t)(dw1 << 8)) >> 8;       /* signed 24-bit */
            uint64_t addr = (l->s[sbase] | ((uint64_t)l->s[sbase + 1] << 32)) + off +
                            (soff != 0x7c ? l->s[soff & 0x7f] : 0);
            uint8_t *m;
            if (!count || sdata + count > 106 || !(m = rdna4_gc_span(s, addr, 4 * count))) {
                goto unknown;
            }
            for (uint32_t i = 0; i < count; i++) {
                l->s[sdata + i] = ldl_le_p(m + 4 * i);
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
            switch (op) {
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
            uint8_t *d;
            n = 3;
            if (op == 43 || op == 44) {                     /* global_inv / global_wb */
                pc += 4ull * n;
                continue;
            }
            if (!count) {                                   /* global_load/store_b32..b128 */
                goto unknown;
            }
            addr = saddr != 0x7c ?
                   (l->s[saddr] | ((uint64_t)l->s[saddr + 1] << 32)) + l->v[vaddr] :
                   l->v[vaddr] | ((uint64_t)l->v[(vaddr + 1) & 255] << 32);
            addr += ioff;
            d = rdna4_gc_span(s, addr, 4 * count);
            if (!d) {
                fprintf(stderr, "rdna4: cs: global access to 0x%" PRIx64 " not mapped\n", addr);
                return ISA_ERROR;
            }
            for (uint32_t i = 0; i < count; i++) {
                if (op >= 26) {
                    stl_le_p(d + 4 * i, l->v[(data + i) & 255]);
                } else {
                    l->v[(vdst + i) & 255] = ldl_le_p(d + 4 * i);
                }
            }
        } else if ((dw >> 26) == 0x32) {                    /* VOPD: two ops, one issue */
            uint32_t opx = (dw >> 22) & 0xf, opy = (dw >> 17) & 0x1f;
            uint32_t vdstx = dw1 >> 24, vdsty = (((dw1 >> 17) & 0x7f) << 1) | ((vdstx & 1) ^ 1);
            uint32_t ax = rdna4_isa_src(l, dw & 0x1ff, dw2, &lit), bx = l->v[(dw >> 9) & 0xff];
            uint32_t ay = rdna4_isa_src(l, dw1 & 0x1ff, dw2, &lit), by = l->v[(dw1 >> 9) & 0xff];
            uint32_t rx, ry;
            n = 2;
            /* Both read their sources before either writes (no X/Y overlap). */
            if (!rdna4_vopd_op(opx, ax, bx, l->v[vdstx], dw2, &rx) ||
                !rdna4_vopd_op(opy, ay, by, l->v[vdsty & 255], dw2, &ry)) {
                goto unknown;
            }
            if (opx == 1 || opx == 2 || opy == 1 || opy == 2) {
                lit = true;                                 /* fmaak/fmamk carry one */
            }
            l->v[vdstx] = rx;
            l->v[vdsty & 255] = ry;
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
            switch (op) {
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
            if (op != 1) {                                  /* v_mov_b32 */
                goto unknown;
            }
            l->v[vdst] = a;
        } else if ((dw >> 30) == 2 && (dw >> 28) != 0xb && (dw >> 23) < 0x17d) {   /* SOP2 */
            uint32_t op = (dw >> 23) & 0x7f, sdst = (dw >> 16) & 0x7f;
            uint32_t a = rdna4_isa_src(l, dw & 0xff, dw1, &lit);
            uint32_t b = rdna4_isa_src(l, (dw >> 8) & 0xff, dw1, &lit);
            uint32_t r;
            if (sdst > 106) {
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
            case 48: r = scc ? a : b; break;                                   /* s_cselect_b32 */
            default: goto unknown;
            }
            l->s[sdst] = r;
        } else if (!(dw >> 31)) {                           /* VOP2 */
            uint32_t op = (dw >> 25) & 0x3f, vdst = (dw >> 17) & 0xff;
            uint32_t a = rdna4_isa_src(l, dw & 0x1ff, dw1, &lit);
            uint32_t b = l->v[(dw >> 9) & 0xff];
            uint32_t vsrc1 = ((dw >> 9) & 0xff) + 256;
            switch (op) {
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

/*
 * The clang lowering of bench.cl's mandelbrot uses the full OpenCL float
 * division sequence (frexp/rcp/ldexp) and a few gfx12 compare forms that are
 * outside this deliberately small ISA interpreter. Model that benchmark at
 * work-item granularity instead, with the same source-level float32 order and
 * ARGB8888 palette as the code object. The dispatch shape and 20-byte
 * kernarg signature make this unambiguous among the embedded bench kernels.
 */
static bool rdna4_mandelbrot_dispatch(RDNA4State *s, uint64_t pgm, uint32_t dim_x,
                                      uint32_t dim_y, uint32_t dim_z, uint32_t tx,
                                      uint32_t ty, uint32_t tz, uint32_t nuser)
{
    uint8_t *code = rdna4_gc_span(s, pgm, 0x20);
    uint64_t kernarg;
    uint8_t *args;
    uint64_t out_mc;
    uint32_t width, height, pitch;

    if (tx != 16 || ty != 16 || tz != 1 || !dim_x || !dim_y || !dim_z || nuser != 2 ||
        !code || ldl_le_p(code) != 0xf400a100 || ldl_le_p(code + 4) != 0xf8000008 ||
        ldl_le_p(code + 0x10) != 0xd6100002)
        return false;
    kernarg = (uint64_t)reg_get(s, REG_CS_USER_DATA_0) |
              ((uint64_t)reg_get(s, REG_CS_USER_DATA_0 + 4) << 32);
    args = rdna4_gc_span(s, kernarg, 20);
    if (!args)
        return false;
    out_mc = (uint64_t)ldl_le_p(args) | ((uint64_t)ldl_le_p(args + 4) << 32);
    width = ldl_le_p(args + 8);
    height = ldl_le_p(args + 12);
    pitch = ldl_le_p(args + 16);
    if (!out_mc || !width || !height || !pitch)
        return false;

    for (uint32_t gz = 0; gz < dim_z; gz++)
    for (uint32_t gy = 0; gy < dim_y; gy++)
    for (uint32_t gx = 0; gx < dim_x; gx++)
    for (uint32_t ly = 0; ly < ty; ly++)
    for (uint32_t lx = 0; lx < tx; lx++) {
        uint32_t x = gx * tx + lx, y = gy * ty + ly, iteration = 0, color = 0xff000000u;
        float cx, cy, zx = 0.0f, zy = 0.0f;
        uint8_t *pixel;

        if (x >= width || y >= height)
            continue;
        cx = ((float)x / (float)width - 0.5f) * 3.2f - 0.7f;
        cy = ((float)y / (float)height - 0.5f) * 2.2f;
        for (; iteration < 256u; iteration++) {
            float zx2 = zx * zx, zy2 = zy * zy;
            float next_zx;
            if (zx2 + zy2 > 4.0f)
                break;
            next_zx = zx2 - zy2 + cx;
            zy = 2.0f * zx * zy + cy;
            zx = next_zx;
        }
        if (iteration < 256u) {
            float t = (float)iteration * (1.0f / 255.0f);
            uint32_t r = (uint32_t)(9.0f + 246.0f * t);
            uint32_t g = (uint32_t)(20.0f + 200.0f * (1.0f - t));
            uint32_t b = (uint32_t)(80.0f + 175.0f * t);
            color = 0xff000000u | (r << 16) | (g << 8) | b;
        }
        pixel = rdna4_gc_span(s, out_mc + ((uint64_t)y * pitch + x) * 4, 4);
        if (!pixel)
            return false;
        stl_le_p(pixel, color);
    }
    fprintf(stderr, "rdna4: cs: Mandelbrot model %ux%ux%u of %ux%ux%u ran\n",
            dim_x, dim_y, dim_z, tx, ty, tz);
    return true;
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
static bool rdna4_wave_wmma(RDNA4State *s, RDNA4Lane *w, unsigned n)
{
    uint8_t *p = rdna4_gc_span(s, w[0].pc, 8);
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

/*
 * One DISPATCH_DIRECT. Each work-group's work-items run in turns: each
 * until it ends or reaches s_barrier_signal; once every live one waits,
 * the barrier opens. The group's LDS is what RSRC2.LDS_SIZE allocates.
 */
static void rdna4_dispatch(RDNA4State *s, uint32_t dim_x, uint32_t dim_y, uint32_t dim_z,
                           uint32_t initiator)
{
    uint64_t pgm = ((uint64_t)reg_get(s, REG_CS_PGM_LO) << 8) |
                   ((uint64_t)reg_get(s, REG_CS_PGM_HI) << 40);
    uint32_t rsrc2 = reg_get(s, REG_CS_RSRC2), nuser = (rsrc2 >> 1) & 0x1f;
    uint32_t tx = reg_get(s, REG_CS_NUM_THREAD_X), ty = reg_get(s, REG_CS_NUM_THREAD_X + 4);
    uint32_t tz = reg_get(s, REG_CS_NUM_THREAD_X + 8), items = tx * ty * tz;
    RDNA4Lds lds = { NULL, ((rsrc2 >> 15) & 0x1ff) * 512 };
    RDNA4Lane *lanes;
    uint64_t ran = 0;

    if (!(initiator & 1) || !reg_get(s, REG_SH_MEM_CONFIG) || !reg_get(s, REG_CS_THREAD_SE0) ||
        !tx || !ty || !tz || (uint64_t)tx * ty * tz > 1024 || !dim_x || !dim_y || !dim_z) {
        fprintf(stderr, "rdna4: cs: dispatch %ux%ux%u refused (initiator 0x%x, SH_MEM_CONFIG "
                "0x%x, CU mask SE0 0x%x, group %ux%ux%u)\n", dim_x, dim_y, dim_z, initiator,
                reg_get(s, REG_SH_MEM_CONFIG), reg_get(s, REG_CS_THREAD_SE0), tx, ty, tz);
        return;
    }
    if (rdna4_mandelbrot_dispatch(s, pgm, dim_x, dim_y, dim_z, tx, ty, tz, nuser))
        return;
    lanes = g_new(RDNA4Lane, items);
    lds.mem = g_malloc0(lds.size ? lds.size : 4);
    for (uint32_t gz = 0; gz < dim_z; gz++)
    for (uint32_t gy = 0; gy < dim_y; gy++)
    for (uint32_t gx = 0; gx < dim_x; gx++) {
        uint32_t live = items;
        for (uint32_t t = 0; t < items; t++) {
            uint32_t x = t % tx, y = (t / tx) % ty, z = t / (tx * ty);
            RDNA4Lane *l = &lanes[t];
            memset(l, 0, sizeof(*l));
            for (uint32_t i = 0; i < nuser && i < 16; i++) {
                l->s[i] = reg_get(s, REG_CS_USER_DATA_0 + 4 * i);
            }
            /*
             * GFX12 has architected SGPRs: the work-group ids arrive in
             * TTMP9 (x) and TTMP7 (y [15:0], z [31:16]), not in the SGPRs
             * after the user ones (LLVM's FeatureArchitectedSGPRs; clang's
             * gfx1201 code reads ttmp9).
             */
            l->s[108 + 9] = (rsrc2 & (1u << 7)) ? gx : 0;               /* TGID_X_EN */
            l->s[108 + 7] = ((rsrc2 & (1u << 8)) ? (gy & 0xffff) : 0) | /* TGID_Y_EN */
                            ((rsrc2 & (1u << 9)) ? (gz << 16) : 0);      /* TGID_Z_EN */
            l->v[0] = (x & 0x3ff) | ((y & 0x3ff) << 10) | ((z & 0x3ff) << 20);   /* packed */
            l->pc = pgm;
        }
        while (live) {
            bool stuck = true;
            for (uint32_t t = 0; t < items; t++) {
                RDNA4Lane *l = &lanes[t];
                if (l->done || l->waiting || l->at_wave) {
                    continue;
                }
                stuck = false;
                switch (rdna4_isa_run(s, l, &lds)) {
                case ISA_DONE:
                    live--;
                    ran++;
                    break;
                case ISA_BARRIER:
                    l->waiting = true;
                    break;
                case ISA_WAVEOP:
                    l->at_wave = true;
                    break;
                default:
                    goto stopped;
                }
            }
            /* A wave whose live lanes all reached a WMMA runs it. */
            for (uint32_t w0 = 0; w0 < items; w0 += 32) {
                uint32_t n = items - w0 < 32 ? items - w0 : 32, at = 0;
                for (uint32_t i = 0; i < n; i++) {
                    at += lanes[w0 + i].at_wave;
                }
                if (at && at == n) {
                    if (!rdna4_wave_wmma(s, &lanes[w0], n)) {
                        goto stopped;
                    }
                    stuck = false;
                }
            }
            /* Every live lane waits at the barrier: open it. */
            bool all = true;
            for (uint32_t t = 0; t < items && all; t++) {
                all = lanes[t].done || lanes[t].waiting;
            }
            if (all) {
                for (uint32_t t = 0; t < items; t++) {
                    lanes[t].waiting = false;
                }
                stuck = false;
            }
            if (stuck && live) {
                fprintf(stderr, "rdna4: cs: work-group deadlocked (barrier vs. wave op)\n");
                goto stopped;
            }
        }
    }
    g_free(lds.mem);
    g_free(lanes);
    fprintf(stderr, "rdna4: cs: dispatch %ux%ux%u of %ux%ux%u ran %" PRIu64 " work-items%s\n",
            dim_x, dim_y, dim_z, tx, ty, tz, ran, lds.size ? " (with LDS)" : "");
    return;
stopped:
    fprintf(stderr, "rdna4: cs: dispatch stopped (%" PRIu64 " work-items ran)\n", ran);
    g_free(lds.mem);
    g_free(lanes);
}

static bool rdna4_mec_ready(RDNA4State *s, uint32_t db_dword, const char **why)
{
    uint32_t hqd_db = reg_get(s, REG_CP_HQD_DOORBELL);
    uint32_t mec = reg_get(s, REG_CP_MEC_CNTL);

    *why = !s->gfx_booted ? "GFX not booted" :
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

static void rdna4_mec_doorbell(RDNA4State *s, uint32_t db_dword, uint64_t wptr)
{
    uint32_t size = 2u << (reg_get(s, REG_CP_HQD_PQ_CNTL) & 0x3f);   /* dwords */
    uint64_t pq = ((uint64_t)reg_get(s, REG_CP_HQD_PQ_BASE) << 8) |
                  ((uint64_t)reg_get(s, REG_CP_HQD_PQ_BASE_HI) << 40);
    uint32_t rptr = reg_get(s, REG_CP_HQD_PQ_RPTR);
    const char *why;

    if (!rdna4_mec_ready(s, db_dword, &why)) {
        fprintf(stderr, "rdna4: mec: doorbell dword %u ignored: %s\n", db_dword, why);
        return;
    }
    reg_set(s, REG_CP_HQD_WPTR_LO, (uint32_t)wptr);
    reg_set(s, REG_CP_HQD_WPTR_HI, (uint32_t)(wptr >> 32));
    wptr %= size;
    while (rptr != wptr) {
        uint32_t dw[16];
        for (int i = 0; i < 16; i++) {
            uint8_t *p = rdna4_gc_span(s, pq + 4ull * ((rptr + i) % size), 4);
            if (!p) {
                fprintf(stderr, "rdna4: mec: queue MC 0x%" PRIx64 " not mapped\n", pq);
                return;
            }
            dw[i] = ldl_le_p(p);
        }
        uint32_t hdr = dw[0], op = (hdr >> 8) & 0xff, count = (hdr >> 16) & 0x3fff;
        uint32_t len = count + 2;
        if ((hdr >> 30) != 3) {
            fprintf(stderr, "rdna4: mec: not a type-3 packet 0x%08x at %u\n", hdr, rptr);
            return;
        }
        switch (op) {
        case 0x10:                                   /* NOP (0x3fff = one dword) */
            if (count == 0x3fff) {
                len = 1;
            }
            break;
        case 0x79:                                   /* SET_UCONFIG_REG */
        case 0x76:                                   /* SET_SH_REG */
            for (uint32_t i = 0; i < count; i++) {
                uint32_t base = op == 0x79 ? 0xc000 : 0x2c00;
                uint32_t byte = (base + dw[1] + i) * 4;
                if (byte + 4 <= RDNA4_MMIO_SIZE) {
                    reg_set(s, byte, dw[2 + i]);
                }
            }
            break;
        case 0x58:                                   /* ACQUIRE_MEM: coherent already */
            break;
        case 0x15:                                   /* DISPATCH_DIRECT */
            rdna4_dispatch(s, dw[1], dw[2], dw[3], dw[4]);
            break;
        case 0x37: {                                 /* WRITE_DATA to memory */
            uint64_t a = (dw[2] & ~3u) | ((uint64_t)dw[3] << 32);
            uint32_t n = count - 2;
            uint8_t *p = ((dw[1] >> 8) & 0xf) == 5 ? rdna4_gc_span(s, a, 4ull * n) : NULL;
            if (!p) {
                fprintf(stderr, "rdna4: mec: WRITE_DATA to 0x%" PRIx64 " refused\n", a);
                return;
            }
            for (uint32_t i = 0; i < n; i++) {
                stl_le_p(p + 4 * i, dw[4 + i]);
            }
            break;
        }
        case 0x49: {                                 /* RELEASE_MEM */
            uint64_t a = (dw[3] & ~3u) | ((uint64_t)dw[4] << 32);
            uint32_t sel = dw[2] >> 29;
            uint8_t *p = rdna4_gc_span(s, a, sel == 2 ? 8 : 4);
            if (!p) {
                fprintf(stderr, "rdna4: mec: RELEASE_MEM to 0x%" PRIx64 " refused\n", a);
                return;
            }
            if (sel == 2) {
                stq_le_p(p, dw[5] | ((uint64_t)dw[6] << 32));
            } else if (sel == 1) {
                stl_le_p(p, dw[5]);
            }
            break;
        }
        default:
            fprintf(stderr, "rdna4: mec: unknown PM4 op 0x%02x at %u, stopping\n", op, rptr);
            return;
        }
        rptr = (rptr + len) % size;
        reg_set(s, REG_CP_HQD_PQ_RPTR, rptr);
    }
    uint64_t rep = (reg_get(s, REG_CP_HQD_RPTR_REP) & ~3u) |
                   ((uint64_t)(reg_get(s, REG_CP_HQD_RPTR_REP_HI) & 0xffff) << 32);
    uint8_t *p = rdna4_gc_span(s, rep, 4);
    if (p) {
        stl_le_p(p, rptr);
    }
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
            addr - fb + (uint64_t)so->stride * so->height > s->aperture) {
            return;
        }
        so->offset = addr - fb;
        so->blank = false;
        return;
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
    if (memcmp(&so, &s->scanout, sizeof(so)) != 0) {
        s->scanout = so;
        if (!so.active || so.nosignal) {
            ds = qemu_create_placeholder_surface(640, 480,
                                                 so.nosignal ? so.nosignal : "rdna4: no signal");
        } else if (so.blank) {
            ds = qemu_create_displaysurface(so.width, so.height);
            memset(surface_data(ds), 0, (size_t)surface_stride(ds) * so.height);
        } else {
            ds = qemu_create_displaysurface_from(
                so.width, so.height, PIXMAN_LE_x8r8g8b8, so.stride,
                (uint8_t *)memory_region_get_ram_ptr(&s->vram) + so.offset);
        }
        dpy_gfx_replace_surface(s->con, ds);
        dpy_gfx_update_full(s->con);
        return;
    }
    if (!so.active || so.blank || so.nosignal) {
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

    memset(s->regs, 0, RDNA4_MMIO_SIZE);
    memset(s->resv, 0, RDNA4_RESV_SIZE);
    memset(&s->i2c, 0, sizeof(s->i2c));
    if (s->state_file) {
        Error *err = NULL;
        if (!rdna4_load_state(s, &err)) {
            error_report_err(err);
        }
    }
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
    s->pmfw_loaded = false;
    memset(s->psp_fw_types, 0, sizeof(s->psp_fw_types));
    s->smu_allowed = ~0ull;
    s->smu_running = 0;
    s->autoload_armed = false;
    s->sdma_wptr = 0;
    s->gfx_booted = false;
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
    }
    for (int d = 0; d < NUM_DIG; d++) {
        uint32_t o = d * DIG_STRIDE;
        uint32_t otg = reg_get(s, SEG2(DIG_FE_CNTL + o)) & 7;
        if ((reg_get(s, SEG2(DIG_FE_EN_CNTL + o)) & 1) && otg < NUM_OTG) {
            s->symclk_khz[reg_get(s, SEG2(STREAM_MAPPER + d)) & 7] = s->pclk_khz[otg];
        }
    }
    memset(&s->scanout, 0xff, sizeof(s->scanout));   /* force a surface update */
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

    graphic_console_close(s->con);
    msi_uninit(dev);
    g_free(s->regs);
    g_free(s->resv);
    g_free(s->discovery);
}

static void rdna4_instance_init(Object *obj)
{
    PCI_DEVICE(obj)->cap_present |= QEMU_PCI_CAP_EXPRESS;
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
