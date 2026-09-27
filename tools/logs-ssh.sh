#!/usr/bin/env bash
# Collect RDNA4FB's kernel log lines and registry properties from the last boot
# of a macOS machine over SSH (defaults: the OSX-KVM guest on port 10022).
#
#   REMOTE_USER=me tools/logs-ssh.sh
set -euo pipefail

HOST=${HOST:-127.0.0.1}
PORT=${PORT:-10022}
KEY=${KEY:-$HOME/.ssh/tahoe_vm}
: "${REMOTE_USER:?set REMOTE_USER to the macOS account name}"

ssh -i "$KEY" -p "$PORT" -o StrictHostKeyChecking=accept-new "$REMOTE_USER@$HOST" '
	echo "== system"; sw_vers; uname -v; csrutil status
	echo "== kext status"; kmutil showloaded --list-only 2>/dev/null | grep -i rdna4 || echo "(RDNA4FB not loaded)"
	echo "== kernel log (last boot)"
	log show --last boot --style compact --predicate "eventMessage CONTAINS \"RDNA4FB\"" 2>/dev/null | tail -200
	echo "== registry"
	ioreg -lw0 -c RDNA4FB | grep -E "\"(Console|GPU|Discovery|AtomBIOS|VRAM|MMIO|Pipe|Modes|IOFB)[^\"]*\"" | head -60
	echo "== displays"
	system_profiler SPDisplaysDataType 2>/dev/null | head -40'
