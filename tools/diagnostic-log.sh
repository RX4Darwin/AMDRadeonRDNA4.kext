#!/bin/bash
#
# rdna4fb-diagnose.sh — collect every RDNA4FB diagnostic in one file.
# Run on the target hackintosh as: sudo bash rdna4fb-diagnose.sh
# Output: rdna4fb-diag-<timestamp>.txt in the current directory.
#
# The script detects which rdna4-* boot-args are active and labels each
# gated section accordingly (so an empty section reads "inactive — add the
# arg" vs "active but no output — check the build"). Append-only: never
# remove a capture; add a line whenever the kext gains a new diagnostic.
#
# Boot-args for the full survey round:
#   rdna4-smuping=1 rdna4-ihdump=1 rdna4-pspdump=1 rdna4-dmubping=1 \
#   rdna4-dmubhist=1 rdna4-dmubver=1 rdna4-modedump=1 rdna4-vbl=1
# (add rdna4-hwcursor=1 / rdna4-dmubcursor=1 for cursor experiments;
#  optionally remove the ATY,bin_image DeviceProperties entry to exercise
#  the on-die IP discovery path — check "Discovery,Source" below)
#

set -u
[ "$(id -u)" -eq 0 ] || { echo "run with sudo (dmesg needs root)"; exit 1; }

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="rdna4fb-diag-$(date +%Y%m%d-%H%M%S).txt"

# Active rdna4-* boot-args (space-padded for whole-token matching). The
# kernel's own copy first: `nvram boot-args` has come back empty in the
# recovery environment (2026-09-28) while the kernel had them.
ARGS="$(sysctl -n kern.bootargs 2>/dev/null)"
[ -n "$ARGS" ] || ARGS="$(nvram boot-args 2>/dev/null | cut -f2-)"
BOOTARGS=" $(echo "$ARGS" | tr -s '[:space:]' ' ') "
have_arg() { case "$BOOTARGS" in *" rdna4-$1=1 "*) return 0;; *) return 1;; esac; }

section() { echo; echo "=== $1 ==="; }

# Stages 2+ run on their own thread after the desktop: let the bring-up
# finish (or stop) before collecting, for up to 2 minutes.
case "$BOOTARGS" in *" rdna4-compute="[2-9]" "*)
	echo "waiting for the compute bring-up to finish (up to 2 minutes)..."
	for i in $(seq 1 120); do
		dmesg | grep -qE 'RDNA4FB: compute: (bring-up finished|.*failed; stopping|could not start)' && break
		sleep 1
	done
	dmesg | grep -E 'RDNA4FB: compute: (bring-up finished|.*failed; stopping)' | tail -1
	;;
esac

# gated <title> <grep-ERE> <boot-arg-name>
# Prints matching dmesg lines; if none, distinguishes "arg inactive" from
# "arg active but produced nothing" (a build/version red flag).
gated() {
	local title="$1" pat="$2" arg="$3" out
	section "$title"
	out="$(dmesg | grep -E "$pat")"
	if [ -n "$out" ]; then
		echo "$out"
	elif have_arg "$arg"; then
		echo "(rdna4-$arg=1 active but no output — check kext build/version)"
	else
		echo "(inactive — add rdna4-$arg=1 to boot-args)"
	fi
}

