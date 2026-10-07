// air-run: launch ONE compute kernel from a gfx1201 code object on the RX 9070 XT through amdgpu (libdrm_amdgpu, no root) and dump its buffers.
// A generalisation of tools/linux-replay/replay-compute.cpp (the vadd replay): same compute-ring submission of our own IB, same PM4 builder
// (userspace/pm4build.h), same code-object reader (src/codeobj.cpp); the kernel name, grid, work-group size, kernargs and LDS are parameters.
// Used by tools/air/test-kernels.py to run kernels lowered from Apple AIR (docs/m3-air-spike.md). RUNS ON THE USER'S GPU: it refuses to start
// without REPLAY_COMPUTE_OK=1, which means the reviewer has said go.
//
//   air-run KERNEL.hsaco NAME --groups GX,GY,GZ --tg TX,TY,TZ --out DIR ARG...
//   ARG: buf:FILE   a GPU buffer holding FILE's bytes (zero padded to 4 KiB); after the run DIR/buf<N>.bin holds its contents (N = buffer index)
//        zero:BYTES a zero-filled GPU buffer of that size (same dump)
//        u32:N      a 32-bit kernarg
//   Kernargs are laid out in ARG order (buffers 8-byte aligned pointers, u32 4 bytes), as the lowered kernels declare them.
//
// Exit status: 0 = the dispatch completed (the data is NOT checked here), 1 = error or the fence did not signal in 5 s (never retried).
#include "codeobj.hpp"
#include "pm4build.h"

#include <amdgpu.h>
#include <amdgpu_drm.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unistd.h>
#include <vector>
#include <xf86drm.h>

#define CHECK(x) do { int r_ = (x); if (r_) { fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)

struct Buf {
	amdgpu_bo_handle bo;
	amdgpu_va_handle vah;
	uint64_t va, size;
	void *cpu;
};

static amdgpu_device_handle dev;

static Buf alloc(uint64_t size, uint64_t align) {
	Buf b = {};
	size = (size + 0xfff) & ~0xfffull;
	b.size = size;
	amdgpu_bo_alloc_request req = {};
	req.alloc_size = size;
	req.phys_alignment = align;
	req.preferred_heap = AMDGPU_GEM_DOMAIN_GTT;
	CHECK(amdgpu_bo_alloc(dev, &req, &b.bo));
	CHECK(amdgpu_va_range_alloc(dev, amdgpu_gpu_va_range_general, size, align, 0, &b.va, &b.vah, 0));
	CHECK(amdgpu_bo_va_op(b.bo, 0, size, b.va, 0, AMDGPU_VA_OP_MAP));
	CHECK(amdgpu_bo_cpu_map(b.bo, &b.cpu));
	memset(b.cpu, 0, size);
	return b;
}

static bool readFile(const char *path, std::vector<uint8_t> &v) {
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); return false; }
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	v.resize(n > 0 ? (size_t)n : 0);
	bool ok = v.empty() || fread(v.data(), 1, v.size(), f) == v.size();
	fclose(f);
	return ok;
}

// COMPUTE_PGM_RSRC2.LDS_SIZE [23:15]: 1 KiB allocation granule, 512-byte units (src/gfxregs.hpp ldsSizeField; Mesa ac_shader_encode_lds_size, gfx12).
static uint32_t ldsField(uint32_t bytes) { return ((bytes + 1023) / 1024 * 1024 / 512) << 15; }
static const uint32_t kRsrc2LdsMask = 0x1ffu << 15;

