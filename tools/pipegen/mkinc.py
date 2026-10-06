#!/usr/bin/env python3
"""mkinc.py <dcn_4_1_0_offset.h> <pipegen trace> <linux commit> <trace of each mode> ... > src/pipe2_linux.inc

Turns pipegen's trace into the table src/pipe2.cpp builds its plan from, and refuses to if a step leaves
the blocks of the pipe being lit:

  - every register must be one of that pipe's own (by its name in Linux's register header), or
  - one of the shared registers listed in SHARED, and then only the bits listed there. Where Linux writes a
    shared register whole, the step is narrowed to those bits.
"""
import collections, re, sys

hdr, trace, commit = sys.argv[1:4]
mode_traces = sys.argv[4:]      # the lit mode first

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
    f'SYMCLK{"ABCD"[dig]}_CLOCK_ENABLE': 0x710,         # SYMCLKn_FE_EN, _FE_SRC_SEL: the stream encoder's half
}
WAITABLE = {'DENTIST_DISPCLK_CNTL': 0x00080000}        # DENTIST_DISPCLK_CHG_DONE

# A DisplayPort stream (pipegen's "dplink" scenario, told by its AUX transfers): the DP halves of the stream and
# link encoder, the pipe's pixel-rate DTO and its bits in the DTO's shared registers.
dp = any(p[0] == 'MARK' and p[1].startswith('aux:') for p in lines)
if dp:
    OWN += [f'DP{dig}_', f'DP{link}_', f'DP_DTO{pipe}_']
    SHARED['OTG_PIXEL_RATE_DIV'] |= 0xf << (1 + 5 * pipe)      # DPDTOn_INT
    SHARED['DTBCLK_P_CNTL'] = 0x7 << (3 * pipe)                # DTBCLK_Pn_SRC_SEL, _EN
    SHARED['DCCG_GATE_DISABLE_CNTL5'] = 1 << pipe              # DTBCLK_Pn_GATE_DISABLE
    SHARED['DCCG_GATE_DISABLE_CNTL3'] = 0x3 << (8 + 2 * pipe) | 0x3 << (20 + 2 * pipe)   # SYMCLK32_SEn, _LEn gates
SOURCE_OUI = bytes([0x00, 0x00, 0x1a])
# Two things in a DisplayPort trace are not the same on every link or in every mode, and the plan works them
# out when it runs (src/pipe2.cpp): the first DP_VID_M, 0x8000 x pixel clock / the link's symbol clock, before
# the hardware measures it ('m'), and the four MSA words of the timing ('M'), which wake sends again.
symclk = next((int(re.search(r':symclk=(\d+):', p[1]).group(1)) for p in lines
               if p[0] == 'MARK' and p[1].startswith('dmub:transmitter_control:action=1:')), 0)
def first_vid_m(pixel_khz):
    return 0x8000 * pixel_khz // symclk
MSA = [f'DP{dig}_DP_MSA_TIMING_PARAM{i}' for i in range(1, 5)]
# Link training: from the lane count going to the encoder to the look at the link status afterwards. The plan
# has one entry for it; src/dptrain.cpp does it when the plan runs. These are the functions Linux writes
# registers in on the way (src/dpphy.cpp has them).
TRAINING = ('enc1_configure_encoder', 'dcn10_link_encoder_set_dp_phy_pattern_training_pattern',
            'set_link_training_complete', 'enable_phy_bypass_mode', 'disable_prbs_mode', 'setup_panel_mode',
            'set_dp_phy_pattern_passthrough_mode', 'dp_wait_for_training_aux_rd_interval',
            'dp_transition_to_video_idle')
