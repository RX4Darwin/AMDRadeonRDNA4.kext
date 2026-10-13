# src/kext.mk — the kext's objects and its executable.
#
# Run by the top-level Makefile, never by hand: with the object directory as
# the working directory, ROOT the way from there to the top of the repository,
# and the configuration (SDK, CXX, CC, PRODUCT, VERSION, ARCH, DEPLOY, CONFIG,
# VMTEST, LILU_SHIM) on the command line. buildrev.h and fwconfig.h are already
# in the object directory.
#
# Same language rules as the Makefile (GNU make and BSD make): the sources are
# found through VPATH and compiled by suffix rules, which is the one way both
# makes build objects away from their sources.

.SUFFIXES:
.SUFFIXES: .cpp .c .o

LILU      = $(ROOT)/lib/Lilu/Lilu
VPATH     = $(ROOT)/src:$(LILU)/Library

# VM test builds switch modes on QEMU's vmware-svga through Bochs VBE.
VMTEST_OBJS_0  =
VMTEST_OBJS_1  = bochsvbe.o
VMTEST_FLAGS_0 =
VMTEST_FLAGS_1 = -DAMDRadeonRDNA4_VM_TEST

# RELEASE is what has run on the card. DEBUG has not, as of 0.1.0: no optimisation (larger stack frames), and
# DEBUG defined, which turns Lilu's DBGLOG lines on.
OPT_RELEASE    = -Os
OPT_DEBUG      = -O0 -DDEBUG

# In link order. plugin_start.cpp is Lilu's (lib/Lilu/Lilu/Library); fwblobs.o is the firmware, or nothing.
USER_OBJS = plugin.o device.o cursor.o ndrv.o modeset.o pipe2.o dptrain.o dpphy.o compute.o gfxring.o pmidle.o \
            flip.o amdfw.o psp.o sdma.o ih.o ihdecode.o pm4.o codeobj.o gpuheap.o gpuvm.o vmtree.o n48n.o \
            n48nkext.o vmid.o ptpages.o gpuvmtable.o runtime.o vmtest.o vmshared.o userclient.o accelcensus.o \
            pvgpu.o pvstream.o plugin_start.o atombios.o ipdiscovery.o edid.o otgtiming.o modes.o pipe.o \
            $(VMTEST_OBJS_$(VMTEST)) fwblobs.o
# kmod_info.o is linked after the other objects.
OBJS      = $(USER_OBJS) kmod_info.o

TARGET    = -arch $(ARCH) -target $(ARCH)-apple-macos$(DEPLOY)

COMMON_FLAGS = \
	$(TARGET) \
	-isysroot $(SDK) \
	-I$(ROOT)/lib/MacKernelSDK/Headers \
	-I$(ROOT)/include \
	-I$(LILU_SHIM) \
	-I$(LILU) \
	-I. \
	-mmacosx-version-min=$(DEPLOY) \
	-DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
	-D_FORTIFY_SOURCE=0 \
	-DPRODUCT_NAME=$(PRODUCT) -DMODULE_VERSION=$(VERSION) \
	-nostdinc \
	-fno-builtin -fno-common -fno-stack-protector -mkernel -fapple-kext \
	-fno-asynchronous-unwind-tables \
	-Wall -Wno-vla -Wno-unknown-warning-option -Wno-ossharedptr-misuse $(OPT_$(CONFIG)) -g \
	-MMD -MP

# -fno-c++-static-destructors: at -Os clang can lower the gMetaClass static
# object's destructor into an __cxa_atexit registration, which the kernel
# does not export — kmutil then fails to link ("could not find a kext which
# exports ___cxa_atexit"). Kext teardown is handled by the kmod _stop path,
# so static destructors are never wanted here.
# fwconfig.h defines AMDRadeonRDNA4_NO_FIRMWARE when the firmware is not there.
CXXFLAGS  = $(COMMON_FLAGS) -std=c++17 -fno-exceptions -fno-rtti -fcheck-new \
            -fno-c++-static-destructors -include fwconfig.h $(VMTEST_FLAGS_$(VMTEST))
CFLAGS    = $(COMMON_FLAGS)

LDFLAGS   = \
	$(TARGET) \
	-isysroot $(SDK) \
	-nostdlib \
	-Xlinker -kext \
	-L$(ROOT)/lib/MacKernelSDK/Library/$(ARCH) \
	-lkmodc++ -lkmod \
	-Wl,-no_deduplicate

$(PRODUCT): $(OBJS)
	$(CXX) $(LDFLAGS) $(OBJS) -o $@

.cpp.o:
	$(CXX) $(CXXFLAGS) -c $< -o $@

.c.o:
	$(CC) $(CFLAGS) -c $< -o $@

# kmod_info.o carries the version (MODULE_VERSION): it follows the header so that a new version reaches it.
kmod_info.o: buildrev.h

# The firmware, embedded by the assembler (.incbin paths are relative to the top of the repository), or an object
# with nothing in it when the firmware is not there.
fwblobs.o: $(ROOT)/src/fwblobs.S fwconfig.h
	@if grep -q NO_FIRMWARE fwconfig.h; then \
		echo "no firmware in firmware/amdgpu (tools/build/fetch-firmware.sh): the kext is built without it"; \
		echo "" | $(CC) $(TARGET) -x c -c - -o $@; \
	else \
		echo "$(CC) $(TARGET) -I$(ROOT) -c $(ROOT)/src/fwblobs.S -o $@"; \
		$(CC) $(TARGET) -I$(ROOT) -c $(ROOT)/src/fwblobs.S -o $@; \
	fi

# Header dependencies from -MMD.
-include $(OBJS:.o=.d)
