#!/bin/bash
#
# rdna4fb-diagnose.sh — collect the RDNA4FB log and run the bounded feature
# batch. Run on the target hackintosh as: sudo bash diagnostic-log.sh
# Output: rdna4fb-diag-<timestamp>.txt in the current directory.
#
# The script is also used by the VM dry run. Keep every user-space test
# behind run_step(): Recovery has no timeout(1), and a GPU fence must never
# leave this diagnostic shell waiting forever.

set -u
[ "$(id -u)" -eq 0 ] || { echo "run with sudo (dmesg needs root)"; exit 1; }

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="rdna4fb-diag-$(date +%Y%m%d-%H%M%S).txt"
SUMMARY="/tmp/rdna4fb-summary.$$"
STEP_PREFIX="/tmp/rdna4fb-step.$$"
KLOG="/tmp/rdna4fb-dmesg.$$"
STEP_TIMEOUT=120
STEP_SEQ=0
: > "$SUMMARY"
trap 'rm -f "$SUMMARY" "$KLOG" "$STEP_PREFIX"-*' EXIT

# The kernel copy is preferred because Recovery's nvram command can return an
# empty boot-args value while the running kernel still has its arguments.
ARGS="$(sysctl -n kern.bootargs 2>/dev/null)"
[ -n "$ARGS" ] || ARGS="$(nvram boot-args 2>/dev/null | cut -f2-)"
BOOTARGS=" $(echo "$ARGS" | tr -s '[:space:]' ' ') "

arg_value() {
	echo "$BOOTARGS" | tr ' ' '\n' | sed -n "s/^rdna4-$1=//p" | tail -1
}

have_arg() { [ "$(arg_value "$1")" = 1 ]; }

COMPUTE_STAGE="$(arg_value compute)"
IH_MODE="$(arg_value ih)"
VM_MODE="$(arg_value vm)"
FLIP_MODE="$(arg_value flip)"
GFX_MODE="$(arg_value gfx)"
HANG_MODE="$(arg_value hang)"
SLEEPTEST_MODE="$(arg_value sleeptest)"
case "$COMPUTE_STAGE" in ''|*[!0-9]*) COMPUTE_STAGE=0;; esac
case "$IH_MODE" in ''|*[!0-9]*) IH_MODE=0;; esac
case "$VM_MODE" in ''|*[!0-9]*) VM_MODE=0;; esac
case "$FLIP_MODE" in ''|*[!0-9]*) FLIP_MODE=0;; esac
case "$GFX_MODE" in ''|*[!0-9]*) GFX_MODE=0;; esac
case "$HANG_MODE" in ''|*[!0-9]*) HANG_MODE=1;; esac
case "$SLEEPTEST_MODE" in ''|*[!0-9]*) SLEEPTEST_MODE=0;; esac

section() { echo; echo "=== $1 ==="; }

# Run one command with a hard wall-clock bound. Perl's alarm is available in
# the Recovery image; the background watchdog is the fallback for a minimal
# image without Perl. STEP_FILE remains available for classification.
run_step() {
	local label="$1"
	shift
	STEP_SEQ=$((STEP_SEQ + 1))
	STEP_FILE="${STEP_PREFIX}-${STEP_SEQ}.txt"
	section "$label"
	if command -v perl >/dev/null 2>&1; then
		perl -e 'alarm shift; exec @ARGV' "$STEP_TIMEOUT" "$@" > "$STEP_FILE" 2>&1
		STEP_RC=$?
	else
		"$@" > "$STEP_FILE" 2>&1 &
		step_pid=$!
		(
			sleep "$STEP_TIMEOUT"
			kill -TERM "$step_pid" 2>/dev/null || exit 0
			sleep 2
			kill -KILL "$step_pid" 2>/dev/null || true
		) &
		watchdog_pid=$!
		wait "$step_pid"
		STEP_RC=$?
		kill "$watchdog_pid" 2>/dev/null || true
		wait "$watchdog_pid" 2>/dev/null || true
	fi
	if [ "$STEP_RC" -eq 142 ] || [ "$STEP_RC" -eq 143 ]; then
		echo "(timed out after ${STEP_TIMEOUT}s; continuing)" >> "$STEP_FILE"
	fi
	cat "$STEP_FILE"
	return 0
}