{
	section "system"
	sw_vers
	sysctl -n machdep.cpu.brand_string
	date

	section "active RDNA4FB boot-args"
	found=""
	for a in off cmap lutbypass 8bpc noedid nosleep modedump hwcursor \
	         curmode curtest dmubping dmubhist dmubver dmubcursor smuping \
	         ihdump pspdump vbl; do
		have_arg "$a" && found="$found rdna4-$a=1"
	done
	stage="$(echo "$BOOTARGS" | grep -oE ' rdna4-compute=[0-9]+ ' | tr -d ' ')"
	[ -n "$stage" ] && found="$found $stage"
	[ -n "$found" ] && echo "active:$found" || echo "(no rdna4-* boot-args set)"

	section "kext loaded?"
	kextstat | grep -i rdna4 || echo "RDNA4FB NOT LOADED"

	section "boot-args"
	echo "kernel: $(sysctl -n kern.bootargs 2>/dev/null || echo '(unreadable)')"
	nvram boot-args 2>/dev/null || echo "(nvram boot-args unreadable)"

	section "dmesg: full RDNA4FB log"
	dmesg | grep 'RDNA4FB:' || echo "(no RDNA4FB dmesg lines — buffer wrapped or kext absent)"

	section "dmesg: discovery / variant"
	dmesg | grep -E 'RDNA4FB: (probe|Navi 48|discovery):' || true

	section "dmesg: EDID / I2C / HPD"
	dmesg | grep -E 'RDNA4FB: (edid|i2c|cmd):' || true

	section "dmesg: lit pipe + boot timing"
	dmesg | grep -E 'RDNA4FB: (pipe|latch):' || true

	section "dmesg: display modes"
	dmesg | grep -E 'RDNA4FB: modes:' || true

	section "dmesg: display power (sleep/wake)"
	dmesg | grep -E 'RDNA4FB: power:' || true

	section "dmesg: HW cursor (incl. vm routing + curtest)"
	dmesg | grep -E 'RDNA4FB: cursor:' || true

	# DMUB: catches both 'dmub:' (ping) and 'dmub-hist:' (GOP command decode).
	gated "DMUB mailbox + GOP command history" 'RDNA4FB: dmub' dmubping

	gated "DMUB firmware fingerprint (dialect check vs amdgpu)" \
		'RDNA4FB: dmubver:' dmubver

	gated "SMU handshake" 'RDNA4FB: smu:' smuping
	gated "interrupt-delivery survey" 'RDNA4FB: ih:' ihdump
	gated "PSP status" 'RDNA4FB: psp:' pspdump

	section "dmesg: emulated VBL"
	dmesg | grep -E 'RDNA4FB: vbl:' || true

	gated "mode-setting survey" 'RDNA4FB: mode:' modedump

	# Any stage (rdna4-compute=1..7), so not a gated "=1" section.
	section "compute bring-up (rdna4-compute=<stage>)"
	dmesg | grep -E 'RDNA4FB: compute:' || \
		echo "(no compute lines — add rdna4-compute=1 to boot-args)"

	# Written before every risky step and flushed, so it survives a hang:
	# after a freeze, this names the step that never finished.
	section "compute NVRAM trail (last step reached)"
	nvram 4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:rdna4-trail 2>/dev/null || \
		echo "(no trail in NVRAM)"

	# The user-space runtime (published after stage 6/7): rdna4-run sits
	# next to this script on the stick. Copied out first: FAT keeps no
	# execute bit. The recovery shell is root, which opening it needs.
	section "user-space compute runtime (rdna4-run selftest)"
	if [ -f "$HERE/rdna4-run" ] && cp "$HERE/rdna4-run" /tmp/rdna4-run && chmod +x /tmp/rdna4-run; then
		/tmp/rdna4-run selftest || echo "(rdna4-run exited $?)"
		section "compute benchmarks (rdna4-run bench)"
		/tmp/rdna4-run bench || echo "(rdna4-run bench exited $?)"
	else
		echo "(no rdna4-run next to this script)"
	fi
	dmesg | grep -E 'RDNA4FB: runtime:' || true

	section "ioreg: framebuffer properties"
	ioreg -l -w0 | grep -E '"(Console|AtomBIOS|Discovery|VRAM|MMIO|EDID|GPU|SMU|PSP|Pipe|Modes|Compute),' || true

	section "ioreg: what the OS sees (display identity)"
	ioreg -lw0 | grep -E 'IODisplayEDID|DisplayProductID|DisplayVendorID' || true

	section "WindowServer attached?"
	ioreg -l -w0 | grep -c IOFramebufferUserClient | \
		xargs -I{} echo "{} IOFramebufferUserClient instance(s)"
} > "$OUT" 2>&1

echo "wrote $OUT"
echo
echo "Verdict lines to look for:"
echo "  pipe: 'lit pipe OTGn DIGn -> linkn ... signal HDMI' + boot timing / pixel clock"
echo "  modes: one line per mode; '(live)' marks the one on screen"
echo "  smu:  'PING OK — TestMessage acked' + PMFW version"
echo "  psp:  'verdict: bootloader READY, sOS ALIVE, ...'"
echo "  ih:   RB cntl/base all-zero = GOP left no interrupt ring (expected)"
echo "  dmub: 'PING OK — rptr advanced' + dmub-hist command decode"
echo "  dmubver: SCRATCH bank — compare vs Debian 'dmesg | grep -i dmub'"
echo "           version; a match => GOP DMUB speaks mainline VBIOS dialect"
echo "  Discovery,Source = 'on-die TMR' if ATY,bin_image was removed"
echo "  compute: 'verdict:' lines — PSP sOS, GFX/SDMA firmware state, GC/MM hub"
echo "           apertures, and the VRAM pool chosen for compute"
echo "  runtime: 'selftest: PASS' — a user-space program ran vadd on the GPU"
echo "  bench:   host<->GPU MB/s, VRAM GB/s vs CPU memcpy, sgemm GFLOPS vs the CPU"
echo "           (Accelerate) and 'GPU Nx'; 'bench: PASS' = every result exact"
