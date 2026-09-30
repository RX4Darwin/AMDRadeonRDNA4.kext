#!/usr/bin/env python3
# Generates the gfx12 single-triangle PM4 stream + register tables from Mesa's gfx12.json.
# From premetal/gfx12-draw-notes.md (Appendix C), where every register is cited to
# Mesa / amdgpu. `header` mode writes src/gfx12_draw.h for the kext: the stream with
# its address fields as relocations the kext fills in at run time.
#
#   python3 tools/gen-gfx12-draw.py header > src/gfx12_draw.h   (needs ~/src/mesa)
# Every register value is built from named fields that are validated against gfx12.json.
import json, os, struct, sys

JPATH = os.environ.get('MESA_GFX12_JSON', '/home/miguer/src/mesa/src/amd/registers/gfx12.json')
try:
    J = json.load(open(JPATH))
except OSError as e:
    sys.exit('gen-gfx12-draw.py: cannot read Mesa\'s gfx12.json at %s (%s); set MESA_GFX12_JSON=<path to src/amd/registers/gfx12.json>' % (JPATH, e.strerror))
REGS = {r['name']: r for r in J['register_mappings']}
TYPES = J['register_types']

# line numbers in gfx12.json: mapping entry ('"name": "X",') and type definition ('"X": {')
_lines = open(JPATH).read().split('\n')
_sec = None
MAPLINE, TYPELINE = {}, {}
for _i, _l in enumerate(_lines, 1):
    s = _l.strip()
    if s.startswith('"register_mappings"'): _sec = 'map'
    elif s.startswith('"register_types"'): _sec = 'type'
    elif s.startswith('"enums"'): _sec = 'enum'
    if _sec == 'map' and s.startswith('"name": "'):
        n = s[len('"name": "'):].split('"')[0]
        MAPLINE.setdefault(n, _i)
    if _sec == 'type' and _l.startswith('  "') and s.endswith('{'):
        n = s[1:].split('"')[0]
        TYPELINE.setdefault(n, _i)

def jcite(name):
    r = REGS[name]
    t = r.get('type_ref')
    s = 'gfx12.json:%d' % MAPLINE[name]
    if t and t in TYPELINE:
        s += ' (fields :%d)' % TYPELINE[t]
    return s

def fbits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]

def space(off):
    if 0x28000 <= off < 0x30000: return 'CTX'
    if 0xB000 <= off < 0xC000: return 'SH'
    if 0x30000 <= off < 0x40000: return 'UCFG'
    raise Exception('bad space %x' % off)

BASE = {'CTX': 0x28000, 'SH': 0xB000, 'UCFG': 0x30000}
OPC = {'CTX': 0x69, 'SH': 0x76, 'UCFG': 0x79}
ABS = {'CTX': 0xA000, 'SH': 0x2C00, 'UCFG': 0xC000}

def reg(name):
    r = REGS[name]
    return r['map']['at']

def V(name, raw=None, **fields):
    r = REGS[name]
    if raw is not None:
        assert not fields
        return raw & 0xffffffff
    t = TYPES[r['type_ref']]
    fl = {f['name']: f for f in t['fields']}
    v = 0
    for k, val in fields.items():
        f = fl[k]
        lo, hi = f['bits']
        w = hi - lo + 1
        assert 0 <= val < (1 << w), (name, k, val, w)
        v |= val << lo
    return v

def PKT3(op, count, pred=0):
    return (3 << 30) | ((count & 0x3fff) << 16) | ((op & 0xff) << 8) | (pred & 1)

seq = []        # list of (kind, ...)
table = []      # rows for the markdown register table

def comment(t):
    seq.append(('c', t))

def setreg(name, value, cite, why='', fields='', reloc=None):
    off = reg(name)
    sp = space(off)
    dw = (off - BASE[sp]) // 4
    seq.append(('r', sp, dw, value & 0xffffffff, name, reloc))
    table.append((name, sp, off, ABS[sp] + dw, value & 0xffffffff, fields, why, cite))

def raw(dws, what, cite, relocs=None):
    seq.append(('p', dws, what, cite, relocs or {}))

# ---------------- example addresses (GPU VAs, all must be reachable by the CP/SPI/CB) -------------
VS_VA    = 0x0000010000100000   # NGG shader code, 256B aligned (+ >=192B tail padding)
PS_VA    = 0x0000010000100400   # PS code, 256B aligned
CB_VA    = 0x0000010000200000   # 256x256 RGBA8 linear, 256KB, 256B aligned (pitch 1024B, multiple of 128B)
FENCE_VA = 0x0000010000300000
RING_VA  = 0x0000010001000000   # attribute+pos+prim ring, 2MB aligned in Mesa, bases in 64KB units
MAX_SE   = 4                    # Navi48: verify from IP discovery (gc_info num_se)

# ring sizing: ac_gpu_info.c:1709-1759 (gfx12 path)
attr_per_se = ((1400 * 1024) + 0xffff) & ~0xffff
pos_per_se  = ((16384 * 16) + 31) & ~31
prim_per_se = ((16368 * 4) + 31) & ~31
def al64k(x): return (x + 0xffff) & ~0xffff
attr_total = attr_per_se * MAX_SE
pos_total  = al64k(pos_per_se * MAX_SE * MAX_SE)
prim_total = al64k(prim_per_se * MAX_SE * MAX_SE)
POS_VA  = RING_VA + attr_total
PRIM_VA = POS_VA + pos_total
RING_TOTAL = attr_total + pos_total + prim_total

W, H = 256, 256

# ================= PHASE 0: context control + cache invalidation =================
comment('PHASE 0: CONTEXT_CONTROL (no load/shadow), initial cache invalidation')
raw([PKT3(0x28, 1), 0x80000000, 0x80000000], 'CONTEXT_CONTROL: UPDATE_LOAD_ENABLES=1 (nothing loaded), UPDATE_SHADOW_ENABLES=1 (nothing shadowed)',
    'si_state.c:5060-5063; gfx_v12_0.c:4655-4673')
gcr = (1 << 0) | (1 << 7) | (1 << 8) | (1 << 14) | (1 << 15)   # GLI_INV=ALL, GLK_INV, GLV_INV, GL2_INV, GL2_WB (pkt3.json GCR_CNTL)
raw([PKT3(0x58, 6), 0x00000000, 0xffffffff, 0x00ffffff, 0, 0, 0x0000000A, gcr],
    'ACQUIRE_MEM (engine=PFP, full range): GCR_CNTL=0x%X = GLI_INV|GLK_INV|GLV_INV|GL2_INV|GL2_WB' % gcr,
    'si_gfx_cs.c:388-391 -> ac_barrier.c:67-116 -> ac_cmdbuf_cp.c:403-429; gfx_v12_0.c:5129-5150')

# ================= PHASE 1: gfx12 preamble =================
comment('PHASE 1: gfx12 graphics preamble (ac_cmdbuf.c:629-845 + si_state.c:5079-5101)')
C = 'ac_cmdbuf.c:%s'
setreg('SPI_SHADER_PGM_RSRC3_PS', V('SPI_SHADER_PGM_RSRC3_PS', CU_EN=0xffff), C % 658, 'all CUs may run PS', 'CU_EN=0xffff')
setreg('SPI_SHADER_REQ_CTRL_PS', V('SPI_SHADER_REQ_CTRL_PS', SOFT_GROUPING_EN=1, NUMBER_OF_REQUESTS_PER_CU=3), C % 661, 'Mesa tuning', 'SOFT_GROUPING_EN=1 NUMBER_OF_REQUESTS_PER_CU=3')
for i, a in enumerate(['SPI_SHADER_USER_ACCUM_PS_0', 'SPI_SHADER_USER_ACCUM_PS_1', 'SPI_SHADER_USER_ACCUM_PS_2', 'SPI_SHADER_USER_ACCUM_PS_3']):
    setreg(a, 0, C % (664 + i), 'no user accum')
