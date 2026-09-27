/*
 * AMD Radeon RX 9070 XT (Navi 48, RDNA 4) display-engine model.
 *
 * A development model for RDNA4FB: it emulates what the kext's bare-metal
 * path touches, so the unmodified kext runs its real code paths in a VM.
 *
 *  - PCI identity and BAR layout of the card: BAR0/1 VRAM aperture, BAR2/3
 *    doorbells (inert), BAR4 I/O (inert), BAR5 registers, expansion ROM
 *    (romfile=, normally the card's legacy AtomBIOS image + an EFI GOP driver).
 *  - VRAM: the aperture is RAM; the rest of VRAM is reachable through
 *    MM_INDEX/MM_DATA, with the top 64 MiB modelled (the IP discovery copy
 *    the PSP leaves at VRAM top - 64 KiB lives there).
 *  - Registers: a flat BAR5 image, loaded at reset from a register image in
 *    tools/linux-capture.sh's format (state=), i.e. the state the GOP leaves.
 *  - Live engines: OTG frame counter and vblank status, DC_I2C (EDID from a
 *    DDC slave on one line), DP AUX (no sink attached), RCC_CONFIG_MEMSIZE.
 *  - Scanout: follows the lit OTG -> OPP -> MPCC -> HUBP surface registers
 *    into the QEMU console; the OPP pattern generator blanks it.
 *
 * Not a GPU: no GFX, SDMA, VCN, SMU or PSP behaviour.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "qemu/timer.h"
#include "qemu/host-utils.h"
#include "qemu/error-report.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pcie.h"
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
#define OTG_CONTROL          0x1b43     /* MASTER_EN [0], CURRENT_MASTER_EN_STATE [16] */
#define OTG_STATUS           0x1b49     /* V_BLANK [0] */
#define OTG_FRAME_COUNT      0x1b4d     /* [23:0] */
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
#define DCN_VM_FB_LOC_BASE   0x0475     /* [23:0] = MC address >> 24 */
/* MPC (seg3) */
#define MPCC_STRIDE          0x15
#define MPCC_OPP_ID          0x0002     /* [3:0], 0xf = none */

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

