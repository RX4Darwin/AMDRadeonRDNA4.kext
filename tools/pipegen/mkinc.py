#!/usr/bin/env python3
"""mkinc.py <dcn_4_1_0_offset.h> <pipegen trace> <linux commit> > src/pipe2_linux.inc

Turns pipegen's trace into the table src/pipe2.cpp builds its plan from, and refuses to if a step leaves
the blocks of the pipe being lit:

  - every register must be one of that pipe's own (by its name in Linux's register header), or
  - one of the shared registers listed in SHARED, and then only the bits listed there. Where Linux writes a
    shared register whole, the step is narrowed to those bits.
"""
import collections, re, sys

hdr, trace, commit = sys.argv[1:4]

base, off = {}, {}
for line in open(hdr):
    m = re.match(r'#define (reg\w+?)(_BASE_IDX)?\s+(0x[0-9a-fA-F]+|\d+)\s*$', line)
    if m:
        (base if m.group(2) else off)[m.group(1)[3:]] = int(m.group(3), 0)
names = collections.defaultdict(list)
for n, o in off.items():
    if n in base:
        names[(base[n], o)].append(n)
def addr(name):
    return base[name], off[name]

lines = [l.split() for l in open(trace) if l.strip()]
cfg = lines[0]
assert cfg[0] == 'CONFIG', 'no CONFIG line'
pipe, dig, link, hpd = map(int, cfg[1:5])
ha, hfp, hsw, hbp, va, vfp, vsw, vbp, khz, hpos, vpos, vstartup, det = map(int, cfg[5:18])

OWN = [f'{b}{pipe}_' for b in ('OTG', 'ODM', 'HUBP', 'HUBPREQ', 'HUBPRET', 'CURSOR0_', 'CNVC_CFG', 'CM_CUR', 'DSCL',
                               'CM', 'DPP_TOP', 'MPCC', 'MPCC_OGAM', 'MPCC_MCM', 'MPC_OUT', 'OPP_PIPE', 'FMT', 'DPG',
                               'VTG')]
OWN += [f'DOMAIN{pipe}_PG_', f'DPPCLK{pipe}_DTO_PARAM', f'DCHUBBUB_DET{pipe}_CTRL', f'PHYPLL{"ABCD"[link]}_']
OWN += [f'{b}{dig}_' for b in ('DIG', 'VPG', 'AFMT', 'DME')] + [f'DIG{link}_']
# Shared registers: the bits a step may change. Anything else of the register is left as found.
SHARED = {
    'OTG_PIXEL_RATE_DIV': 1 << (5 * pipe),             # OTGn_TMDS_PIXEL_RATE_DIV
    'DPPCLK_CTRL': 1 << (3 * pipe),                    # DPPCLKn_EN
    'MPC_OUT_CSC_COEF_FORMAT': 1 << pipe,              # MPC_OCSCn_COEF_FORMAT (Linux clears all four)
    'DENTIST_DISPCLK_CNTL': 0,                         # written back as read; Linux's FIFO-error workaround
    'DCHUBBUB_ARB_DATA_URGENCY_WATERMARK_A': 0,        # written back as read, to reach the new pipe
}
WAITABLE = {'DENTIST_DISPCLK_CNTL': 0x00080000}        # DENTIST_DISPCLK_CHG_DONE

def classify(seg, dword):
    ns = names.get((seg, dword))
    if not ns:
        sys.exit(f'refused: no register name for segment {seg} dword 0x{dword:04x}')
    for n in ns:
        if n in SHARED:
            return n, SHARED[n]
    for n in ns:
        if any(n.startswith(p) for p in OWN):
            return n, None
    sys.exit(f'refused: {" / ".join(ns)} is not a register of pipe {pipe}, DIG{dig} or link {link}')

out = []
def emit(kind, seg=0, dword=0, mask=0, value=0, arg=0, what=''):
    out.append((kind, seg, dword, mask, value, arg, what))

def require(name, mask, value, why):
    seg, dword = addr(name)
    emit('R', seg, dword, mask, value, 0, f'{name}: {why}')

require(f'OTG{pipe}_OTG_CONTROL', 0x00010001, 0, 'the timing generator is not running')
require(f'DIG{dig}_DIG_FE_EN_CNTL', 1, 0, 'the stream encoder is off')
require(f'DIG{link}_DIG_BE_EN_CNTL', 1, 0, 'the link encoder is off')