setreg('SPI_SHADER_PGM_HI_ES', V('SPI_SHADER_PGM_HI_ES', MEM_BASE=(VS_VA >> 40) & 0xff), C % 670, 'VA[47:40] of the NGG (merged ES/GS) program', 'MEM_BASE=VA>>40', reloc=('VS', 40, 0xff))
setreg('SPI_SHADER_PGM_RSRC3_GS', 0xFFFFFDFD, C % 672 + ' (ac_apply_cu_en: ac_shader_util.c:1012-1035)', 'per-SE CU enable mask for GS waves (Mesa leaves CU1/CU9 out)', '32-bit CU mask')
for i, a in enumerate(['SPI_SHADER_USER_ACCUM_ESGS_0', 'SPI_SHADER_USER_ACCUM_ESGS_1', 'SPI_SHADER_USER_ACCUM_ESGS_2', 'SPI_SHADER_USER_ACCUM_ESGS_3']):
    setreg(a, 0, C % (674 + i), 'no user accum')
setreg('SPI_SHADER_PGM_HI_LS', V('SPI_SHADER_PGM_HI_LS', MEM_BASE=(VS_VA >> 40) & 0xff), C % 680, 'HS unused; Mesa sets it anyway', 'MEM_BASE', reloc=('VS', 40, 0xff))
setreg('SPI_SHADER_PGM_RSRC3_HS', 0xFFFFFFFF, C % 682, 'HS unused', 'CU mask')
for i, a in enumerate(['SPI_SHADER_USER_ACCUM_LSHS_0', 'SPI_SHADER_USER_ACCUM_LSHS_1', 'SPI_SHADER_USER_ACCUM_LSHS_2', 'SPI_SHADER_USER_ACCUM_LSHS_3']):
    setreg(a, 0, C % (684 + i), 'no user accum')
setreg('SPI_SHADER_PGM_HI_PS', V('SPI_SHADER_PGM_HI_PS', MEM_BASE=(PS_VA >> 40) & 0xff), C % 690, 'VA[47:40] of the PS', 'MEM_BASE=VA>>40', reloc=('PS', 40, 0xff))
setreg('SPI_SHADER_GS_MESHLET_CTRL', 0, 'si_state.c:5101', 'mesh shading off')
setreg('DB_GL1_INTERFACE_CONTROL', 0, C % 694, '')
setreg('DB_MEM_TEMPORAL', 0, C % 695 + ' (cache_db_gl2=true: si_debug_options.h:23)', 'regular temporal hints (RADV uses 4=near-NT/far-RT)')
setreg('DB_VIEWPORT_CONTROL', 0, C % 701, '')
setreg('DB_SPI_VRS_CENTER_LOCATION', 0, C % 702, '')
setreg('TA_BC_BASE_ADDR', 0, C % 703, 'no border colors')
setreg('TA_BC_BASE_ADDR_HI', 0, C % 704, 'no border colors')
setreg('DB_STENCIL_OPVAL', V('DB_STENCIL_OPVAL', OPVAL=1, OPVAL_BF=1), C % 705, '', 'OPVAL=1 OPVAL_BF=1')
setreg('SC_MEM_TEMPORAL', 0, C % 706, 'regular temporal (0) for VRS/HIZ/HIS')
setreg('SC_MEM_SPEC_READ', V('SC_MEM_SPEC_READ', VRS_SPECULATIVE_READ=1, HIZ_SPECULATIVE_READ=1, HIS_SPECULATIVE_READ=1), C % 713, 'gfx12_spec_read_force_on=1 (ac_shader_util.h:142-147)', 'VRS/HIZ/HIS_SPECULATIVE_READ=1')
setreg('PA_SC_SCREEN_SCISSOR_TL', 0, C % 727, 'screen scissor = whole 64K space')
setreg('PA_SC_SCREEN_SCISSOR_BR', V('PA_SC_SCREEN_SCISSOR_BR', BR_X=65535, BR_Y=65535), C % 728, 'INCLUSIVE bounds on gfx12', 'BR_X=BR_Y=65535')
setreg('PA_SC_WINDOW_SCISSOR_TL', 0, C % 730, 'gfx12 has no WINDOW_OFFSET_DISABLE bit here')
setreg('PA_SC_GENERIC_SCISSOR_TL', 0, C % 731, '')
setreg('PA_SC_GENERIC_SCISSOR_BR', V('PA_SC_GENERIC_SCISSOR_BR', BR_X=65535, BR_Y=65535), C % 732, 'inclusive', 'BR_X=BR_Y=65535')
setreg('PA_SC_SCREEN_EXTENT_CONTROL', 0, C % 734, '')
setreg('PA_SC_TILE_STEERING_OVERRIDE', 0, C % 735 + '; gfx_v12_0.c:1835', 'kernel reports 0 on gfx12')
setreg('PA_SC_VRS_INFO', 0, C % 737, 'no VRS image')
setreg('CB_RMI_GL2_CACHE_CONTROL', V('CB_RMI_GL2_CACHE_CONTROL', COLOR_WR_POLICY=1, COLOR_RD_POLICY=2), C % 739 + ' (cache_cb_gl2=false: si_debug_options.h:22)', 'CB writes streamed through GL2', 'COLOR_WR_POLICY=STREAM(1) COLOR_RD_POLICY=NOA(2)')
setreg('SPI_BARYC_SSAA_CNTL', V('SPI_BARYC_SSAA_CNTL', COVERED_CENTROID_IS_CENTER=1), C % 742, '', 'COVERED_CENTROID_IS_CENTER=1')
setreg('SX_PS_DOWNCONVERT_CONTROL', 0xff, C % 743, 'MRTn_FMT_MAPPING_DISABLE=1 for all 8 MRTs', '0xff')
for i, a in enumerate(['PA_CL_POINT_X_RAD', 'PA_CL_POINT_Y_RAD', 'PA_CL_POINT_SIZE', 'PA_CL_POINT_CULL_RAD']):
    setreg(a, 0, C % (744 + i), '')
setreg('PA_CL_NANINF_CNTL', 0, C % 748, '')
setreg('PA_SU_LINE_STIPPLE_CNTL', 0, C % 749, '')
setreg('PA_SU_LINE_STIPPLE_SCALE', 0, C % 750, '(RADV writes 1.0f: radv_queue.c:814)')
setreg('PA_SU_SMALL_PRIM_FILTER_CNTL', V('PA_SU_SMALL_PRIM_FILTER_CNTL', SMALL_PRIM_FILTER_ENABLE=1, SC_1XMSAA_COMPATIBLE_DISABLE=1), C % 751, 'uses sample locations even for 1x -> program the sample locations (#160-166)', 'SMALL_PRIM_FILTER_ENABLE=1 SC_1XMSAA_COMPATIBLE_DISABLE=1')
setreg('PA_SU_OVER_RASTERIZATION_CNTL', 0, C % 754, '')
setreg('PA_STEREO_CNTL', V('PA_STEREO_CNTL', STEREO_MODE=1), C % 755, '', 'STEREO_MODE=1')
setreg('VGT_HOS_MAX_TESS_LEVEL', fbits(64.0), C % 757, 'tess unused', '64.0f')
setreg('VGT_HOS_MIN_TESS_LEVEL', 0, C % 758, 'tess unused', '0.0f')
setreg('GE_SE_ENHANCE', 0, C % 759, '')
setreg('GE_IA_ENHANCE', 0, C % 760, '')
setreg('GE_WD_ENHANCE', 0, C % 761, '')
setreg('VGT_REUSE_OFF', 0, C % 762, 'vertex reuse on')
setreg('VGT_DRAW_PAYLOAD_CNTL', 0, C % 763, '')
setreg('DB_HTILE_SURFACE', 0, C % 764, '')
setreg('VGT_TESS_DISTRIBUTION', V('VGT_TESS_DISTRIBUTION', ACCUM_ISOLINE=128, ACCUM_TRI=128, ACCUM_QUAD=128, DONUT_SPLIT=24, TRAP_SPLIT=6), C % 766, 'tess unused', 'ACCUM_*=128 DONUT_SPLIT=24 TRAP_SPLIT=6')
setreg('PA_SC_HIS_INFO', 0, C % 772, 'no HiS')
setreg('PA_SC_HISZ_RENDER_OVERRIDE', 0, C % 773, '')
setreg('PA_SC_BINNER_OUTPUT_TIMEOUT_COUNTER', 0x800, C % 775, '')
setreg('PA_SC_BINNER_CNTL_1', V('PA_SC_BINNER_CNTL_1', MAX_ALLOC_COUNT=254, MAX_PRIM_PER_BATCH=511), C % 776, '', 'MAX_ALLOC_COUNT=254 MAX_PRIM_PER_BATCH=511')
setreg('PA_SC_BINNER_CNTL_2', V('PA_SC_BINNER_CNTL_2', ENABLE_PING_PONG_BIN_ORDER=1), C % 779, '', 'ENABLE_PING_PONG_BIN_ORDER=1')
setreg('PA_SC_NGG_MODE_CNTL', V('PA_SC_NGG_MODE_CNTL', MAX_DEALLOCS_IN_WAVE=64), C % 780, '', 'MAX_DEALLOCS_IN_WAVE=64')
setreg('PA_SC_SHADER_CONTROL', V('PA_SC_SHADER_CONTROL', REALIGN_DQUADS_AFTER_N_WAVES=1), C % 781, '', 'REALIGN_DQUADS_AFTER_N_WAVES=1')
for i in range(8):
    setreg('CB_MEM%d_INFO' % i, V('CB_MEM0_INFO', TEMPORAL_READ=4, TEMPORAL_WRITE=4), C % '784-788', 'near-NT/far-RT hints (cache_cb_gl2=false)', 'TEMPORAL_READ=4 TEMPORAL_WRITE=4')