aux, training = [], False

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
# The AVI infoframe (generic packet 0, nine data words): words 1 to 4 depend on the timing and become 'A'
# entries, which src/pipe2.cpp computes for the timing the pipe runs. The steps of the last update are repeated
# as a part of their own, for after a mode switch.
avi_word, avi_from = 0, None
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
            emit({'init': '1', 'stream': '2', 'plane': '3', 'sleep': '4', 'wake': '5'}[part], what=part)
        elif training:
            assert what.startswith(('aux:', 'dmub:transmitter_control:action=1:', 'dmub:transmitter_control:action=11:')), what
            training = not what.startswith('aux:r:200:')
        elif what.startswith('aux:r:300:'):
            # dpcd_set_source_specific_data writes the source's OUI if the sink does not have it: the plan
            # writes it without asking
            aux.append((0x300, SOURCE_OUI))
            emit('a', arg=len(aux) - 1, what='DPCD source OUI')
        elif what.startswith('aux:r:107:'):
            pass                                        # read to see whether MSA_TIMING_PAR_IGNORE has to change: no
        elif what.startswith('aux:w:'):
            address, data = int(what.split(':')[2], 16), bytes.fromhex(what.split(':')[3])
            if (address, data) != (0x300, SOURCE_OUI):
                aux.append((address, data))
                emit('a', arg=len(aux) - 1, what=f'DPCD 0x{address:03x}')
        elif what.startswith('dmub:transmitter_control:action=0:'):
            emit('O', what='DIG1_TRANSMITTER_CONTROL disable')
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
    func = p[-1]
    if func == 'enc1_configure_encoder' and not training:
        training = True
        emit('t', what='link training')
    if training:
        assert func in TRAINING, f'a register write of {func} inside link training'
        continue
    if k == 'DELAY':
        emit('D', arg=int(p[1]), what=p[3])
        continue
    seg, dword, mask, value, arg = int(p[1]), int(p[2], 0), int(p[3], 16), int(p[4], 16), int(p[5])
    name, allowed = classify(seg, dword)
    what = f'{func} {name}'
    if dp and name == f'DP{dig}_DP_VID_M':
        assert k == 'U' and value == first_vid_m(khz), f'{what}: {value:#x} is not the first Mvid of this link'
        emit('m', seg, dword, mask, 0, 0, what)
        continue
    if dp and name in MSA:
        assert k == 'W'
        emit('M', seg, dword, mask, value, MSA.index(name), what)
        continue
    if func == 'vpg3_update_generic_info_packet':
        if k == 'WAIT':             # Linux sends it twice while enabling a stream: the last one is repeated
            avi_from = len(out)
        if name == f'VPG{dig}_VPG_GENERIC_PACKET_ACCESS_CTRL':
            assert k == 'U' and value == 0, 'generic packet 0 expected'
            avi_word = 0
        elif name == f'VPG{dig}_VPG_GENERIC_PACKET_DATA':
            assert k == 'W'
            if 1 <= avi_word <= 4:
                emit('A', seg, dword, mask, value, avi_word, what)
                avi_word += 1
                continue
            assert value == (0x000d0282 if avi_word == 0 else 0), 'AVI infoframe header or tail changed'
            avi_word += 1
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

assert part == 'wake' and not training
if not dp:
    # wait, lock, index, nine words, update
    avi = out[avi_from:avi_from + 13] if avi_from is not None else []
    assert ''.join(o[0] for o in avi) == 'TUUWAAAAWWWWU', 'no AVI infoframe update in the wake part'
    out += [('6', 0, 0, 0, 0, 0, 'avi')] + avi

first = next(i for i, o in enumerate(out) if o[0] != 'R')
out[first:first] = requires_late
kinds = collections.Counter(o[0] for o in out)
assert kinds['H'] == 1 and kinds['L'] == 1 and kinds['K'] == 3, kinds
if dp:      # lit and woken by training the link, put to sleep by the transmitter going off; no VBIOS-style setup
    assert kinds['t'] == 2 and kinds['O'] == 1 and not any(kinds[k] for k in 'PXEAS6'), kinds
    assert kinds['m'] == 2 and kinds['M'] >= 8 and kinds['M'] % 4 == 0, kinds
else:
    assert kinds['P'] == 2 and kinds['X'] == 2 and kinds['E'] == 4 and kinds['A'] == 20 and kinds['6'] == 1, kinds

det_regs = [addr(f'DCHUBBUB_DET{i}_CTRL') for i in range(4)]
assert all(s == det_regs[0][0] for s, _ in det_regs)
hblank, vblank = hfp + hsw + hbp, vfp + vsw + vbp
print(f'''// GENERATED by tools/pipegen from Linux {commit[:12]} (drivers/gpu/drm/amd/display). Do not edit:
// regenerate with tools/pipegen/run.sh. Every register step is one Linux's own DCN 4.01 code made when asked
// to light this stream and plane; the text of each step is the Linux function and the register.
//
// Pipe {pipe} (OTG{pipe}, OPP{pipe}, HUBP{pipe}, DPP{pipe}, MPCC{pipe}), stream encoder DIG{dig}, link {link} (PHY PLL {link}), HPD{hpd},
// {ha}x{va} at {khz} kHz, 8 bpc RGB over {'DisplayPort (the link is trained when the plan runs)' if dp else 'HDMI'}. {len(out)} entries.

static constexpr Config kConfig = {{
	{pipe}, {dig}, {link}, {hpd},
	{{ {khz}, {ha}, {hblank}, {hfp}, {hsw}, {va}, {vblank}, {vfp}, {vsw}, {'true' if hpos else 'false'}, {'true' if vpos else 'false'}, false }},
	{vstartup}, {det},
	{det_regs[0][0]}, {{ {', '.join(f'0x{d:04x}' for _, d in det_regs)} }},{' true,' if dp else ''}
}};
''')
if dp:
    print('// What the plan writes to the sink over AUX, in the order its \'a\' entries name them.')
    print('static const AuxWrite kAux[] = {')
    for address, data in aux:
        print(f"\t{{ 0x{address:03x}, {len(data)}, {{ {', '.join(f'0x{b:02x}' for b in data)} }} }},")
    print('};\n')
print('static const Gen kGen[] = {')
for kind, seg, dword, mask, value, arg, what in out:
    print(f"\t{{ '{kind}', {seg}, 0x{dword:04x}, 0x{mask:08x}, 0x{value:08x}, {arg}, \"{what}\" }},")
