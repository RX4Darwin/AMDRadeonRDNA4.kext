#!/bin/bash
#
# rdna4fb-diagnose.sh — collect the RDNA4FB log and run the bounded feature
# batch. Run on the target hackintosh as: sudo bash diagnostic-log.sh
# Output: rdna4fb-diag-<timestamp>.txt next to this script.
#
# The script is also used by the VM dry run. Keep every user-space test
# behind run_step(): Recovery has no timeout(1), and a GPU fence must never
# leave this diagnostic shell waiting forever.

set -u
[ "$(id -u)" -eq 0 ] || { echo "run with sudo (dmesg needs root)"; exit 1; }

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/rdna4fb-diag-$(date +%Y%m%d-%H%M%S).txt"
SUMMARY="/tmp/rdna4fb-summary.$$"
STEP_PREFIX="/tmp/rdna4fb-step.$$"
KLOG="/tmp/rdna4fb-dmesg.$$"
REG_RESULTS=""
STEP_TIMEOUT=120
STEP_SEQ=0
: > "$SUMMARY"
trap 'rm -f "$SUMMARY" "$KLOG" "$STEP_PREFIX"-* "/tmp/rdna4fb-klogfb.$$" "/tmp/rdna4fb-klogfb.$$.raw" "/tmp/rdna4fb-klogfb.$$.err"' EXIT

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
GFXCOL_MODE="$(arg_value gfxcol)"
GFXCLIENT_MODE="$(arg_value gfxclient)"
HANG_MODE=0
SLEEPTEST_MODE="$(arg_value sleeptest)"
GFXPM_MODE="$(arg_value gfxpm)"
GFXCG_MODE="$(arg_value gfxcg)"
GFXOFF_MODE="$(arg_value gfxoff)"
VMIDTEST_MODE="$(arg_value vmid-test)"     # W13 S1 / boot 9: mask, 1 probes, 2 surveys at the flow points, 4 client-op trace
VMDIAG_MODE="$(arg_value vm-diag)"
case "$COMPUTE_STAGE" in ''|*[!0-9]*) COMPUTE_STAGE=0;; esac
case "$IH_MODE" in ''|*[!0-9]*) IH_MODE=0;; esac
case "$VM_MODE" in ''|*[!0-9]*) VM_MODE=0;; esac
case "$FLIP_MODE" in ''|*[!0-9]*) FLIP_MODE=0;; esac
case "$GFX_MODE" in ''|*[!0-9]*) GFX_MODE=0;; esac
case "$GFXCOL_MODE" in ''|*[!0-9]*) GFXCOL_MODE=0;; esac
case "$GFXCLIENT_MODE" in ''|*[!0-9]*) GFXCLIENT_MODE=0;; esac
case "$SLEEPTEST_MODE" in ''|*[!0-9]*) SLEEPTEST_MODE=0;; esac
case "$GFXPM_MODE" in ''|*[!0-9]*) GFXPM_MODE=0;; esac
case "$VMIDTEST_MODE" in ''|*[!0-9]*) VMIDTEST_MODE=0;; esac

# Queue recovery is an explicit last step. It is never part of an ordinary
# collection, even when rdna4-hang=1 is present in the boot arguments.
[ "${1:-}" = hang ] && HANG_MODE=1

# The emulator dry run can deliberately omit rdna4-vm (boot 1) while still
# needing bounded VM-sized diagnostics. Keep VM_MODE tied to the boot arg so
# the VM feature row remains SKIPPED, and detect the emulated card separately.
EMULATED_CARD=0
if [ "$(sysctl -n kern.hv_vmm_present 2>/dev/null || echo 0)" = 1 ]; then
	EMULATED_CARD=1
elif [ "$VM_MODE" -eq 0 ]; then
	if ioreg -r -w0 -l 2>/dev/null |
		grep -q '"GPU,Variant"[[:space:]]*=[[:space:]]*"VM test'; then
		EMULATED_CARD=1
	fi
fi

section() { echo; echo "=== $1 ==="; }

# The kernel message buffer wraps on a long boot (round 2, boot 6: not one
# RDNA4FB line survived, so the HW cursor and compute sections came out empty).
# klines PATTERN prints the matching dmesg lines; when dmesg has none it falls
# back to the same lines from the unified log (log show, read once per run).
KFB="/tmp/rdna4fb-klogfb.$$"
# One bounded `log show` attempt: run it in the background and stop it after
# $1 seconds (macOS has no timeout(1)); the rest of the arguments are log show's.
# Output goes to $KFB.raw, errors to $KFB.err. Returns 0 when RDNA4FB lines came out.
klog_try() {
	local secs="$1" lpid t=0
	shift
	: > "$KFB.raw"
	log show "$@" --style compact --info --debug \
		--predicate 'eventMessage CONTAINS "RDNA4FB:"' > "$KFB.raw" 2>> "$KFB.err" &
	lpid=$!
	while kill -0 "$lpid" 2>/dev/null && [ "$t" -lt "$secs" ]; do
		sleep 1
		t=$((t + 1))
	done
	if kill -0 "$lpid" 2>/dev/null; then
		kill "$lpid" 2>/dev/null || true
		sleep 1
		kill -9 "$lpid" 2>/dev/null || true
		echo "log show $* stopped after ${t} s" >> "$KFB.err"
	fi
	grep -q 'RDNA4FB:' "$KFB.raw" 2>/dev/null
}
klog_fallback() {
	if [ ! -e "$KFB" ]; then
		# Round 3: the single `log show --last boot` printed nothing and its error
		# was thrown away. Try the boot, then the last hour, then everything the
		# store still holds (20 s each), and keep log show's own messages so an
		# empty result says why.
		: > "$KFB.err"
		if klog_try 20 --last boot || klog_try 20 --last 1h || klog_try 20; then
			grep 'RDNA4FB:' "$KFB.raw" > "$KFB"
		else
			: > "$KFB"
			echo "# log show fallback found no RDNA4FB lines (no unified-log store in this environment?):" >> "$KFB"
			head -5 "$KFB.err" | sed 's/^/# /' >> "$KFB"
		fi
		rm -f "$KFB.raw" "$KFB.err"
	fi
	cat "$KFB"
}
klines() {
	local out
	out="$(dmesg | grep -E "$1")"
	if [ -n "$out" ]; then
		echo "$out"
	else
		klog_fallback | grep -E "$1" || true
	fi
}