PLACEHOLDER_HI, PLACEHOLDER_LO = 0x0000c3e1, 0x5a3c1e00
requires_late = []      # requirements the trace states further down: they still go before the first write
part = None
for p in lines[1:]:
    k = p[0]
    if k == 'MARK':
        what, arg = p[1], int(p[2], 0)
        if what == 'require:mpcc_opp_id_none':
            emit('R', (arg >> 24) - 1, arg & 0xffffff, 0xf, 0xf, 0, f'{names[((arg >> 24) - 1, arg & 0xffffff)][0]}: the blender feeds no OPP')
        elif what == 'require:ip_request_open':
            seg, dword = (arg >> 24) - 1, arg & 0xffffff
            assert names[(seg, dword)] == ['DC_IP_REQUEST_CNTL']
            requires_late.append(('R', seg, dword, 1, 1, 0, 'DC_IP_REQUEST_CNTL: the IP request window is open'))
        elif what.startswith('begin:'):
            part = what[6:]
            emit({'init': '1', 'stream': '2', 'plane': '3'}[part], what=part)
        elif what == 'dmub:set_pixel_clock':
            emit('P', what='SET_PIXEL_CLOCK')
        elif what == 'dmub:encoder_control':
            emit('E', what='DIGX_ENCODER_CONTROL stream setup')
        elif what == 'dmub:transmitter_control':
            assert arg == 1, 'only the transmitter enable is expected'
            emit('X', what='DIG1_TRANSMITTER_CONTROL enable')
        elif what == 'scdc:write_tmds_config':
            emit('S', arg=arg, what='SCDC TMDS_CONFIG')
        elif what == 'scdc:read_status':
            pass                                        # Linux reads the scrambler status back and only logs it
        elif what == 'wait:flip':
            seg, dword = (arg >> 24) - 1, arg & 0xffffff
            name, _ = classify(seg, dword)
            emit('T', seg, dword, 0x100, 0, 100000, f'dcn401_post_unlock_program_front_end {name}')
        elif what != 'end':
            sys.exit(f'refused: unknown mark {what}')
        continue
    if k == 'DELAY':
        emit('D', arg=int(p[1]), what=p[3])
        continue
    seg, dword, mask, value, arg, func = int(p[1]), int(p[2], 0), int(p[3], 16), int(p[4], 16), int(p[5]), p[7]
    name, allowed = classify(seg, dword)
    what = f'{func} {name}'
    if k == 'WAIT':
        if allowed is not None and mask & ~WAITABLE.get(name, 0):
            sys.exit(f'refused: wait on {name} mask {mask:08x}')
        emit('T', seg, dword, mask, value, arg, what)
        continue
    if allowed is not None:
        if k == 'W' or mask & ~allowed:
            if k != 'W' and value & ~allowed & mask:
                # an update that sets bits outside the allowed ones (a full write only clears them)
                sys.exit(f'refused: {what} sets {value & ~allowed:08x} outside {allowed:08x}')
            mask &= allowed; value &= allowed; k = 'U'
    if name == f'HUBPREQ{pipe}_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH':
        assert k == 'W' and value == PLACEHOLDER_HI
        for vm in ('DCN_VM_SYSTEM_APERTURE_LOW_ADDR', 'DCN_VM_SYSTEM_APERTURE_HIGH_ADDR', 'DCN_VM_MX_L1_TLB_CNTL'):
            s, d = addr(f'HUBPREQ{pipe}_{vm}')
            s0, d0 = addr(f'HUBPREQ0_{vm}')
            assert s == s0 and (d - d0) % pipe == 0
            emit('C', s, d, 0, 0, (d - d0) // pipe, f'as the lit pipe has it: HUBPREQ{pipe}_{vm}')
        emit('H', seg, dword, what=what)
    elif name == f'HUBPREQ{pipe}_DCSURF_PRIMARY_SURFACE_ADDRESS':
        assert k == 'W' and value == PLACEHOLDER_LO
        emit('L', seg, dword, what=what)
    elif part == 'stream' and k == 'W' and name.startswith(f'DPG{pipe}_DPG_COLOUR_'):
        emit('K', seg, dword, mask, value, 'RGB'.index(name[len(f'DPG{pipe}_DPG_COLOUR_')]), what)
    else:
        emit(k, seg, dword, mask, value, arg, what)

first = next(i for i, o in enumerate(out) if o[0] != 'R')
out[first:first] = requires_late
kinds = collections.Counter(o[0] for o in out)
assert kinds['P'] == 1 and kinds['X'] == 1 and kinds['E'] == 2 and kinds['H'] == 1 and kinds['L'] == 1 and kinds['K'] == 3, kinds

det_regs = [addr(f'DCHUBBUB_DET{i}_CTRL') for i in range(4)]
assert all(s == det_regs[0][0] for s, _ in det_regs)
hblank, vblank = hfp + hsw + hbp, vfp + vsw + vbp
print(f'''// GENERATED by tools/pipegen from Linux {commit[:12]} (drivers/gpu/drm/amd/display). Do not edit:
// regenerate with tools/pipegen/run.sh. Every register step is one Linux's own DCN 4.01 code made when asked
// to light this stream and plane; the text of each step is the Linux function and the register.
//
// Pipe {pipe} (OTG{pipe}, OPP{pipe}, HUBP{pipe}, DPP{pipe}, MPCC{pipe}), stream encoder DIG{dig}, link {link} (PHY PLL {link}), HPD{hpd},
// {ha}x{va} at {khz} kHz, 8 bpc RGB over HDMI. {len(out)} entries.

static constexpr Config kConfig = {{
	{pipe}, {dig}, {link}, {hpd},
	{{ {khz}, {ha}, {hblank}, {hfp}, {hsw}, {va}, {vblank}, {vfp}, {vsw}, {'true' if hpos else 'false'}, {'true' if vpos else 'false'}, false }},
	{vstartup}, {det},
	{det_regs[0][0]}, {{ {', '.join(f'0x{d:04x}' for _, d in det_regs)} }},
}};

static const Gen kGen[] = {{''')
for kind, seg, dword, mask, value, arg, what in out:
    print(f"\t{{ '{kind}', {seg}, 0x{dword:04x}, 0x{mask:08x}, 0x{value:08x}, {arg}, \"{what}\" }},")
print('};')
sys.stderr.write(f'mkinc: {len(out)} entries, ' + ', '.join(f'{k}:{v}' for k, v in sorted(kinds.items())) + '\n')