record() {
	printf "%-12s %-8s %s\n" "$1" "$2" "$3" >> "$SUMMARY"
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
	         ihdump pspdump vbl trace compute ih vm flip gfx hang sleeptest; do
		value="$(arg_value "$a")"
		[ -n "$value" ] && found="$found rdna4-$a=$value"
	done
	[ -n "$found" ] && echo "active:$found" || echo "(no rdna4-* boot-args set)"

	section "kext loaded?"
	kextstat | grep -i rdna4 || echo "RDNA4FB NOT LOADED"

	section "boot-args"
	echo "kernel: $(sysctl -n kern.bootargs 2>/dev/null || echo '(unreadable)')"
	nvram boot-args 2>/dev/null || echo "(nvram boot-args unreadable)"

	# Stages 2+ run on their own thread after the desktop: let the bring-up
	# finish (or stop) before collecting, for up to two minutes.
	if [ "$COMPUTE_STAGE" -ge 2 ]; then
		echo "waiting for the compute bring-up to finish (up to 2 minutes)..."
		for i in $(seq 1 120); do
			dmesg | grep -qE 'RDNA4FB: compute: (bring-up finished|.*failed; stopping|could not start)' && break
			sleep 1
		done
		dmesg | grep -E 'RDNA4FB: compute: (bring-up finished|.*failed; stopping)' | tail -1
	fi

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

	# DMUB: catches both dmub: (ping) and dmub-hist: (GOP command decode).
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
	gated "DMUB mailbox + GOP command history" 'RDNA4FB: dmub' dmubping
	gated "DMUB firmware fingerprint (dialect check vs amdgpu)" \
		'RDNA4FB: dmubver:' dmubver
	gated "SMU handshake" 'RDNA4FB: smu:' smuping
	gated "interrupt-delivery survey" 'RDNA4FB: ih:' ihdump
	gated "PSP status" 'RDNA4FB: psp:' pspdump

	section "dmesg: emulated VBL"
	dmesg | grep -E 'RDNA4FB: vbl:' || true
	gated "mode-setting survey" 'RDNA4FB: mode:' modedump

	section "compute bring-up (rdna4-compute=<stage>)"
	dmesg | grep -E 'RDNA4FB: compute:' || \
		echo "(no compute lines — add rdna4-compute=1 to boot-args)"

	section "compute NVRAM trail (last step reached)"
	TRAIL_VALUE="$(nvram 4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:rdna4-trail 2>/dev/null || true)"
	[ -n "$TRAIL_VALUE" ] && echo "$TRAIL_VALUE" || echo "(no trail in NVRAM)"

	RUNTIME_ACTIVE=0
	RUNTIME_READY=0
	INFO_OK=0
	SELFTEST_RC=125
	SELFTEST_FILE=""
	BENCH_RC=125
	BENCH_FILE=""
	SENSORS_RC=125
	SENSORS_FILE=""
	VSYNC_RC=125
	VSYNC_FILE=""
	SHOW_RC=125
	SHOW_FILE=""
	ANIM_RC=125
	ANIM_FILE=""
	SLEEP_RC=125
	SLEEP_FILE=""
	HANG_RC=125
	HANG_FILE=""
	HANGTEST_RC=125
	HANGTEST_FILE=""
	if [ "$COMPUTE_STAGE" -ge 6 ]; then
		RUNTIME_ACTIVE=1
	fi
	if [ "$RUNTIME_ACTIVE" -eq 1 ] && [ -f "$HERE/rdna4-run" ]; then
		RUN=/tmp/rdna4-run
		if [ "$HERE/rdna4-run" != "$RUN" ]; then
			cp "$HERE/rdna4-run" "$RUN"
		fi
		chmod +x "$RUN"
		run_step "runtime info (ABI and flags)" "$RUN" info
		INFO_FILE="$STEP_FILE"
		INFO_RC=$STEP_RC
		if [ "$INFO_RC" -eq 0 ] && grep -q '^ABI ' "$INFO_FILE"; then
			INFO_OK=1
		fi
	else
		RUN=""
	fi

