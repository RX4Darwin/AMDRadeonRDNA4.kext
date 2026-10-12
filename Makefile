# Makefile — cross-compile the RDNA4FB kext for x86_64 (Intel hackintosh)
# from any host, including Apple Silicon. Targets macOS 11 (Big Sur) ABI.
#
#   make            build RDNA4FB.kext
#   make VMTEST=1   build a VM smoke-test variant into build-vmtest/ (see below)
#   make clean      remove build products
#
# RDNA4FB is a Lilu plugin (see src/plugin.cpp): Lilu's plugin_start.cpp is
# compiled in, Lilu symbols are left undefined in the -kext bundle and
# resolved by the kernel loader through OSBundleLibraries (as.vit9696.Lilu).
# Install it with OpenCore (Kernel -> Add, after Lilu.kext).

PRODUCT      := RDNA4FB
BUNDLE_ID    := com.hackintosh.RDNA4FB
VERSION      := 0.0.1

ARCH         := x86_64
DEPLOY       := 11.0
SDK          ?= $(shell xcrun --sdk macosx --show-sdk-path 2>/dev/null)

MKSDK        := MacKernelSDK
LILU         := Lilu/Lilu

BUILD        := build

# VMTEST=1: the kext additionally adopts QEMU's vmware-svga (15ad:0405) boot
# framebuffer, so loading and WindowServer behaviour on a given macOS release
# can be checked in a VM without RDNA 4 hardware. All BAR5 register work is
# skipped there because the device has no BAR5.
VMTEST       ?= 0
ifeq ($(VMTEST),1)
BUILD        := build-vmtest
endif

KEXT         := $(BUILD)/$(PRODUCT).kext

# Lilu/Lilu/Headers/{hde32.h,hde64.h,capstone} are git symlinks; checkouts
# without symlink support (Windows, core.symlinks=false) turn them into
# one-line text files. The shim holds real copies and is searched first.
LILU_SHIM    := $(BUILD)/liluhdr
LILU_STAMP   := $(LILU_SHIM)/.stamp
MACOS        := $(KEXT)/Contents/MacOS
EXEC         := $(MACOS)/$(PRODUCT)

CXX          := clang++
CC           := clang

# --- sources -----------------------------------------------------------------
CXX_SRCS := \
	src/plugin.cpp \
	src/device.cpp \
	src/cursor.cpp \
	src/ndrv.cpp \
	src/modeset.cpp \
	src/pipe2.cpp \
	src/dptrain.cpp \
	src/dpphy.cpp \
	src/compute.cpp \
	src/gfxring.cpp \
	src/pmidle.cpp \
	src/flip.cpp \
	src/amdfw.cpp \
	src/psp.cpp \
	src/sdma.cpp \
	src/ih.cpp \
	src/ihdecode.cpp \
	src/pm4.cpp \
	src/codeobj.cpp \
	src/gpuheap.cpp \
	src/gpuvm.cpp \
	src/vmtree.cpp \
	src/n48n.cpp \
	src/n48nkext.cpp \
	src/vmid.cpp \
	src/ptpages.cpp \
	src/gpuvmtable.cpp \
	src/runtime.cpp \
	src/vmtest.cpp \
	src/vmshared.cpp \
	src/userclient.cpp \
	src/accelcensus.cpp \
	src/pvgpu.cpp \
	src/pvstream.cpp \
	$(LILU)/Library/plugin_start.cpp \
	src/atombios.cpp \
	src/ipdiscovery.cpp \
	src/edid.cpp \
	src/otgtiming.cpp \
	src/modes.cpp \
	src/pipe.cpp

# VM test builds switch modes on QEMU's vmware-svga through Bochs VBE.
ifeq ($(VMTEST),1)
CXX_SRCS += src/bochsvbe.cpp
endif

C_SRCS := \
	src/kmod_info.c

# AMD firmware for the compute bring-up (src/compute.cpp, stage 2+): the
# linux-firmware blobs in firmware/amdgpu/ are embedded when all are present;
# otherwise the kext builds without them and those stages report it.
FW_BLOBS := firmware/amdgpu/psp_14_0_3_sos.bin firmware/amdgpu/smu_14_0_3.bin \
            firmware/amdgpu/sdma_7_0_1.bin firmware/amdgpu/gc_12_0_1_pfp.bin \
            firmware/amdgpu/gc_12_0_1_me.bin firmware/amdgpu/gc_12_0_1_mec.bin \
            firmware/amdgpu/gc_12_0_1_uni_mes.bin firmware/amdgpu/gc_12_0_1_imu.bin \
            firmware/amdgpu/gc_12_0_1_rlc.bin
