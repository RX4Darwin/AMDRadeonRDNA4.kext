#!/usr/bin/env python3
"""M1 helper (docs/m1-stream.md): extract the command table of Apple's PGSerializer from AppleParavirtGPUMetal's x86_64 slice.

Every serialiser method that emits a command calls -[PGSerializerCommandEncoder getCommandBytes:forCommand:] (payload length, command id) and then stores
the operands into the returned pointer. This scans each method for such call sites, recovers (command id, payload length) from the immediates, and lists
the stores into the returned buffer as (offset, width, source). Sources are recognised as: an immediate, an incoming argument of the method, or the result
of an Objective-C message (named by its selector). It is a reading aid, not a decompiler: variable-length commands and branchy writers are listed with
what could be seen.

  apvcmds.py table              one line per command: id, payload length, method(s)
  apvcmds.py layout [substr]    the per-method store lists
  apvcmds.py json               everything as JSON (for tools/m1/gen-opcodes.py)
Uses tools/m1/apvdis.py (same $APV)."""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import apvdis

ARG_REGS = ['rdi', 'rsi', 'rdx', 'rcx', 'r8', 'r9']
REG32 = {'edi': 'rdi', 'esi': 'rsi', 'edx': 'rdx', 'ecx': 'rcx', 'r8d': 'r8', 'r9d': 'r9', 'eax': 'rax', 'ebx': 'rbx', 'r10d': 'r10', 'r11d': 'r11',
         'r12d': 'r12', 'r13d': 'r13', 'r14d': 'r14', 'r15d': 'r15'}
REG16 = {'di': 'rdi', 'si': 'rsi', 'dx': 'rdx', 'cx': 'rcx', 'ax': 'rax', 'bx': 'rbx', 'r8w': 'r8', 'r9w': 'r9'}
REG8 = {'dil': 'rdi', 'sil': 'rsi', 'dl': 'rdx', 'cl': 'rcx', 'al': 'rax', 'bl': 'rbx', 'r8b': 'r8', 'r9b': 'r9', 'r12b': 'r12', 'r13b': 'r13',
        'r14b': 'r14', 'r15b': 'r15'}


def canon(r):
    r = r.lstrip('%')
    return REG32.get(r) or REG16.get(r) or REG8.get(r) or r


def parse(line):
    m = re.match(r'\s*([0-9a-f]+):\s*(\S+)\s*(.*?)(?:\s+##.*)?$', line)
    if not m:
        return None
    return int(m.group(1), 16), m.group(2), m.group(3)


def width_of(op):
    if op in ('movups', 'movaps', 'movdqu', 'movdqa'):
        return 16
    if op in ('movsd', 'movlps', 'movhps', 'movlpd'):
        return 8
    if op == 'movss' or op == 'movd':
        return 4
    if op.endswith('b'):
        return 1
    if op.endswith('w'):
        return 2
    if op.endswith('l'):
        return 4
    if op.endswith('q'):
        return 8
    return 0


