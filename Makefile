# Makefile — AMDRadeonRDNA4, cross-compiled for x86_64 (Intel hackintosh) from any
# host, including Apple Silicon. Targets the macOS 11 (Big Sur) ABI.
#
#   make                  the kext and the programs that go to the test machine
#   make CONFIG=DEBUG     the same without optimisation and with DEBUG defined
#   make VMTEST=1         the VM smoke-test variant (see below)
#   make test             the host tests (no GPU needed)
#   make clean            remove what this Makefile builds; `make distclean` removes Build/
#   make product-dir      print the directory the products of this configuration go to
#
# Everything that is built goes to Build/:
#
#   Build/AMDRadeonRDNA4-<version>-<CONFIG>/AMDRadeonRDNA4.kext
#   Build/AMDRadeonRDNA4-<version>-<CONFIG>/bin/     rdna4-run, n48nprobe, the rig's scripts, and with Mesa built
#                                                   (`make mesa`) libvulkan_radeon.dylib, vkprobe and swapprobe;
#                                                   `make vkcube` adds Khronos's vkcube and loader
#   Build/obj/<CONFIG>/                             the kext's objects
#   Build/host/                                     the host tests' programs
#   Build/vmtest/...                                the same two levels for VMTEST=1
#   Build/radv-build/                               Mesa (`make mesa`); kept by `make clean`
#
# This file and src/kext.mk are written in the part of make's language that GNU
# make (3.81, the one macOS ships, and later) and BSD make (bmake) share: no
# conditionals, no functions, no pattern rules, no $< or $^ outside suffix
# rules. A choice is made through a variable's name ($(X_$(CONFIG))) or in the
# shell; a value only the shell knows (the SDK, the commit) stays a command
# substitution that the shell runs.
#
# AMDRadeonRDNA4 is a Lilu plugin (see src/plugin.cpp): Lilu's plugin_start.cpp is
# compiled in, Lilu symbols are left undefined in the -kext bundle and
# resolved by the kernel loader through OSBundleLibraries (as.vit9696.Lilu).
# Install it with OpenCore (Kernel -> Add, after Lilu.kext).

PRODUCT      = AMDRadeonRDNA4
# The one place the version is written (CONTRIBUTING.md says when it changes). It goes into the bundle's Info.plist,
# the kext's kmod_info and the name of the product directory. The commit a build was made from ("-dirty": with
# uncommitted changes) goes into the kernel log and the registry (AMDRadeonRDNA4,Build) beside it.
VERSION      = 0.1.0

# RELEASE: -Os, what every card test up to 0.1.0 ran. DEBUG: -O0 and DEBUG defined (Lilu's DBGLOG lines).
CONFIG       ?= RELEASE

# VMTEST=1: the kext additionally adopts QEMU's vmware-svga (15ad:0405) boot
# framebuffer, so loading and WindowServer behaviour on a given macOS release
# can be checked in a VM without RDNA 4 hardware. All BAR5 register work is
# skipped there because the device has no BAR5.
VMTEST       ?= 0

ARCH         = x86_64
DEPLOY       = 11.0
SDK          ?= `xcrun --sdk macosx --show-sdk-path 2>/dev/null`
CXX          = clang++
CC           = clang

BUILD        = Build
OUT_0        = $(BUILD)
OUT_1        = $(BUILD)/vmtest
OUT          = $(OUT_$(VMTEST))
OBJDIR       = $(OUT)/obj/$(CONFIG)
# The way back from the object directory to this one.
UP_0         = ../../..
UP_1         = ../../../..
PRODUCT_DIR  = $(OUT)/$(PRODUCT)-$(VERSION)-$(CONFIG)
KEXT         = $(PRODUCT_DIR)/$(PRODUCT).kext
BIN          = $(PRODUCT_DIR)/bin
HOST         = $(BUILD)/host

LILU         = lib/Lilu/Lilu
# lib/Lilu/Lilu/Headers/{hde32.h,hde64.h,capstone} are git symlinks; checkouts
# without symlink support (Windows, core.symlinks=false) turn them into
# one-line text files. The shim holds real copies and is searched first.
LILU_SHIM    = $(BUILD)/liluhdr
LILU_STAMP   = $(LILU_SHIM)/.stamp

