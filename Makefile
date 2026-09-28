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
	src/ndrv.cpp \
	src/modeset.cpp \
	src/compute.cpp \
	src/amdfw.cpp \
	src/psp.cpp \
	src/sdma.cpp \
	src/ih.cpp \
	src/ihdecode.cpp \
	src/pm4.cpp \
	src/codeobj.cpp \
	src/gpuheap.cpp \
	src/runtime.cpp \
	src/userclient.cpp \
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
.PHONY: all clean test userspace
all: $(KEXT) $(RUN_TOOL)

$(ATOMDUMP): tools/atomdump.cpp src/atombios.cpp src/atombios.hpp src/ipdiscovery.cpp src/ipdiscovery.hpp src/edid.cpp src/edid.hpp src/otgtiming.cpp src/otgtiming.hpp src/modes.cpp src/modes.hpp src/dmub.hpp src/pipe.cpp src/pipe.hpp src/ndrv.cpp src/ndrv.hpp src/modeset.cpp src/modeset.hpp src/amdfw.cpp src/amdfw.hpp src/psp.cpp src/psp.hpp src/sdma.cpp src/sdma.hpp src/ihdecode.cpp src/ih.hpp src/pm4.cpp src/pm4.hpp src/codeobj.cpp src/codeobj.hpp src/vadd_codeobj.h src/bench_codeobj.h src/gfxregs.hpp src/gpuheap.cpp src/gpuheap.hpp include/rdna4compute.h
	@mkdir -p $(BUILD)
	$(CXX) -std=c++17 -Wall -O2 -Iinclude -o $@ tools/atomdump.cpp src/atombios.cpp src/ipdiscovery.cpp src/edid.cpp src/otgtiming.cpp src/modes.cpp src/pipe.cpp src/ndrv.cpp src/modeset.cpp src/amdfw.cpp src/psp.cpp src/sdma.cpp src/ihdecode.cpp src/pm4.cpp src/codeobj.cpp src/gpuheap.cpp

# Linked by the C++ driver: it is the one pointed at ld64 (build-osxcross.sh).
$(RUN_TOOL): userspace/rdna4-run.c userspace/librdna4.c userspace/librdna4.h include/rdna4compute.h src/vadd_codeobj.h src/bench_codeobj.h
	@mkdir -p $(BUILD)
	$(CXX) -x c $(USER_FLAGS) userspace/rdna4-run.c userspace/librdna4.c \
		-framework IOKit -framework CoreFoundation -weak_framework Accelerate -o $@

userspace: $(RUN_TOOL)

atomdump: $(ATOMDUMP)

# Runs the kext's AtomBIOS parser (compiled for the host) against the real
# ROM dump — verifies parsing logic without GPU hardware.
test: $(ATOMDUMP)
	$(ATOMDUMP) $(FIRMWARE)

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
