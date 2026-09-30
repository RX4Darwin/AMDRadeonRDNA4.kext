// replay-compute.cpp: a COMPUTE-ring submission of our own IB on the RX 9070 XT through amdgpu
// (libdrm_amdgpu, no root). Direct evidence for assumption U1 of docs/w13-vmid.md: amdgpu's kernel
// compute rings are VMID-0 HQDs (cp_hqd_vmid = 0, gfx_v12_0.c:3268, PRIV_STATE | KMD_QUEUE) whose jobs take
// their VMID from the INDIRECT_BUFFER packet (gfx_v12_0_ring_emit_ib_compute, :4520-4536). This submits the
// kext's vadd kernel (src/vadd_codeobj.h, the code object the card ran in stage 7 on 2026-09-28) as an
// unprivileged compute IB, recorded by the same builder rdna4-run's SubmitIb uses (userspace/pm4build.h),
// into a per-process VMID, and checks c[i] = a[i] + 3 * b[i] in memory the CPU can read.
//
//   result 0 = all results right  -> a VMID-0 MEC queue runs an unprivileged IB in the process VMID on this card
//   result 2 = wrong results      -> the IB ran but the data is wrong (look at the dump)
//   result 1 = error / fence not done in 5 s (context reset state is printed)
//
// What it does NOT show: which HQD/VMID the job used (that needs the ring/HQD decode; ring-capture.sh can
// dump amdgpu_ring_comp_1.0.0 with sudo), nor anything about the kext's own queue programming. The kext
// is not involved.
//
// Environment:
//   REPLAY_COMPUTE_OK=1      required. A new kind of GPU work on the user's desktop GPU: it needs the reviewer's
//                            go-ahead first (same gate as REPLAY_COL_OK for G4).
//   REPLAY_COMPUTE_COUNT=n   submit n times back to back (default 1, at most 64), each verified.
//
// Build and run: REPLAY_IP=compute REPLAY_COMPUTE_OK=1 tools/linux-replay/run.sh

#include "vadd_codeobj.h"     // src/
#include "codeobj.hpp"        // src/ (compiled in: replay.cpp builds without it, see run.sh)
#include "pm4build.h"         // userspace/

#include <amdgpu.h>
#include <amdgpu_drm.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>

#define CHECK(x) do { int r_ = (x); if (r_) { fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)

struct Buf {
	amdgpu_bo_handle bo;
	amdgpu_va_handle vah;
	uint64_t va;
	uint64_t size;
	void *cpu;
};

static amdgpu_device_handle dev;

static Buf alloc(uint64_t size, uint64_t align, uint32_t domain) {
	Buf b = {};
	b.size = size;
	amdgpu_bo_alloc_request req = {};
	req.alloc_size = size;
	req.phys_alignment = align;
	req.preferred_heap = domain;
	CHECK(amdgpu_bo_alloc(dev, &req, &b.bo));
	CHECK(amdgpu_va_range_alloc(dev, amdgpu_gpu_va_range_general, size, align, 0, &b.va, &b.vah, 0));
	CHECK(amdgpu_bo_va_op(b.bo, 0, size, b.va, 0, AMDGPU_VA_OP_MAP));
	CHECK(amdgpu_bo_cpu_map(b.bo, &b.cpu));
	memset(b.cpu, 0, size);
	return b;
}