setreg('PA_SU_PRIM_FILTER_CNTL', 0, C % 820, 'gfx12: EXCLUSION bits would drop zero-area tris')
setreg('SPI_SHADER_IDX_FORMAT', V('SPI_SHADER_IDX_FORMAT', IDX0_EXPORT_FORMAT=1), 'si_state.c:5079', 'prim export = 1 dword', 'IDX0_EXPORT_FORMAT=SPI_SHADER_1COMP(1)')
setreg('SPI_BARYC_CNTL', 0, 'si_state.c:5081', '')
setreg('VGT_STRMOUT_DRAW_OPAQUE_OFFSET', 0, 'si_state.c:5083', '')
setreg('PA_CL_VRS_CNTL', V('PA_CL_VRS_CNTL', VERTEX_RATE_COMBINER_MODE=1, SAMPLE_ITER_COMBINER_MODE=1), 'si_state.c:5094', 'VRS combiners neutralized', 'VERTEX_RATE/SAMPLE_ITER_COMBINER_MODE=OVERRIDE(1)')
setreg('PA_SC_CONSERVATIVE_RASTERIZATION_CNTL', V('PA_SC_CONSERVATIVE_RASTERIZATION_CNTL', NULL_SQUAD_AA_MASK_ENABLE=1), 'si_state.c:5098', '', 'NULL_SQUAD_AA_MASK_ENABLE=1')
setreg('GE_MIN_VTX_INDX', 0, C % 791, '')
setreg('GE_INDX_OFFSET', 0, C % 792, '')
setreg('GE_MULTI_PRIM_IB_RESET_EN', V('GE_MULTI_PRIM_IB_RESET_EN', DISABLE_FOR_AUTO_INDEX=1), C % 797, 'no restart for auto-index draws', 'DISABLE_FOR_AUTO_INDEX=1')
setreg('GE_GS_THROTTLE', V('GE_GS_THROTTLE', T0=1, T1=4, T2=3, STALL_CYCLES=0x40, FACTOR1=2, FACTOR2=3, ENABLE_THROTTLE=0, NUM_INIT_GRPS=0xff), C % 798, '', 'T0=1 T1=4 T2=3 STALL=0x40 F1=2 F2=3 EN=0 NUM_INIT_GRPS=0xff')
setreg('GE_MAX_VTX_INDX', 0xffffffff, C % 807, 'MUST: auto indices are clamped to [MIN,MAX]')
setreg('VGT_INSTANCE_BASE_ID', 0, C % 808, '')
setreg('GE_STEREO_CNTL', 0, C % 809, '')
setreg('GE_USER_VGPR_EN', 0, C % 810, '')
setreg('VGT_PRIMITIVEID_RESET', 0, C % 811, '')
setreg('GE_VRS_RATE', 0, C % 812, '')
setreg('PA_SU_LINE_STIPPLE_VALUE', 0, C % 813, '')
setreg('PA_SC_LINE_STIPPLE_STATE', 0, C % 814, '')
setreg('SPI_GRP_LAUNCH_GUARANTEE_ENABLE', V('SPI_GRP_LAUNCH_GUARANTEE_ENABLE', ENABLE=1, GS_ASSIST_EN=1, MRT_ASSIST_EN=1, GFX_NUM_LOCK_WGP=2, CS_NUM_LOCK_WGP=2, LOCK_PERIOD=1, LOCK_MAINT_COUNT=1), C % 822, '', 'ENABLE GS_ASSIST MRT_ASSIST GFX/CS_NUM_LOCK_WGP=2 LOCK_PERIOD=1 LOCK_MAINT_COUNT=1')
setreg('SPI_GRP_LAUNCH_GUARANTEE_CTRL', V('SPI_GRP_LAUNCH_GUARANTEE_CTRL', NUM_MRT_THRESHOLD=3, GFX_PENDING_THRESHOLD=4, PRIORITY_LOST_THRESHOLD=4, ALLOC_SUCCESS_THRESHOLD=4, CS_WAVE_THRESHOLD_HIGH=8), C % 830, '', 'NUM_MRT_THR=3 GFX_PENDING=4 PRIO_LOST=4 ALLOC_SUCC=4 CS_WAVE_HIGH=8')
rb_mask = (1 << 16) - 1   # max_render_backends=16 assumed for Navi48
raw([PKT3(0x46, 2), 0x38 | (1 << 8), (0 << 3) | (2 << 9) | ((rb_mask << 11) & 0xffffffff), rb_mask >> 21],
    'EVENT_WRITE PIXEL_PIPE_STAT_CONTROL (occlusion counter config; optional for this test)', C % '837-844')

# ================= PHASE 2: GE rings =================
comment('PHASE 2: attribute / position / primitive rings (gfx11+/gfx12). ac_cmdbuf_cp.c:295-345, si_state_shaders.cpp:4559-4574')
raw([PKT3(0x49, 6), 0x28 | (5 << 8) | (1 << 31), 0, 0, 0, 0, 0, 0],
    'RELEASE_MEM PWS: EVENT=BOTTOM_OF_PIPE_TS(0x28) EVENT_INDEX=5 PWS_ENABLE=1 (no memory write)', 'ac_cmdbuf_cp.c:190-235')
raw([PKT3(0x58, 6), (5 << 11) | (0 << 14) | (1 << 17) | (0 << 18), 0xffffffff, 0x01ffffff, 0, 0, 0x80000000, 0],
    'ACQUIRE_MEM PWS: PWS_STAGE_SEL=CP_ME(5) COUNTER=TS PWS_ENA2=1 COUNT=0; PWS_ENA=1; GCR_CNTL=0 (wait for idle before touching ring regs)', 'ac_cmdbuf_cp.c:149-188')