def scan_method(ix, imp, name, getsel_slot, selprefix='getCommandBytes', mode='cmd'):
    end = ix.bounds(imp)
    lines = apvdis.run_objdump(imp, end)
    addrs = [int(re.match(r'\s*([0-9a-f]+):', l).group(1), 16) for l in lines]
    for a, b in zip(addrs, addrs[1:] + [end]):
        ix.next_ip[a] = b
    ins = []
    for ln in lines:
        p = parse(ix.annotate(ln).split('   ; ')[0])
        note = ix.annotate(ln).split('   ; ')[1] if '   ; ' in ix.annotate(ln) else ''
        if p:
            ins.append((p[0], p[1], p[2], note))
    # incoming argument spills: movX %reg, -N(%rbp) in the prologue
    spill = {}
    for a, op, args, note in ins[:30]:
        m = re.match(r'%(\w+), (-0x[0-9a-f]+)\(%rbp\)$', args)
        if m and op.startswith('mov'):
            r = canon(m.group(1))
            if r in ARG_REGS[2:]:
                spill[m.group(2)] = 'arg%d' % (ARG_REGS.index(r) - 1)
    # forward pass over the linear listing: what is in rsi/rdx/rcx at each message send (selector, immediates), through copies and stack spills
    cmds = []
    st = {}
    slots = {}
    for i, (a, op, args, note) in enumerate(ins):
        m = re.match(r'(\S+), %(\w+)$', args)
        if (op.startswith('mov')) and m:
            src, dst = m.group(1), canon(m.group(2))
            if 'sel: ' in note and src.endswith('(%rip)'):
                st[dst] = ('sel', note.split('sel: ')[1].split(' | ')[0])
            elif src.startswith('$'):
                st[dst] = ('imm', int(src[1:], 0))
            elif src.startswith('%'):
                st[dst] = st.get(canon(src))
            elif re.match(r'-0x[0-9a-f]+\(%rbp\)$', src):
                st[dst] = slots.get(src)
            else:
                st.pop(dst, None)
            if st.get(dst) is None:
                st.pop(dst, None)
            continue
        m = re.match(r'%(\w+), (-0x[0-9a-f]+\(%rbp\))$', args)
        if op.startswith('mov') and m:
            v = st.get(canon(m.group(1)))
            if v:
                slots[m.group(2)] = v
            else:
                slots.pop(m.group(2), None)
            continue
        m = re.match(r'%(\w+), %(\w+)$', args)
        if op.startswith('xor') and m and m.group(1) == m.group(2):
            st[canon(m.group(1))] = ('imm', 0)
            continue
        if op.startswith('callq'):
            sel = st.get('rsi')
            if sel and sel[0] == 'sel' and sel[1].startswith(selprefix):
                edx, ecx = st.get('rdx'), st.get('rcx')
                cmds.append((i, a, edx[1] if edx and edx[0] == 'imm' else None, ecx[1] if ecx and ecx[0] == 'imm' else None))
            for r in ('rax', 'rcx', 'rdx', 'rsi', 'rdi', 'r8', 'r9', 'r10', 'r11'):
                st.pop(r, None)
    out = []
    for (i, a, edx, ecx) in cmds:
        if ecx is None and selprefix.startswith('getCommandBytes'):
            out.append({'cmd': None, 'len': edx, 'stores': []})
            continue
        # the pointer: rax after the call, copied to a register
        ptr = {'rax'}
        stores = []
        last_sel = None
        defs = {}
        immregs = {}
        for wa, wop, wargs, wnote in ins[i + 1:i + 120]:
            mi = re.match(r'\$(-?0x[0-9a-f]+|-?\d+), %(\w+)$', wargs)
            if mi and wop.startswith('mov'):
                immregs[canon(mi.group(2))] = int(mi.group(1), 0)
            else:
                mz = re.match(r'%(\w+), %(\w+)$', wargs)
                if mz and wop.startswith('xor') and mz.group(1) == mz.group(2):
                    immregs[canon(mz.group(1))] = 0
                else:
                    md = re.search(r', %(\w+)$', wargs)       # any other write to a register invalidates what we knew about it
                    if md and not wop.startswith(('cmp', 'test', 'push', 'j')):
                        immregs.pop(canon(md.group(1)), None)
                    if wop.startswith('callq'):
                        for rr in ('rax', 'rcx', 'rdx', 'rsi', 'rdi', 'r8', 'r9', 'r10', 'r11'):
                            immregs.pop(rr, None)
            if wop.startswith('callq'):
                # result of a message: remember the selector that preceded it
                if last_sel:
                    defs['rax'] = 'result of -' + last_sel
                if wnote and 'getCommandBytes' in wnote:
                    break
                continue
            if 'sel: ' in wnote:
                last_sel = wnote.split('sel: ')[1].split(' | ')[0]
            m = re.match(r'%(\w+), %(\w+)$', wargs)
            if m and wop.startswith('mov') and canon(m.group(1)) in ptr and m.group(2).startswith('r') and not m.group(2).startswith('rsp'):
                ptr.add(canon(m.group(2)))
                continue
            m = re.match(r'(%\w+|\$-?0x[0-9a-f]+|\$-?\d+|-?0x[0-9a-f]+\(%\w+\)), (-?0x[0-9a-f]+)?\(%(\w+)(?:,%\w+,\d)?\)$', wargs)
            if m and wop.startswith('mov') and canon(m.group(3)) in ptr:
                src, off = m.group(1), int(m.group(2) or '0', 0)
                if src.startswith('$'):
                    sd = 'imm ' + src[1:]
                elif src.startswith('%'):
                    sd = ('imm 0x%x' % immregs[canon(src)]) if canon(src) in immregs else defs.get(canon(src), 'reg ' + src)
                else:
                    sd = spill.get(re.sub(r'\(%rbp\)', '', src), src)
                stores.append((off, width_of(wop), sd))
                continue
            m = re.match(r'(-0x[0-9a-f]+\(%rbp\)|%\w+), %(\w+)$', wargs)
            if m and wop.startswith('mov') and m.group(2) in ('rax', 'eax', 'rcx', 'ecx', 'rdx', 'edx', 'rsi', 'esi', 'rdi', 'edi', 'r8', 'r8d', 'r9', 'r9d',
                                                          'r10', 'r10d', 'r11', 'r11d'):
                src = m.group(1)
                d = spill.get(re.sub(r'\(%rbp\)', '', src)) if src.startswith('-') else defs.get(canon(src))
                if d:
                    defs[canon(m.group(2))] = d
                else:
                    defs.pop(canon(m.group(2)), None)
        labels = [x for x in name.split(' ', 1)[1].rstrip(']').split(':')[:-1]] if ':' in name else []
        nice = []
        for off, w, sd in sorted(set(stores)):
            m = re.match(r'arg(\d+)$', sd)
            if m and 0 < int(m.group(1)) <= len(labels):
                sd = 'arg%s(%s)' % (m.group(1), labels[int(m.group(1)) - 1])
            nice.append((off, w, sd))
        out.append({'cmd': ecx, 'len': edx, 'stores': nice})
    return out