typedef struct RDNA4Scanout {
    bool     active;         /* an OTG is running */
    bool     blank;          /* pattern generator on, or nothing to fetch */
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

    uint32_t *regs;           /* BAR5 image, RDNA4_MMIO_SIZE bytes */
    uint8_t  *resv;           /* top RDNA4_RESV_SIZE bytes of VRAM */
    uint8_t  edid[256];
    uint32_t edid_len;
    uint8_t  *discovery;      /* IP discovery binary from the flash image */
    uint32_t discovery_len;

    RDNA4I2C     i2c;
    int64_t      otg_epoch[NUM_OTG];
    RDNA4Scanout scanout;
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
    if (off >= resv_base && off + 4 <= rdna4_vram_size()) {
        return s->resv + (off - resv_base);
    }
    return NULL;
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
 * Pixels scanned out since the OTG was enabled. The pixel clock lives in
 * the PHY PLL, which the model does not have yet: the OTG runs at 60 Hz of
 * whatever raster its timing registers describe.
 */
static bool rdna4_otg_position(RDNA4State *s, int otg, uint64_t *frame,
                               uint32_t *line)
{
    uint64_t htot = (rdna4_otg_reg(s, otg, OTG_H_TOTAL) & 0x7fff) + 1;
    uint64_t vtot = (rdna4_otg_reg(s, otg, OTG_V_TOTAL) & 0x7fff) + 1;
    uint64_t pclk = htot * vtot * 60;
    int64_t ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->otg_epoch[otg];
    uint64_t pixels;

    if (!(rdna4_otg_reg(s, otg, OTG_CONTROL) & 1) || ns < 0 ||
        pclk > UINT32_MAX) {
        return false;
    }
    pixels = muldiv64(ns, (uint32_t)pclk, NANOSECONDS_PER_SECOND);
    *frame = pixels / (htot * vtot);
    *line = (pixels % (htot * vtot)) / htot;
    return true;
}

static uint32_t rdna4_otg_read(RDNA4State *s, int otg, uint32_t dw)
{
    uint32_t val = rdna4_otg_reg(s, otg, dw);
    uint64_t frame;
    uint32_t line, vb;

    switch (dw) {
    case OTG_CONTROL:
        return (val & ~(1u << 16)) | ((val & 1) << 16);
    case OTG_FRAME_COUNT:
        if (rdna4_otg_position(s, otg, &frame, &line)) {
            return (val & ~0xffffffu) | (frame & 0xffffff);
        }
        return val;
    case OTG_STATUS:
        if (rdna4_otg_position(s, otg, &frame, &line)) {
            vb = rdna4_otg_reg(s, otg, OTG_V_BLANK);
            bool in_blank = line < ((vb >> 16) & 0x7fff) || line >= (vb & 0x7fff);
            return (val & ~1u) | in_blank;
        }
        return val;
    }
    return val;
}

static void rdna4_otg_write(RDNA4State *s, int otg, uint32_t dw, uint32_t val)
{
    uint32_t old = rdna4_otg_reg(s, otg, dw);

    if (dw == OTG_CONTROL && (val & 1) && !(old & 1)) {
        s->otg_epoch[otg] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    reg_set(s, SEG2(dw + otg * OTG_STRIDE), val);
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
        if (d2 >= OTG_H_TOTAL && d2 < OTG_H_TOTAL + NUM_OTG * OTG_STRIDE) {
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
    } else if (dw >= DMU_SEG2 && dw < DMU_SEG3) {
        uint32_t d2 = dw - DMU_SEG2;
        if (d2 >= OTG_H_TOTAL && d2 < OTG_H_TOTAL + NUM_OTG * OTG_STRIDE) {
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

/* BAR2 doorbells and BAR4 I/O: present, inert. */
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

/* ---- scanout ------------------------------------------------------------ */

/* What the display pipe would put on the wire right now. */
static void rdna4_get_scanout(RDNA4State *s, RDNA4Scanout *so)
{
    memset(so, 0, sizeof(*so));
    for (int otg = 0; otg < NUM_OTG; otg++) {
        uint32_t hb, vb, opp, pitch, hp;
        uint64_t addr, fb;
        int hubp = -1;

        if (!(rdna4_otg_reg(s, otg, OTG_CONTROL) & 1)) {
            continue;
        }
        hb = rdna4_otg_reg(s, otg, OTG_H_BLANK);
        vb = rdna4_otg_reg(s, otg, OTG_V_BLANK);
        so->active = true;
        so->width = (hb & 0x7fff) - ((hb >> 16) & 0x7fff);
        so->height = (vb & 0x7fff) - ((vb >> 16) & 0x7fff);
        so->blank = true;

        opp = (reg_get(s, SEG2(OPTC_DATA_SOURCE + otg * ODM_STRIDE)) >> 16) & 0xf;
        if (opp >= NUM_OTG || (reg_get(s, SEG2(DPG_CONTROL + opp * OPP_STRIDE)) & 1)) {
            return;
        }
        for (int m = 0; m < NUM_OTG && hubp < 0; m++) {
            if ((reg_get(s, SEG3(MPCC_OPP_ID + m * MPCC_STRIDE)) & 0xf) == opp) {
                hubp = m;
            }
        }
        if (hubp < 0) {
            return;
        }
        hp = hubp * HUBP_STRIDE;
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
        if (!so.active) {
            ds = qemu_create_placeholder_surface(640, 480, "rdna4: no signal");
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
    if (!so.active || so.blank) {
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
    for (int otg = 0; otg < NUM_OTG; otg++) {
        s->otg_epoch[otg] = now;
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
    if (!rdna4_load_state(s, errp)) {
        return;
    }

    if (!memory_region_init_ram(&s->vram, obj, "rdna4.vram", s->aperture, errp)) {
        return;
    }
    memory_region_init_io(&s->mmio, obj, &rdna4_mmio_ops, s, "rdna4.mmio",
                          RDNA4_MMIO_SIZE);
    memory_region_init_io(&s->doorbell, obj, &rdna4_inert_ops, s, "rdna4.doorbell",
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

    s->con = graphic_console_init(DEVICE(dev), 0, &rdna4_gfx_ops, s);
    memory_region_set_log(&s->vram, true, DIRTY_MEMORY_VGA);
}

static void rdna4_exit(PCIDevice *dev)
{
    RDNA4State *s = RDNA4(dev);

    graphic_console_close(s->con);
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