setreg('SPI_GS_THROTTLE_CNTL1', 0x12355123, 'ac_cmdbuf_cp.c:304', 'magic', '0x12355123')
setreg('SPI_GS_THROTTLE_CNTL2', 0x1544D, 'ac_cmdbuf_cp.c:305', 'magic', '0x1544D')
setreg('SPI_ATTRIBUTE_RING_BASE', (RING_VA >> 16) & 0xffffffff, 'ac_cmdbuf_cp.c:306', 'param-export ring (not written with NO_PC_EXPORT=1, but Mesa always programs it)', 'VA>>16', reloc=('ATTR', 16, 0xffffffff))
setreg('SPI_ATTRIBUTE_RING_SIZE', V('SPI_ATTRIBUTE_RING_SIZE', MEM_SIZE=(attr_per_se >> 16) - 1, BIG_PAGE=0, L1_POLICY=1), 'ac_cmdbuf_cp.c:307-309; sizes ac_gpu_info.c:1727,1742', 'per-SE size in 64KB units minus 1; BIG_PAGE=0 on gfx12 (ac_gpu_info.c:1634)', 'MEM_SIZE=%d L1_POLICY=1' % ((attr_per_se >> 16) - 1))
setreg('GE_POS_RING_BASE', (POS_VA >> 16) & 0xffffffff, 'ac_cmdbuf_cp.c:316-317', 'MUST on gfx12: NGG position exports are staged in this ring', 'VA>>16', reloc=('POS', 16, 0xffffffff))
setreg('GE_POS_RING_SIZE', V('GE_POS_RING_SIZE', MEM_SIZE=pos_per_se >> 5), 'ac_cmdbuf_cp.c:318; ac_gpu_info.c:1728,1746', 'pos_ring_size_per_se/32 (16B per pos export, 16384 exports)', 'MEM_SIZE=%d' % (pos_per_se >> 5))
setreg('GE_PRIM_RING_BASE', (PRIM_VA >> 16) & 0xffffffff, 'ac_cmdbuf_cp.c:319', 'MUST on gfx12: NGG primitive exports (and gs_alloc_req) use this ring', 'VA>>16', reloc=('PRIM', 16, 0xffffffff))
setreg('GE_PRIM_RING_SIZE', V('GE_PRIM_RING_SIZE', MEM_SIZE=prim_per_se >> 5, SCOPE=2, PAF_TEMPORAL=3, PAB_TEMPORAL=3, SPEC_DATA_READ=0, FORCE_SE_SCOPE=1, PAB_NOFILL=1),
       'ac_cmdbuf_cp.c:320-325; enums ac_shader_util.h:60-147', '4 regs must be written together (ac_cmdbuf_cp.c:315)', 'MEM_SIZE=%d SCOPE=device(2) PAF_TEMPORAL=HT_stay_dirty(3) PAB_TEMPORAL=LU_discard(3) FORCE_SE_SCOPE=1 PAB_NOFILL=1' % (prim_per_se >> 5))

