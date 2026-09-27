#!/usr/bin/env bash
# Boot the OSX-KVM macOS VM on the emulated RX 9070 XT (QEMU `rdna4` device)
# instead of vmware-svga. Same disks, serial log, monitor socket and VNC
# display as ~/OSX-KVM/boot-tahoe.sh, so only one of the two runs at a time.
#
#   tools/emu-boot.sh [extra rdna4 device options, e.g. trace=on]
#
# Build first: tools/emu-build.sh (QEMU + option ROM), and an OpenCore image
# with the bare-metal kext:
#   tools/vm-opencore.sh --kext build/RDNA4FB.kext --out OpenCore-emu.qcow2
# Env: OSXKVM (~/OSX-KVM), QEMU_SRC (~/qemu-10.0.13), RAM_MB (16384), OC_IMAGE
# (OpenCore-emu.qcow2 in $OSXKVM/OpenCore). Display: VNC 127.0.0.1:5900.
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
OSXKVM=${OSXKVM:-$HOME/OSX-KVM}
QEMU_SRC=${QEMU_SRC:-$HOME/qemu-10.0.13}
QEMU=$QEMU_SRC/build/qemu-system-x86_64
OC_IMAGE=${OC_IMAGE:-OpenCore-emu.qcow2}
ROM=$HERE/build-emu/rdna4.rom
STATE=$HERE/emu/qemu/gop-state.txt
FLASH=$HERE/firmware/Sapphire.RX9070XT.16384.241213.rom
MONITOR=$HOME/tahoe-monitor.sock
EXTRA=${1:+,$1}

for f in "$QEMU" "$ROM" "$OSXKVM/OpenCore/$OC_IMAGE"; do
	[ -e "$f" ] || { echo "missing $f (see the header of $0)" >&2; exit 1; }
done

cd "$OSXKVM"
# Own NVRAM: the console device paths OVMF stores differ from the
# vmware-svga setup's.
[ -f OVMF_VARS-rdna4.fd ] || cp OVMF_VARS-1920x1080.fd OVMF_VARS-rdna4.fd

MY_OPTIONS="+ssse3,+sse4.2,+popcnt,+avx,+aes,+xsave,+xsaveopt,check"
args=(
	-enable-kvm -m "${RAM_MB:-16384}"
	-cpu Skylake-Client,-hle,-rtm,kvm=on,vendor=GenuineIntel,+invtsc,vmware-cpuid-freq=on,"$MY_OPTIONS"
	-machine q35
	-smp 8,cores=4,sockets=1
	-device qemu-xhci,id=xhci
	-device usb-kbd,bus=xhci.0 -device usb-tablet,bus=xhci.0
	-device usb-ehci,id=ehci
	-device isa-applesmc,osk="ourhardworkbythesewordsguardedpleasedontsteal(c)AppleComputerInc"
	-drive if=pflash,format=raw,readonly=on,file=OVMF_CODE_4M.fd
	-drive if=pflash,format=raw,file=OVMF_VARS-rdna4.fd
	-smbios type=2
	-device ich9-intel-hda -device hda-duplex
	-device ich9-ahci,id=sata
	-drive id=OpenCoreBoot,if=none,snapshot=on,format=qcow2,file="OpenCore/$OC_IMAGE"
	-device ide-hd,bus=sata.2,drive=OpenCoreBoot
	-device ide-hd,bus=sata.3,drive=InstallMedia
	-drive id=InstallMedia,if=none,file=BaseSystem.img,format=raw
	-drive id=MacHDD,if=none,file=mac_hdd_ng.img,format=qcow2
	-device ide-hd,bus=sata.4,drive=MacHDD
	-netdev user,id=net0,hostfwd=tcp:127.0.0.1:10022-:22
	-device virtio-net-pci,netdev=net0,id=net0,mac=52:54:00:c9:18:27
	-monitor unix:"$MONITOR",server,nowait
	-serial file:"$HOME/tahoe-serial.log"
	# The emulated card, alone: -vga none keeps QEMU from adding a stdvga
	# that OVMF's QemuVideoDxe would pick as the boot display. It sits on
	# the root bus: behind a pcie-root-port, macOS's PCI configurator
	# closed the port's windows at boot and the card stopped decoding.
	-vga none
	-device "rdna4,bus=pcie.0,addr=0x10,romfile=$ROM,state=$STATE,flash=$FLASH$EXTRA"
	-display none -vnc 127.0.0.1:0
)

rm -f "$MONITOR"
exec "$QEMU" -L "$QEMU_SRC/pc-bios" "${args[@]}"
