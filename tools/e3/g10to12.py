#!/usr/bin/env python3
"""E3 of docs/metal-spike.md (hub-task-354): the smallest honest gfx1030 -> gfx1201 compute translator.

Input:  a gfx1030 code object (clang/ld.lld output, what Apple's RDNA2 libSC approximates for compute) and the name of one kernel.
Output: a gfx1201 code object whose kernel runs the same program, built from the DISASSEMBLED gfx1030 text, rewritten rule by rule and
        re-assembled with llvm-mc -mcpu=gfx1201. NOTHING is run: the output is for review (tools/e3/run-e3.sh prints the evidence).

"Honest" means the translator REFUSES (exit 2) anything it has no rule for, instead of guessing. The scope of this first version:

  * straight-line kernels (no branches, no barriers, no LDS/scratch/images/buffer descriptors, no exports);
  * only instructions from ALLOWED; each is either assembled unchanged for gfx1201 (checked: llvm-mc must accept the text) or rewritten by
    a rule in RENAMES (a mnemonic rename with identical semantics);
  * s_waitcnt with all counters 0 only (exact); lgkmcnt(0) -> s_wait_kmcnt 0 + s_wait_dscnt 0, vmcnt(0) -> s_wait_loadcnt 0,
    vscnt(0) -> s_wait_storecnt 0, expcnt(0) -> s_wait_expcnt 0;
  * the compute ABI shim (docs/metal-spike.md s.3.1): gfx10.3 hands a kernel the private-segment buffer in s[0:3], the kernarg pointer in
    s[4:5] (user SGPRs), the workgroup id X in s6 (system SGPR) and local id X in v0; gfx12 hands it kernarg in s[0:1], the workgroup id in
    ttmp9 and a packed {z,y,x} local id in v0. The shim copies them to where the original code expects them. The private-segment buffer is
    not provided: the translator refuses kernels that read s[0:3] before writing it;
  * hazards: gfx12 needs s_wait_alu between a VALU that writes VCC and the VALU that reads it in cases the hardware does not interlock
    (LLVM's GCNHazardRecognizer); the translator inserts s_wait_alu depctr_va_vcc(0) after EVERY VALU that writes VCC, a superset of what
    LLVM inserts (never wrong, slower), and s_wait_alu depctr_sa_sdst(0) after the prolog's SALU writes.

The kernel descriptor directives are copied from clang's own gfx1201 output for the same source (--native), so only the instruction body
comes from the translation.
"""
import argparse, os, re, subprocess, sys

LLVM_OBJDUMP = os.environ.get("LLVM_OBJDUMP", "llvm-objdump")
LLVM_MC = os.environ.get("LLVM_MC", "llvm-mc")
LD_LLD = os.environ.get("LD_LLD", "ld.lld")

ALLOWED = {
    "s_load_dword", "s_load_dwordx2", "s_load_dwordx4", "s_load_dwordx8",
    "v_mov_b32", "v_mov_b32_e32", "v_lshl_or_b32", "v_lshlrev_b64", "v_lshlrev_b32", "v_lshrrev_b32", "v_and_b32", "v_or_b32", "v_xor_b32",
    "v_add_co_u32", "v_add_co_ci_u32", "v_sub_co_u32", "v_sub_co_ci_u32", "v_add_nc_u32", "v_sub_nc_u32", "v_mul_lo_u32",
    "v_mad_u64_u32", "v_mad_i64_i32",
    "global_load_dword", "global_load_dwordx2", "global_load_dwordx4", "global_store_dword", "global_store_dwordx2", "global_store_dwordx4",
    "s_waitcnt", "s_endpgm", "s_nop", "s_mov_b32", "s_mov_b64", "s_add_u32", "s_addc_u32", "s_lshl_b32", "s_and_b32",
}
# Same semantics, new name (LLVM's gfx12 naming).
RENAMES = {
    "s_load_dword": "s_load_b32", "s_load_dwordx2": "s_load_b64", "s_load_dwordx4": "s_load_b128", "s_load_dwordx8": "s_load_b256",
    "global_load_dword": "global_load_b32", "global_load_dwordx2": "global_load_b64", "global_load_dwordx4": "global_load_b128",
    "global_store_dword": "global_store_b32", "global_store_dwordx2": "global_store_b64", "global_store_dwordx4": "global_store_b128",
    "v_mad_u64_u32": "v_mad_co_u64_u32", "v_mad_i64_i32": "v_mad_co_i64_i32",
}
# Known semantic or encoding differences that have no rule here: refuse, never translate by text.
REFUSE = re.compile(r"^(s_(c?branch|barrier|setpc|swappc|getpc|sendmsg|setreg|getreg|clause|inst_prefetch|waitcnt_(vs|exp|lgkm|vm)cnt|memtime|memrealtime)|"
                    r"v_(min|max)_f(16|32|64)|v_interp|v_dot|v_cmp|exp|buffer_|image_|scratch_|flat_|ds_|v_readlane|v_writelane|v_readfirstlane)")