# AMD firmware for the compute bring-up (src/compute.cpp, stage 2+): the
# linux-firmware blobs in firmware/amdgpu/ (tools/build/fetch-firmware.sh) are
# embedded when all are present; otherwise the kext builds without them and
# those stages report it.
FW_DIR       = firmware/amdgpu
FW_BLOBS     = $(FW_DIR)/psp_14_0_3_sos.bin $(FW_DIR)/smu_14_0_3.bin \
               $(FW_DIR)/sdma_7_0_1.bin $(FW_DIR)/gc_12_0_1_pfp.bin \
               $(FW_DIR)/gc_12_0_1_me.bin $(FW_DIR)/gc_12_0_1_mec.bin \
               $(FW_DIR)/gc_12_0_1_uni_mes.bin $(FW_DIR)/gc_12_0_1_imu.bin \
               $(FW_DIR)/gc_12_0_1_rlc.bin

# A dump of the card's ROM, which the host tests parse and the emulator's option ROM is made from. It is the card
# vendor's and not in the repository (firmware/README.md): without it `make test` leaves the ROM checks out.
ROM          ?= firmware/Sapphire.RX9070XT.16384.241213.rom

CONFIG_NOTE_RELEASE =
CONFIG_NOTE_DEBUG   = , DEBUG

# --- user space (macOS, for the test machine) ----------------------------------
USER_FLAGS   = -arch $(ARCH) -target $(ARCH)-apple-macos$(DEPLOY) -isysroot $(SDK) \
               -mmacosx-version-min=$(DEPLOY) -std=c11 -O2 -Wall -Iinclude -Isrc
OBJC_FLAGS   = -arch $(ARCH) -target $(ARCH)-apple-macos$(DEPLOY) -isysroot $(SDK) \
               -mmacosx-version-min=$(DEPLOY) -std=gnu11 -O1 -Wall
RUN          = userspace/rdna4-run
VULKAN       = userspace/vulkan
CENSUS       = tools/metal/accelcensus

.PHONY: all kext tools userspace n48nprobe mesa vulkan-products vkcube census-tool census-stub census-pathlog \
        test atomdump check-isa check-config product-dir clean distclean

all: check-config kext tools

check-config:
	@case "$(CONFIG)/$(VMTEST)" in DEBUG/[01]|RELEASE/[01]) ;; \
	*) echo "CONFIG is DEBUG or RELEASE, VMTEST is 0 or 1 (got CONFIG=$(CONFIG) VMTEST=$(VMTEST))" >&2; exit 1 ;; esac

product-dir:
	@echo $(PRODUCT_DIR)

