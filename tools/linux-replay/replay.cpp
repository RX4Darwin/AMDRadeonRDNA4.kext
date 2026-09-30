// replay.cpp: the kext's G3 draw stream (src/gfx12_draw.h) and its shaders, submitted
// unchanged to the RX 9070 XT through amdgpu on Linux (libdrm_amdgpu, no root).
//
// The kext runs this stream on a bare gfx ring it brought up itself (VMID0, RB0 without
// MQD/HQD, rings in its device heap) and gets C_INVOCATIONS 1, C_PRIMITIVES 0, 0 pixels.
// Here the same bytes run on amdgpu's own gfx queue, in a normal per-process VMID:
//   8192 px  -> the stream and shaders are right; the bug is in the kext's queue/memory setup;
//   0 px     -> the stream itself is wrong; bisect it against RADV (tools/radv-triangle).
//
// It does what gfxring.cpp gfxDrawRun does for the baseline draw: shaders padded with
// s_code_end, relocations (va >> shift) & mask, the target cleared, PIPELINESTAT_START +
// SAMPLE_PIPELINESTAT around the stream, then counts the target.
//
// Environment:
//   REPLAY_VS=new|old|file:<hex>  new (default) = the committed src/ngg_kernel.h,
//                        old = the round-6 ngg.s (edc5f81, s_and_saveexec: 0 px on the card)
//                        REPLAY_VS=file: wins over the shader a variant would place (4096 with file: keeps the file's shader
//                        and only applies 4096's register patches), unlike the kext, where the variant picks the shader.
//   REPLAY_VARIANT=<n>   the kext's ladder patches; implemented: 1, 2, 128, 256, 2048, 4096 (= wave64 ngg64.s + GS_W32_EN 0 +
//                        VGPRS 2). Any other bit (4, 8, 32, 64, 512, 1024, ...) is rejected. Bits are applied one at a time as in
//                        the kext's ladder; do not combine 4096 with the marker shaders.
//   REPLAY_SET=...       extra register writes before the draw (see below).
// Exit status: 0 = the triangle is right (8192 px of the expected colour and no other pixel), 2 = wrong image, 1 = error
// (including a patch whose register is not in the stream).
//
// Build and run: tools/linux-replay/run.sh. Findings: docs/linux-replay.md

#include "gfx12tri.h"         // userspace/: the shared builder rdna4-run tri uses (gfx12_draw.h, ngg, psred)
#include "ngg_old_kernel.h"
#include "ngg64_kernel.h"

#include <amdgpu.h>
#include <amdgpu_drm.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>

using namespace Gfx12Draw;

#define CHECK(x) do { int r_ = (x); if (r_) { fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)

struct Buf {
	amdgpu_bo_handle bo;
	amdgpu_va_handle vah;
	uint64_t va;
	uint64_t size;
	void *cpu;
};

static amdgpu_device_handle dev;

static Buf alloc(uint64_t size, uint64_t align, uint32_t domain, uint64_t flags, bool map = true) {
	Buf b = {};
	b.size = size;
	amdgpu_bo_alloc_request req = {};
	req.alloc_size = size;
	req.phys_alignment = align;
	req.preferred_heap = domain;
	req.flags = flags;
	CHECK(amdgpu_bo_alloc(dev, &req, &b.bo));
	CHECK(amdgpu_va_range_alloc(dev, amdgpu_gpu_va_range_general, size, align, 0, &b.va, &b.vah, 0));
	CHECK(amdgpu_bo_va_op(b.bo, 0, size, b.va, 0, AMDGPU_VA_OP_MAP));
	if (map) {
		CHECK(amdgpu_bo_cpu_map(b.bo, &b.cpu));
		memset(b.cpu, 0, size);
	}
	return b;
}

// The dword index of the value of register `reg` written by packet `opcode` in the stream
// (gfxring.cpp findStreamReg): walk the type-3 packets, match SET_*_REG ranges.
static int findStreamReg(const uint32_t *s, uint32_t n, uint32_t opcode, uint32_t reg) {
	for (uint32_t i = 0; i < n;) {
		const uint32_t hdr = s[i];
		const uint32_t count = ((hdr >> 16) & 0x3fff) + 1;
		const uint32_t op = (hdr >> 8) & 0xff;
		if (op == opcode && count >= 2) {
			const uint32_t first = s[i + 1] & 0xffff;
			if (reg >= first && reg < first + count - 1)
				return static_cast<int>(i + 2 + (reg - first));
		}
		i += 1 + count;
	}
	return -1;
}