class Refuse(Exception):
    pass


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw).stdout


def disassemble(hsaco, kernel):
    text = run([LLVM_OBJDUMP, "-d", "--mcpu=gfx1030", "--no-show-raw-insn", hsaco])
    out, on = [], False
    for line in text.splitlines():
        if re.match(r"^[0-9a-f]+ <%s>:" % re.escape(kernel), line):
            on = True
            continue
        if on and (not line.strip() or line.startswith("0")):
            break
        if on:
            out.append(re.sub(r"\s*//.*$", "", line).strip())
    if not out:
        raise Refuse("kernel %s not found in %s" % (kernel, hsaco))
    return out


def assembles(line):
    p = subprocess.run([LLVM_MC, "-triple=amdgcn-amd-amdhsa", "-mcpu=gfx1201", "-show-encoding"], input=line + "\n", text=True, capture_output=True)
    return p.returncode == 0


def sgprs_written(mn, ops):
    """The SGPR range written by an instruction that we know the shape of (first operand); None if not an SGPR destination."""
    if not ops:
        return None
    m = re.match(r"^s(\d+)$|^s\[(\d+):(\d+)\]$", ops[0])
    if not m:
        return None
    return (int(m.group(1)), int(m.group(1))) if m.group(1) else (int(m.group(2)), int(m.group(3)))


def split_ops(rest):
    return [o.strip() for o in rest.split(",")] if rest else []


def translate(lines, info):
    body, waits_needed = [], 0
    written = set()
    max_sgpr, max_vgpr = 0, 0
    for line in lines:
        m = re.match(r"^(\S+)\s*(.*)$", line)
        mn, ops = m.group(1), split_ops(m.group(2))
        base = re.sub(r"_e(32|64)$", "", mn)
        if REFUSE.match(base) or (base not in ALLOWED and mn not in ALLOWED):
            raise Refuse("no rule for '%s'" % line)
        # registers read before written: the private segment buffer s[0:3] is not provided on gfx12
        for o in ops[1:] if ops else []:
            for a, b in [(int(x), int(y)) for x, y in re.findall(r"s\[(\d+):(\d+)\]", o)] + [(int(x), int(x)) for x in re.findall(r"\bs(\d+)\b", o)]:
                if a <= 3 and not all(r in written for r in range(a, b + 1)):
                    raise Refuse("'%s' reads the private segment buffer s[0:3] (scratch): not provided on gfx12" % line)
        w = sgprs_written(mn, ops)
        if w:
            written.update(range(w[0], w[1] + 1))
        for x, y in re.findall(r"s\[(\d+):(\d+)\]", line):
            max_sgpr = max(max_sgpr, int(y))
        for x in re.findall(r"\bs(\d+)\b", line):
            max_sgpr = max(max_sgpr, int(x))
        for x, y in re.findall(r"v\[(\d+):(\d+)\]", line):
            max_vgpr = max(max_vgpr, int(y))
        for x in re.findall(r"\bv(\d+)\b", line):
            max_vgpr = max(max_vgpr, int(x))
        if mn == "s_waitcnt":
            counters = re.findall(r"(vmcnt|vscnt|lgkmcnt|expcnt)\((\d+)\)", m.group(2))
            if not counters or any(int(n) != 0 for _, n in counters):
                raise Refuse("s_waitcnt with a non-zero counter is not translatable exactly: '%s'" % line)
            for c, _ in counters:
                body += {"vmcnt": ["s_wait_loadcnt 0x0"], "vscnt": ["s_wait_storecnt 0x0"], "expcnt": ["s_wait_expcnt 0x0"],
                         "lgkmcnt": ["s_wait_kmcnt 0x0", "s_wait_dscnt 0x0"]}[c]
            continue
        new = RENAMES.get(base, None)
        text = line
        if new:
            text = new + ((" " + m.group(2)) if m.group(2) else "")
        text = re.sub(r",\s*null$", ", 0x0", text) if base.startswith("s_load") else text
        if not assembles(text):
            raise Refuse("llvm-mc -mcpu=gfx1201 does not accept '%s' (from '%s')" % (text, line))
        body.append(text)
        # a VALU that writes VCC (the second operand of the *_co_u32 forms that have a carry-out): s_wait_alu after it
        if re.match(r"^v_(add|sub|subrev)_co_u32", mn) and len(ops) > 1 and ops[1] in ("vcc_lo", "vcc"):
            body.append("s_wait_alu depctr_va_vcc(0)")
    return body, max_sgpr, max_vgpr