ifeq ($(wildcard $(FW_BLOBS)),$(FW_BLOBS))
ASM_SRCS := src/fwblobs.S
FW_FLAGS :=
else
ASM_SRCS :=
FW_FLAGS := -DRDNA4FB_NO_FIRMWARE
endif

OBJS := $(patsubst %.cpp,$(BUILD)/%.o,$(CXX_SRCS)) \
        $(patsubst %.S,$(BUILD)/%.o,$(ASM_SRCS)) \
        $(patsubst %.c,$(BUILD)/%.o,$(C_SRCS))

# --- flags -------------------------------------------------------------------
COMMON_FLAGS := \
	-arch $(ARCH) \
	-target $(ARCH)-apple-macos$(DEPLOY) \
	-isysroot $(SDK) \
	-I$(MKSDK)/Headers \
	-Iinclude \
	-I$(LILU_SHIM) \
	-I$(LILU) \
	-mmacosx-version-min=$(DEPLOY) \
	-DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
	-D_FORTIFY_SOURCE=0 \
	-DPRODUCT_NAME=$(PRODUCT) -DMODULE_VERSION=$(VERSION) \
	-nostdinc \
	-fno-builtin -fno-common -fno-stack-protector -mkernel -fapple-kext \
	-fno-asynchronous-unwind-tables \
	-Wall -Wno-vla -Wno-unknown-warning-option -Wno-ossharedptr-misuse -Os -g \
	-MMD -MP

# -fno-c++-static-destructors: at -Os clang can lower the gMetaClass static
# object's destructor into an __cxa_atexit registration, which the kernel
# does not export — kmutil then fails to link ("could not find a kext which
# exports ___cxa_atexit"). Kext teardown is handled by the kmod _stop path,
# so static destructors are never wanted here.
CXXFLAGS := $(COMMON_FLAGS) -std=c++17 -fno-exceptions -fno-rtti -fcheck-new \
            -fno-c++-static-destructors $(FW_FLAGS)
CFLAGS   := $(COMMON_FLAGS)

ifeq ($(VMTEST),1)
CXXFLAGS += -DRDNA4FB_VM_TEST
endif

LDFLAGS := \
	-arch $(ARCH) \
	-target $(ARCH)-apple-macos$(DEPLOY) \
	-isysroot $(SDK) \
	-nostdlib \
	-Xlinker -kext \
	-L$(MKSDK)/Library/$(ARCH) \
	-lkmodc++ -lkmod \
	-Wl,-no_deduplicate

# --- host-side tools (native arch, run on the build Mac) ---------------------
ATOMDUMP := $(BUILD)/atomdump
FIRMWARE := firmware/Sapphire.RX9070XT.16384.241213.rom

# --- user space: the compute runtime's client (userspace/, macOS) -------------
RUN_TOOL := $(BUILD)/rdna4-run
USER_FLAGS := -arch $(ARCH) -target $(ARCH)-apple-macos$(DEPLOY) -isysroot $(SDK) \
              -mmacosx-version-min=$(DEPLOY) -std=c11 -O2 -Wall -Iinclude -Isrc

# --- rules -------------------------------------------------------------------
.PHONY: all clean test userspace n48nprobe mesa check-isa census-tool census-stub census-pathlog
all: $(KEXT) $(RUN_TOOL)

$(ATOMDUMP): tools/atomdump.cpp src/atombios.cpp src/atombios.hpp src/ipdiscovery.cpp src/ipdiscovery.hpp src/edid.cpp src/edid.hpp src/otgtiming.cpp src/otgtiming.hpp src/modes.cpp src/modes.hpp src/dmub.hpp src/pipe.cpp src/pipe.hpp src/ndrv.cpp src/ndrv.hpp src/modeset.cpp src/modeset.hpp src/pipe2.cpp src/pipe2.hpp src/pipe2_linux.inc src/pipe2_linux_hpd4.inc src/pipe2_linux_dp1.inc src/pipe2_linux_dp2.inc tools/dp_retime_linux.inc src/dptrain.cpp src/dptrain.hpp src/dpphy.cpp src/dpphy.hpp tools/dp_train_linux.inc src/amdfw.cpp src/amdfw.hpp src/psp.cpp src/psp.hpp src/sdma.cpp src/sdma.hpp src/pm4.cpp src/pm4.hpp src/codeobj.cpp src/codeobj.hpp src/vadd_codeobj.h src/bench_codeobj.h src/gfxregs.hpp src/linuxref.hpp src/gpuheap.cpp src/gpuheap.hpp src/flip.hpp include/rdna4compute.h src/ihdecode.cpp src/ih.hpp src/gpuvm.cpp src/vmtree.cpp src/vmtree.hpp src/n48n.cpp src/n48n.hpp tools/n48n-host/hostbackend.hpp vulkan/navi48_native_abi.h src/gpuvm.hpp src/vmid.cpp src/vmid.hpp src/ptpages.cpp src/ptpages.hpp src/gpuvmtable.cpp src/gpuvmtable.hpp
	@mkdir -p $(BUILD)
	$(CXX) -std=c++17 -Wall -O2 -Iinclude -Itools -o $@ tools/atomdump.cpp src/atombios.cpp src/ipdiscovery.cpp src/edid.cpp src/otgtiming.cpp src/modes.cpp src/pipe.cpp src/ndrv.cpp src/modeset.cpp src/pipe2.cpp src/dptrain.cpp src/dpphy.cpp src/amdfw.cpp src/psp.cpp src/sdma.cpp src/ihdecode.cpp src/pm4.cpp src/codeobj.cpp src/gpuheap.cpp src/gpuvm.cpp src/vmtree.cpp src/n48n.cpp src/vmid.cpp src/ptpages.cpp src/gpuvmtable.cpp