	if [ "$INFO_OK" -eq 1 ]; then
		run_step "user-space compute runtime (rdna4-run selftest)" "$RUN" selftest
		SELFTEST_FILE="$STEP_FILE"
		SELFTEST_RC=$STEP_RC
		if [ "$VM_MODE" -eq 1 ]; then
			echo "note: VM mode — using bounded rdna4-run bench small"
			run_step "compute benchmarks (rdna4-run bench small)" "$RUN" bench small
		else
			run_step "compute benchmarks (rdna4-run bench)" "$RUN" bench
		fi
		BENCH_FILE="$STEP_FILE"
		BENCH_RC=$STEP_RC
		run_step "GPU sensors (rdna4-run sensors)" "$RUN" sensors
		SENSORS_FILE="$STEP_FILE"
		SENSORS_RC=$STEP_RC
	else
		section "user-space compute runtime"
		if [ "$RUNTIME_ACTIVE" -eq 1 ]; then
			echo "(runtime unavailable — no usable rdna4-run or info failed)"
		else
			echo "(inactive — rdna4-compute is below stage 6)"
		fi
	fi

	# W1b: DCN vblank waits require IH mode 2. The timeout is inside the
	# user client as well as this outer bound.
	if [ "$INFO_OK" -eq 1 ] && [ "$IH_MODE" -ge 2 ]; then
		run_step "display interrupts (rdna4-run vsync 120)" "$RUN" vsync 120
		VSYNC_FILE="$STEP_FILE"
		VSYNC_RC=$STEP_RC
	else
		section "display interrupts (rdna4-run vsync 120)"
		echo "(inactive — requires rdna4-ih=2 and a ready runtime)"
	fi

	# W5b: show exercises present/flip and restores the desktop. W5c is
	# optional so an older rdna4-run remains a valid diagnostic image.
	if [ "$INFO_OK" -eq 1 ] && [ "$FLIP_MODE" -gt 0 ]; then
		run_step "display ownership (rdna4-run show 3)" "$RUN" show 3
		SHOW_FILE="$STEP_FILE"
		SHOW_RC=$STEP_RC
		run_step "rdna4-run command availability (anim)" "$RUN"
		ANIM_LIST_FILE="$STEP_FILE"
		if grep -q 'anim' "$ANIM_LIST_FILE"; then
			run_step "animation (rdna4-run anim 5)" "$RUN" anim 5
			ANIM_FILE="$STEP_FILE"
			ANIM_RC=$STEP_RC
		else
			section "animation (rdna4-run anim 5)"
			echo "(skipped — this rdna4-run has no anim command)"
		fi
	else
		section "display ownership (rdna4-run show 3)"
		echo "(inactive — requires rdna4-flip and a ready runtime)"
		section "animation (rdna4-run anim 5)"
		echo "(inactive — requires rdna4-flip and a ready runtime)"
	fi

	# W6 is part of the compute runtime. rdna4-hang=0 deliberately disables
	# recovery; otherwise exercise both public entry points.
	if [ "$INFO_OK" -eq 1 ] && [ "$COMPUTE_STAGE" -ge 6 ] && [ "$HANG_MODE" -ne 0 ]; then
		run_step "queue recovery (rdna4-run selftest hang)" "$RUN" selftest hang
		HANG_FILE="$STEP_FILE"
		HANG_RC=$STEP_RC
		run_step "queue recovery (rdna4-run hangtest)" "$RUN" hangtest
		HANGTEST_FILE="$STEP_FILE"
		HANGTEST_RC=$STEP_RC
	else
		section "queue recovery (W6)"
		echo "(inactive — requires compute stage 6 and rdna4-hang != 0)"
	fi

	if [ "$INFO_OK" -eq 1 ] && [ "$SLEEPTEST_MODE" -eq 1 ]; then
		run_step "debug sleep cycle (rdna4-run sleeptest)" "$RUN" sleeptest
		SLEEP_FILE="$STEP_FILE"
		SLEEP_RC=$STEP_RC
	else
		section "debug sleep cycle (rdna4-run sleeptest)"
		echo "(inactive — add rdna4-sleeptest=1 for the emulator/debug cycle)"
	fi

	dmesg > "$KLOG" 2>&1
	section "dmesg: feature lines (IH, VM, GFX, flip, trails and hangs)"
	grep -E 'RDNA4FB: (.*ih:|.*vm:|.*vmid|.*gfx:|.*flip:|.*trail|.*hang|.*PreviousHang)' "$KLOG" || \
		echo "(no feature-specific lines)"

