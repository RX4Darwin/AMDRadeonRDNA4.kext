#!/bin/sh
# spike-bar.sh — S1: does the resizable BAR buy us anything?
#
# Context: docs/install-tahoe.md section 4, spike S1.
#
# nullmoth/1401 gives the macOS installer a SMALL GPU BAR (ResizeAppleGpuBars 0,
# ResizeGpuBars -1) because the installer draws on the firmware GOP screen, which
# only survives macOS's PCI setup with a small BAR; it restores the full BAR only
# after the driver is installed.  RDNA4FB adopts that same GOP scanout, so the
# small BAR is the safe profile for us too -- IF the large BAR buys us nothing.
#
# It may well buy nothing: since the DMA path landed (SDMA + AGP aperture +
# 16 MiB pinned bounce buffer, docs/hw-logs/2026-09-28-recovery-dma-pass.txt)
# buffers come from VRAM past the BAR, not from the BAR-limited 96 MiB
# CPU-visible heap.  If the numbers match, our documented profile becomes
# "small BAR, always", which is strictly safer than 1401's two-phase dance.
#
# This script MEASURES ONLY.  It changes no hardware state, writes no registers,
# and touches no OpenCore config: it runs the two commands the question needs and
# saves the evidence.  You change the BAR setting yourself between the two runs.
#
#   Run A (as the machine is today, large BAR if that is what is configured):
#       bash /Volumes/OPENCORE/spike-bar.sh A
#
#   then set ResizeGpuBars = -1 and ResizeAppleGpuBars = 0 in config.plist,
#   reboot into Recovery, and:
#
#   Run B (small BAR):
#       bash /Volumes/OPENCORE/spike-bar.sh B
#
#   then: bash /Volumes/OPENCORE/spike-bar.sh compare
#
# Boot args: use the normal base set (set-boot.sh 1).  The compute runtime must
# be up, so the kext needs rdna4-compute=7 as usual; the script says so if the
# runtime is not there.  Nothing here needs rdna4-vm, gfx or any probe boot.
#
# Safe to re-run.  Each run appends nothing; it writes one new file per tag.

set -u

tag="${1:-}"
case "$tag" in
  A|B|a|b) tag=$(printf '%s' "$tag" | tr 'ab' 'AB') ;;
  compare) ;;
  *)
    echo "usage: $0 A|B|compare" >&2
    echo "  A        measure with the BAR as currently configured" >&2
    echo "  B        measure after changing the BAR setting" >&2
    echo "  compare  print both results side by side" >&2
    exit 2
    ;;
esac

# Where we live: next to the kext and rdna4-run on the stick.
dir=$(cd "$(dirname "$0")" 2>/dev/null && pwd) || dir=.
run="$dir/rdna4-run"
[ -x "$run" ] || run="$dir/build/rdna4-run"

out_for() { echo "$dir/spike-bar-$1.txt"; }

# ---------------------------------------------------------------- compare mode
if [ "$tag" = compare ]; then
  a=$(out_for A); b=$(out_for B)
  for f in "$a" "$b"; do
    [ -f "$f" ] || { echo "missing $f -- run both A and B first" >&2; exit 1; }
  done
  echo "=== S1: resizable BAR, A vs B ==================================="
  echo
  printf '%-34s | %-22s | %-22s\n' "measure" "A" "B"
  printf -- '-----------------------------------+------------------------+------------------------\n'
  # Pull the figures both runs record under KEY: value lines.
  for key in \
    "heap total MiB" "heap free MiB" "DMA" \
    "host->GPU MB/s" "GPU->host MB/s" "VRAM copy GB/s" \
    "SGEMM GFLOPS" "selftest" "BAR0 bytes"
  do
    va=$(grep -m1 "^$key: " "$a" 2>/dev/null | sed "s/^$key: //")
    vb=$(grep -m1 "^$key: " "$b" 2>/dev/null | sed "s/^$key: //")
    printf '%-34s | %-22s | %-22s\n' "$key" "${va:-?}" "${vb:-?}"
  done
  echo
  echo "Read it as:"
  echo "  * figures within noise of each other  -> the large BAR buys nothing;"
  echo "    adopt the small-BAR profile everywhere (installer and daily)."
  echo "  * B's heap collapses to ~96 MiB and the transfer rates fall"
  echo "    -> DMA did not engage in B; look at the 'DMA' line and the raw log"
  echo "    before concluding anything about the BAR itself."
  echo "  * B is materially slower with DMA on -> the large BAR earns its risk;"
  echo "    keep 1401's two-phase profile (small for the installer only)."
  echo
  echo "Raw logs: $a"
  echo "          $b"
  exit 0
