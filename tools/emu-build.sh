#!/usr/bin/env bash
# Build the emulated RX 9070 XT: a QEMU with the `rdna4` device (emu/qemu/),
# the EFI GOP driver for its option ROM (emu/efi/RdnaGopDxe), and the ROM
# image itself (the card's legacy AtomBIOS image + that driver).
#
#   tools/emu-build.sh [qemu|gop|rom|all]      (default: all)
#
# Trees (outside the repo, reused between runs):
#   $QEMU_SRC  QEMU 10.0.13 source, device linked in as hw/display/rdna4.c
#              (default ~/qemu-10.0.13, from download.qemu.org)
#   $EDK2      edk2 checkout with BaseTools (default ~/edk2)
# Outputs: $QEMU_SRC/build/qemu-system-x86_64, build-emu/rdna4.rom
#
# Needs (Debian): build-essential ninja-build meson python3-venv pkg-config
# libglib2.0-dev libpixman-1-dev libslirp-dev zlib1g-dev flex bison nasm
# acpica-tools uuid-dev.
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
QEMU_VER=10.0.13
QEMU_SRC=${QEMU_SRC:-$HOME/qemu-$QEMU_VER}
EDK2=${EDK2:-$HOME/edk2}
OUT=$HERE/build-emu
FLASH=$HERE/firmware/Sapphire.RX9070XT.16384.241213.rom
JOBS=$(nproc)

build_qemu() {
	if [ ! -d "$QEMU_SRC" ]; then
		curl -sSL "https://download.qemu.org/qemu-$QEMU_VER.tar.xz" | tar xJ -C "$(dirname "$QEMU_SRC")"
	fi
	# The device source stays in the repo; QEMU builds it through a link.
	ln -sf "$HERE/emu/qemu/rdna4.c" "$QEMU_SRC/hw/display/rdna4.c"
	grep -q '^config RDNA4$' "$QEMU_SRC/hw/display/Kconfig" || cat >> "$QEMU_SRC/hw/display/Kconfig" <<'EOF'

config RDNA4
    bool
    default y if PCI_DEVICES
    depends on PCI
EOF
	grep -q "CONFIG_RDNA4" "$QEMU_SRC/hw/display/meson.build" ||
		echo "system_ss.add(when: 'CONFIG_RDNA4', if_true: [files('rdna4.c'), pixman])" \
			>> "$QEMU_SRC/hw/display/meson.build"
	if [ ! -f "$QEMU_SRC/build/build.ninja" ]; then
		mkdir -p "$QEMU_SRC/build"
		(cd "$QEMU_SRC/build" && ../configure --target-list=x86_64-softmmu \
			--enable-kvm --enable-vnc --enable-slirp --disable-docs --disable-gtk \
			--disable-sdl --disable-werror)
	fi
	ninja -C "$QEMU_SRC/build" -j"$JOBS" qemu-system-x86_64
	echo "built $QEMU_SRC/build/qemu-system-x86_64"
}

build_gop() {
	if [ ! -d "$EDK2" ]; then
		git clone -q --depth 1 --recurse-submodules --shallow-submodules \
			https://github.com/tianocore/edk2 "$EDK2"
	fi
	[ -x "$EDK2/BaseTools/Source/C/bin/EfiRom" ] || make -C "$EDK2/BaseTools" -j"$JOBS"
	# emu/efi is its own small platform (RdnaGopPkg.dsc), linked into the tree.
	ln -sfn "$HERE/emu/efi" "$EDK2/RdnaGopPkg"
	(
		cd "$EDK2"
		set +u
		. ./edksetup.sh >/dev/null
		set -u
		build -a X64 -t GCC -b RELEASE -n "$JOBS" -p RdnaGopPkg/RdnaGopPkg.dsc
	)
	mkdir -p "$OUT"
	cp "$EDK2/Build/RdnaGopPkg/RELEASE_GCC/X64/RdnaGopDxe.efi" "$OUT/"
	echo "built $OUT/RdnaGopDxe.efi"
}

build_rom() {
	mkdir -p "$OUT"
	# Legacy AtomBIOS image of the real card: flash 0x40000, 0xe600 bytes
	# (PCI data structure: 1002:7550, code type 0, not the last image).
	dd if="$FLASH" of="$OUT/atombios-legacy.bin" bs=4096 skip=$((0x40000 / 4096)) \
		count=$((0xe600 / 4096)) status=none
	dd if="$FLASH" bs=1 skip=$((0x40000 + 0xe000)) count=$((0xe600 - 0xe000)) status=none \
		>> "$OUT/atombios-legacy.bin"
	"$EDK2/BaseTools/Source/C/bin/EfiRom" -f 0x1002 -i 0x7550 -l 0x030000 \
		-b "$OUT/atombios-legacy.bin" -ec "$OUT/RdnaGopDxe.efi" -o "$OUT/rdna4.rom"
	"$EDK2/BaseTools/Source/C/bin/EfiRom" -d "$OUT/rdna4.rom" | grep -E "Image|Code type|Vendor|Device|Indicator|size" || true
	echo "built $OUT/rdna4.rom ($(stat -c %s "$OUT/rdna4.rom") bytes)"
}

case ${1:-all} in
	qemu) build_qemu ;;
	gop)  build_gop ;;
	rom)  build_rom ;;
	all)  build_qemu; build_gop; build_rom ;;
	*)    echo "usage: $0 [qemu|gop|rom|all]" >&2; exit 2 ;;
esac