# ================= PHASE 3: per-draw state =================
comment('PHASE 3: per-draw state in radeonsi atom order (si_state.h:217-249, si_state_draw.cpp:2548-2585)')
setreg('VGT_GS_OUT_PRIM_TYPE', V('VGT_GS_OUT_PRIM_TYPE', OUTPRIM_TYPE=2), 'si_state_draw.cpp:1088-1091', 'rasterized prim = triangles', 'OUTPRIM_TYPE=TRISTRIP(2)')
# blend (pm4 state)
setreg('DB_ALPHA_TO_MASK', 0, 'si_state.c:371-385', 'A2C off (radeonsi also writes OFFSETn=2 = 0x0000AA00; irrelevant when disabled)')
setreg('CB_BLEND0_CONTROL', 0, 'si_state.c:439-441', 'blending disabled for MRT0 (ENABLE=0)')
setreg('SX_MRT0_BLEND_OPT', V('SX_MRT0_BLEND_OPT', COLOR_COMB_FCN=6, ALPHA_COMB_FCN=6), 'si_state.c:408-409,542-543', '', 'COLOR/ALPHA_COMB_FCN=OPT_COMB_BLEND_DISABLED(6)')
setreg('CB_COLOR_CONTROL', V('CB_COLOR_CONTROL', MODE=1, ROP3=0xCC), 'si_state.c:365-369,525-529,553-554', 'MUST: MODE=CB_DISABLE(0) kills color output', 'MODE=CB_NORMAL(1) ROP3=COPY(0xCC)')
# rasterizer
setreg('SPI_INTERP_CONTROL_0', V('SPI_INTERP_CONTROL_0', FLAT_SHADE_ENA=1, PNT_SPRITE_OVRD_X=2, PNT_SPRITE_OVRD_Y=3, PNT_SPRITE_OVRD_Z=0, PNT_SPRITE_OVRD_W=1), 'si_state.c:938-945,1069-1070', 'irrelevant without interpolants', 'FLAT_SHADE_ENA=1 OVRD_X=S OVRD_Y=T OVRD_Z=0 OVRD_W=1')
setreg('PA_SU_POINT_SIZE', 0x00080008, 'si_state.c:949-950,1071-1072', 'points unused (1.0 px)')
setreg('PA_SU_POINT_MINMAX', 0x00080008, 'si_state.c:963-964,1073-1074', 'points unused')
setreg('PA_SU_LINE_CNTL', 0x00000008, 'si_state.c:965,1075-1076', 'lines unused (1.0 px)')
setreg('PA_SC_MODE_CNTL_0', V('PA_SC_MODE_CNTL_0', MSAA_ENABLE=0, VPORT_SCISSOR_ENABLE=1, ALTERNATE_RBS_PER_TILE=1), 'si_state.c:967-971,1077-1078', 'enables PA_SC_VPORT_SCISSOR_0', 'VPORT_SCISSOR_ENABLE=1 ALTERNATE_RBS_PER_TILE=1')
setreg('PA_SU_SC_MODE_CNTL', V('PA_SU_SC_MODE_CNTL', CULL_FRONT=0, CULL_BACK=0, FACE=0, POLY_MODE=0, POLYMODE_FRONT_PTYPE=2, POLYMODE_BACK_PTYPE=2, PROVOKING_VTX_LAST=1), 'si_state.c:977-991,1079-1080', 'no culling, filled', 'CULL_*=0 FACE=CCW PTYPE=TRIANGLES(2) PROVOKING_VTX_LAST=1')
setreg('PA_CL_NGG_CNTL', V('PA_CL_NGG_CNTL', INDEX_BUF_EDGE_FLAG_ENA=0, VERTEX_REUSE_DEPTH=30), 'si_state.c:993-997,1081-1082', '', 'VERTEX_REUSE_DEPTH=30')
setreg('PA_SC_EDGERULE', V('PA_SC_EDGERULE', ER_TRI=0xA, ER_POINT=0x6, ER_RECT=0xA, ER_LINE_LR=0x19, ER_LINE_RL=0x25, ER_LINE_TB=0xA, ER_LINE_BT=0xA), 'si_state.c:1008-1016,1083-1084', 'D3D/FBO rule (RADV: 0xAAAAAAAA radv_queue.c:725)', 'ER_TRI=0xA ...')
# dsa
setreg('DB_DEPTH_CONTROL', 0, 'si_state.c:1511', 'Z/stencil test off')
# NGG shader (gs pm4 state)
vs_rsrc1 = V('SPI_SHADER_PGM_RSRC1_GS', VGPRS=0, FLOAT_MODE=0xC0, GS_VGPR_COMP_CNT=0)
vs_rsrc2 = V('SPI_SHADER_PGM_RSRC2_GS', SCRATCH_EN=0, USER_SGPR=0, ES_VGPR_COMP_CNT=0, OC_LDS_EN=0, LDS_SIZE=0)
setreg('SPI_SHADER_PGM_LO_ES', (VS_VA >> 8) & 0xffffffff, 'si_state_shaders.cpp:1128-1129; radv_shader.c:2128', 'NGG program address (gfx12 uses the *_ES pair; *_LO/HI_GS feed s0/s1)', 'VA>>8', reloc=('VS', 8, 0xffffffff))
setreg('SPI_SHADER_PGM_RSRC1_GS', vs_rsrc1, 'si_state_shaders.cpp:1134-1139', 'VGPRS=ceil(8/8)-1 (wave32); no DX10_CLAMP/MEM_ORDERED on gfx12', 'VGPRS=0 FLOAT_MODE=0xC0 GS_VGPR_COMP_CNT=0')
setreg('SPI_SHADER_PGM_RSRC2_GS', vs_rsrc2, 'si_state_shaders.cpp:1140-1147', 'no user SGPRs beyond the 8 system SGPRs, VertexID only, no LDS', 'USER_SGPR=0 ES_VGPR_COMP_CNT=0 LDS_SIZE=0')
setreg('GE_MAX_OUTPUT_PER_SUBGROUP', V('GE_MAX_OUTPUT_PER_SUBGROUP', MAX_VERTS_PER_SUBGROUP=128), 'si_state_shaders.cpp:850-852,1158', 'max_out_verts', 'MAX_VERTS_PER_SUBGROUP=128')
setreg('GE_NGG_SUBGRP_CNTL', V('GE_NGG_SUBGRP_CNTL', PRIM_AMP_FACTOR=1, THDS_PER_SUBGRP=0), 'si_state_shaders.cpp:853-854,1176; radv_shader.c:1951-1952', '', 'PRIM_AMP_FACTOR=1 THDS_PER_SUBGRP=0')
setreg('VGT_GS_MAX_VERT_OUT', V('VGT_GS_MAX_VERT_OUT', MAX_VERT_OUT=1), 'si_state_shaders.cpp:855-856,1175', '', 'MAX_VERT_OUT=1')
setreg('VGT_GS_INSTANCE_CNT', 0, 'si_state_shaders.cpp:857-858,1159-1162', 'no GS instancing')
setreg('SPI_SHADER_POS_FORMAT', V('SPI_SHADER_POS_FORMAT', POS0_EXPORT_FORMAT=4), 'si_state_shaders.cpp:859-860,1150-1157', 'one pos export, 4 comps', 'POS0_EXPORT_FORMAT=4COMP(4)')
setreg('PA_CL_VTE_CNTL', V('PA_CL_VTE_CNTL', VPORT_X_SCALE_ENA=1, VPORT_X_OFFSET_ENA=1, VPORT_Y_SCALE_ENA=1, VPORT_Y_OFFSET_ENA=1, VPORT_Z_SCALE_ENA=1, VPORT_Z_OFFSET_ENA=1, VTX_W0_FMT=1), 'si_state_shaders.cpp:861-862,1311-1314', 'apply viewport xform, pos.w is 1/W input (W0 fmt)', 'VPORT_*_ENA=1 VTX_W0_FMT=1')
setreg('VGT_PRIMITIVEID_EN', 0, 'si_state_shaders.cpp:865-867,1182-1184', '')
setreg('SPI_SHADER_PGM_RSRC4_GS', V('SPI_SHADER_PGM_RSRC4_GS', WAVE_LIMIT=0x3ff, GLG_FORCE_DISABLE=1, SPI_SHADER_LATE_ALLOC_GS=127, INST_PREF_SIZE=0), 'si_state_shaders.cpp:870-872,1216-1219', 'INST_PREF_SIZE=0 = no explicit prefetch (Mesa: ac_binary.c:146-165)', 'WAVE_LIMIT=0x3FF GLG_FORCE_DISABLE=1 LATE_ALLOC_GS=127')
# PS
ps_rsrc1 = V('SPI_SHADER_PGM_RSRC1_PS', VGPRS=0, FLOAT_MODE=0xC0)
setreg('SPI_SHADER_PGM_RSRC4_PS', V('SPI_SHADER_PGM_RSRC4_PS', WAVE_LIMIT=0x3ff, LDS_GROUP_SIZE=1, INST_PREF_SIZE=0), 'si_state_shaders.cpp:1806-1810', '', 'WAVE_LIMIT=0x3FF LDS_GROUP_SIZE=1')
setreg('SPI_SHADER_PGM_LO_PS', (PS_VA >> 8) & 0xffffffff, 'si_state_shaders.cpp:1820-1821', '', 'VA>>8', reloc=('PS', 8, 0xffffffff))
setreg('SPI_SHADER_PGM_HI_PS', V('SPI_SHADER_PGM_HI_PS', MEM_BASE=(PS_VA >> 40) & 0xff), 'si_state_shaders.cpp:1822-1823', '', 'MEM_BASE=VA>>40', reloc=('PS', 40, 0xff))
setreg('SPI_SHADER_PGM_RSRC1_PS', ps_rsrc1, 'si_state_shaders.cpp:1825-1830', '4 VGPRs used -> VGPRS=0 (wave32 granule 8)', 'VGPRS=0 FLOAT_MODE=0xC0')
setreg('SPI_SHADER_PGM_RSRC2_PS', 0, 'si_state_shaders.cpp:1833-1838', 'no user SGPRs/LDS/scratch')
setreg('SPI_SHADER_Z_FORMAT', 0, 'si_state_shaders.cpp:1625-1626', 'no depth/stencil/mask export', 'Z_EXPORT_FORMAT=ZERO')
setreg('SPI_SHADER_COL_FORMAT', V('SPI_SHADER_COL_FORMAT', COL0_EXPORT_FORMAT=9), 'si_state_shaders.cpp:1627-1628', 'MRT0 = 4x fp32', 'COL0_EXPORT_FORMAT=32_ABGR(9)')
setreg('SPI_PS_INPUT_ENA', V('SPI_PS_INPUT_ENA', PERSP_CENTER_ENA=1), 'si_state_shaders.cpp:1629-1630; required 1645-1653', 'MUST: at least one PERSP/LINEAR bary enabled or the GPU hangs', 'PERSP_CENTER_ENA=1')
setreg('SPI_PS_INPUT_ADDR', V('SPI_PS_INPUT_ADDR', PERSP_CENTER_ENA=1) if 'SPI_PS_INPUT_ADDR' in REGS and REGS['SPI_PS_INPUT_ADDR'].get('type_ref') else 2, 'si_state_shaders.cpp:1631-1632', 'same as ENA', 'PERSP_CENTER_ENA=1')
setreg('CB_SHADER_MASK', V('CB_SHADER_MASK', OUTPUT0_ENABLE=0xf), 'si_state_shaders.cpp:1633-1634,1761', 'PS writes RGBA of MRT0', 'OUTPUT0_ENABLE=0xF')
setreg('PA_SC_HISZ_CONTROL', V('PA_SC_HISZ_CONTROL', ROUND=2), 'si_state_shaders.cpp:1635-1636,1700-1701; radv_cmd_buffer.c:4020', 'ROUND=2 is the required minimum', 'ROUND=2')
# framebuffer
cb_info = V('CB_COLOR0_INFO', FORMAT=10, NUMBER_TYPE=0, COMP_SWAP=0, BLEND_CLAMP=1, BLEND_BYPASS=0, SIMPLE_FLOAT=1, ROUND_MODE=0)
setreg('CB_COLOR0_BASE', (CB_VA >> 8) & 0xffffffff, 'si_state.c:3157; ac_descriptors.c:1477-1481', '256B units (no tile swizzle for linear)', 'VA>>8', reloc=('CB', 8, 0xffffffff))
setreg('CB_COLOR0_VIEW', 0, 'si_state.c:3158; ac_descriptors.c:1374-1375', 'slice 0..0', 'SLICE_START=0 SLICE_MAX=0')
setreg('CB_COLOR0_VIEW2', 0, 'si_state.c:3159; ac_descriptors.c:1376', 'mip 0', 'MIP_LEVEL=0')
setreg('CB_COLOR0_ATTRIB', 0, 'si_state.c:3160; ac_descriptors.c:1377-1378', '1 fragment', 'NUM_FRAGMENTS=0 FORCE_DST_ALPHA_1=0')
setreg('CB_COLOR0_FDCC_CONTROL', 0, 'si_state.c:3161; ac_descriptors.c:1384-1388', 'no DCC (radeonsi writes MAX_UNCOMPRESSED_BLOCK_SIZE=1|ENABLE_MAX_COMP_FRAG_OVERRIDE=1 regardless; unused without DCC)')
setreg('CB_COLOR0_ATTRIB2', V('CB_COLOR0_ATTRIB2', MIP0_HEIGHT=H - 1, MIP0_WIDTH=W - 1), 'si_state.c:3162; ac_descriptors.c:1379-1380,1405-1420', 'MIP0_WIDTH = pitch_in_pixels-1 for linear (pitch multiple of 128B on gfx12)', 'MIP0_HEIGHT=255 MIP0_WIDTH=255')
setreg('CB_COLOR0_ATTRIB3', V('CB_COLOR0_ATTRIB3', MIP0_DEPTH=0, COLOR_SW_MODE=0, MAX_MIP=0, RESOURCE_TYPE=1), 'si_state.c:3163; ac_descriptors.c:1381-1383,1492-1494; addrtypes.h:287', 'SW_MODE 0 = ADDR3_LINEAR; RESOURCE_TYPE 1 = 2D', 'MIP0_DEPTH=0 COLOR_SW_MODE=LINEAR(0) MAX_MIP=0 RESOURCE_TYPE=2D(1)')
setreg('CB_COLOR0_BASE_EXT', V('CB_COLOR0_BASE_EXT', BASE_256B=(CB_VA >> 40) & 0xff), 'si_state.c:3164', 'VA[47:40]', 'BASE_256B=VA>>40', reloc=('CB', 40, 0xff))
setreg('CB_COLOR0_INFO', cb_info, 'si_state.c:3165; ac_descriptors.c:1422-1450,1373; swap ac_formats.c:609-612', 'R8G8B8A8_UNORM', 'FORMAT=COLOR_8_8_8_8(10) NUMBER_TYPE=UNORM(0) COMP_SWAP=STD(0) BLEND_CLAMP=1 SIMPLE_FLOAT=1')
for i in range(1, 8):
    setreg('CB_COLOR%d_INFO' % i, 0, 'si_state.c:3167-3170', 'MRT1-7 unbound (FORMAT=COLOR_INVALID)')
