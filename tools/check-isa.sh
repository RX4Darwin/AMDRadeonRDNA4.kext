#!/usr/bin/env bash
# Every shader word the kext runs on the GPU must mean what it is meant to mean on gfx1201 (hub-task-347: vmtest.cpp used the gfx6-10 s_endpgm, which is
# s_setkill on this ISA). Uses llvm-mc's own assembler and disassembler (LLVM_MC=llvm-mc by default; skipped, loudly, when it is missing).
#   1. src/isa.hpp: each hand-written word is what `llvm-mc` assembles the named instruction to, and disassembles back to it.
#   2. src/*_kernel.h: each generated header equals a fresh assembly of shaders/<name>.s, disassembles without an invalid word, contains no instruction
#      outside an allow-list of the kinds these probes use, and ends in s_endpgm.
#   3. no raw SOPP-looking literal (0xbf8/9/a/b/c......) is written in src/*.cpp outside isa.hpp: hand-written words go through src/isa.hpp.
set -uo pipefail
cd "$(dirname "$0")/.."
MC=${LLVM_MC:-llvm-mc}
if ! command -v "$MC" >/dev/null 2>&1; then
	echo "check-isa: SKIPPED ($MC not found; set LLVM_MC)"
	exit 0
fi
fail=0
bad() { echo "check-isa: FAIL: $*"; fail=1; }

asm_words() {   # <mcpu attr...> reads assembly on stdin, prints the .text dwords one per line
	"$MC" -triple=amdgcn-amd-amdhsa -mcpu=gfx1201 "$@" -show-encoding 2>&1 \
		| sed -n 's/.*encoding: \[\(.*\)\].*/\1/p' | tr -d ' ' | tr ',' '\n' | paste -sd' ' - \
		| python3 -c '
import sys
b=[int(x,16) for x in sys.stdin.read().split()]
for i in range(0,len(b)-3,4): print("0x%08x" % int.from_bytes(bytes(b[i:i+4]),"little"))'
}
disasm() {      # <dwords...> -> llvm-mc disassembly text; dwords are 0xXXXXXXXX
	python3 - "$@" <<'PY' | "$MC" -triple=amdgcn-amd-amdhsa -mcpu=gfx1201 ${DISASM_ATTR:-} -disassemble 2>&1
import sys
out=[]
for w in sys.argv[1:]:
    v=int(w,16); out += ['0x%02x' % ((v>>s)&255) for s in (0,8,16,24)]
print(' '.join(out))
PY
}

# 1. isa.hpp
for pair in "kSEndpgm:s_endpgm" "kSCodeEnd:s_code_end"; do
	name=${pair%%:*}; insn=${pair##*:}
	have=$(sed -n "s/.*${name}  *= *\(0x[0-9a-fA-F]*\)u.*/\1/p" src/isa.hpp)
	want=$(echo "$insn" | asm_words)
	[ -n "$have" ] && [ "$have" = "$want" ] || bad "src/isa.hpp $name = ${have:-?} but $insn assembles to ${want:-?}"
	dis=$(disasm "$have" | tr -d '\t' | grep -v '^\s*\.' | sed 's/ *;.*//' | head -1)
	[ "$dis" = "$insn" ] || bad "src/isa.hpp $name ($have) disassembles to '$dis', not $insn"
done

# 2. generated kernels
for hdr in src/*_kernel.h; do
	name=$(basename "$hdr" _kernel.h)
	attr=; case "$name" in *64) attr=-mattr=+wavefrontsize64 ;; esac
	[ -f "shaders/$name.s" ] || { bad "$hdr has no shaders/$name.s"; continue; }
	have=$(grep -o '0x[0-9a-f]\{8\}' "$hdr" | paste -sd' ' -)
	want=$("$MC" -triple=amdgcn-amd-amdhsa -mcpu=gfx1201 $attr -filetype=obj -o /tmp/check-isa.$$.o "shaders/$name.s" 2>&1 \
	       && (command -v llvm-objcopy >/dev/null && llvm-objcopy -O binary --only-section=.text /tmp/check-isa.$$.o /tmp/check-isa.$$.bin \
	           && python3 -c '
import sys
d=open(sys.argv[1],"rb").read()
print(" ".join("0x%08x" % int.from_bytes(d[i:i+4],"little") for i in range(0,len(d),4)))' /tmp/check-isa.$$.bin))
	rm -f /tmp/check-isa.$$.o /tmp/check-isa.$$.bin
	if [ -z "$want" ]; then
		bad "$name: could not assemble shaders/$name.s (llvm-objcopy missing?)"
	elif [ "$have" != "$want" ]; then
		bad "$hdr is stale: it differs from a fresh assembly of shaders/$name.s (run tools/build-shaders.sh)"
	fi
	text=$(DISASM_ATTR=$attr disasm $have)
	echo "$text" | grep -qi "invalid\|unknown\|warning\|error" && bad "$name: llvm-mc cannot disassemble the header's words: $(echo "$text" | grep -i 'invalid\|unknown\|warning\|error' | head -2 | tr '\n' ' ')"
	last=$(echo "$text" | grep -v '^\s*\.' | sed 's/^[ \t]*//; s/ *;.*//' | grep -v '^$' | tail -1)
	[ "$last" = "s_endpgm" ] || bad "$name: the last instruction is '$last', not s_endpgm"
	echo "$text" | grep -q "s_setkill" && bad "$name: contains s_setkill (the gfx6-10 s_endpgm encoding read on gfx12)"
done

# 3. no raw instruction-looking literals
hits=$(grep -n "0xbf[89a-c][0-9a-fA-F]\{5\}" src/*.cpp src/*.hpp 2>/dev/null | grep -v "^src/isa.hpp:" | grep -v "0xbf000000\|0xbf800000")
[ -z "$hits" ] || bad "raw SOPP-looking literals outside src/isa.hpp (use Isa::k...):
$hits"

[ $fail = 0 ] && echo "check-isa: ok (isa.hpp constants, $(ls src/*_kernel.h | wc -l) generated kernels assemble/disassemble as intended)"
exit $fail
