#!/bin/sh
# package.sh <product dir> <version>: AMDRadeonRDNA4-<version>.pkg in the product dir (`make package`; docs/packaging-plan.md), from the
# driver bundle `make metal-driver` built. Three components, one installer:
#   core    (required)  /Library/GPUBundles/AMDRadeonRDNA4MTLDriver.bundle; the LaunchDaemons org.rdna4.air2amdgpu (the shader translator
#                       for WindowServer, whose sandbox cannot start it) and org.rdna4.iop (CoreDisplay's setup each boot), both run from
#                       the bundle's Contents/Helpers; /usr/local/bin/rdna4-run
#   logd    (chosen)    the LaunchDaemon org.rdna4.logd: each boot's kernel log in /var/log/rdna4
#   vulkan  (chosen)    RADV's manifest in /usr/local/share/vulkan/icd.d, where Khronos's Vulkan loader looks
# The scripts stop and remove what the per-component install.sh scripts of 0.3 put in place first. Unsigned (packaging-plan.md:
# no Developer ID while the driver needs AMFI relaxed). The kext is not in it: OpenCore injects it (README, Installing).
set -eu
dir=${1:?usage: package.sh <product dir> <version>}
version=${2:?usage: package.sh <product dir> <version>}
bundle=AMDRadeonRDNA4MTLDriver.bundle
installed=/Library/GPUBundles/$bundle
helpers=$installed/Contents/Helpers
id=com.rx4darwin.AMDRadeonRDNA4
[ -f "$dir/$bundle/Contents/MacOS/AMDRadeonRDNA4MTLDriver" ] || { echo "no $dir/$bundle: make metal-driver" >&2; exit 1; }
[ -x "$dir/$bundle/Contents/Helpers/air2amdgpu" ] || { echo "the bundle has no translator: make metal-helper, then make metal-driver" >&2; exit 1; }

w=$dir/pkg
rm -rf "$w"
mkdir -p "$w"

# A LaunchDaemon plist: daemon <file> <label> <program args...> -- <extra keys>
daemon() {
	file=$1 label=$2
	shift 2
	{
		echo '<?xml version="1.0" encoding="UTF-8"?>'
		echo '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">'
		echo '<plist version="1.0">'
		echo '<dict>'
		echo "	<key>Label</key><string>$label</string>"
		printf '	<key>ProgramArguments</key><array>'
		while [ "$1" != -- ]; do
			printf '<string>%s</string>' "$1"
			shift
		done
		shift
		echo '</array>'
		echo '	<key>RunAtLoad</key><true/>'
		for k in "$@"; do
			echo "	$k"
		done
		echo '</dict>'
		echo '</plist>'
	} > "$file"
	plutil -lint -s "$file"
}

# core
r=$w/core-root
mkdir -p "$r/Library/GPUBundles" "$r/Library/LaunchDaemons" "$r/usr/local/bin"
cp -R "$dir/$bundle" "$r/Library/GPUBundles/"
cp tools/package/uninstall.sh "$r/Library/GPUBundles/$bundle/Contents/Resources/uninstall.sh"
codesign -s - -f "$r/Library/GPUBundles/$bundle" > /dev/null 2>&1   # (the uninstaller is part of its resources)
daemon "$r/Library/LaunchDaemons/org.rdna4.air2amdgpu.plist" org.rdna4.air2amdgpu "$helpers/air2amdgpu" --serve -- \
	'<key>KeepAlive</key><true/>' '<key>ProcessType</key><string>Interactive</string>' '<key>ThrottleInterval</key><integer>10</integer>' \
	'<key>StandardErrorPath</key><string>/var/log/rdna4-air2amdgpu.log</string>'
daemon "$r/Library/LaunchDaemons/org.rdna4.iop.plist" org.rdna4.iop "$helpers/rdna4-iop" -- \
	'<key>StandardOutPath</key><string>/var/log/rdna4-iop.log</string>' '<key>StandardErrorPath</key><string>/var/log/rdna4-iop.log</string>'
ln -s "$helpers/rdna4-run" "$r/usr/local/bin/rdna4-run"