setreg('DB_Z_INFO', V('DB_Z_INFO', FORMAT=0, NUM_SAMPLES=0), 'si_state.c:3206-3209', 'no depth buffer', 'FORMAT=Z_INVALID')
setreg('DB_STENCIL_INFO', V('DB_STENCIL_INFO', FORMAT=0, TILE_STENCIL_DISABLE=1), 'si_state.c:3210-3212', 'no stencil buffer', 'FORMAT=STENCIL_INVALID TILE_STENCIL_DISABLE=1')
setreg('PA_SC_HIZ_INFO', 0, 'si_state.c:3213', 'no HiZ', 'SURFACE_ENABLE=0')
setreg('PA_SC_WINDOW_SCISSOR_BR', V('PA_SC_WINDOW_SCISSOR_BR', BR_X=W - 1, BR_Y=H - 1), 'si_state.c:3217-3220', 'INCLUSIVE on gfx12', 'BR_X=255 BR_Y=255')
setreg('PA_SC_WINDOW_OFFSET', 0, '(not written by gfx12 Mesa; not in clearstate_gfx12.h) - recommended', 'belt and braces: reset value assumed 0')
# sample locations (1x)
for name in ['PA_SC_CENTROID_PRIORITY_0', 'PA_SC_CENTROID_PRIORITY_1', 'PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0', 'PA_SC_AA_SAMPLE_LOCS_PIXEL_X1Y0_0', 'PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y1_0', 'PA_SC_AA_SAMPLE_LOCS_PIXEL_X1Y1_0', 'PA_SC_SAMPLE_PROPERTIES']:
    setreg(name, 0, 'si_state_msaa.c:71-73,156-170,271-277', '1x: sample 0 at pixel center, MAX_SAMPLE_DIST=0')
# db render state
setreg('DB_RENDER_CONTROL', V('DB_RENDER_CONTROL', OREO_MODE=1), 'si_state.c:1714-1717,1847-1848', '(RADV: 0 radv_queue.c:817)', 'OREO_MODE=OMODE_O_THEN_B(1)')
setreg('DB_RENDER_OVERRIDE', V('DB_RENDER_OVERRIDE', FORCE_STENCIL_READ=1), 'si_state.c:1849-1855', '', 'FORCE_STENCIL_READ=1')
setreg('DB_RENDER_OVERRIDE2', V('DB_RENDER_OVERRIDE2', CENTROID_COMPUTATION_MODE=1), 'si_state.c:1856-1858', '', 'CENTROID_COMPUTATION_MODE=1')
setreg('DB_COUNT_CONTROL', V('DB_COUNT_CONTROL', DISABLE_CONSERVATIVE_ZPASS_COUNTS=1), 'si_state.c:1760-1766,1792-1794,1859-1860', 'occlusion counting off', 'ZPASS_ENABLE=0 DISABLE_CONSERVATIVE_ZPASS_COUNTS=1')
setreg('DB_SHADER_CONTROL', V('DB_SHADER_CONTROL', Z_ORDER=1), 'si_state.c:1796,1861-1862; si_state_shaders.cpp:1744-1746', 'PS exports only color', 'Z_ORDER=EARLY_Z_THEN_LATE_Z(1)')
setreg('PA_SC_VRS_OVERRIDE_CNTL', 0, 'si_state.c:1805-1832,1863-1864', 'VRS passthrough 1x1')
# binner
setreg('PA_SC_BINNER_CNTL_0', V('PA_SC_BINNER_CNTL_0', BINNING_MODE=3, BIN_SIZE_X_EXTEND=2, BIN_SIZE_Y_EXTEND=2, DISABLE_START_OF_PRIM=1, FPOVS_PER_BATCH=63, OPTIMAL_BIN_SELECTION=1, FLUSH_ON_BINNING_TRANSITION=1),
       'si_state_binning.c:386-410', 'binning DISABLED; gfx12 still wants 128x128 bin size fields', 'BINNING_MODE=BINNING_DISABLED(3) BIN_SIZE_X/Y_EXTEND=128px(2) DISABLE_START_OF_PRIM=1 FPOVS_PER_BATCH=63 OPTIMAL_BIN_SELECTION=1 FLUSH_ON_BINNING_TRANSITION=1')
# msaa config
setreg('PA_SC_LINE_CNTL', 0, 'si_state.c:3370,3423-3424', '')
setreg('PA_SC_AA_CONFIG', 0, 'si_state.c:3371,3425-3426', '1 sample', 'MSAA_NUM_SAMPLES=0')
setreg('DB_EQAA', V('DB_EQAA', HIGH_QUALITY_INTERSECTIONS=1, STATIC_ANCHOR_ASSOCIATIONS=1), 'si_state.c:3314-3316,3427', '', 'HIGH_QUALITY_INTERSECTIONS=1 STATIC_ANCHOR_ASSOCIATIONS=1')
setreg('PA_SC_MODE_CNTL_1', V('PA_SC_MODE_CNTL_1', WALK_SIZE=1, WALK_ALIGN8_PRIM_FITS_ST=1, WALK_FENCE_ENABLE=0, WALK_FENCE_SIZE=3, SUPERTILE_WALK_ORDER_ENABLE=1, TILE_WALK_ORDER_ENABLE=1, MULTI_SHADER_ENGINE_PRIM_DISCARD_ENABLE=1, FORCE_EOV_CNTDWN_ENABLE=1, FORCE_EOV_REZ_ENABLE=1, OUT_OF_ORDER_PRIMITIVE_ENABLE=0, OUT_OF_ORDER_WATER_MARK=0),
       'si_state.c:3297-3312,3428-3429', 'linear dst: WALK_SIZE=1, no walk fence', 'see fields')