// The vadd IB exactly as userspace/rdna4-run.c recordVaddIb builds it (compute IBs carry no fence: the
// kernel's own end-of-pipe fence follows the IB on the ring).
static uint32_t recordVaddIb(uint32_t *ib, uint64_t codeVa, const CodeObj::Kernel &k, uint64_t kernarg, uint32_t groups) {
	uint32_t n = 0;
	const uint32_t zero = 0, all[2] = { 0xffffffffu, 0xffffffffu };
	const uint32_t none4[4] = { 0, 0, 0, 0 }, start[3] = { 0, 0, 0 };
	const uint32_t threads[3] = { 64, 1, 1 };
	const uint32_t pgm[2] = { (uint32_t)(codeVa >> 8), (uint32_t)(codeVa >> 40) };
	const uint32_t rsrc[2] = { k.rsrc1, k.rsrc2 };
	const uint32_t rsrc3 = k.rsrc3;
	const uint32_t user[2] = { (uint32_t)kernarg, (uint32_t)(kernarg >> 32) };
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
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_NUM_THREAD_X, threads, 3);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_USER_DATA0, user, 2);
	n += rdna4_pm4_dispatch_direct(ib + n, groups, 1, 1,
	                               RDNA4_PM4_DISPATCH_SHADER_EN | RDNA4_PM4_DISPATCH_FORCE_START0 |
	                               (k.wave32() ? RDNA4_PM4_DISPATCH_WAVE32 : 0));
	while (n & 7)                                  // pad the IB to 8 dwords with the one-dword PKT3 NOP (PKT3(NOP, 0x3fff))
		ib[n++] = 0xffff1000u;
	return n;
}