def shim(info):
    """gfx12 -> gfx10.3 launch state, for a kernel that was given: s[0:3] private segment buffer (user), s[4:5] kernarg ptr (user),
    s6 workgroup id X (system), v0 = local id X. info comes from the gfx1030 .amdhsa_kernel block."""
    out = []
    if info["user_sgpr_private_segment_buffer"]:
        kern = 4          # s[0:3] buffer, then the kernarg pointer
    else:
        kern = 0
    if info["user_sgpr_kernarg_segment_ptr"]:
        out.append("s_mov_b64 s[%d:%d], s[0:1]" % (kern, kern + 1))
    user = info["user_sgpr_count"]
    if info["system_sgpr_workgroup_id_x"]:
        out.append("s_mov_b32 s%d, ttmp9" % user)
        user += 1
    if info["system_sgpr_workgroup_id_y"] or info["system_sgpr_workgroup_id_z"] or info["system_vgpr_workitem_id"]:
        raise Refuse("only workgroup id X and local id X (1-D kernels) are translated")
    out.append("s_wait_alu depctr_sa_sdst(0)")
    out.append("v_and_b32 v0, 0x3ff, v0")          # gfx12 v0 is {z[29:20], y[19:10], x[9:0]}: keep x
    return out


def descriptor_info(asm_path):
    text = open(asm_path).read()
    blk = re.search(r"\.amdhsa_kernel \S+\n(.*?)\.end_amdhsa_kernel", text, re.S).group(1)
    return {k: int(v) for k, v in re.findall(r"\.amdhsa_(\w+) (\d+)", blk)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("hsaco", help="gfx1030 code object")
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--src1030", required=True, help="clang -S output for gfx1030 (only its .amdhsa_kernel block is read)")
    ap.add_argument("--native", required=True, help="clang -S output for gfx1201 of the same source (template: descriptor + metadata)")
    ap.add_argument("-o", required=True)
    ap.add_argument("--keep", help="write the generated assembly here")
    a = ap.parse_args()
    try:
        info = descriptor_info(a.src1030)
        lines = disassemble(a.hsaco, a.kernel)
        pro = shim(info)
        body, max_sgpr, max_vgpr = translate(lines, info)
        if not body or body[-1] != "s_endpgm":
            raise Refuse("the kernel does not end in s_endpgm")
        code = pro + body
        # the shim writes s4..s6: count them
        for l in pro:
            for x in re.findall(r"\bs(\d+)\b", l):
                max_sgpr = max(max_sgpr, int(x))
            for x, y in re.findall(r"s\[(\d+):(\d+)\]", l):
                max_sgpr = max(max_sgpr, int(y))
    except Refuse as e:
        print("g10to12: REFUSED: %s" % e, file=sys.stderr)
        return 2
    native = open(a.native).read()
    nl = native.split("\n")
    try:
        lab = next(i for i, l in enumerate(nl) if re.match(r"^%s:(\s|$)" % re.escape(a.kernel), l))
        end = next(i for i in range(lab, len(nl)) if nl[i].strip() == "s_endpgm")
    except StopIteration:
        print("g10to12: the native template has no '%s:' body ending in s_endpgm" % a.kernel, file=sys.stderr)
        return 3
    text = "\n".join(nl[:lab + 1] + ["\t; ---- translated from gfx1030 by tools/e3/g10to12.py (docs/metal-spike.md E3); the native body is replaced ----"] +
                     ["\t%s" % l for l in code] + nl[end + 1:])
    # the unused native helper function (clang's __clang_ocl_kern_imp_<kernel>) is left out
    text, n = re.subn(r"\t\.text\n\t\.protected\t__clang_ocl_kern_imp_%s.*?; MemoryBound: 0\n" % re.escape(a.kernel), "", text, count=1, flags=re.S)
    if n != 1:
        print("g10to12: could not drop the native helper function from the template", file=sys.stderr)
        return 3
    need = max_sgpr + 1
    nat = int(re.search(r"\.amdhsa_next_free_sgpr (\d+)", native).group(1))
    if need > nat:
        text = text.replace(".amdhsa_next_free_sgpr %d" % nat, ".amdhsa_next_free_sgpr %d" % need)
    nvg = int(re.search(r"\.amdhsa_next_free_vgpr (\d+)", native).group(1))
    if max_vgpr + 1 > nvg:
        text = text.replace(".amdhsa_next_free_vgpr %d" % nvg, ".amdhsa_next_free_vgpr %d" % (max_vgpr + 1))
    size = sum(8 if len(l.split()) > 4 else 4 for l in code)    # only a bound for the prefetch-size check below
    if size > 240:
        raise SystemExit("g10to12: kernel too big for the native .amdhsa_inst_pref_size (256 B)")
    if a.keep:
        open(a.keep, "w").write(text)
    obj = a.o + ".o"
    p = subprocess.run([LLVM_MC, "-triple=amdgcn-amd-amdhsa", "-mcpu=gfx1201", "-filetype=obj", "-o", obj], input=text, text=True, capture_output=True)
    if p.returncode:
        print("g10to12: llvm-mc failed:\n%s" % p.stderr, file=sys.stderr)
        return 3
    run([LD_LLD, "-shared", obj, "-o", a.o])
    print("g10to12: %d gfx1030 instructions -> %d gfx1201 instructions (+%d ABI shim), %s" % (len(lines), len(body), len(pro), a.o))
    return 0


if __name__ == "__main__":
    sys.exit(main())