# logd
r=$w/logd-root
mkdir -p "$r/Library/LaunchDaemons"
daemon "$r/Library/LaunchDaemons/org.rdna4.logd.plist" org.rdna4.logd "$helpers/rdna4-logd" -- \
	'<key>KeepAlive</key><true/>' '<key>ThrottleInterval</key><integer>30</integer>'

# vulkan
r=$w/vulkan-root
mkdir -p "$r/usr/local/share/vulkan/icd.d"
cp "$dir/$bundle/Contents/Resources/radeon_icd.json" "$r/usr/local/share/vulkan/icd.d/radeon_icd.json"

# scripts: preinstall stops and removes the old pieces and this component's daemons, postinstall starts them
script() {
	mkdir -p "$w/$1-scripts"
	{
		printf '#!/bin/sh\n# %s %s (tools/package/package.sh)\nset -e\n' "$1" "$2"
		printf '%b\n' "$3"
		printf 'exit 0\n'
	} > "$w/$1-scripts/$2"
	chmod 755 "$w/$1-scripts/$2"
}
script core preinstall '
for l in org.rdna4.air2amdgpu org.rdna4.iop com.hackintosh.AMDRadeonRDNA4.air2amdgpu; do launchctl bootout system/$l 2> /dev/null || true; done
rm -f /Library/LaunchDaemons/com.hackintosh.AMDRadeonRDNA4.air2amdgpu.plist /usr/local/libexec/rdna4-air2amdgpu /usr/local/libexec/rdna4-iop
rm -rf /Library/GPUBundles/AMDRadeonRDNA4Metal.bundle /Library/Frameworks/AMDRadeonRDNA4.framework '"$installed"
script core postinstall '
for l in org.rdna4.air2amdgpu org.rdna4.iop; do launchctl bootstrap system /Library/LaunchDaemons/$l.plist; done'
script logd preinstall '
launchctl bootout system/org.rdna4.logd 2> /dev/null || true
rm -f /usr/local/libexec/rdna4-logd'
script logd postinstall '
launchctl bootstrap system /Library/LaunchDaemons/org.rdna4.logd.plist'

# (pkgutil lists ._ entries: the files' com.apple.provenance attribute, which macOS sets again at once; the installer writes them back
# as attributes, not as files)
for c in core logd vulkan; do
	scripts=
	[ -d "$w/$c-scripts" ] && scripts="--scripts $w/$c-scripts"
	# shellcheck disable=SC2086
	pkgbuild --quiet --root "$w/$c-root" $scripts --identifier "$id.$c" --version "$version" --install-location / \
		--ownership recommended "$w/$c.pkg"
done

cat > "$w/distribution.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
	<title>AMDRadeonRDNA4 $version</title>
	<options customize="allow" require-scripts="true" hostArchitectures="x86_64" rootVolumeOnly="true"/>
	<os-version min="26.0"/>
	<choices-outline>
		<line choice="core"/>
		<line choice="logd"/>
		<line choice="vulkan"/>
	</choices-outline>
	<choice id="core" title="Metal driver" description="The Metal driver with the kext's runtime client, the shader translator and the display setup (required). The kext itself goes to the EFI by hand: see the README." enabled="false" selected="true">
		<pkg-ref id="$id.core"/>
	</choice>
	<choice id="logd" title="Boot logs" description="Each boot's kernel log in /var/log/rdna4, for bug reports." selected="true">
		<pkg-ref id="$id.logd"/>
	</choice>
	<choice id="vulkan" title="Vulkan (RADV)" description="Mesa's Vulkan driver for programs that use Khronos's Vulkan loader." selected="true">
		<pkg-ref id="$id.vulkan"/>
	</choice>
	<pkg-ref id="$id.core" version="$version">core.pkg</pkg-ref>
	<pkg-ref id="$id.logd" version="$version">logd.pkg</pkg-ref>
	<pkg-ref id="$id.vulkan" version="$version">vulkan.pkg</pkg-ref>
</installer-gui-script>
EOF
productbuild --quiet --distribution "$w/distribution.xml" --package-path "$w" "$dir/AMDRadeonRDNA4-$version.pkg"
echo "Built $dir/AMDRadeonRDNA4-$version.pkg"