int main() {
	const char *vsSel = getenv("REPLAY_VS") ? getenv("REPLAY_VS") : "new";
	const uint32_t variant = getenv("REPLAY_VARIANT") ? strtoul(getenv("REPLAY_VARIANT"), nullptr, 0) : 0;
	if (variant & ~(1u | 2u | 128u | 256u | 2048u | 4096u)) {
		fprintf(stderr, "REPLAY_VARIANT 0x%x: unsupported bits 0x%x (implemented: 1, 2, 128, 256, 2048, 4096)\n", variant,
		        variant & ~(1u | 2u | 128u | 256u | 2048u | 4096u));
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
	printf("amdgpu %u.%u, family %u, %u SEs\n", major, minor, info.family_id, info.num_shader_engines);
	if (info.num_shader_engines > kMaxSe) { fprintf(stderr, "rings sized for %u SEs\n", kMaxSe); return 1; }

	amdgpu_context_handle ctx;
	CHECK(amdgpu_cs_ctx_create(dev, &ctx));

	const uint64_t vramCpu = AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;
	Buf code = alloc(0x2000, 0x1000, AMDGPU_GEM_DOMAIN_VRAM, vramCpu);                 // VS at 0, PS at 0x1000
	Buf target = alloc(kWidth * kHeight * 4, 0x1000, AMDGPU_GEM_DOMAIN_VRAM, vramCpu);
	Buf rings = alloc(kRingBytes, 2ull << 20, AMDGPU_GEM_DOMAIN_VRAM, 0, false);              // Mesa aligns the block to 2 MiB
	Buf misc = alloc(0x1000, 0x1000, AMDGPU_GEM_DOMAIN_GTT, 0);                         // fence 0, pstat pre 0x100, post 0x200
	Buf ibuf = alloc(0x4000, 0x1000, AMDGPU_GEM_DOMAIN_GTT, 0);

	// Shaders: the shared builder places ngg.s + psred.s (what rdna4-run tri runs); REPLAY_VS may then swap
	// the VS, padded with s_code_end the same way.
	rdna4_tri_place_shaders(code.cpu);
	auto place = [&](uint32_t at, const uint32_t *k, uint32_t dwords) {
		uint32_t *p = reinterpret_cast<uint32_t *>(static_cast<char *>(code.cpu) + at);
		for (uint32_t i = 0; i < RDNA4_TRI_SHADER_PAD; i++)
			p[i] = i < dwords ? k[i] : 0xbf9f0000u;
	};
	if (!strncmp(vsSel, "file:", 5)) {   // raw dwords in hex, one or more per line (e.g. RADV's VS from its dump)
		FILE *f = fopen(vsSel + 5, "r");
		if (!f) { perror(vsSel + 5); return 1; }
		static uint32_t k[0x100];
		uint32_t nk = 0;
		while (nk < 0x100 && fscanf(f, "%x", &k[nk]) == 1)
			nk++;
		fclose(f);
		printf("VS from %s: %u dwords\n", vsSel + 5, nk);
		place(0, k, nk);
	} else if (variant & 4096)   // the wave64 fallback (ngg64.s), as the kext's ladder variant 4096
		place(0, kNgg64Kernel, sizeof(kNgg64Kernel) / 4);
	else if (!strcmp(vsSel, "old"))
		place(0, kNggOldKernel, sizeof(kNggOldKernel) / 4);
	else if (strcmp(vsSel, "new")) {
		fprintf(stderr, "REPLAY_VS: new, old or file:<hex>\n");
		return 1;
	}
	// Marker literals (nggstore.s / nggvgpr.s convention): 0xdead0001 + 2k / + 2k+1 = low / high of misc + 0x300 + 0x80 k.
	{
		uint32_t *p = static_cast<uint32_t *>(code.cpu);
		for (uint32_t i = 0; i < 0x100; i++)
			if (p[i] >= 0xdead0001u && p[i] <= 0xdead0008u) {
				const uint32_t k = p[i] - 0xdead0001u;
				const uint64_t a = misc.va + 0x300 + 0x80 * (k / 2);
				p[i] = (k & 1) ? static_cast<uint32_t>(a >> 32) : static_cast<uint32_t>(a);
			}
		memset(static_cast<char *>(misc.cpu) + 0x300, 0xee, 0x200);
	}

	// The stream with its addresses: the shared builder (rdna4_tri_record), as rdna4-run tri records it.
	const rdna4_tri_va tva = { code.va, target.va, rings.va, misc.va };
	printf("code 0x%llx target 0x%llx rings 0x%llx fence 0x%llx (VS %s, variant %u)\n",
	       (unsigned long long)tva.code, (unsigned long long)tva.target, (unsigned long long)tva.rings,
	       (unsigned long long)tva.fence, vsSel, variant);
	constexpr uint32_t n = sizeof(kStream) / 4;
	uint32_t s[n];
	if (rdna4_tri_record(s, &tva) != n) { fprintf(stderr, "rdna4_tri_record refused the addresses\n"); return 1; }
	auto patch = [&](uint32_t opcode, uint32_t reg, uint32_t mask, uint32_t value, const char *what) {
		const int at = findStreamReg(kStream, n, opcode, reg);
		if (at < 0) { fprintf(stderr, "patch %s: register not in the stream\n", what); exit(1); }
		const uint32_t old = s[at];
		s[at] = (old & ~mask) | (value & mask);
		printf("patch %s: 0x%08x -> 0x%08x\n", what, old, s[at]);
	};
	if (variant & 1)
		patch(0x76, 0x8b, 0x0000003e, 1u << 1, "SPI_SHADER_PGM_RSRC2_GS.USER_SGPR=1");
	if (variant & 2) {
		patch(0x76, 0x88, 0x7f800000, 3u << 23, "SPI_SHADER_PGM_RSRC4_GS.INST_PREF_SIZE=3");
		patch(0x76, 0x07, 0x00ff0000, 2u << 16, "SPI_SHADER_PGM_RSRC4_PS.INST_PREF_SIZE=2");
	}
	if (variant & 128)
		patch(0x69, 0x204, 0x00010000, 0x00010000, "PA_CL_CLIP_CNTL.CLIP_DISABLE=1");
	if (variant & 256)
		patch(0x69, 0x20b, 0x0000000f, 0x0000000f, "PA_SU_PRIM_FILTER_CNTL.*_FILTER_DISABLE=1");
	if (variant & 4096) {   // wave64: GS_W32_EN off, 12 VGPRs (granules of 4 in wave64)
		patch(0x69, 0x2a6, 0x00400000, 0, "VGT_SHADER_STAGES_EN.GS_W32_EN=0 (wave64 NGG)");
		patch(0x76, 0x8a, 0x0000003f, 2, "SPI_SHADER_PGM_RSRC1_GS.VGPRS=2 (wave64)");
	}
	if (variant & 2048)
		patch(0x79, 0x26b, 0xffffffff, 0x000007fe, "GE_PRIM_RING_SIZE = MEM_SIZE only");

	// IB: PIPELINESTAT_START, SAMPLE_PIPELINESTAT (pre), the stream, SAMPLE_PIPELINESTAT (post).
	uint32_t *ib = static_cast<uint32_t *>(ibuf.cpu);
	uint32_t w = 0;
	uint32_t *pre = reinterpret_cast<uint32_t *>(static_cast<char *>(misc.cpu) + 0x100);
	uint32_t *post = reinterpret_cast<uint32_t *>(static_cast<char *>(misc.cpu) + 0x200);
	for (uint32_t i = 0; i < 28; i++)
		pre[i] = post[i] = 0xdeadf00du;
	ib[w++] = 0xc0004600; ib[w++] = 0x19;                                          // PIPELINESTAT_START
	ib[w++] = 0xc0024600; ib[w++] = 0x1e | (2u << 8);                              // SAMPLE_PIPELINESTAT
	ib[w++] = static_cast<uint32_t>(misc.va + 0x100); ib[w++] = static_cast<uint32_t>((misc.va + 0x100) >> 32);
	// REPLAY_SET="c:0x10b=0x43800000;u:0x242=4;s:0x88=0x..." : SET_CONTEXT_REG (c), SET_SH_REG (s) or
	// SET_UCONFIG_REG (u) writes inserted right before NUM_INSTANCES, i.e. after all of the stream's state.
	uint32_t split = n;
	for (uint32_t i = 0; i < n; i += 2 + ((s[i] >> 16) & 0x3fff))
		if (((s[i] >> 8) & 0xff) == 0x2f) { split = i; break; }
	memcpy(ib + w, s, 4 * split);
	w += split;
	if (const char *set = getenv("REPLAY_SET")) {
		char *copy = strdup(set);
		for (char *tok = strtok(copy, ";, "); tok; tok = strtok(nullptr, ";, ")) {
			char kind = 0;
			unsigned reg = 0, val = 0;
			if (sscanf(tok, "%c:%x=%x", &kind, &reg, &val) != 3 || !strchr("csu", kind)) {
				fprintf(stderr, "bad REPLAY_SET entry '%s'\n", tok);
				return 1;
			}
			ib[w++] = 0xc0010000u | ((kind == 'c' ? 0x69u : kind == 's' ? 0x76u : 0x79u) << 8);   // 1 register
			ib[w++] = reg;
			ib[w++] = val;
			printf("set %c 0x%03x = 0x%08x\n", kind, reg, val);
		}
		free(copy);
	}
	memcpy(ib + w, s + split, 4 * (n - split));
	w += n - split;
	ib[w++] = 0xc0024600; ib[w++] = 0x1e | (2u << 8);
	ib[w++] = static_cast<uint32_t>(misc.va + 0x200); ib[w++] = static_cast<uint32_t>((misc.va + 0x200) >> 32);
	while (w & 7)
		ib[w++] = 0xffff1000u;                                                     // type-2 NOP padding

	amdgpu_bo_handle bos[5] = { code.bo, target.bo, rings.bo, misc.bo, ibuf.bo };
	amdgpu_bo_list_handle list;
	CHECK(amdgpu_bo_list_create(dev, 5, bos, nullptr, &list));
	amdgpu_cs_ib_info ibi = {};
	ibi.ib_mc_address = ibuf.va;
	ibi.size = w;
	amdgpu_cs_request req = {};
	req.ip_type = AMDGPU_HW_IP_GFX;
	req.resources = list;
	req.number_of_ibs = 1;
	req.ibs = &ibi;
	CHECK(amdgpu_cs_submit(ctx, 0, &req, 1));
	printf("IB VA 0x%llx, %u dwords, seq %llu\n", (unsigned long long)ibuf.va, w, (unsigned long long)req.seq_no);

	amdgpu_cs_fence fence = {};
	fence.context = ctx;
	fence.ip_type = AMDGPU_HW_IP_GFX;
	fence.fence = req.seq_no;
	uint32_t expired = 0;
	int r = amdgpu_cs_query_fence_status(&fence, 5000000000ull, 0, &expired);
	uint32_t state = 0, hangs = 0;
	amdgpu_cs_query_reset_state(ctx, &state, &hangs);
	printf("submission: %s (query %d), context reset state %u\n", expired ? "done" : "NOT done in 5 s", r, state);

	const uint32_t *px = static_cast<const uint32_t *>(target.cpu);
	uint32_t builderOthers = 0;
	const uint32_t builderRed = rdna4_tri_count(px, &builderOthers);
	uint32_t red = 0, other = 0, minX = kWidth, maxX = 0, minY = kHeight, maxY = 0;
	for (uint32_t y = 0; y < kHeight; y++)
		for (uint32_t x = 0; x < kWidth; x++) {
			const uint32_t v = px[y * kWidth + x];
			if (!v)
				continue;
			if (v == kCoveredRgba) red++; else other++;
			if (x < minX) minX = x;
			if (x > maxX) maxX = x;
			if (y < minY) minY = y;
			if (y > maxY) maxY = y;
		}
	printf("pixels: %u 0x%08x (want %u), %u others; bounds x %u..%u y %u..%u\n", red, kCoveredRgba,
	       kCoveredPixels, other, minX, maxX, minY, maxY);
	printf("rdna4_tri_count: %u px, %u others: %s\n", builderRed, builderOthers,
	       rdna4_tri_ok(builderRed, builderOthers) ? "THE TRIANGLE IS RIGHT" : "wrong image");
	printf("draw fence: 0x%08x\n", *static_cast<uint32_t *>(misc.cpu));
	if (getenv("REPLAY_DUMP_MARKER")) {
		const uint32_t *m = reinterpret_cast<const uint32_t *>(static_cast<char *>(misc.cpu) + 0x300);
		for (uint32_t row = 0; row < 4; row++) {   // nggvgpr.s: v0, v3, x, y arrays 0x80 apart; nggstore.s: row 0
			printf("marker +0x%02x:", 0x80 * row);
			for (uint32_t i = 0; i < 4; i++)
				printf(" 0x%08x", m[0x20 * row + i]);
			printf("\n");
		}
	}
	static const char *names[8] = { "PS_INV", "C_PRIM", "C_INV", "VS_INV", "GS_INV", "GS_PRIM", "IA_PRIM", "IA_VERT" };
	printf("stats:");
	for (uint32_t i = 0; i < 8; i++) {
		if (pre[2 * i] == 0xdeadf00du || post[2 * i] == 0xdeadf00du) {
			printf(" %s unwritten", names[i]);
			continue;
		}
		const uint64_t a = pre[2 * i] | (uint64_t)pre[2 * i + 1] << 32, b = post[2 * i] | (uint64_t)post[2 * i + 1] << 32;
		printf(" %s %llu", names[i], (unsigned long long)(b - a));
	}
	printf("\n");
	return rdna4_tri_ok(builderRed, builderOthers) ? 0 : 2;
}