	# Feature summary. PASS means the bounded command and its result marker
	# succeeded. SKIPPED means its boot-arg or command is absent.
	if [ "$RUNTIME_ACTIVE" -eq 0 ]; then
		record runtime SKIPPED "compute stage < 6"
	elif [ "$INFO_OK" -eq 1 ] && [ "$SELFTEST_RC" -eq 0 ] && \
		[ "$BENCH_RC" -eq 0 ] && grep -q 'selftest: PASS' "$SELFTEST_FILE" && \
		grep -q 'bench: PASS' "$BENCH_FILE"; then
		record runtime PASS "selftest + bench PASS"
	else
		record runtime FAIL "info/selftest/bench did not all pass"
	fi

	# W8: selftest's SubmitIb section proves a single IB, ordered fences and
	# ten back-to-back IBs. Keep it separate from the general runtime result so
	# the real-card report shows which queue path was actually exercised.
	if [ "$INFO_OK" -eq 0 ]; then
		record submitib SKIPPED "runtime unavailable"
	elif [ "$SELFTEST_RC" -eq 0 ] && \
		grep -q '  ok  SubmitIb vadd:' "$SELFTEST_FILE" && \
		grep -q 'ok    three IBs back-to-back' "$SELFTEST_FILE" && \
		grep -q 'ok    ten back-to-back IBs completed in order' "$SELFTEST_FILE"; then
		record submitib PASS "single + ordered + ten IBs PASS"
	else
		record submitib FAIL "SubmitIb selftest did not prove ordered completion"
	fi

	# W2 fault-page scrub: the selftest dispatches through a freed host VA and
	# requires a clean fence. The kernel clears the shared fault-default page
	# after servicing that fault, so retain the user-visible proof in the table.
	if [ "$INFO_OK" -eq 0 ]; then
		record fault SKIPPED "runtime unavailable"
	elif [ "$SELFTEST_RC" -eq 0 ] && \
		grep -q 'ok    dispatch through freed host VA faulted cleanly' "$SELFTEST_FILE"; then
		record fault PASS "freed-VA fault recovered; page scrubbed"
	else
		record fault FAIL "freed-VA fault did not recover cleanly"
	fi

	if [ "$VM_MODE" -eq 0 ]; then
		record vm SKIPPED "rdna4-vm not enabled"
	elif [ "$SELFTEST_RC" -eq 0 ] && \
		grep -q 'VM isolation: client B could not read client A' "$SELFTEST_FILE" && \
		grep -q 'two VM clients dispatched concurrently' "$SELFTEST_FILE"; then
		record vm PASS "isolation + concurrent queues"
	else
		record vm FAIL "GPUVM selftest did not pass"
	fi

	if [ "$IH_MODE" -eq 0 ]; then
		record ih SKIPPED "rdna4-ih not enabled"
	elif ! grep -q 'RDNA4FB: compute:' "$KLOG"; then
		record ih SKIPPED "kernel compute log unavailable"
	elif grep -q 'RDNA4FB: .*ih: ring up:' "$KLOG"; then
		ih_key="ring up"
		grep -q 'RDNA4FB: .*ih: self-test:.*polling' "$KLOG" && ih_key="ring up; polling fallback"
		record ih PASS "$ih_key"
	else
		record ih FAIL "IH did not reach ring up"
	fi

	if [ "$IH_MODE" -lt 2 ]; then
		record vblank SKIPPED "requires rdna4-ih=2"
	elif [ "$VSYNC_RC" -eq 0 ] && grep -q 'vsync: 120 vblanks' "$VSYNC_FILE"; then
		vblank_key="$(grep 'vsync: 120 vblanks' "$VSYNC_FILE" | tail -1)"
		record vblank PASS "$vblank_key"
	else
		record vblank FAIL "vsync did not complete 120 waits"
	fi