# --- the kext ----------------------------------------------------------------
# The objects are built by src/kext.mk, run in the object directory. Two small headers are made there first, each
# rewritten only when its content changes, so that only what includes it is compiled again: buildrev.h (the version
# and the commit) and fwconfig.h (whether the firmware is there, and which). The objects that follow a header are
# removed with it: a change within the second of the last build would otherwise go unseen (make compares whole
# seconds).
kext: check-config $(LILU_STAMP)
	@mkdir -p $(OBJDIR) $(KEXT)/Contents/MacOS
	@rev=`git describe --always --dirty --abbrev=8 2>/dev/null || echo unknown`; \
	echo "#define AMDRADEONRDNA4_BUILD \"$(VERSION) ($$rev$(CONFIG_NOTE_$(CONFIG)))\"" > $(OBJDIR)/buildrev.h.new; \
	cmp -s $(OBJDIR)/buildrev.h.new $(OBJDIR)/buildrev.h || { cp $(OBJDIR)/buildrev.h.new $(OBJDIR)/buildrev.h; \
		rm -f $(OBJDIR)/plugin.o $(OBJDIR)/device.o $(OBJDIR)/kmod_info.o; }; \
	rm -f $(OBJDIR)/buildrev.h.new
	@missing=; for f in $(FW_BLOBS); do [ -f $$f ] || missing=1; done; \
	if [ -n "$$missing" ]; then echo "#define AMDRadeonRDNA4_NO_FIRMWARE 1"; else cksum $(FW_BLOBS) | sed 's|^|// |'; fi \
		> $(OBJDIR)/fwconfig.h.new; \
	cmp -s $(OBJDIR)/fwconfig.h.new $(OBJDIR)/fwconfig.h || { cp $(OBJDIR)/fwconfig.h.new $(OBJDIR)/fwconfig.h; \
		rm -f $(OBJDIR)/*.o; }; \
	rm -f $(OBJDIR)/fwconfig.h.new
	@(cd $(OBJDIR) && $(MAKE) -f $(UP_$(VMTEST))/src/kext.mk ROOT=$(UP_$(VMTEST)) SDK="$(SDK)" CXX="$(CXX)" CC="$(CC)" \
		PRODUCT=$(PRODUCT) VERSION=$(VERSION) ARCH=$(ARCH) DEPLOY=$(DEPLOY) CONFIG=$(CONFIG) VMTEST=$(VMTEST) \
		LILU_SHIM=$(UP_$(VMTEST))/$(LILU_SHIM))
	@cp $(OBJDIR)/$(PRODUCT) $(KEXT)/Contents/MacOS/$(PRODUCT)
	@sed 's/@VERSION@/$(VERSION)/' Info.plist > $(KEXT)/Contents/Info.plist
	@# AMD's firmware license travels with the binary that embeds its firmware.
	@rm -rf $(KEXT)/Contents/Resources; \
	if ! grep -q NO_FIRMWARE $(OBJDIR)/fwconfig.h && [ -f $(FW_DIR)/LICENSE.amdgpu ]; then \
		mkdir -p $(KEXT)/Contents/Resources && cp $(FW_DIR)/LICENSE.amdgpu $(KEXT)/Contents/Resources/LICENSE.amdgpu; fi
	@# Minimal, ad-hoc code signature so kextutil is happier during testing.
	@codesign --force --sign - $(KEXT) 2>/dev/null || true
	@echo "Built $(KEXT) for $(ARCH) (min macOS $(DEPLOY))"
	@command -v file >/dev/null && file $(KEXT)/Contents/MacOS/$(PRODUCT) || true

$(LILU_STAMP): $(LILU)/../hde/hde32.h $(LILU)/../hde/hde64.h
	@mkdir -p $(LILU_SHIM)/Headers/capstone
	cp $(LILU)/../hde/hde32.h $(LILU)/../hde/hde64.h $(LILU_SHIM)/Headers/
	cp $(LILU)/../capstone/include/*.h $(LILU_SHIM)/Headers/capstone/
	@touch $@

# --- what goes to the test machine beside the kext -----------------------------
tools: $(BIN)/rdna4-run $(BIN)/n48nprobe $(BIN)/diagnostic-log.sh $(BIN)/boot-history.sh vulkan-products

# The compute runtime's client. Linked by the C++ driver: it is the one pointed at ld64 (tools/build/build-osxcross.sh).
$(BIN)/rdna4-run: $(RUN)/rdna4-run.c $(RUN)/librdna4.c $(RUN)/librdna4.h $(RUN)/pm4build.h $(RUN)/gfx12tri.h $(RUN)/gfx12tricol.h $(RUN)/gfx12trirun.h src/gfx12_draw.h src/gfx12_draw_col.h src/ngg_kernel.h src/psred_kernel.h src/nggcol_kernel.h src/pscol_kernel.h include/rdna4compute.h src/vadd_codeobj.h src/bench_codeobj.h
	@mkdir -p $(BIN)
	$(CXX) -x c $(USER_FLAGS) $(RUN)/rdna4-run.c $(RUN)/librdna4.c \
		-framework IOKit -framework CoreFoundation -weak_framework Accelerate -o $@

userspace: $(BIN)/rdna4-run

# The card test of the Vulkan interface's memory half (docs/vulkan-port.md): runs on the test machine, as root.
$(BIN)/n48nprobe: $(VULKAN)/n48nprobe.c $(VULKAN)/navi48_native_abi.h include/rdna4vulkan.h
	@mkdir -p $(BIN)
	$(CXX) -x c $(USER_FLAGS) -I$(VULKAN) $(VULKAN)/n48nprobe.c -framework IOKit -framework CoreFoundation -o $@

n48nprobe: $(BIN)/n48nprobe

# The script every card test ends with: it looks for rdna4-run beside itself.
$(BIN)/diagnostic-log.sh: tools/rig/diagnostic-log.sh
	@mkdir -p $(BIN)
	cp tools/rig/diagnostic-log.sh $@

# For a machine that freezes: which of the last boots were cut off, and their last kernel lines.
$(BIN)/boot-history.sh: tools/rig/boot-history.sh
	@mkdir -p $(BIN)
	cp tools/rig/boot-history.sh $@

# The Vulkan driver and its test programs for the card tests (docs/todo-vulkantest.md): Mesa with the patches of
# userspace/vulkan/mesa-patches, fetched (about 150 MB, the first time) and built in $(BUILD)/radv-build by `make mesa`.
# `make clean` keeps that tree. Once it is built, every `make` puts the library, vkprobe and swapprobe (built against
# Mesa's headers) beside the other programs, for whichever configuration is being made.
RADV_DIR     = $(BUILD)/radv-build
RADV_BUILT   = $(RADV_DIR)/build/src/amd/vulkan/libvulkan_radeon.dylib
RADV_LIB     = $(BIN)/libvulkan_radeon.dylib
mesa:
	$(VULKAN)/build-mesa.sh $(RADV_DIR)
	@$(MAKE) vulkan-products
	@otool -l $(RADV_LIB) | grep -A4 LC_BUILD_VERSION | grep -q 'minos $(DEPLOY)' || \
		{ echo "$(RADV_LIB) is not built for macOS $(DEPLOY)" >&2; exit 1; }
	@echo "Built $(RADV_LIB), $(BIN)/vkprobe and $(BIN)/swapprobe for $(ARCH) (min macOS $(DEPLOY))"

vulkan-products:
	@[ -f $(RADV_BUILT) ] || exit 0; \
	mkdir -p $(BIN); \
	cmp -s $(RADV_BUILT) $(RADV_LIB) || { echo "cp $(RADV_BUILT) $(RADV_LIB)"; cp $(RADV_BUILT) $(RADV_LIB); }; \
	for p in vkprobe swapprobe; do \
		if [ ! -f $(BIN)/$$p ] || [ $(VULKAN)/$$p.c -nt $(BIN)/$$p ]; then \
			echo "$(CXX) -x c ... $(VULKAN)/$$p.c -o $(BIN)/$$p"; \
			$(CXX) -x c $(USER_FLAGS) -I$(RADV_DIR)/mesa/include -I$(VULKAN) $(VULKAN)/$$p.c -o $(BIN)/$$p || exit 1; \
		fi; \
	done

# A Vulkan application that is not ours: Khronos's vkcube with their loader and the manifest that tells the loader
# where the driver of `make mesa` is (bin/vulkan/icd.d/), beside that driver in bin/ (userspace/vulkan/build-app.sh; fetches about 65 MB
# into $(BUILD)/vk-app the first time; needs cmake). docs/todo-vulkantest.md has the commands.
vkcube: mesa
	$(VULKAN)/build-app.sh $(BUILD)/vk-app $(RADV_BUILT) $(BIN)
	@echo "Built $(BIN)/vkcube with libvulkan.dylib and vulkan/icd.d/radeon_icd.json beside it"

# E1 trigger (docs/metal-spike.md): Objective-C, runs in Recovery next to rdna4-run.
$(BIN)/rdna4-census: $(CENSUS)/census.m
	@mkdir -p $(BIN)
	$(CXX) -x objective-c -fobjc-arc $(OBJC_FLAGS) $(CENSUS)/census.m \
		-framework Foundation -framework Metal -framework IOKit -o $@

census-tool: $(BIN)/rdna4-census

# E1c stub bundle (docs/metal-spike.md s.11.4): a Metal device bundle that only logs. MTLIOAccelDevice is private
# (exported by Metal), hence dynamic_lookup for the superclass reference.
CENSUS_STUB  = $(BIN)/RDNA4CensusMTLDriver.bundle
$(CENSUS_STUB): $(CENSUS)/stub/RDNA4CensusMTLDriver.m $(CENSUS)/stub/Info.plist
	@mkdir -p $@/Contents/MacOS
	$(CXX) -x objective-c -fno-objc-arc $(OBJC_FLAGS) -bundle -undefined dynamic_lookup \
		$(CENSUS)/stub/RDNA4CensusMTLDriver.m -framework Foundation -o $@/Contents/MacOS/RDNA4CensusMTLDriver
	cp $(CENSUS)/stub/Info.plist $@/Contents/Info.plist

census-stub: $(CENSUS_STUB)

$(BIN)/libpathlog.dylib: $(CENSUS)/pathlog.c
	@mkdir -p $(BIN)
	$(CXX) -x c $(OBJC_FLAGS) -dynamiclib $(CENSUS)/pathlog.c -o $@

census-pathlog: $(BIN)/libpathlog.dylib

# --- host tests (native arch, run on the build machine) -----------------------
# atomdump compiles the kext's own parsers and engines for the host and runs them against the card's ROM dump and
# against stand-ins for the hardware: the logic is verified without a GPU.
ATOMDUMP     = $(HOST)/atomdump
$(ATOMDUMP): test/atomdump.cpp src/atombios.cpp src/atombios.hpp src/ipdiscovery.cpp src/ipdiscovery.hpp src/edid.cpp src/edid.hpp src/otgtiming.cpp src/otgtiming.hpp src/modes.cpp src/modes.hpp src/dmub.hpp src/pipe.cpp src/pipe.hpp src/ndrv.cpp src/ndrv.hpp src/modeset.cpp src/modeset.hpp src/pipe2.cpp src/pipe2.hpp src/pipe2_linux.inc src/pipe2_linux_hpd4.inc src/pipe2_linux_dp1.inc src/pipe2_linux_dp2.inc test/ref/dp_retime_linux.inc src/dptrain.cpp src/dptrain.hpp src/dpphy.cpp src/dpphy.hpp test/ref/dp_train_linux.inc test/ref/legacy-vmtable-ref.inc src/amdfw.cpp src/amdfw.hpp src/psp.cpp src/psp.hpp src/sdma.cpp src/sdma.hpp src/pm4.cpp src/pm4.hpp src/codeobj.cpp src/codeobj.hpp src/vadd_codeobj.h src/bench_codeobj.h src/gfxregs.hpp src/linuxref.hpp src/gpuheap.cpp src/gpuheap.hpp src/flip.hpp include/rdna4compute.h src/ihdecode.cpp src/ih.hpp src/gpuvm.cpp src/vmtree.cpp src/vmtree.hpp src/n48n.cpp src/n48n.hpp test/n48n-host/hostbackend.hpp $(VULKAN)/navi48_native_abi.h src/gpuvm.hpp src/vmid.cpp src/vmid.hpp src/ptpages.cpp src/ptpages.hpp src/gpuvmtable.cpp src/gpuvmtable.hpp
	@mkdir -p $(HOST)
	$(CXX) -std=c++17 -Wall -O2 -Iinclude -Itest -o $@ test/atomdump.cpp src/atombios.cpp src/ipdiscovery.cpp src/edid.cpp src/otgtiming.cpp src/modes.cpp src/pipe.cpp src/ndrv.cpp src/modeset.cpp src/pipe2.cpp src/dptrain.cpp src/dpphy.cpp src/amdfw.cpp src/psp.cpp src/sdma.cpp src/ihdecode.cpp src/pm4.cpp src/codeobj.cpp src/gpuheap.cpp src/gpuvm.cpp src/vmtree.cpp src/n48n.cpp src/vmid.cpp src/ptpages.cpp src/gpuvmtable.cpp

atomdump: $(ATOMDUMP)

PVSTREAM_TEST = $(HOST)/pvstream-test
$(PVSTREAM_TEST): test/pvstream-test.cpp src/pvstream.cpp src/pvstream.hpp src/pvopcodes.inc src/pvdevinfo.inc
	@mkdir -p $(HOST)
	$(CXX) -std=c++17 -Wall -O1 -o $@ test/pvstream-test.cpp src/pvstream.cpp

# What rdna4-run records for its triangles, and the ring packets the kernel wraps around a client's graphics work.
TRI_HDRS     = $(RUN)/gfx12tri.h $(RUN)/gfx12tricol.h $(RUN)/gfx12trirun.h $(RUN)/pm4build.h src/gfx12_draw.h src/gfx12_draw_col.h src/ngg_kernel.h src/psred_kernel.h src/nggcol_kernel.h src/pscol_kernel.h
$(HOST)/test-gfx12tri: test/test-gfx12tri.c $(TRI_HDRS)
	@mkdir -p $(HOST)
	$(CC) -std=c11 -Wall -Wextra -pedantic -I$(RUN) -Isrc test/test-gfx12tri.c -o $@

$(HOST)/test-gfx12tricol: test/test-gfx12tricol.c $(TRI_HDRS)
	@mkdir -p $(HOST)
	$(CC) -std=c11 -Wall -Wextra -pedantic -I$(RUN) -Isrc test/test-gfx12tricol.c -lm -o $@

$(HOST)/test-w12k-pm4: test/test-w12k-pm4.cpp src/pm4.cpp src/pm4.hpp
	@mkdir -p $(HOST)
	$(CXX) -std=c++17 -Wall -Wextra -Isrc test/test-w12k-pm4.cpp src/pm4.cpp -o $@

check-isa:
	bash test/check-isa.sh

test: $(ATOMDUMP) $(PVSTREAM_TEST) $(HOST)/test-gfx12tri $(HOST)/test-gfx12tricol $(HOST)/test-w12k-pm4
	@if [ -f $(ROM) ]; then echo "$(ATOMDUMP) $(ROM)"; $(ATOMDUMP) $(ROM); else echo "$(ATOMDUMP)   (no $(ROM))"; $(ATOMDUMP); fi
	$(PVSTREAM_TEST)
	$(HOST)/test-gfx12tri
	$(HOST)/test-gfx12tricol
	$(HOST)/test-w12k-pm4
	bash test/check-isa.sh

# --- cleaning ----------------------------------------------------------------
# `clean` leaves what took a download and a long build: Mesa ($(RADV_DIR)) and the other trees fetched into Build/.
clean:
	rm -rf $(BUILD)/obj $(BUILD)/host $(BUILD)/liluhdr $(BUILD)/vmtest $(BUILD)/$(PRODUCT)-*

distclean:
	rm -rf $(BUILD)