int main() {
	const char *ok = getenv("REPLAY_COMPUTE_OK");
	if (!ok || strcmp(ok, "1")) {
		fprintf(stderr, "replay-compute: a compute-ring submission is a new kind of work on the desktop GPU; it needs the\n"
		                "reviewer's go-ahead. Set REPLAY_COMPUTE_OK=1 once that is given (docs/w13-vmid.md section 8, U1).\n");
		return 1;
	}
	const uint32_t count = getenv("REPLAY_COMPUTE_COUNT") ? strtoul(getenv("REPLAY_COMPUTE_COUNT"), nullptr, 0) : 1;
	if (count < 1 || count > 64) { fprintf(stderr, "REPLAY_COMPUTE_COUNT is 1..64\n"); return 1; }

	const uint8_t *elf = kVaddCodeObject;
	const size_t elfSize = sizeof(kVaddCodeObject);
	CodeObj::Kernel k;
	CodeObj::Image img;
	const char *why = nullptr;
	if (!CodeObj::findKernel(elf, elfSize, "vadd", k, &why) || !CodeObj::parseImage(elf, elfSize, img, &why)) {
		fprintf(stderr, "vadd code object: %s\n", why ? why : "?");
		return 1;
	}
	if (!k.wantsKernargPtr() || k.userSgprCount() != 2 || k.privateSegmentSize || k.groupSegmentSize) {
		fprintf(stderr, "vadd does not have the expected shape (kernarg pointer only, no scratch, no LDS)\n");
		return 1;
	}

	int fd = -1;
	for (int m = 128; m < 136 && fd < 0; m++) {
		char path[32];
		snprintf(path, sizeof(path), "/dev/dri/renderD%d", m);
		int f = open(path, O_RDWR | O_CLOEXEC);
		if (f < 0)
			continue;
		drmVersionPtr v = drmGetVersion(f);
		if (v && !strcmp(v->name, "amdgpu"))
			fd = f;
		else
			close(f);
		if (v)
			drmFreeVersion(v);
	}
	if (fd < 0) { fprintf(stderr, "no amdgpu render node\n"); return 1; }
	uint32_t major, minor;
	CHECK(amdgpu_device_initialize(fd, &major, &minor, &dev));
	amdgpu_gpu_info info;
	CHECK(amdgpu_query_gpu_info(dev, &info));
	printf("amdgpu %u.%u, family %u\n", major, minor, info.family_id);

	amdgpu_context_handle ctx;
	CHECK(amdgpu_cs_ctx_create(dev, &ctx));

	// The code image at a 256-byte-aligned base (the kext's rtLoad does the same), the data and kernarg in one
	// CPU-visible buffer, the IB in another. GTT everywhere: the CPU reads the results back directly.
	const uint64_t imgBytes = (img.size + 0xfff) & ~0xfffull;
	Buf code = alloc(imgBytes, 0x1000, AMDGPU_GEM_DOMAIN_GTT);
	for (uint32_t i = 0; i < img.count; i++)
		memcpy(static_cast<char *>(code.cpu) + img.seg[i].vaddr, elf + img.seg[i].fileOffset, img.seg[i].fileSize);
	Buf data = alloc(0x4000, 0x1000, AMDGPU_GEM_DOMAIN_GTT);          // a 0, b 0x1000, c 0x2000, kernarg 0x3000
	Buf ibuf = alloc(0x1000, 0x1000, AMDGPU_GEM_DOMAIN_GTT);
	uint32_t *a = static_cast<uint32_t *>(data.cpu);
	uint32_t *b = a + 0x400, *c = a + 0x800;
	uint64_t *args = reinterpret_cast<uint64_t *>(static_cast<char *>(data.cpu) + 0x3000);
	const uint32_t items = 256;
	args[0] = data.va;
	args[1] = data.va + 0x1000;
	args[2] = data.va + 0x2000;

	const uint64_t codeVa = code.va + k.entryVa;
	const uint32_t ibDw = recordVaddIb(static_cast<uint32_t *>(ibuf.cpu), codeVa, k, data.va + 0x3000, items / 64);
	printf("vadd at 0x%llx (image %llu bytes), kernarg 0x%llx, IB 0x%llx, %u dwords, rsrc1 0x%08x rsrc2 0x%08x rsrc3 0x%08x%s\n",
	       (unsigned long long)codeVa, (unsigned long long)img.size, (unsigned long long)(data.va + 0x3000),
	       (unsigned long long)ibuf.va, ibDw, k.rsrc1, k.rsrc2, k.rsrc3, k.wave32() ? " wave32" : " wave64");

	amdgpu_bo_handle bos[3] = { code.bo, data.bo, ibuf.bo };
	amdgpu_bo_list_handle list;
	CHECK(amdgpu_bo_list_create(dev, 3, bos, nullptr, &list));

	int status = 0;
	for (uint32_t round = 0; round < count; round++) {
		for (uint32_t i = 0; i < items; i++) {
			a[i] = i * 7u + 1u + round;
			b[i] = (i ^ 0x55u) + round;
			c[i] = 0xdeadbeefu;
		}
		amdgpu_cs_ib_info ibi = {};
		ibi.ib_mc_address = ibuf.va;
		ibi.size = ibDw;
		amdgpu_cs_request req = {};
		req.ip_type = AMDGPU_HW_IP_COMPUTE;       // the kernel's compute ring, not gfx
		req.ip_instance = 0;
		req.ring = 0;
		req.resources = list;
		req.number_of_ibs = 1;
		req.ibs = &ibi;
		CHECK(amdgpu_cs_submit(ctx, 0, &req, 1));

		amdgpu_cs_fence fence = {};
		fence.context = ctx;
		fence.ip_type = AMDGPU_HW_IP_COMPUTE;
		fence.ip_instance = 0;
		fence.ring = 0;
		fence.fence = req.seq_no;
		uint32_t expired = 0;
		int r = amdgpu_cs_query_fence_status(&fence, 5000000000ull, 0, &expired);
		uint32_t state = 0, hangs = 0;
		amdgpu_cs_query_reset_state(ctx, &state, &hangs);
		printf("round %u: seq %llu %s (query %d), context reset state %u\n", round, (unsigned long long)req.seq_no,
		       expired ? "done" : "NOT done in 5 s", r, state);
		if (!expired || state) {
			status = 1;
			break;                                // never retry a hung submission
		}
		uint32_t bad = 0, first = ~0u;
		for (uint32_t i = 0; i < items; i++) {
			if (c[i] != a[i] + 3u * b[i]) {
				bad++;
				if (first == ~0u)
					first = i;
			}
		}
		if (bad) {
			printf("round %u: %u of %u results wrong; first at %u: got 0x%08x want 0x%08x\n", round, bad, items, first,
			       c[first], a[first] + 3u * b[first]);
			status = 2;
		} else {
			printf("round %u: all %u results right\n", round, items);
		}
	}
	printf("%s\n", status == 0 ? "RESULT: a compute-ring IB ran in a per-process VMID and produced the right data"
	                           : "RESULT: not proven (see above)");
	return status;
}