	if [ "$GFX_MODE" -eq 0 ]; then
		record gfx SKIPPED "rdna4-gfx not enabled"
	elif grep -Eq 'RDNA4FB: .*gfx: .*failure|RDNA4FB: .*stage gfx ring.*failed|RDNA4FB: .*gfx ring.*off' "$KLOG"; then
		record gfx FAIL "ring/draw path reported a failure"
	elif grep -Eq 'RDNA4FB: .*gfx: .*THE TRIANGLE IS RIGHT.*8192 pixels' "$KLOG"; then
		gfx_key="THE TRIANGLE IS RIGHT; 8192 pixels"
		record gfx PASS "$gfx_key"
	elif printf '%s\n' "$TRAIL_VALUE" | grep -q 'gfx draw right'; then
		record gfx PASS "G3 trail: gfx draw right (dmesg wrapped)"
	elif grep -Eq 'RDNA4FB: .*gfx: .*ring|RDNA4FB: .*gfx: .*draw' "$KLOG"; then
		record gfx FAIL "ring/draw present but triangle proof missing"
	else
		record gfx SKIPPED "feature skipped before ring test"
	fi

	if [ "$FLIP_MODE" -eq 0 ]; then
		record flip SKIPPED "rdna4-flip not enabled"
	elif [ "$SHOW_RC" -eq 0 ] && grep -q 'show: desktop restored' "$SHOW_FILE"; then
		flip_key="show restored desktop"
		grep -q 'RDNA4FB: .*flip:' "$KLOG" && flip_key="show restored desktop; kernel flip lines"
		record flip PASS "$flip_key"
	else
		record flip FAIL "show did not restore the desktop"
	fi

	if [ "$FLIP_MODE" -eq 0 ] || [ "$INFO_OK" -eq 0 ]; then
		record anim SKIPPED "flip/runtime not enabled"
	elif [ -z "$ANIM_FILE" ]; then
		record anim SKIPPED "command unavailable"
	elif [ "$ANIM_RC" -eq 0 ]; then
		anim_key="frames rendered"
		grep -q 'anim: desktop restored' "$ANIM_FILE" && anim_key="$(grep 'anim: frames rendered' "$ANIM_FILE" | tail -1); desktop restored"
		record anim PASS "$anim_key"
	else
		record anim FAIL "animation command failed"
	fi

	if [ "$COMPUTE_STAGE" -lt 6 ] || [ "$HANG_MODE" -eq 0 ]; then
		record w6 SKIPPED "queue recovery disabled or runtime absent"
	elif [ "$HANG_RC" -eq 0 ] && [ "$HANGTEST_RC" -eq 0 ] && \
		grep -q 'hangtest: PASS' "$HANG_FILE" && grep -q 'hangtest: PASS' "$HANGTEST_FILE"; then
		record w6 PASS "selftest hang + hangtest PASS"
	else
		record w6 FAIL "queue recovery test failed or timed out"
	fi

	if [ "$INFO_OK" -eq 0 ]; then
		record sensors SKIPPED "runtime unavailable"
	elif [ "$SENSORS_RC" -eq 0 ] && grep -q '^sensors: edge ' "$SENSORS_FILE"; then
		sensor_key="$(grep '^sensors: edge ' "$SENSORS_FILE" | tail -1)"
		record sensors PASS "$sensor_key"
	else
		record sensors FAIL "metrics query did not complete"
	fi

	if [ "$SLEEPTEST_MODE" -ne 1 ]; then
		record sleep SKIPPED "rdna4-sleeptest not enabled"
	elif [ "$SLEEP_RC" -eq 0 ] && grep -q 'pre-sleep client aborted as expected' "$SLEEP_FILE"; then
		record sleep PASS "quiesce/reset/rebring-up/Aborted"
	else
		record sleep FAIL "debug sleep cycle failed or timed out"
	fi

	section "ioreg: framebuffer properties"
	ioreg -l -w0 | grep -E '"(Console|AtomBIOS|Discovery|VRAM|MMIO|EDID|GPU|SMU|PSP|Pipe|Modes|Compute),' || true

	section "ioreg: what the OS sees (display identity)"
	ioreg -lw0 | grep -E 'IODisplayEDID|DisplayProductID|DisplayVendorID' || true

	section "WindowServer attached?"
	ioreg -l -w0 | grep -c IOFramebufferUserClient | \
		xargs -I{} echo "{} IOFramebufferUserClient instance(s)"

	section "feature summary"
	printf "%-12s %-8s %s\n" FEATURE STATUS KEY
	cat "$SUMMARY"
} > "$OUT" 2>&1

echo "wrote $OUT"
echo
echo "Feature summary:"
cat "$SUMMARY"
echo
echo "Key log sections are in $OUT: dmesg feature lines, the NVRAM trail, and every bounded test output."