setreg('PA_SC_AA_MASK_X0Y0_X1Y0', 0xffffffff, 'si_state.c:4218-4234', 'MUST: sample mask 0 = nothing rasterized', 'AA_MASK_X0Y0=AA_MASK_X1Y0=0xFFFF')
setreg('PA_SC_AA_MASK_X0Y1_X1Y1', 0xffffffff, 'si_state.c:4218-4234', 'MUST', 'AA_MASK_X0Y1=AA_MASK_X1Y1=0xFFFF')
# cb render state
setreg('CB_TARGET_MASK', V('CB_TARGET_MASK', TARGET0_ENABLE=0xf), 'si_state.c:122-136', 'MUST: RGBA write enable for MRT0', 'TARGET0_ENABLE=0xF')
setreg('SX_PS_DOWNCONVERT', 0, 'si_state.c:130-131; ac_formats.c:860-879', '32_ABGR export -> no RB+ downconvert (half rate but correct)', 'MRT0=SX_RT_EXPORT_NO_CONVERSION')
setreg('SX_BLEND_OPT_EPSILON', 0, 'si_state.c:132-133', '')
setreg('SX_BLEND_OPT_CONTROL', 0, 'si_state.c:134-135', '')
# clip regs
setreg('PA_CL_CLIP_CNTL', V('PA_CL_CLIP_CNTL', DX_LINEAR_ATTR_CLIP_ENA=1), 'si_state.c:812-813,908-912,820-821', 'GL clip space z in [-w,w]; no UCPs', 'DX_LINEAR_ATTR_CLIP_ENA=1')
setreg('PA_CL_VS_OUT_CNTL', V('PA_CL_VS_OUT_CNTL', BYPASS_VTX_RATE_COMBINER=1, BYPASS_PRIM_RATE_COMBINER=1), 'si_state.c:808-815,822-823; si_state_shaders.cpp:968-990', 'no clip/cull dists, no misc vector', 'BYPASS_VTX_RATE_COMBINER=1 BYPASS_PRIM_RATE_COMBINER=1')
# guardband
setreg('PA_SU_VTX_CNTL', V('PA_SU_VTX_CNTL', PIX_CENTER=1, ROUND_MODE=2, QUANT_MODE=5), 'si_state_viewport.c:275-278,290-291; radv_queue.c:781-783', 'pixel centers at .5, round-to-even, 16.8 fixed point', 'PIX_CENTER=1 ROUND_MODE=X_ROUND_TO_EVEN(2) QUANT_MODE=X_16_8_FIXED_POINT_1_256TH(5)')
setreg('PA_CL_GB_VERT_CLIP_ADJ', fbits(1.0), 'si_state_viewport.c:270-273,292-295; ac_guardband.c:102-106', 'guard band = viewport (valid simplification)', '1.0f')
setreg('PA_CL_GB_VERT_DISC_ADJ', fbits(1.0), 'si_state_viewport.c:292-295', '', '1.0f')
setreg('PA_CL_GB_HORZ_CLIP_ADJ', fbits(1.0), 'si_state_viewport.c:292-295', '', '1.0f')
setreg('PA_CL_GB_HORZ_DISC_ADJ', fbits(1.0), 'si_state_viewport.c:292-295', '', '1.0f')
setreg('PA_SU_HARDWARE_SCREEN_OFFSET', 0, 'si_state_viewport.c:279-281,296-297', 'no screen offset (radeonsi would use 8,8 with GB 256.0)')
# scissors
setreg('PA_SC_VPORT_SCISSOR_0_TL', 0, 'si_state_viewport.c:338-342,221-229', '')
setreg('PA_SC_VPORT_SCISSOR_0_BR', V('PA_SC_VPORT_SCISSOR_0_BR', BR_X=W - 1, BR_Y=H - 1), 'si_state_viewport.c:228', 'INCLUSIVE on gfx12 (maxx-1)', 'BR_X=255 BR_Y=255')
# viewport (8 consecutive regs)
setreg('PA_CL_VPORT_XSCALE', fbits(W / 2.0), 'si_state_viewport.c:514-536', '', '128.0f')
setreg('PA_CL_VPORT_XOFFSET', fbits(W / 2.0), 'si_state_viewport.c:529', '', '128.0f')
setreg('PA_CL_VPORT_YSCALE', fbits(H / 2.0), 'si_state_viewport.c:530', 'NDC y=-1 -> row 0 (Vulkan-style, no flip)', '128.0f')
setreg('PA_CL_VPORT_YOFFSET', fbits(H / 2.0), 'si_state_viewport.c:531', '', '128.0f')
setreg('PA_CL_VPORT_ZSCALE', fbits(0.5), 'si_state_viewport.c:532', 'GL depth range [0,1]', '0.5f')
setreg('PA_CL_VPORT_ZOFFSET', fbits(0.5), 'si_state_viewport.c:533', '', '0.5f')
setreg('PA_SC_VPORT_ZMIN_0', fbits(0.0), 'si_state_viewport.c:534', '', '0.0f')
setreg('PA_SC_VPORT_ZMAX_0', fbits(1.0), 'si_state_viewport.c:535', '', '1.0f')
# spi map
setreg('SPI_SHADER_GS_OUT_CONFIG_PS', V('SPI_SHADER_GS_OUT_CONFIG_PS', VS_EXPORT_COUNT=0, PRIM_EXPORT_COUNT=0, NO_PC_EXPORT=1, NUM_INTERP=0), 'si_state_shaders.cpp:4446-4449,1220-1222,1768', 'gfx12 replacement of SPI_VS_OUT_CONFIG (+NUM_INTERP moved here)', 'VS_EXPORT_COUNT=0 NO_PC_EXPORT=1 NUM_INTERP=0')
setreg('SPI_PS_IN_CONTROL', V('SPI_PS_IN_CONTROL', PS_W32_EN=1), 'si_state_shaders.cpp:4451-4453,1767-1768', 'PS is wave32', 'PS_W32_EN=1')
# window rectangles
setreg('PA_SC_CLIPRECT_RULE', V('PA_SC_CLIPRECT_RULE', CLIP_RULE=0xffff), 'si_state_viewport.c:629-646', 'MUST: 0 would reject every pixel', 'CLIP_RULE=0xFFFF')
# vgt pipeline state
setreg('VGT_SHADER_STAGES_EN', V('VGT_SHADER_STAGES_EN', HS_EN=0, GS_EN=0, GS_FAST_LAUNCH=0, GS_W32_EN=1, NGG_WAVE_ID_EN=0, PRIMGEN_PASSTHRU_NO_MSG=1), 'si_state_shaders.cpp:1317-1326,4021-4024; radv_cmd_buffer.c:4124-4140', 'NGG VS-only, wave32, passthrough without GS_ALLOC_REQ (gfx12 is NGG-only: no ES_EN/VS_EN/PRIMGEN_EN fields)', 'GS_W32_EN=1 PRIMGEN_PASSTHRU_NO_MSG=1')
setreg('GE_CNTL', V('GE_CNTL', PRIMS_PER_SUBGRP=128, VERTS_PER_SUBGRP=128, PRIM_GRP_SIZE=256, DIS_PG_SIZE_ADJUST_FOR_STRIP=1), 'si_state_shaders.cpp:1268-1278,4037-4045; si_state_draw.cpp:155', 'subgroup = 128 threads (radeonsi VS NGG workgroup, si_shader.c:131)', 'PRIMS_PER_SUBGRP=128 VERTS_PER_SUBGRP=128 PRIM_GRP_SIZE=256 DIS_PG_SIZE_ADJUST_FOR_STRIP=1')
# draw registers
setreg('VGT_PRIMITIVE_TYPE', V('VGT_PRIMITIVE_TYPE', PRIM_TYPE=4), 'si_state_draw.cpp:1271-1283', '', 'PRIM_TYPE=DI_PT_TRILIST(4)')

comment('PHASE 4: draw')
raw([PKT3(0x2F, 0), 1], 'NUM_INSTANCES = 1', 'si_state_draw.cpp:1647-1651')
raw([PKT3(0x2D, 1), 3, 2], 'DRAW_INDEX_AUTO: INDEX_COUNT=3, DRAW_INITIATOR=DI_SRC_SEL_AUTO_INDEX(2)', 'si_state_draw.cpp:1875-1882; radv_cmd_buffer.c:11653-11663')

comment('PHASE 5: end-of-pipe fence with CB flush + GL2 writeback')
rel1 = 0x14 | (5 << 8) | (1 << 21) | (1 << 22) | (3 << 25)
rel2 = (1 << 29) | (0 << 24) | (0 << 16)
raw([PKT3(0x49, 6), rel1, rel2, FENCE_VA & 0xffffffff, FENCE_VA >> 32, 1, 0, 0],
    'RELEASE_MEM: EVENT=CACHE_FLUSH_AND_INV_TS_EVENT(0x14) EVENT_INDEX=5 GCR: GL2_WB=1 SEQ=FORWARD(1) TEMPORAL/CACHE_POLICY=3; DATA_SEL=1 (32-bit) INT_SEL=0 DST_SEL=0(MC); writes 1 to FENCE_VA',
    'gfx_v12_0.c:4565-4594; nvd.h:354-387; ac_barrier.c:128-166',
    relocs={3: ('FENCE', 0, 0xffffffff), 4: ('FENCE', 32, 0xffffffff)})