snapshot_logs() {
	dmesg > "$KLOG" 2>&1
	REG_RESULTS="$(ioreg -r -w0 -l -k 'RDNA4FB,Results' 2>/dev/null || true)"
}

# Run one command with a hard wall-clock bound. STEP_FILE remains available
# for classification; the pure-shell watchdog works in the Recovery image.
run_step() {
	local label="$1"
	shift
	STEP_SEQ=$((STEP_SEQ + 1))
	STEP_FILE="${STEP_PREFIX}-${STEP_SEQ}.txt"
	TIMEOUT_FILE="${STEP_PREFIX}-${STEP_SEQ}.timeout"
	rm -f "$TIMEOUT_FILE"
	section "$label"
	# Flush Recovery's journal before every operation which may touch the GPU.
	sync >/dev/null 2>&1 || true
	# Recovery does not guarantee Perl. Keep the watchdog in the shell and
	# use 137 as the unambiguous timeout result in the report.
	"$@" > "$STEP_FILE" 2>&1 &
	step_pid=$!
	(
		timer_pid=""
		grace_pid=""
		watchdog_cleanup() {
			[ -z "$timer_pid" ] || kill "$timer_pid" 2>/dev/null || true
			[ -z "$grace_pid" ] || kill "$grace_pid" 2>/dev/null || true
		}
		watchdog_stop() {
			watchdog_cleanup
			exit 0
		}
		trap watchdog_cleanup EXIT
		trap watchdog_stop TERM INT
		sleep "$STEP_TIMEOUT" >/dev/null 2>&1 &
		timer_pid=$!
		wait "$timer_pid" || exit 0
		timer_pid=""
		if kill -0 "$step_pid" 2>/dev/null; then
			echo timeout > "$TIMEOUT_FILE"
			kill -TERM "$step_pid" 2>/dev/null || true
			sleep 2 >/dev/null 2>&1 &
			grace_pid=$!
			wait "$grace_pid" || true
			grace_pid=""
			kill -KILL "$step_pid" 2>/dev/null || true
		fi
	) &
	watchdog_pid=$!
	wait "$step_pid"
	STEP_RC=$?
	kill "$watchdog_pid" 2>/dev/null || true
	wait "$watchdog_pid" 2>/dev/null || true
	if [ -f "$TIMEOUT_FILE" ] || [ "$STEP_RC" -eq 137 ]; then
		STEP_RC=137
		echo "(TIMEOUT after ${STEP_TIMEOUT}s; continuing)" >> "$STEP_FILE"
	fi
	cat "$STEP_FILE"
	return 0
}

record() {
	# Each feature has one summary row even if a future diagnostic branch
	# reaches the classifier more than once.
	grep -q "^$1[[:space:]]" "$SUMMARY" 2>/dev/null && return
	printf "%-12s %-8s %s\n" "$1" "$2" "$3" >> "$SUMMARY"
}

