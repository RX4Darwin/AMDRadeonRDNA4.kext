// Host-only check for E3 (docs/metal-spike.md, hub-task-354): load two gfx1201 code objects with the kext's own code-object reader
// (src/codeobj.cpp, the reader the card-side runtime uses) and print the kernel descriptor facts side by side: the translated kernel and clang's
// native build of the same source. No GPU, nothing is run.
//   check-codeobj <native.hsaco> <translated.hsaco> <kernel>
#include "codeobj.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <vector>

static bool load(const char *path, std::vector<uint8_t> &v) {
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); return false; }
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	v.resize(static_cast<size_t>(n));
	bool ok = fread(v.data(), 1, v.size(), f) == v.size();
	fclose(f);
	return ok;
}

int main(int argc, char **argv) {
	if (argc != 4) { fprintf(stderr, "usage: %s native.hsaco translated.hsaco kernel\n", argv[0]); return 2; }
	std::vector<uint8_t> a, b;
	if (!load(argv[1], a) || !load(argv[2], b)) return 2;
	CodeObj::Kernel ka, kb;
	CodeObj::Image ia, ib;
	const char *why = nullptr;
	if (!CodeObj::findKernel(a.data(), a.size(), argv[3], ka, &why) || !CodeObj::parseImage(a.data(), a.size(), ia, &why)) { fprintf(stderr, "native: %s\n", why ? why : "?"); return 1; }
	if (!CodeObj::findKernel(b.data(), b.size(), argv[3], kb, &why) || !CodeObj::parseImage(b.data(), b.size(), ib, &why)) { fprintf(stderr, "translated: %s\n", why ? why : "?"); return 1; }
	printf("%-22s %-14s %-14s\n", "field", "native", "translated");
#define ROW(name, x) printf("%-22s 0x%-12llx 0x%-12llx%s\n", name, (unsigned long long)ka.x, (unsigned long long)kb.x, ka.x == kb.x ? "" : "  <-- differs")
	ROW("COMPUTE_PGM_RSRC1", rsrc1);
	ROW("COMPUTE_PGM_RSRC2", rsrc2);
	ROW("COMPUTE_PGM_RSRC3", rsrc3);
	ROW("kernel_code_properties", properties);
	ROW("kernarg size", kernargSize);
	ROW("group segment (LDS)", groupSegmentSize);
	ROW("private segment", privateSegmentSize);
	ROW("entry VA in image", entryVa);
	printf("%-22s %-14u %-14u\n", "user SGPR count", ka.userSgprCount(), kb.userSgprCount());
	printf("%-22s %-14d %-14d\n", "wave32", ka.wave32(), kb.wave32());
	printf("%-22s %-14llu %-14llu\n", "image bytes", (unsigned long long)ia.size, (unsigned long long)ib.size);
	const bool same = ka.rsrc1 == kb.rsrc1 && ka.rsrc2 == kb.rsrc2 && ka.rsrc3 == kb.rsrc3 && ka.properties == kb.properties && ka.kernargSize == kb.kernargSize;
	printf("%s\n", same ? "descriptors identical" : "descriptors DIFFER (see above)");
	const bool shape = kb.wantsKernargPtr() && kb.userSgprCount() == 2 && !kb.privateSegmentSize && !kb.groupSegmentSize;
	printf("replay-compute shape check (kernarg pointer only, no scratch, no LDS): %s\n", shape ? "ok" : "FAILS");
	return same && shape ? 0 : 1;
}