def collect():
    img = apvdis.Image(apvdis.APV)
    ix = apvdis.Index(img)
    rows = []
    for imp, nm, ty in ix.methods:
        if 'PGSerializer' not in nm and 'AppleParavirt' not in nm:
            continue
        for c in scan_method(ix, imp, nm, None):
            c['method'] = nm
            c['kind'] = 'stream'
            rows.append(c)
        for c in scan_method(ix, imp, nm, None, selprefix='allocateOperationBytes:'):
            c['method'] = nm
            c['kind'] = 'operation'
            c['alloc'] = c['len']
            st = {(o, w): sd for o, w, sd in c['stores']}
            hdr = st.get((0, 8))
            if hdr and hdr.startswith('imm '):
                v = int(hdr[4:], 16)
                c['cmd'], c['len'] = v & 0xffffffff, v >> 32          # size here includes the 8-byte header
                c['stores'] = [x for x in c['stores'] if x[0] >= 8]
            elif st.get((0, 4), '').startswith('imm ') and st.get((4, 4), '').startswith('imm '):
                c['cmd'], c['len'] = int(st[(0, 4)][4:], 16), int(st[(4, 4)][4:], 16)
                c['stores'] = [x for x in c['stores'] if x[0] >= 8]
            else:
                c['cmd'] = None
            rows.append(c)
    return rows


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ''
    arg = sys.argv[2] if len(sys.argv) > 2 else ''
    rows = collect()
    if cmd == 'json':
        print(json.dumps(rows, indent=1))
    elif cmd == 'table':
        byid = {}
        for r in rows:
            byid.setdefault(r['cmd'], []).append(r)
        for cid in sorted(byid, key=lambda c: (c is None, c)):
            for r in byid[cid]:
                print('%-6s len %-6s %s' % ('?' if cid is None else '0x%03x' % cid, '?' if r['len'] is None else r['len'], r['method']))
    elif cmd == 'layout':
        for r in rows:
            if arg in r['method'] and r['cmd'] is not None:
                print('0x%03x len %s  %s' % (r['cmd'], r['len'], r['method']))
                for off, w, src in r['stores']:
                    print('      +0x%02x  u%-2d  %s' % (off, w * 8, src))
    else:
        print(__doc__)


if __name__ == '__main__':
    main()