int main(int argc, char **argv) {
	const char *ok = getenv("REPLAY_COMPUTE_OK");
	if (!ok || strcmp(ok, "1")) {
		fprintf(stderr, "air-run: a compute-ring submission runs on the desktop GPU; it needs the reviewer's go-ahead. Set REPLAY_COMPUTE_OK=1 once given.\n");
		return 1;
	}
	if (argc < 4) { fprintf(stderr, "usage: air-run KERNEL.hsaco NAME --groups GX,GY,GZ --tg TX,TY,TZ --out DIR ARG...\n"); return 1; }
	const char *hsaco = argv[1], *name = argv[2];
	uint32_t groups[3] = {1, 1, 1}, tg[3] = {64, 1, 1};
	std::string outDir = ".";
	struct Arg { char kind; std::string v; };
	std::vector<Arg> args;
	for (int i = 3; i < argc; i++) {
		std::string a = argv[i];
		if (a == "--groups" && i + 1 < argc) sscanf(argv[++i], "%u,%u,%u", &groups[0], &groups[1], &groups[2]);
		else if (a == "--tg" && i + 1 < argc) sscanf(argv[++i], "%u,%u,%u", &tg[0], &tg[1], &tg[2]);
		else if (a == "--out" && i + 1 < argc) outDir = argv[++i];
		else if (a.rfind("buf:", 0) == 0) args.push_back({'b', a.substr(4)});
		else if (a.rfind("zero:", 0) == 0) args.push_back({'z', a.substr(5)});
		else if (a.rfind("u32:", 0) == 0) args.push_back({'u', a.substr(4)});
		else { fprintf(stderr, "bad argument %s\n", a.c_str()); return 1; }
	}
	if (tg[0] * tg[1] * tg[2] == 0 || tg[0] * tg[1] * tg[2] > 1024 || !groups[0] || !groups[1] || !groups[2]) { fprintf(stderr, "bad --tg/--groups\n"); return 1; }

	std::vector<uint8_t> elf;
	if (!readFile(hsaco, elf)) return 1;
	CodeObj::Kernel k;
	CodeObj::Image img;
	const char *why = nullptr;
	if (!CodeObj::findKernel(elf.data(), elf.size(), name, k, &why) || !CodeObj::parseImage(elf.data(), elf.size(), img, &why)) {
		fprintf(stderr, "%s: %s\n", hsaco, why ? why : "?");
		return 1;
	}
	if (!k.wantsKernargPtr() || k.userSgprCount() != 2 || k.privateSegmentSize) {
		fprintf(stderr, "%s: unexpected shape (want: kernarg pointer only as user SGPRs, no scratch): user SGPRs %u, kernarg ptr %d, scratch %u\n", name,
		        k.userSgprCount(), k.wantsKernargPtr(), k.privateSegmentSize);
		return 1;
	}

	int fd = -1;
	for (int m = 128; m < 136 && fd < 0; m++) {
		char path[32];
		snprintf(path, sizeof(path), "/dev/dri/renderD%d", m);
		int f = open(path, O_RDWR | O_CLOEXEC);
		if (f < 0) continue;
		drmVersionPtr v = drmGetVersion(f);
		if (v && !strcmp(v->name, "amdgpu")) fd = f; else close(f);
		if (v) drmFreeVersion(v);
	}
	if (fd < 0) { fprintf(stderr, "no amdgpu render node\n"); return 1; }
	uint32_t major, minor;
	CHECK(amdgpu_device_initialize(fd, &major, &minor, &dev));
	amdgpu_context_handle ctx;
	CHECK(amdgpu_cs_ctx_create(dev, &ctx));

	const uint64_t imgBytes = (img.size + 0xfff) & ~0xfffull;
	Buf code = alloc(imgBytes, 0x1000);
	for (uint32_t i = 0; i < img.count; i++)
		memcpy((char *)code.cpu + img.seg[i].vaddr, elf.data() + img.seg[i].fileOffset, img.seg[i].fileSize);
	Buf kern = alloc(0x1000, 0x1000);
	Buf ibuf = alloc(0x1000, 0x1000);
	std::vector<Buf> bufs;
	std::vector<int> bufIndex;       // arg index of each buffer
	uint32_t off = 0;
	for (size_t i = 0; i < args.size(); i++) {
		const Arg &a = args[i];
		if (a.kind == 'u') {
			off = (off + 3) & ~3u;
			const uint32_t v = (uint32_t)strtoul(a.v.c_str(), nullptr, 0);
			memcpy((char *)kern.cpu + off, &v, 4);
			off += 4;
			continue;
		}
		uint64_t size = 0;
		std::vector<uint8_t> data;
		if (a.kind == 'b') {
			if (!readFile(a.v.c_str(), data)) return 1;
			size = data.size();
		} else {
			size = strtoull(a.v.c_str(), nullptr, 0);
		}
		Buf b = alloc(size ? size : 4096, 0x1000);
		if (!data.empty()) memcpy(b.cpu, data.data(), data.size());
		off = (off + 7) & ~7u;
		memcpy((char *)kern.cpu + off, &b.va, 8);
		off += 8;
		bufs.push_back(b);
		bufIndex.push_back((int)i);
		if (off > 0x800) { fprintf(stderr, "kernarg block too large\n"); return 1; }
	}
	if (off > k.kernargSize + 0)
		fprintf(stderr, "note: kernarg bytes given %u, the kernel declares %u explicit+hidden bytes\n", off, k.kernargSize);

	// IB: exactly the replay-compute recording, with the work-group size, grid and LDS as parameters.
	uint32_t *ib = (uint32_t *)ibuf.cpu;
	uint32_t n = 0;
	const uint32_t zero = 0, all[2] = {0xffffffffu, 0xffffffffu}, none4[4] = {0, 0, 0, 0}, start[3] = {0, 0, 0};
	const uint64_t codeVa = code.va + k.entryVa;
	const uint32_t pgm[2] = {(uint32_t)(codeVa >> 8), (uint32_t)(codeVa >> 40)};
	const uint32_t rsrc[2] = {k.rsrc1, (k.rsrc2 & ~kRsrc2LdsMask) | ldsField(k.groupSegmentSize)};
	const uint32_t rsrc3 = k.rsrc3;
	const uint32_t user[2] = {(uint32_t)kern.va, (uint32_t)(kern.va >> 32)};
	n += rdna4_pm4_acquire_mem(ib + n, RDNA4_PM4_GCR_MEM_SYNC);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_PGM_LO, pgm, 2);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_PGM_RSRC1, rsrc, 2);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_PGM_RSRC3, &rsrc3, 1);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_RESOURCE_LIM, &zero, 1);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_TMPRING, &zero, 1);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_THREAD_SE0, all, 2);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_THREAD_SE2, all, 2);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_THREAD_SE4, none4, 4);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_START_X, start, 3);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_NUM_THREAD_X, tg, 3);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_USER_DATA0, user, 2);
	n += rdna4_pm4_dispatch_direct(ib + n, groups[0], groups[1], groups[2],
	                               RDNA4_PM4_DISPATCH_SHADER_EN | RDNA4_PM4_DISPATCH_FORCE_START0 | (k.wave32() ? RDNA4_PM4_DISPATCH_WAVE32 : 0));
	while (n & 7)
		ib[n++] = 0xffff1000u;
	printf("%s: code 0x%llx rsrc1 0x%08x rsrc2 0x%08x rsrc3 0x%08x LDS %u B, kernarg 0x%llx (%u B used), groups %u,%u,%u tg %u,%u,%u, IB %u dwords%s\n", name,
	       (unsigned long long)codeVa, rsrc[0], rsrc[1], rsrc3, k.groupSegmentSize, (unsigned long long)kern.va, off, groups[0], groups[1], groups[2], tg[0],
	       tg[1], tg[2], n, k.wave32() ? " wave32" : " wave64");

	std::vector<amdgpu_bo_handle> bos = {code.bo, kern.bo, ibuf.bo};
	for (Buf &b : bufs) bos.push_back(b.bo);
	amdgpu_bo_list_handle list;
	CHECK(amdgpu_bo_list_create(dev, bos.size(), bos.data(), nullptr, &list));
	amdgpu_cs_ib_info ibi = {};
	ibi.ib_mc_address = ibuf.va;
	ibi.size = n;
	amdgpu_cs_request req = {};
	req.ip_type = AMDGPU_HW_IP_COMPUTE;
	req.resources = list;
	req.number_of_ibs = 1;
	req.ibs = &ibi;
	CHECK(amdgpu_cs_submit(ctx, 0, &req, 1));
	amdgpu_cs_fence fence = {};
	fence.context = ctx;
	fence.ip_type = AMDGPU_HW_IP_COMPUTE;
	fence.fence = req.seq_no;
	uint32_t expired = 0;
	int r = amdgpu_cs_query_fence_status(&fence, 5000000000ull, 0, &expired);
	uint32_t state = 0, hangs = 0;
	amdgpu_cs_query_reset_state(ctx, &state, &hangs);
	printf("seq %llu %s (query %d), context reset state %u\n", (unsigned long long)req.seq_no, expired ? "done" : "NOT done in 5 s", r, state);
	if (!expired || state) return 1;
	for (size_t i = 0; i < bufs.size(); i++) {
		char path[512];
		snprintf(path, sizeof(path), "%s/buf%d.bin", outDir.c_str(), bufIndex[i]);
		FILE *f = fopen(path, "wb");
		if (!f) { perror(path); return 1; }
		fwrite(bufs[i].cpu, 1, bufs[i].size, f);
		fclose(f);
	}
	printf("dumped %zu buffer(s) to %s\n", bufs.size(), outDir.c_str());
	return 0;
}