registry_value() {
	printf '%s\n' "$REG_RESULTS" |
		sed -n -E "s/.*\"$1\"[[:space:]]*=[[:space:]]*\"([^\"]*)\".*/\1/p" |
		tail -1
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
	         ihdump pspdump vbl cursor pm trace compute ih vm vm-diag vm-exec vm-ispte vm-force-fail vmid-test flip gfx gfxcol gfxclient \
	         gfxpm gfxcg gfxoff gfxcap hang sleeptest; do
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
	dmesg | grep 'RDNA4FB:' || { echo "(no RDNA4FB dmesg lines — buffer wrapped or kext absent; unified log follows)"; klog_fallback; }

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

	section "dmesg: GFX power-management experiment (rdna4-gfxpm)"
	dmesg | grep -E 'RDNA4FB: compute: (pm|cg|gfxoff):' || echo "(rdna4-gfxpm/gfxcg/gfxoff not enabled or no such lines)"

	section "dmesg: HW cursor (incl. vm routing + curtest)"
	klines 'RDNA4FB: cursor:'
	echo '--- registry copy (survives the kernel log wrapping): RDNA4FB,Cursor'
	# ioreg prints the string with literal \n between lines; show one per line.
	CUR_PROP="$(ioreg -l -w0 2>/dev/null | grep 'RDNA4FB,Cursor' || true)"
	if [ -n "$CUR_PROP" ]; then
		# One ioreg line; the kext joins its cursor lines with " ## ".
		echo "$CUR_PROP" | sed 's/ ## /\
/g'
	else
		echo "(no RDNA4FB,Cursor property: the kext never reached cursor init; with rdna4-cursor off it holds only the "unavailable" line)"
	fi

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
	klines 'RDNA4FB: compute:' | grep . || \
		echo "(no compute lines — add rdna4-compute=1 to boot-args)"

	section "compute NVRAM trail (last step reached)"
	TRAIL_VALUE="$(nvram 4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:rdna4-trail 2>/dev/null || true)"
	[ -n "$TRAIL_VALUE" ] && echo "$TRAIL_VALUE" || echo "(no trail in NVRAM)"

	# Capture durable evidence before any user-space command or display/GPU
	# exercise. Refresh it after the steps below for the final registry values.
	snapshot_logs

	RUNTIME_ACTIVE=0
	RUNTIME_READY=0
	INFO_OK=0
	SELFTEST_RC=125
	SELFTEST_FILE=""
	BENCH_RC=125
	BENCH_FILE=""
	SENSORS_RC=125
	SENSORS_FILE=""
	SENSORS_IDLE_FILE=""
	VSYNC_RC=125
	VSYNC_FILE=""
	SHOW_RC=125
	SHOW_FILE=""
	ANIM_RC=125
	ANIM_FILE=""
	GFXTRI_RC=125
	GFXTRI_FILE=""
	GFXTRICOL_RC=125
	GFXTRICOL_FILE=""
	GFXAPP_WHY=""
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
		# The idle reading: before any user-space workload, so the SMU's
		# averages cannot be the tail of the selftest or the benchmarks
		# (the round-2 300 W / 3.2 GHz reading was taken right after bench).
		run_step "GPU sensors, idle baseline (before selftest and bench)" "$RUN" sensors
		SENSORS_IDLE_FILE="$STEP_FILE"
		if [ "$EMULATED_CARD" -eq 1 ]; then
			echo "note: emulated card — using bounded rdna4-run selftest 16384"
			run_step "user-space compute runtime (rdna4-run selftest 16384)" "$RUN" selftest 16384
		else
			run_step "user-space compute runtime (rdna4-run selftest)" "$RUN" selftest
		fi
		SELFTEST_FILE="$STEP_FILE"
		SELFTEST_RC=$STEP_RC
		if [ "$VM_MODE" -eq 1 ] || [ "$EMULATED_CARD" -eq 1 ]; then
			echo "note: emulated card — using bounded rdna4-run bench small"
			run_step "compute benchmarks (rdna4-run bench small)" "$RUN" bench small
		else
			run_step "compute benchmarks (rdna4-run bench)" "$RUN" bench
		fi
		BENCH_FILE="$STEP_FILE"
		BENCH_RC=$STEP_RC
		run_step "GPU sensors (rdna4-run sensors, right after the benchmarks)" "$RUN" sensors
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

	# W12k: applications' own gfx IBs. Only where the ring stays up for clients: rdna4-gfx enabled, NOT a probe boot (its PFP/ME park after the draws), a ready VM
	# runtime. `rdna4-run info` must show the GFX line (RDNA4_FLAG_GFX); then `rdna4-run tri` (G3 from the app) and `rdna4-run tricol` (G4), each behind run_step.
	# A client gfx IB that does not finish wedges the gfx ring until the next bring-up (docs/w12k-gfx-submit.md), so tricol runs only after tri passed.
	if [ "$GFX_MODE" -eq 0 ]; then
		GFXAPP_WHY="rdna4-gfx not enabled"
	elif have_arg gfxprobe; then
		GFXAPP_WHY="rdna4-gfxprobe=1: the probe boot parks PFP/ME, clients cannot draw"
	elif [ "$INFO_OK" -ne 1 ]; then
		GFXAPP_WHY="runtime unavailable"
	elif [ "$VM_MODE" -eq 0 ]; then
		GFXAPP_WHY="requires rdna4-vm=1"
	elif ! grep -q '^GFX: client gfx IBs available' "$INFO_FILE"; then
		GFXAPP_WHY="NOFLAG"
	else
		run_step "application gfx IB (rdna4-run tri)" "$RUN" tri
		GFXTRI_FILE="$STEP_FILE"
		GFXTRI_RC=$STEP_RC
		if [ "$GFXTRI_RC" -eq 0 ] && grep -q '^  PASS  tri:' "$GFXTRI_FILE"; then
			run_step "application gfx IB with a colour attribute (rdna4-run tricol)" "$RUN" tricol
			GFXTRICOL_FILE="$STEP_FILE"
			GFXTRICOL_RC=$STEP_RC
		else
			section "application gfx IB with a colour attribute (rdna4-run tricol)"
			echo "(skipped — rdna4-run tri did not pass)"
		fi
	fi
	if [ -n "$GFXAPP_WHY" ]; then
		section "application gfx IB (rdna4-run tri / tricol)"
		if [ "$GFXAPP_WHY" = NOFLAG ]; then
			echo "(rdna4-run info does not show the GFX line: the gfx ring is not available to clients)"
		else
			echo "(inactive — $GFXAPP_WHY)"
		fi
	fi

	# W6 is part of the compute runtime. rdna4-hang=0 deliberately disables
	# recovery; otherwise exercise both public entry points.
	if [ "$INFO_OK" -eq 1 ] && [ "$COMPUTE_STAGE" -ge 6 ] && [ "$HANG_MODE" -ne 0 ]; then
		if [ "$EMULATED_CARD" -eq 1 ]; then
			echo "note: emulated card — W6 selftest uses 16384 items"
			run_step "queue recovery (rdna4-run selftest 16384)" "$RUN" selftest 16384
		else
			run_step "queue recovery (rdna4-run selftest hang)" "$RUN" selftest hang
		fi
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

	# The property is the durable result channel; the dmesg copy is retained
	# for diagnosis and as the fallback when the property is unavailable.
	snapshot_logs
	section "IORegistry: compact feature results"
	[ -n "$REG_RESULTS" ] && echo "$REG_RESULTS" || echo "(no RDNA4FB,Results property)"
	section "dmesg: feature lines (IH, VM, GFX, flip, trails and hangs)"
	grep -E 'RDNA4FB: (.*ih:|.*vm:|.*vmid|.*gfx:|.*flip:|.*trail|.*hang|.*PreviousHang)' "$KLOG" || \
		echo "(no feature-specific lines)"

	# Boot 9 (docs/vm-client-rootcause.md). The feature-line filter above drops every runtime line without "vmid" in it (round 6 lost all the
	# "dispatch timed out" / recovery lines that way), so the VM/queue evidence gets sections of its own, with registry copies for the day the
	# kernel log window has lost the bring-up lines (round 6 had: its window began 146 s into the boot).
	section "dmesg: VM/queue diagnostic (rdna4-vmid-test: probes T0-T7, surveys at the flow points, client-op trace)"
	if [ "$VMIDTEST_MODE" -eq 0 ]; then
		echo "(inactive - add rdna4-vmid-test=7 to boot-args)"
	else
		klines 'RDNA4FB: vmidtest:' || true
	fi
	section "dmesg: runtime client lines (open, close, dequeue, dispatch, wedge, recovery; host buffer map/unmap noise removed)"
	klines 'RDNA4FB: runtime:' | grep -v 'host buffer' || true
	section "dmesg: MEC / HQD / VM boot-test lines (compute: mec|vm|runtime|hqd|pm: survey)"
	klines 'RDNA4FB: compute: (mec|vm|runtime|hqd|pm: survey)' || true
	section "registry copies that survive the kernel log wrapping: Compute,VMSurvey / Compute,VMOps / RDNA4FB,Results"
	for prop in VMSurvey VMOps; do
		echo "--- Compute,$prop"
		REGP="$(ioreg -l -w0 2>/dev/null | grep "\"Compute,$prop\"" | sed -E "s/.*\"Compute,$prop\" = \"([^\"]*)\".*/\1/" | head -1)"
		if [ -n "$REGP" ]; then
			echo "$REGP" | sed 's/ ## /\
/g'
		else
			echo "(no Compute,$prop property: the kext build has no registry copy of the survey/trace, or rdna4-vmid-test is off)"
		fi
	done
	echo "--- RDNA4FB,Results"
	echo "$REG_RESULTS"

	# Feature summary. PASS means the bounded command and its result marker
	# succeeded. SKIPPED means its boot-arg or command is absent.
	if [ "$RUNTIME_ACTIVE" -eq 0 ]; then
		record runtime SKIPPED "compute stage < 6"
	elif [ "$INFO_OK" -eq 1 ] && [ "$SELFTEST_RC" -eq 0 ] && \
		[ "$BENCH_RC" -eq 0 ] && grep -q 'selftest: PASS' "$SELFTEST_FILE" && \
		grep -q 'bench: PASS' "$BENCH_FILE" && \
		case "$(registry_value runtime)" in PASS*) true;; *) false;; esac; then
		record runtime PASS "selftest + bench PASS"
	else
		record runtime FAIL "info/selftest/bench did not all pass"
	fi

	# W8: selftest's SubmitIb section proves a single IB, ordered fences and
	# ten back-to-back IBs. Keep it separate from the general runtime result so
	# the real-card report shows which queue path was actually exercised.
	if [ "$VM_MODE" -eq 0 ]; then
		record submitib SKIPPED "requires rdna4-vm=1"
	elif [ "$INFO_OK" -eq 0 ]; then
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
	if [ "$VM_MODE" -eq 0 ]; then
		record fault SKIPPED "requires rdna4-vm=1"
	elif [ "$INFO_OK" -eq 0 ]; then
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
		grep -q 'two VM clients dispatched concurrently' "$SELFTEST_FILE" && \
		case "$(registry_value vm)" in PASS*) true;; *) false;; esac; then
		record vm PASS "isolation + concurrent queues"
	elif printf '%s\n' "$(registry_value vm)" | grep -q '^FAIL'; then
		record vm FAIL "$(registry_value vm)"
	else
		record vm FAIL "GPUVM command and RDNA4FB,Results did not both pass"
	fi

	if [ "$IH_MODE" -eq 0 ]; then
		record ih SKIPPED "rdna4-ih not enabled"
	elif ! grep -q 'RDNA4FB: compute:' "$KLOG"; then
		record ih SKIPPED "kernel compute log unavailable"
	elif grep -q 'RDNA4FB: ih: self-test:' "$KLOG" && \
		grep -q 'RDNA4FB: ih: self-test totals:' "$KLOG" && \
		case "$(registry_value ih)" in PASS*) true;; *) false;; esac; then
		ih_key="$(registry_value ih | sed 's/^PASS //')"
		if grep -q 'RDNA4FB: ih: self-test: SDMA fence/trap delivered' "$KLOG"; then
			ih_key="$ih_key; self-test SDMA trap delivered"
		else
			ih_key="$ih_key; self-test completed with polling fallback"
		fi
		grep -q 'RDNA4FB: ih: self-test: CP EOP delivered' "$KLOG" &&
			ih_key="$ih_key; CP EOP delivered"
		record ih PASS "$ih_key"
	else
		record ih FAIL "IH self-test and RDNA4FB,Results did not both pass"
	fi

	if [ "$IH_MODE" -lt 2 ]; then
		record vblank SKIPPED "requires rdna4-ih=2"
	elif [ "$VSYNC_RC" -eq 0 ] && grep -q 'vsync: 120 vblanks' "$VSYNC_FILE" && \
		case "$(registry_value vblank)" in PASS*) true;; *) false;; esac; then
		vblank_key="$(grep 'vsync: 120 vblanks' "$VSYNC_FILE" | tail -1)"
		record vblank PASS "$vblank_key"
	else
		record vblank FAIL "vsync did not complete 120 waits"
	fi

	if [ "$GFX_MODE" -eq 0 ]; then
		record gfx SKIPPED "rdna4-gfx not enabled"
	elif grep -Eq 'RDNA4FB: .*gfx: .*failure|RDNA4FB: .*stage gfx ring.*failed|RDNA4FB: .*gfx ring.*off' "$KLOG"; then
		record gfx FAIL "ring/draw path reported a failure"
	elif printf '%s\n' "$(registry_value gfx)" |
		grep -Eq '^PASS.*THE TRIANGLE IS RIGHT.*8192'; then
		record gfx PASS "$(registry_value gfx | sed 's/^PASS //')"
	elif printf '%s\n' "$(registry_value gfx)" | grep -q '^PASS'; then
		record gfx FAIL "gfx ring passed but the draw result was not proven"
	elif grep -Eq 'RDNA4FB: .*gfx: .*ring|RDNA4FB: .*gfx: .*draw' "$KLOG"; then
		record gfx FAIL "gfx command result and RDNA4FB,Results did not both pass"
	else
		record gfx SKIPPED "feature skipped before ring test"
	fi

	# G4, the colour triangle (rdna4-gfxcol=1, after a passing G3 baseline): docs/g4-colour.md. Registry key "gfx-col"; the PASS text carries the
	# colour verdict and 8192 pixels. The per-draw details are in the gfx: dmesg lines ("col: ...") and the Compute,GFXVerdict / GFXColPixels properties.
	if [ "$GFXCOL_MODE" -eq 0 ]; then
		record gfx-col SKIPPED "rdna4-gfxcol not enabled"
	elif printf '%s\n' "$(registry_value gfx-col)" |
		grep -Eq '^PASS.*THE COLOUR TRIANGLE IS RIGHT.*8192'; then
		record gfx-col PASS "$(registry_value gfx-col | sed 's/^PASS //')"
	elif printf '%s\n' "$(registry_value gfx-col)" | grep -q '^SKIPPED'; then
		record gfx-col SKIPPED "$(registry_value gfx-col | sed 's/^SKIPPED //')"
	elif printf '%s\n' "$(registry_value gfx-col)" | grep -q '^PASS'; then
		record gfx-col FAIL "colour draw passed but the image was not proven"
	elif printf '%s\n' "$(registry_value gfx-col)" | grep -q '^FAIL'; then
		record gfx-col FAIL "$(registry_value gfx-col | sed 's/^FAIL //')"
	elif grep -Eq 'RDNA4FB: .*gfx: col:' "$KLOG"; then
		record gfx-col FAIL "colour draw logged but RDNA4FB,Results has no result"
	else
		record gfx-col SKIPPED "G4 not reached (the gfx stage did not get to the draw)"
	fi

	# W12k: the applications' own gfx IBs (steps above). PASS only on the command's own success line.
	if [ -n "$GFXAPP_WHY" ] && [ "$GFXAPP_WHY" != NOFLAG ]; then
		record gfx-app-tri SKIPPED "$GFXAPP_WHY"
		record gfx-app-tricol SKIPPED "$GFXAPP_WHY"
	elif [ "$GFXAPP_WHY" = NOFLAG ]; then
		record gfx-app-tri FAIL "rdna4-run info shows no GFX flag (gfx ring not available to clients)"
		record gfx-app-tricol FAIL "rdna4-run info shows no GFX flag (gfx ring not available to clients)"
	elif [ "$GFXTRI_RC" -eq 0 ] && grep -q '^  PASS  tri:' "$GFXTRI_FILE"; then
		record gfx-app-tri PASS "$(grep '^  PASS  tri:' "$GFXTRI_FILE" | head -1 | sed 's/^  PASS  tri: //')"
		if [ "$GFXTRICOL_RC" -eq 0 ] && grep -q '^  PASS  tricol:' "$GFXTRICOL_FILE"; then
			record gfx-app-tricol PASS "$(grep '^  PASS  tricol:' "$GFXTRICOL_FILE" | head -1 | sed 's/^  PASS  tricol: //')"
		else
			record gfx-app-tricol FAIL "$(grep -E '^  FAIL' "$GFXTRICOL_FILE" | head -1 | sed 's/^  FAIL  //')${GFXTRICOL_FILE:+ (rc $GFXTRICOL_RC)}"
		fi
	else
		record gfx-app-tri FAIL "$(grep -E '^  FAIL' "$GFXTRI_FILE" 2>/dev/null | head -1 | sed 's/^  FAIL  //') (rc $GFXTRI_RC)"
		record gfx-app-tricol SKIPPED "rdna4-run tri did not pass"
	fi

	# W12k (rdna4-gfxclient=1): a synthetic client gfx IB through SubmitGfxIb's path, after the G3 baseline passed (docs/w12k-gfx-submit.md).
	if [ "$GFXCLIENT_MODE" -eq 0 ]; then
		record gfx-client SKIPPED "rdna4-gfxclient not enabled"
	elif printf '%s\n' "$(registry_value gfx-client)" | grep -q '^PASS'; then
		record gfx-client PASS "$(registry_value gfx-client | sed 's/^PASS //')"
	elif printf '%s\n' "$(registry_value gfx-client)" | grep -q '^SKIPPED'; then
		record gfx-client SKIPPED "$(registry_value gfx-client | sed 's/^SKIPPED //')"
	elif printf '%s\n' "$(registry_value gfx-client)" | grep -q '^FAIL'; then
		record gfx-client FAIL "$(registry_value gfx-client | sed 's/^FAIL //')"
	else
		record gfx-client SKIPPED "not reached (the G3 baseline did not pass)"
	fi

	# Boot 9 rows (docs/vm-client-rootcause.md). vm-confound: the round 6 boots carried rdna4-vm-diag=4065 (bit 512 "F" = the GC hub's fault default
	# page pointed at a system page while the boot test runs); the script's "active:" list did not show it, so the confound was invisible.
	if [ "$VM_MODE" -eq 0 ]; then
		record vm-confound SKIPPED "rdna4-vm not enabled"
	elif [ -z "$VMDIAG_MODE" ]; then
		record vm-confound INFO "plain VM boot: no rdna4-vm-diag (no F fault-default-page, no variants)"
	else
		record vm-confound INFO "rdna4-vm-diag=$VMDIAG_MODE set (bit 512 = F fault default page to system memory, active during the boot test): the round 6 configuration"
	fi
	if [ "$VMIDTEST_MODE" -eq 0 ]; then
		record vmidtest SKIPPED "rdna4-vmid-test not enabled"
	elif [ $((VMIDTEST_MODE & 1)) -eq 0 ]; then
		record vmidtest SKIPPED "rdna4-vmid-test=$VMIDTEST_MODE has no probe bit (1)"
	else
		VT="$(registry_value vmidtest)"
		if [ -z "$VT" ]; then
			record vmidtest FAIL "no RDNA4FB,Results vmidtest: the probes did not finish (see the vmidtest: lines and the NVRAM trail)"
		elif printf '%s\n' "$VT" | grep -Eq '=[FHD]( |$)'; then
			record vmidtest FAIL "$VT  (F fail, H shader hang, D SH_MEM readback differs, S skipped; the first =F/=H is the first failing step)"
		else
			record vmidtest PASS "$VT"
		fi
	fi
	# Late probes (mask bit 8): the client-style probes repeated after clock gating (hypothesis H7: timing vs the clients, which run after cg).
	if [ $((VMIDTEST_MODE & 8)) -eq 0 ]; then
		record vmidtest-late SKIPPED "rdna4-vmid-test bit 8 (late probes) not set"
	else
		VTL="$(registry_value vmidtest-late)"
		if [ -z "$VTL" ]; then
			record vmidtest-late FAIL "no RDNA4FB,Results vmidtest-late: the late probes did not run or finish (an early shader probe hung, or bring-up did not reach them)"
		elif printf '%s\n' "$VTL" | grep -Eq '=[FHD]( |$)'; then
			record vmidtest-late FAIL "$VTL  (compare with the early row: a probe that passes early and fails late points at clock gating / timing, H7)"
		else
			record vmidtest-late PASS "$VTL"
		fi
	fi
	# Surveys at the flow points (GRBM_STATUS bit 31 = GUI_ACTIVE): which step first leaves the GC busy.
	if [ $((VMIDTEST_MODE & 2)) -eq 0 ]; then
		record vm-survey SKIPPED "rdna4-vmid-test bit 2 (surveys) not set"
	else
		SURV="$(klines 'RDNA4FB: vmidtest: survey .*: GRBM 0x' || true)"
		SURV_SRC="kernel log"
		if [ -z "$SURV" ]; then
			# The kernel log window lost the bring-up lines: use the registry copy (entries "<tag> G<grbm>/<grbm2> C... ## ").
			SURV="$(ioreg -l -w0 2>/dev/null | grep '"Compute,VMSurvey"' | sed -n -E 's/.*"Compute,VMSurvey" = "([^"]*)".*/\1/p' | head -1 | sed 's/ ## /\
/g' | sed -n -E 's/^(.*) G([0-9a-fA-F]{8})\/.*$/vmidtest: survey \1: GRBM 0x\2/p')"
			SURV_SRC="Compute,VMSurvey registry copy"
		fi
		if [ -z "$SURV" ]; then
			record vm-survey FAIL "no survey lines in the kernel log window and no parsable Compute,VMSurvey property: bring-up lines lost"
		else
			SURV_FIRST=""
			SURV_LIST=""
			while IFS= read -r sl; do
				stag="$(printf '%s\n' "$sl" | sed -E 's/.*vmidtest: survey (.*): GRBM 0x.*/\1/')"
				sg="$(printf '%s\n' "$sl" | sed -E 's/.*: GRBM 0x([0-9a-fA-F]{8}).*/\1/')"
				SURV_LIST="$SURV_LIST [$stag: $sg]"
				case "$sg" in [89a-fA-F]*) [ -z "$SURV_FIRST" ] && SURV_FIRST="$stag";; esac
			done <<SURVEOF
$SURV
SURVEOF
			if [ -n "$SURV_FIRST" ]; then
				record vm-survey PASS "GRBM bit 31 (GUI_ACTIVE) first set at '$SURV_FIRST' (from the $SURV_SRC);$SURV_LIST"
			else
				record vm-survey PASS "GRBM bit 31 never set at any survey point (from the $SURV_SRC);$SURV_LIST"
			fi
		fi
	fi
	# The client-op trace: does a refusal/timeout/dequeue timeout appear, and where.
	if [ $((VMIDTEST_MODE & 4)) -eq 0 ]; then
		record vm-trace SKIPPED "rdna4-vmid-test bit 4 (client-op trace) not set"
	else
		OPS="$(klines 'RDNA4FB: vmidtest: op ' || true)"
		if [ -z "$OPS" ]; then
			record vm-trace FAIL "no op-trace lines (no client opened, or the window lost them)"
		else
			OPS_N="$(printf '%s\n' "$OPS" | grep -c .)"
			OPS_REF="$(printf '%s\n' "$OPS" | grep -c 'REFUSED' || true)"
			OPS_TO="$(printf '%s\n' "$OPS" | grep -c 'TIMED OUT\|TIMEOUT' || true)"
			OPS_FIRST="$(printf '%s\n' "$OPS" | grep 'REFUSED\|TIMED OUT\|TIMEOUT' | head -1 | sed 's/^.*vmidtest: //')"
			record vm-trace PASS "$OPS_N op lines, $OPS_REF refused, $OPS_TO timeouts; first problem: ${OPS_FIRST:-none}"
		fi
	fi
	# The idle pin itself, from the SMU (what the round 6 logs call "100 % / 80 W"), taken before the selftest.
	IDLE_LINE="$(grep '^sensors-pm\[2\]: GFXCLK' "$SENSORS_FILE" 2>/dev/null | tail -1)"
	if [ -n "$IDLE_LINE" ]; then
		IDLE_ACT="$(printf '%s\n' "$IDLE_LINE" | sed -E 's/.*GFX activity ([0-9]+) %.*/\1/')"
		IDLE_W="$(printf '%s\n' "$IDLE_LINE" | sed -E 's/.*socket ([0-9]+) W.*/\1/')"
		case "$IDLE_ACT" in ''|*[!0-9]*) IDLE_ACT="";; esac
		if [ -z "$IDLE_ACT" ]; then
			record idle-pin SKIPPED "no parsable SMU sample"
		elif [ "$IDLE_ACT" -ge 50 ]; then
			record idle-pin FAIL "PINNED: GFX activity ${IDLE_ACT} %, ${IDLE_W} W at idle (the round 6 VM-boot signature; clean boots read 3-8 % / 20-45 W)"
		else
			record idle-pin PASS "idle: GFX activity ${IDLE_ACT} %, ${IDLE_W} W"
		fi
	fi

	if [ "$FLIP_MODE" -eq 0 ]; then
		record flip SKIPPED "rdna4-flip not enabled"
	elif [ "$SHOW_RC" -eq 0 ] && grep -q 'show: desktop restored' "$SHOW_FILE" && \
		case "$(registry_value flip)" in PASS*) true;; *) false;; esac; then
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
	elif [ "$ANIM_RC" -eq 0 ] && grep -q 'anim: frames rendered' "$ANIM_FILE" && \
		grep -q 'anim: desktop restored' "$ANIM_FILE"; then
		anim_key="frames rendered"
		anim_key="$(grep 'anim: frames rendered' "$ANIM_FILE" | tail -1); desktop restored"
		record anim PASS "$anim_key"
	else
		record anim FAIL "animation command failed"
	fi

	if [ "$COMPUTE_STAGE" -lt 6 ] || [ "$HANG_MODE" -eq 0 ]; then
		record w6 SKIPPED "queue recovery disabled or runtime absent"
	elif [ "$EMULATED_CARD" -eq 1 ] && [ "$HANG_RC" -eq 0 ] && [ "$HANGTEST_RC" -eq 0 ] && \
		grep -q 'selftest: PASS' "$HANG_FILE" && grep -q 'hangtest: PASS' "$HANGTEST_FILE"; then
		record w6 PASS "selftest 16384 + hangtest PASS"
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

	# Power-management view of the same SMU table: two samples one second
	# apart (see r2-sensors-review.md section 5). A stale table or a clock
	# held at max at idle is a finding, not a tool failure, so it stays PASS.
	if [ "$INFO_OK" -eq 0 ]; then
		record sensors-idle SKIPPED "runtime unavailable"
	elif [ -n "$SENSORS_IDLE_FILE" ] && grep -q '^sensors-pm: verdict ' "$SENSORS_IDLE_FILE"; then
		idle_gfx="$(grep '^sensors-pm\[2\]: GFXCLK' "$SENSORS_IDLE_FILE" | tail -1 | sed -E 's/^sensors-pm\[2\]: GFXCLK avg pre-DS ([0-9]+).*GFX activity ([0-9]+) %.*socket ([0-9]+) W.*/avg GFXCLK \1 MHz, activity \2%, \3 W/')"
		idle_verdict="$(grep '^sensors-pm: verdict ' "$SENSORS_IDLE_FILE" | tail -1 | sed 's/^sensors-pm: verdict //')"
		record sensors-idle PASS "$idle_verdict | $idle_gfx"
	else
		record sensors-idle FAIL "no idle power-management sample"
	fi
	if [ "$INFO_OK" -eq 0 ]; then
		record sensors-pm SKIPPED "runtime unavailable"
	elif grep -q '^sensors-pm: verdict ' "$SENSORS_FILE"; then
		pm_gfx="$(grep '^sensors-pm\[2\]: GFXCLK' "$SENSORS_FILE" | tail -1 | sed -E 's/^sensors-pm\[2\]: GFXCLK avg pre-DS ([0-9]+).*GFX activity ([0-9]+) %.*socket ([0-9]+) W.*/avg GFXCLK \1 MHz, activity \2%, \3 W/')"
		pm_verdict="$(grep '^sensors-pm: verdict ' "$SENSORS_FILE" | tail -1 | sed 's/^sensors-pm: verdict //')"
		record sensors-pm PASS "$pm_verdict | $pm_gfx"
	else
		record sensors-pm FAIL "no power-management sample (kext without kRDNA4MethodSensorsEx?)"
	fi

	# rdna4-gfxcg: "cg: ..." lines with a sample before and after (SMU average clock,
	# activity, power). PASS = the enable sequence finished; read the values.
	# Clock gating is on by default; the row reports the before/after sample of the "cg:" block.
	if dmesg | grep -q 'RDNA4FB: compute: cg: finished'; then
		cg_before="$(dmesg | grep 'RDNA4FB: compute: pm: cg before:' | tail -1 | sed -E 's/.*avg GFXCLK pre-DS ([0-9]+) post-DS.*GFX activity ([0-9]+) %.*socket ([0-9]+) W.*/\1 MHz \2% \3 W/')"
		cg_after="$(dmesg | grep 'RDNA4FB: compute: pm: cg after (+1.3 s):' | tail -1 | sed -E 's/.*avg GFXCLK pre-DS ([0-9]+) post-DS.*GFX activity ([0-9]+) %.*socket ([0-9]+) W.*/\1 MHz \2% \3 W/')"
		cg_kind="default on"
		[ -n "$GFXCG_MODE" ] && cg_kind="rdna4-gfxcg=$GFXCG_MODE"
		record gfxcg PASS "$cg_kind: before $cg_before -> after $cg_after"
	elif dmesg | grep -q 'RDNA4FB: compute: cg: RLC is not running\|RLC safe mode did not acknowledge'; then
		record gfxcg FAIL "clock gating not applied (see the cg: lines)"
	elif [ -z "$GFXCG_MODE" ]; then
		record gfxcg SKIPPED "bring-up did not reach stage 7, so the default clock gating was not applied"
	else
		record gfxcg FAIL "clock gating did not finish (see the cg: / pm: lines)"
	fi

	# rdna4-gfxoff: AllowGfxOff at the very end of bring-up, guarded on every GC access.
	# The sample is SMU-only. "first GC access" is the guard waking GFX; WAKE FAILED is a FAIL.
	if [ -z "$GFXOFF_MODE" ] || [ "$GFXOFF_MODE" = 0 ]; then
		record gfxoff SKIPPED "rdna4-gfxoff not enabled"
	elif dmesg | grep -q 'RDNA4FB: compute: gfxoff:.*WAKE FAILED'; then
		record gfxoff FAIL "the guard could not wake GFX (see the gfxoff: lines)"
	elif dmesg | grep -q 'RDNA4FB: compute: gfxoff: AllowGfxOff refused'; then
		go_why="$(dmesg | grep -a -o 'gfxoff: AllowGfxOff refused: [^[]*' | tail -1 | cut -c1-160)"
		record gfxoff SKIPPED "$go_why"
	elif dmesg | grep -q 'RDNA4FB: compute: gfxoff: 1.5 s after AllowGfxOff'; then
		go_line="$(dmesg | grep 'RDNA4FB: compute: gfxoff: 1.5 s after AllowGfxOff' | tail -1 | sed -E 's/.*avg GFXCLK pre-DS ([0-9]+) post-DS.*GFX activity ([0-9]+) %.*socket ([0-9]+) W.*/\1 MHz \2% \3 W/')"
		go_wake="no GC access yet"
		dmesg | grep -q 'RDNA4FB: compute: gfxoff: GFXOFF ended by' && go_wake="the guard woke GFX on a later GC access"
		dmesg | grep -q 'not a GFXOFF reading' && go_wake="a user-space GC access ended GFXOFF inside the wait: the sample is NOT a GFXOFF reading"
		go_q=""
		dmesg | grep -q 'RDNA4FB: compute: gfxoff: the boot HQD is back' && go_q="; boot HQD lost in the power-down and restored"
		dmesg | grep -q 'RDNA4FB: compute: gfxoff: the boot HQD could not be restored' && go_q="; BOOT HQD LOST AND NOT RESTORED"
		record gfxoff PASS "1.5 s after Allow: $go_line; $go_wake$go_q"
	else
		record gfxoff FAIL "GFXOFF was not allowed (see the gfxoff: lines)"
	fi

	# rdna4-gfxpm: the experiment logs "pm: <step>: GFXCLK n MHz ..." after each
	# step; show the first and the last so one row says whether anything moved.
	if [ "$GFXPM_MODE" -eq 0 ]; then
		record gfxpm SKIPPED "rdna4-gfxpm not enabled"
	elif grep -q 'RDNA4FB: compute: pm: experiment finished' "$KLOG"; then
		pm_first="$(grep 'RDNA4FB: compute: pm: baseline' "$KLOG" | tail -1 | sed -E 's/.*pm: baseline[^:]*: avg GFXCLK pre-DS ([0-9]+) post-DS.*GFX activity ([0-9]+) %.*socket ([0-9]+) W.*/\1 MHz \2% \3 W/')"
		pm_last="$(grep 'RDNA4FB: compute: pm: ' "$KLOG" | grep -v 'pm: survey ' | grep 'avg GFXCLK pre-DS [0-9]*'  | tail -1 | sed -E 's/.*pm: ([^:]*): avg GFXCLK pre-DS ([0-9]+) post-DS.*GFX activity ([0-9]+) %.*socket ([0-9]+) W.*/\1: \2 MHz \3% \4 W/')"
		record gfxpm PASS "baseline $pm_first -> $pm_last"
	else
		record gfxpm FAIL "pm experiment did not finish (see the pm: lines)"
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

# Make the durable summary visible before reporting success to the caller.
sync >/dev/null 2>&1 || true

echo "wrote $OUT"
echo
echo "Feature summary:"
cat "$SUMMARY"
echo
echo "Key log sections are in $OUT: dmesg feature lines, the NVRAM trail, and every bounded test output."