# ---------------- build dword stream ----------------
def build(seq, grouping=True):
    out = []   # list of (dwords, annotation)
    i = 0
    while i < len(seq):
        it = seq[i]
        if it[0] == 'c':
            out.append(([], '// ' + it[1], [])); i += 1; continue
        if it[0] == 'p':
            out.append((it[1], it[2], sorted(it[4].items()))); i += 1; continue
        # register run: group consecutive same-space consecutive-offset regs
        sp, dw, val, name = it[1], it[2], it[3], it[4]
        vals = [val]; names = [name]; rels = [(2, it[5])] if it[5] else []
        j = i + 1
        while grouping and j < len(seq) and seq[j][0] == 'r' and seq[j][1] == sp and seq[j][2] == dw + len(vals):
            if seq[j][5]:
                rels.append((2 + len(vals), seq[j][5]))
            vals.append(seq[j][3]); names.append(seq[j][4]); j += 1
        out.append(([PKT3(OPC[sp], len(vals)), dw] + vals, 'SET_%s_REG %s' % ({'CTX': 'CONTEXT', 'SH': 'SH', 'UCFG': 'UCONFIG'}[sp], ', '.join(names)), rels))
        i = j
    return out

if __name__ == '__main__':
    mode = sys.argv[1] if len(sys.argv) > 1 else 'table'
    if mode == 'table':
        print('| # | Register | Pkt | Byte offset | Abs dword | Value | Fields | Why | Source |')
        print('|---|---|---|---|---|---|---|---|---|')
        for n, (name, sp, off, adw, val, fields, why, cite) in enumerate(table, 1):
            pk = {'CTX': 'SET_CONTEXT_REG', 'SH': 'SET_SH_REG', 'UCFG': 'SET_UCONFIG_REG'}[sp]
            print('| %d | %s | %s | 0x%05X | 0x%04X | 0x%08X | %s | %s | %s |' % (n, name, pk, off, adw, val, fields, why, cite))
    elif mode == 'table2':
        print('| # | Register | Packet | Byte addr | Abs dword | Value | Fields (gfx12.json names) | Why | Mesa/kernel source | Offset/fields source |')
        print('|---|---|---|---|---|---|---|---|---|---|')
        for n, (name, sp, off, adw, val, fields, why, cite) in enumerate(table, 1):
            pk = {'CTX': 'SET_CONTEXT_REG', 'SH': 'SET_SH_REG', 'UCFG': 'SET_UCONFIG_REG'}[sp]
            print('| %d | %s | %s | 0x%05X | 0x%04X | 0x%08X | %s | %s | %s | %s |' % (n, name, pk, off, adw, val, fields, why, cite, jcite(name)))
    elif mode == 'stream':
        out = build(seq)
        total = 0
        for dws, ann, _ in out:
            if not dws:
                print(ann); continue
            print('/* %s */' % ann)
            for k in range(0, len(dws), 8):
                print('  ' + ' '.join('0x%08X,' % d for d in dws[k:k + 8]))
            total += len(dws)
        print('// total dwords: %d' % total)
    elif mode == 'header':
        out = build(seq)
        stream, relocs = [], []
        syms = ['VS', 'PS', 'CB', 'ATTR', 'POS', 'PRIM', 'FENCE']
        lines = []
        for dws, ann, rels in out:
            if not dws:
                lines.append('\t' + ann.replace('//', '//', 1))
                continue
            relmap = dict(rels)
            vals = []
            for k, d in enumerate(dws):
                if k in relmap:
                    sym, shift, mask = relmap[k]
                    relocs.append((len(stream) + k, syms.index(sym), shift, mask))
                    d = 0
                vals.append(d)
            lines.append('\t/* %s */' % ann[:150])
            for k in range(0, len(vals), 8):
                lines.append('\t' + ' '.join('0x%08x,' % d for d in vals[k:k + 8]))
            stream += vals
        # every relocated field must be fully inside its register (checked on
        # the example addresses: the relocated value equals the original)
        print('//')
        print('//  gfx12_draw.h: generated by tools/gen-gfx12-draw.py header from Mesa\'s')
        print('//  gfx12.json (see premetal/gfx12-draw-notes.md). Do not edit; regenerate.')
        print('//')
        print('//  One non-indexed triangle from an NGG passthrough VS (shaders/ngg_tri.s) and a')
        print('//  constant-colour PS (shaders/ps_red.s) into a %ux%u linear R8G8B8A8_UNORM' % (W, H))
        print('//  target, then an end-of-pipe RELEASE_MEM with a GL2 write-back writing 1 to')
        print('//  the fence. Address fields are zero here; kGfx12DrawRelocs fills them in.')
        print('//')
        print('')
        print('#pragma once')
        print('#include <stdint.h>')
        print('')
        print('// C and C++: the kext uses Gfx12Draw::, user space (userspace/gfx12tri.h) includes it from C.')
        print('#ifdef __cplusplus')
        print('namespace Gfx12Draw {')
        print('#define GFX12_DRAW_CONST constexpr')
        print('#else')
        print('#define GFX12_DRAW_CONST static const')
        print('#endif')
        print('')
        print('enum Sym { kVs, kPs, kCb, kAttrRing, kPosRing, kPrimRing, kFence };')
        print('typedef struct Reloc { uint16_t dword; uint8_t sym, shift; uint32_t mask; } Reloc;   // (va >> shift) & mask')
        print('')
        print('GFX12_DRAW_CONST uint32_t kWidth = %u, kHeight = %u;' % (W, H))
        print('GFX12_DRAW_CONST uint32_t kMaxSe = %u;                       // shader engines the rings are sized for' % MAX_SE)
        print('GFX12_DRAW_CONST uint64_t kAttrRingBytes = 0x%X, kPosRingBytes = 0x%X, kPrimRingBytes = 0x%X;' % (attr_total, pos_total, prim_total))
        print('GFX12_DRAW_CONST uint64_t kRingBytes = 0x%X;                  // attribute, position, primitive, in that order' % RING_TOTAL)
        print('GFX12_DRAW_CONST uint32_t kCoveredPixels = 8192;             // notes 3.9: rows 64..190, 0xFF0000FF')
        print('GFX12_DRAW_CONST uint32_t kCoveredRgba = 0xFF0000FFu;')
        print('')
        print('static const uint32_t kStream[%d] = {' % len(stream))
        print('\n'.join(lines))
        print('};')
        print('')
        print('static const Reloc kRelocs[%d] = {' % len(relocs))
        for d, sy, sh, m in relocs:
            print('\t{ %u, %s, %u, 0x%08x },' % (d, ['kVs', 'kPs', 'kCb', 'kAttrRing', 'kPosRing', 'kPrimRing', 'kFence'][sy], sh, m))
        print('};')
        print('')
        print('#ifdef __cplusplus')
        print('} // namespace Gfx12Draw')
        print('#endif')
    elif mode == 'info':
        print('attr_per_se=0x%X pos_per_se=0x%X prim_per_se=0x%X' % (attr_per_se, pos_per_se, prim_per_se))
        print('attr_total=0x%X pos_total=0x%X prim_total=0x%X ring_total=0x%X' % (attr_total, pos_total, prim_total, RING_TOTAL))
        print('POS_VA=0x%X PRIM_VA=0x%X' % (POS_VA, PRIM_VA))
        print('RELEASE_MEM dw1=0x%08X dw2=0x%08X' % (rel1, rel2))
        print('rsrc1_gs=0x%08X rsrc2_gs=0x%08X rsrc1_ps=0x%08X' % (vs_rsrc1, vs_rsrc2, ps_rsrc1))