fi

# ---------------------------------------------------------------- measure mode
out=$(out_for "$tag")
: > "$out" || { echo "cannot write $out" >&2; exit 1; }

say() { echo "$@" | tee -a "$out"; }

say "=== RDNA4FB spike S1: resizable BAR, run $tag ==="
say "date: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
say "boot-args: $(nvram boot-args 2>/dev/null | sed 's/^boot-args[[:space:]]*//')"
say ""

# --- 1. what the card's BAR0 actually is, straight from the PCI nub -----------
# assigned-addresses is a packed array of 5-dword entries; the GPU's first
# 64-bit prefetchable memory window is BAR0 (the VRAM aperture).  We do not
# decode it in shell -- we save it raw and print the registry's own view.
say "--- PCI / registry evidence ---"
ioreg -lw0 -p IOService -n "RDNA4FB" 2>/dev/null >> "$out"
ioreg -lw0 -c IOPCIDevice 2>/dev/null \
  | grep -E '"(vendor-id|device-id|assigned-addresses|ranges|IOName)"' >> "$out" 2>/dev/null
# The kext publishes what it found; these are the lines worth reading first.
for prop in "VRAM,TotalMB" "MMIO,Verified" "Console," "Discovery,Source"; do
  ioreg -lw0 2>/dev/null | grep -m2 "$prop" | sed 's/^[[:space:]]*/  /' | tee -a "$out" >/dev/null
done
bar0=$(ioreg -lw0 -c IOPCIDevice 2>/dev/null \
  | grep -m1 -A0 '"assigned-addresses"' | head -c 400)
say "BAR0 bytes: ${bar0:-unknown (see assigned-addresses above)}"
say ""

# --- 2. the runtime's own view: heap size is the BAR-sensitive number ---------
if [ ! -x "$run" ]; then
  say "rdna4-run not found next to this script ($dir) -- copy build/rdna4-run to the stick."
  say "Nothing measured."
  exit 1
fi

say "--- rdna4-run info ---"
info=$("$run" info 2>&1)
echo "$info" >> "$out"
echo "$info"
# The info output names the heap; keep whatever numbers it prints verbatim and
# also normalise the two we compare on.
hs=$(echo "$info" | grep -iEo '[0-9]+ *MiB' | head -1)
hf=$(echo "$info" | grep -iEo 'free[^0-9]*[0-9]+ *MiB' | grep -Eo '[0-9]+ *MiB' | head -1)
dma=$(echo "$info" | grep -iE 'dma|sdma|bounce' | head -1)
say "heap total MiB: ${hs:-?}"
say "heap free MiB: ${hf:-?}"
say "DMA: ${dma:-not stated by info; see bench transfer rates}"
say ""

say "--- rdna4-run selftest ---"
st=$("$run" selftest 2>&1)
echo "$st" >> "$out"
echo "$st" | tail -5
if echo "$st" | grep -qi 'pass'; then say "selftest: PASS"; else say "selftest: FAIL"; fi
say ""

say "--- rdna4-run bench ---"
say "(this is the long one; a few minutes)"
bench=$("$run" bench 2>&1)
echo "$bench" >> "$out"
echo "$bench"
h2g=$(echo "$bench" | grep -iE 'host *-?> *gpu' | grep -Eo '[0-9.]+ *[MG]B/s' | head -1)
g2h=$(echo "$bench" | grep -iE 'gpu *-?> *host' | grep -Eo '[0-9.]+ *[MG]B/s' | head -1)
vram=$(echo "$bench" | grep -iE 'vram' | grep -Eo '[0-9.]+ *GB/s' | head -1)
sgemm=$(echo "$bench" | grep -iE 'sgemm' | grep -Eo '[0-9.]+ *[TG]FLOPS' | head -1)
say ""
say "host->GPU MB/s: ${h2g:-?}"
say "GPU->host MB/s: ${g2h:-?}"
say "VRAM copy GB/s: ${vram:-?}"
say "SGEMM GFLOPS: ${sgemm:-?}"
say ""
say "Saved: $out"
say ""
case "$tag" in
  A) say "Next: set ResizeGpuBars = -1 and ResizeAppleGpuBars = 0 in config.plist," ;;
  B) say "Next: bash $0 compare" ;;
esac
[ "$tag" = A ] && say "      reboot into Recovery, then: bash $0 B"
exit 0