# Linked by the C++ driver: it is the one pointed at ld64 (build-osxcross.sh).
$(RUN_TOOL): userspace/rdna4-run.c userspace/librdna4.c userspace/librdna4.h userspace/pm4build.h userspace/gfx12tri.h userspace/gfx12tricol.h userspace/gfx12trirun.h src/gfx12_draw.h src/gfx12_draw_col.h src/ngg_kernel.h src/psred_kernel.h src/nggcol_kernel.h src/pscol_kernel.h include/rdna4compute.h src/vadd_codeobj.h src/bench_codeobj.h
	@mkdir -p $(BUILD)
	$(CXX) -x c $(USER_FLAGS) userspace/rdna4-run.c userspace/librdna4.c \
		-framework IOKit -framework CoreFoundation -weak_framework Accelerate -o $@

userspace: $(RUN_TOOL)

# The card test of the Vulkan interface's memory half (docs/vulkan-port.md): runs on the test machine, as root.
N48N_PROBE := $(BUILD)/n48nprobe
$(N48N_PROBE): vulkan/n48nprobe.c vulkan/navi48_native_abi.h include/rdna4vulkan.h
	@mkdir -p $(BUILD)
	$(CXX) -x c $(USER_FLAGS) -Ivulkan vulkan/n48nprobe.c -framework IOKit -framework CoreFoundation -o $@

n48nprobe: $(N48N_PROBE)

# The Vulkan driver and its test program for the card tests (docs/todo-vulkantest.md): Mesa with the patches of
# vulkan/mesa-patches, fetched (about 150 MB, the first time) and built in $(BUILD)/radv-build, the library copied
# next to the other tools, and vkprobe built against Mesa's headers. `make clean` removes all of it.
RADV_DIR := $(BUILD)/radv-build
RADV_LIB := $(BUILD)/libvulkan_radeon.dylib
mesa:
	vulkan/build-mesa.sh $(RADV_DIR)
	cp $(RADV_DIR)/build/src/amd/vulkan/libvulkan_radeon.dylib $(RADV_LIB)
	$(CXX) -x c $(USER_FLAGS) -I$(RADV_DIR)/mesa/include vulkan/vkprobe.c -o $(BUILD)/vkprobe
	@otool -l $(RADV_LIB) | grep -A4 LC_BUILD_VERSION | grep -q 'minos $(DEPLOY)' || \
		{ echo "$(RADV_LIB) is not built for macOS $(DEPLOY)" >&2; exit 1; }
	@echo "Built $(RADV_LIB) and $(BUILD)/vkprobe for $(ARCH) (min macOS $(DEPLOY))"

# E1 trigger (docs/metal-spike.md): Objective-C, runs in Recovery next to rdna4-run.
CENSUS_TOOL := $(BUILD)/rdna4-census
$(CENSUS_TOOL): tools/accelcensus/census.m
	@mkdir -p $(BUILD)
	$(CXX) -x objective-c -fobjc-arc -arch $(ARCH) -target $(ARCH)-apple-macos$(DEPLOY) -isysroot $(SDK) \
		-mmacosx-version-min=$(DEPLOY) -std=gnu11 -O1 -Wall $< -framework Foundation -framework Metal -framework IOKit -o $@

census-tool: $(CENSUS_TOOL)

