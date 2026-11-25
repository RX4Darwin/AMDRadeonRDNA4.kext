#!/bin/sh
# rdna4-fan-apply: RDNA4Gadget's fan settings applied as root. The LaunchDaemon org.rdna4.fan (tools/package/package.sh) runs it at boot
# and whenever the settings file changes. Each line of the file is one debug.rdna4_fan command (`reset` first, then the custom
# settings); the kext checks every value against the card's limits and refuses the rest (docs/fan-control-plan.md).
conf="/Library/Application Support/AMDRadeonRDNA4/fan.conf"
[ -s "$conf" ] || exit 0
# at boot the kext's setting appears once the card's SMU has started
i=0
until /usr/sbin/sysctl -n debug.rdna4_fan > /dev/null 2>&1; do
	i=$((i + 1))
	[ $i -lt 120 ] || { echo "rdna4-fan-apply: no debug.rdna4_fan (the driver's kext not running)" >&2; exit 1; }
	sleep 1
done
head -n 16 "$conf" | while IFS= read -r line; do
	case "$line" in '' | '#'*) continue ;; esac
	/usr/sbin/sysctl "debug.rdna4_fan=$line" > /dev/null || echo "rdna4-fan-apply: refused: $line" >&2
done
echo "rdna4-fan-apply: $(/usr/sbin/sysctl -n debug.rdna4_fan)"
