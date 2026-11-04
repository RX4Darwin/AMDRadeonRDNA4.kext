#!/bin/sh
# Install the paravirtual host's compiler service as a LaunchDaemon (org.rdna4.pvexec), run as root from the top of the repository:
#   sudo userspace/rdna4-pvexec/install.sh [LLVM prefix]
# Copies rdna4-pvexec (make userspace), the compiler (Build/air/air2amdgcn, tools/metal/air/build-tools.sh), its scripts and the
# rasterizer (rdna4raster.ll), and llc, ld.lld, llvm-link and opt of the LLVM prefix (default ~/opt/llvm of the user who ran sudo) into /usr/local/libexec/rdna4-pvexec, then (re)loads the daemon.
# It waits in boots without the paravirtual host and the compute runtime, and logs to /var/log/rdna4-pvexec.log.
set -eu
[ "$(id -u)" -eq 0 ] || { echo "run with sudo" >&2; exit 1; }
llvm=${1:-$(eval echo "~${SUDO_USER:-$USER}")/opt/llvm}
dest=/usr/local/libexec/rdna4-pvexec
plist=/Library/LaunchDaemons/org.rdna4.pvexec.plist
bin=$(ls -d Build/AMDRadeonRDNA4-*-RELEASE/bin 2> /dev/null | head -1)

for f in "$bin/rdna4-pvexec" Build/air/air2amdgcn "$llvm/bin/llc" "$llvm/bin/ld.lld" "$llvm/bin/llvm-link" "$llvm/bin/opt"; do
	[ -x "$f" ] || { echo "missing: $f" >&2; exit 1; }
done

launchctl bootout system "$plist" 2> /dev/null || true
mkdir -p "$dest/bin"
cp "$bin/rdna4-pvexec" Build/air/air2amdgcn tools/metal/air/compile-metallib.sh tools/metal/air/compile-pipeline.sh \
	tools/metal/air/metallib.py tools/metal/air/rdna4raster.ll "$dest/"
cp "$llvm/bin/llc" "$llvm/bin/ld.lld" "$llvm/bin/llvm-link" "$llvm/bin/opt" "$dest/bin/"
# Compiled kernels of the previous compiler are not this one's.
rm -rf /var/tmp/rdna4-pvexec

cat > "$plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key>
	<string>org.rdna4.pvexec</string>
	<key>ProgramArguments</key>
	<array>
		<string>$dest/rdna4-pvexec</string>
		<string>-v</string>
		<string>-s</string>
		<string>$dest/compile-metallib.sh</string>
	</array>
	<key>EnvironmentVariables</key>
	<dict>
		<key>LLVM</key>
		<string>$dest</string>
		<key>AIR2AMDGCN</key>
		<string>$dest/air2amdgcn</string>
	</dict>
	<key>RunAtLoad</key>
	<true/>
	<key>KeepAlive</key>
	<true/>
	<key>ThrottleInterval</key>
	<integer>30</integer>
	<key>StandardOutPath</key>
	<string>/var/log/rdna4-pvexec.log</string>
	<key>StandardErrorPath</key>
	<string>/var/log/rdna4-pvexec.log</string>
</dict>
</plist>
EOF
chown root:wheel "$plist"
chmod 644 "$plist"
launchctl bootstrap system "$plist"
echo "installed in $dest; launchctl print system/org.rdna4.pvexec; log in /var/log/rdna4-pvexec.log"