# E1c stub bundle (docs/metal-spike.md s.11.4): a Metal device bundle that only logs. MTLIOAccelDevice is private (exported by Metal), hence
# dynamic_lookup for the superclass reference.
CENSUS_STUB := $(BUILD)/RDNA4CensusMTLDriver.bundle
$(CENSUS_STUB): tools/accelcensus/stub/RDNA4CensusMTLDriver.m tools/accelcensus/stub/Info.plist
	@mkdir -p $@/Contents/MacOS
	$(CXX) -x objective-c -fno-objc-arc -arch $(ARCH) -target $(ARCH)-apple-macos$(DEPLOY) -isysroot $(SDK) \
		-mmacosx-version-min=$(DEPLOY) -std=gnu11 -O1 -Wall -bundle -undefined dynamic_lookup \
		tools/accelcensus/stub/RDNA4CensusMTLDriver.m -framework Foundation -o $@/Contents/MacOS/RDNA4CensusMTLDriver
	cp tools/accelcensus/stub/Info.plist $@/Contents/Info.plist

census-stub: $(CENSUS_STUB)

CENSUS_PATHLOG := $(BUILD)/libpathlog.dylib
$(CENSUS_PATHLOG): tools/accelcensus/pathlog.c
	@mkdir -p $(BUILD)
	$(CXX) -x c -arch $(ARCH) -target $(ARCH)-apple-macos$(DEPLOY) -isysroot $(SDK) -mmacosx-version-min=$(DEPLOY) -std=gnu11 -O1 -Wall \
		-dynamiclib $< -o $@

census-pathlog: $(CENSUS_PATHLOG)

check-isa:
	bash tools/check-isa.sh

atomdump: $(ATOMDUMP)

# Runs the kext's AtomBIOS parser (compiled for the host) against the real
# ROM dump — verifies parsing logic without GPU hardware.
PVSTREAM_TEST := $(BUILD)/pvstream-test
$(PVSTREAM_TEST): tools/pvstream-test.cpp src/pvstream.cpp src/pvstream.hpp src/pvopcodes.inc src/pvdevinfo.inc
	@mkdir -p $(BUILD)
	$(CXX) -std=c++17 -Wall -O1 -o $@ tools/pvstream-test.cpp src/pvstream.cpp

test: $(ATOMDUMP) $(PVSTREAM_TEST)
	$(ATOMDUMP) $(FIRMWARE)
	$(PVSTREAM_TEST)
	bash tools/check-isa.sh

$(LILU_STAMP): $(LILU)/../hde/hde32.h $(LILU)/../hde/hde64.h
	@mkdir -p $(LILU_SHIM)/Headers/capstone
	cp $(LILU)/../hde/hde32.h $(LILU)/../hde/hde64.h $(LILU_SHIM)/Headers/
	cp $(LILU)/../capstone/include/*.h $(LILU_SHIM)/Headers/capstone/
	@touch $@

$(BUILD)/%.o: %.cpp | $(LILU_STAMP)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: %.c | $(LILU_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# .incbin paths are relative to the repository root, where make runs.
$(BUILD)/src/fwblobs.o: src/fwblobs.S $(FW_BLOBS)
	@mkdir -p $(dir $@)
	$(CC) -arch $(ARCH) -target $(ARCH)-apple-macos$(DEPLOY) -c $< -o $@

# Ensure kmod_info.o is linked in the required position: user objects,
# then -lkmodc++, then kmod_info.o, then -lkmod.
KMOD_OBJ := $(BUILD)/src/kmod_info.o
USER_OBJS := $(filter-out $(KMOD_OBJ),$(OBJS))

$(EXEC): $(OBJS)
	@mkdir -p $(MACOS)
	$(CXX) $(LDFLAGS) $(USER_OBJS) $(KMOD_OBJ) -o $@

$(KEXT): $(EXEC) Info.plist
	@mkdir -p $(KEXT)/Contents
	cp Info.plist $(KEXT)/Contents/Info.plist
	@# AMD's firmware license travels with the binary that embeds its firmware.
	@if [ -n "$(ASM_SRCS)" ] && [ -f firmware/amdgpu/LICENSE.amdgpu ]; then \
		mkdir -p $(KEXT)/Contents/Resources && \
		cp firmware/amdgpu/LICENSE.amdgpu $(KEXT)/Contents/Resources/LICENSE.amdgpu; fi
	@# Minimal, ad-hoc code signature so kextutil is happier during testing.
	codesign --force --sign - $(KEXT) 2>/dev/null || true
	@echo "Built $(KEXT) for $(ARCH) (min macOS $(DEPLOY))"
	@command -v file >/dev/null && file $(EXEC) || true

clean:
	rm -rf $(BUILD)

# Header dependencies from -MMD.
-include $(OBJS:.o=.d)