print('};')
sys.stderr.write(f'mkinc: {len(out)} entries, ' + ', '.join(f'{k}:{v}' for k, v in sorted(kinds.items())) + '\n')

# ---- what changes with the mode: the bits Linux leaves in each register after lighting, mode by mode ----
def lit_state(path):
    """CONFIG words and {(seg, dword): (mask, value)} of a trace's lighting part."""
    state, config = {}, None
    for p in (l.split() for l in open(path) if l.strip()):
        if p[0] == 'CONFIG':
            config = p[1:]
        elif p[0] == 'MARK' and p[1] == 'begin:sleep':
            break
        elif p[0] in ('W', 'U'):
            key, m, v = (int(p[1]), int(p[2], 0)), int(p[3], 16), int(p[4], 16)
            m0, v0 = state.get(key, (0, 0))
            state[key] = (0xffffffff, v) if p[0] == 'W' else (m0 | m, (v0 & ~m) | v)
    return config, state

# Who has the register in a mode switch (src/modeset.cpp runs first, then src/pipe2.cpp's modeSteps):
#   M  the mode-set engine programs it, to the value Linux does (tools/atomdump.cpp checks that),
#   S  the engine programs it by rule; Linux's value from DML replaces it for a mode of this table,
#   D  only this table has it.
ENGINE = [f'OTG{pipe}_OTG_{r}' for r in ('H_TOTAL', 'H_SYNC_A', 'H_SYNC_A_CNTL', 'H_BLANK_START_END', 'V_TOTAL',
                                         'V_SYNC_A', 'V_SYNC_A_CNTL', 'V_BLANK_START_END')]
ENGINE += [f'DPG{pipe}_DPG_DIMENSIONS', f'HUBP{pipe}_DCSURF_PRI_VIEWPORT_DIMENSION', f'DSCL{pipe}_RECOUT_SIZE',
           f'DSCL{pipe}_MPC_SIZE', f'HUBPREQ{pipe}_BLANK_OFFSET_0']
if dp:      # the engine's DisplayPort half: the pixel-rate DTO (as the clock it makes: see the host test) and the MSA
    ENGINE += [f'DP_DTO{pipe}_PHASE', f'DP_DTO{pipe}_MODULO'] + MSA
BY_RULE = [f'VTG{pipe}_CONTROL', f'OTG{pipe}_OTG_VSTARTUP_PARAM', f'OTG{pipe}_OTG_VUPDATE_PARAM']

modes = [lit_state(t) for t in mode_traces]
assert modes and modes[0][0][:15] == cfg[1:16], 'the first mode trace must be the lit mode'
assert all(set(m[1]) == set(modes[0][1]) for m in modes), 'a mode writes a register another does not'
keys = [k for k in modes[0][1] if any(m[1][k] != modes[0][1][k] for m in modes)]
if dp:      # the first Mvid follows the link as well as the mode: not in the table, and the rule holds in every mode
    vid_m = addr(f'DP{dig}_DP_VID_M')
    assert all(m[1][vid_m][1] == first_vid_m(int(m[0][12])) for m in modes), 'DP_VID_M is not the first Mvid in a mode'
    keys.remove(vid_m)
print(f'''
// What Linux leaves different from mode to mode in this pipe's registers, the plane showing the top-left of the
// {ha}x{va} surface (tools/pipegen/modes.txt): one row of values per mode, in the order of kModeRegs. Row 0 is
// the mode the plan lights.
static const ModeReg kModeRegs[] = {{''')
for k in keys:
    name, allowed = classify(*k)
    assert allowed is None, f'{name} is shared between pipes'
    masks = {m[1][k][0] for m in modes}
    assert len(masks) == 1, f'{name}: the bits written depend on the mode'
    who = 'M' if name in ENGINE else 'S' if name in BY_RULE else 'D'
    print(f"\t{{ '{who}', {k[0]}, 0x{k[1]:04x}, 0x{masks.pop():08x}, \"{name}\" }},")
print('};\n\nstatic const Edid::DetailedTiming kModeTimings[] = {')
for c, _ in modes:
    mha, mhfp, mhsw, mhbp, mva, mvfp, mvsw, mvbp, mkhz, mhpos, mvpos = map(int, c[4:15])
    print(f"\t{{ {mkhz}, {mha}, {mhfp + mhsw + mhbp}, {mhfp}, {mhsw}, {mva}, {mvfp + mvsw + mvbp}, {mvfp}, {mvsw}, "
          f"{'true' if mhpos else 'false'}, {'true' if mvpos else 'false'}, false }},")
print(f'}};\n\nstatic const uint32_t kModeValues[][{len(keys)}] = {{')
for c, state in modes:
    print('\t{ ' + ', '.join(f'0x{state[k][1]:08x}' for k in keys) + ' },')
print('};')
sys.stderr.write(f'mkinc: {len(modes)} modes, {len(keys)} registers change with the mode\n')
