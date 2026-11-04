/*
 *  rdna4-pvexec.c
 *  AMDRadeonRDNA4
 *
 *  The paravirtual host's compiler service (PVGPU builds, include/rdna4pvjob.h).
 *  A Metal client's compute dispatch reaches the kext as the metallib it
 *  compiled, the buffers it bound and the grid; this program takes each one,
 *  compiles the AIR for gfx1201 (tools/metal/air/compile-metallib.sh, cached
 *  by the metallib and the threadgroup size), runs it on the card through
 *  the compute runtime with copies of the buffers, and writes them back into
 *  the client's memory. A draw's render pipeline is compiled into a vertex
 *  and a fragment kernel (tools/metal/air/compile-pipeline.sh) that run it
 *  as compute. The kext completes the client's command buffer when
 *  the job is done.
 *
 *    rdna4-pvexec [-s compile-metallib.sh] [-c cache dir] [-v]
 *
 *  Needs root. Jobs only come in a boot with the compute runtime
 *  (rdna4-compute=7) and the paravirtual host (rdna4-pvgpu=1); in any other
 *  it waits, trying again every few seconds, so that it can run as a LaunchDaemon
 *  (userspace/rdna4-pvexec/install.sh). LLVM for the compiler is taken from
 *  $LLVM/bin or PATH.
 */

#include <errno.h>
#include <time.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "librdna4.h"

#define MAX_PROGRAMS 256
#define MAX_KERNARGS 64
#define MAX_BUFFER_BYTES (256ull << 20)
#define DISPATCH_TIMEOUT_MS 5000
#define RETRY_SECONDS 5

/* A compiled function, by the hash of its metallib and its threadgroup size. */
typedef struct {
	char key[64];
	rdna4_program_t prog;
	char abi[MAX_KERNARGS][24];
	int nabi;
} program_t;

static const char *gScript = "tools/metal/air/compile-metallib.sh";
static const char *gCache = "/var/tmp/rdna4-pvexec";
static int gVerbose;
static volatile sig_atomic_t gStop;
static program_t gPrograms[MAX_PROGRAMS];
static int gNumPrograms;

static uint64_t nowUs(void)
{
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000;
}

static void onSignal(int sig)
{
	(void)sig;
	gStop = 1;
}

static uint64_t fnv1a(const uint8_t *p, size_t n)
{
	uint64_t h = 0xcbf29ce484222325ull;

	for (size_t i = 0; i < n; i++)
		h = (h ^ p[i]) * 0x100000001b3ull;

	return h;
}

static void *readFile(const char *path, size_t *bytes)
{
	FILE *f = fopen(path, "rb");
	void *buf = NULL;
	long n;

	if (!f)
		return NULL;

	if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0) {
		buf = malloc((size_t)n);
		if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
			free(buf);
			buf = NULL;
		}
		*bytes = (size_t)n;
	}

	fclose(f);

	return buf;
}

static int writeFile(const char *path, const void *buf, size_t bytes)
{
	FILE *f = fopen(path, "wb");
	int ok;

	if (!f)
		return 0;

	ok = fwrite(buf, 1, bytes, f) == bytes;

	return fclose(f) == 0 && ok;
}

/* compile-metallib.sh IN TG DIR; true when DIR/kernel.hsaco is there afterwards. */
static int compile(const char *dir, const uint32_t tg[3])
{
	char in[512], size[48], hsaco[512];
	struct stat st;
	pid_t pid;
	int status;

	snprintf(hsaco, sizeof(hsaco), "%s/kernel.hsaco", dir);
	if (stat(hsaco, &st) == 0)
		return 1;

	snprintf(in, sizeof(in), "%s/in.metallib", dir);
	snprintf(size, sizeof(size), "%u,%u,%u", tg[0], tg[1], tg[2]);

	pid = fork();
	if (pid == 0) {
		execl("/bin/sh", "sh", gScript, in, size, dir, (char *)NULL);
		_exit(127);
	}

	if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status))
		return 0;

	return stat(hsaco, &st) == 0;
}

/* The kernargs of a compiled function, one line each (air2amdgcn --abi). */
static int readAbi(const char *dir, program_t *p)
{
	char path[512], line[64];
	FILE *f;

	snprintf(path, sizeof(path), "%s/kernel.abi", dir);
	f = fopen(path, "r");
	if (!f)
		return 0;

	p->nabi = 0;
	while (p->nabi < MAX_KERNARGS && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = 0;
		snprintf(p->abi[p->nabi++], sizeof(p->abi[0]), "%s", line);
	}

	fclose(f);

	return p->nabi > 0;
}

/* Compile (or find) and load the job's function for its threadgroup size. */
static program_t *program(rdna4_t *gpu, const RDNA4PvJob *job, const uint8_t *code)
{
	char key[64], dir[512], path[512], name[128] = { 0 };
	program_t *p;
	void *elf, *txt;
	size_t elfBytes, txtBytes;
	kern_return_t kr;

	snprintf(key, sizeof(key), "%016" PRIx64 "-%ux%ux%u", fnv1a(code, job->codeBytes),
	         job->groupSize[0], job->groupSize[1], job->groupSize[2]);

	for (int i = 0; i < gNumPrograms; i++) {
		if (!strcmp(gPrograms[i].key, key))
			return &gPrograms[i];
	}

	if (gNumPrograms == MAX_PROGRAMS)
		return NULL;

	snprintf(dir, sizeof(dir), "%s/%s", gCache, key);
	mkdir(gCache, 0755);
	mkdir(dir, 0755);

	snprintf(path, sizeof(path), "%s/in.metallib", dir);
	if (!writeFile(path, code, job->codeBytes) || !compile(dir, job->groupSize)) {
		fprintf(stderr, "rdna4-pvexec: %s: does not compile (%s)\n", key, dir);
		return NULL;
	}

	snprintf(path, sizeof(path), "%s/kernel.name", dir);
	txt = readFile(path, &txtBytes);
	if (txt) {
		snprintf(name, sizeof(name), "%.*s", (int)txtBytes, (const char *)txt);
		name[strcspn(name, "\n")] = 0;
		free(txt);
	}

	snprintf(path, sizeof(path), "%s/kernel.hsaco", dir);
	elf = readFile(path, &elfBytes);
	p = &gPrograms[gNumPrograms];
	if (!elf || !name[0] || !readAbi(dir, p)) {
		free(elf);
		return NULL;
	}

	kr = rdna4_load(gpu, elf, elfBytes, name, &p->prog);
	free(elf);
	if (kr) {
		fprintf(stderr, "rdna4-pvexec: %s: load: %s\n", key, rdna4_error(kr));
		return NULL;
	}

	snprintf(p->key, sizeof(p->key), "%s", key);
	gNumPrograms++;
	if (gVerbose)
		printf("rdna4-pvexec: %s: kernel %s, %d kernarg entries\n", key, name, p->nabi);

	return p;
}

static const RDNA4PvBuffer *bufferAt(const RDNA4PvJob *job, uint32_t slot)
{
	for (uint32_t i = 0; i < job->bufferCount; i++) {
		if (job->buffers[i].slot == slot)
			return &job->buffers[i];
	}

	return NULL;
}

/* The kernargs as the program's ABI lists them, from the GPU copies of the buffers. */
static int kernargs(const program_t *p, const RDNA4PvJob *job, const rdna4_buffer_t *gpuBufs,
                    uint8_t *out, uint32_t *bytes)
{
	uint32_t at = 0;

	for (int i = 0; i < p->nabi; i++) {
		unsigned slot;

		if (sscanf(p->abi[i], "buffer %u", &slot) == 1) {
			const RDNA4PvBuffer *b = bufferAt(job, slot);
			uint64_t va;

			if (!b)
				return 0;

			va = gpuBufs[b - job->buffers].gpu;
			at = (at + 7) & ~7u;
			memcpy(out + at, &va, 8);
			at += 8;
		} else if (!strcmp(p->abi[i], "grid")) {
			memcpy(out + at, job->threads, 12);
			at += 12;
		} else {
			return 0;   /* threadgroup memory arguments: not yet */
		}
	}

	*bytes = at;

	return 1;
}

/* Every buffer of the job between the client's memory and a GPU copy. */
static int moveBuffers(rdna4_t *gpu, const RDNA4PvJob *job, rdna4_buffer_t *gpuBufs, int toGpu)
{
	for (uint32_t i = 0; i < job->bufferCount; i++) {
		const RDNA4PvBuffer *b = &job->buffers[i];
		void *host = malloc(b->length);
		kern_return_t kr;

		if (!host)
			return 0;

		if (toGpu) {
			kr = rdna4_pv_access(gpu, job->task, b->va, host, b->length, 0);
			if (!kr)
				kr = rdna4_write(gpu, &gpuBufs[i], 0, host, b->length);
		} else {
			kr = rdna4_read(gpu, &gpuBufs[i], 0, host, b->length);
			if (!kr)
				kr = rdna4_pv_access(gpu, job->task, b->va, host, b->length, RDNA4_PVACCESS_WRITE);
		}

		free(host);
		if (kr) {
			fprintf(stderr, "rdna4-pvexec: buffer slot %u: %s\n", b->slot, rdna4_error(kr));
			return 0;
		}
	}

	return 1;
}

/* ---- draws ------------------------------------------------------------------------------------------------- */

/* A compiled render pipeline: its two kernels and what their ABIs say. */
typedef struct {
	char key[56];
	rdna4_program_t vertex, fragment;
	char vabi[MAX_KERNARGS][48];
	char fabi[MAX_KERNARGS][48];
	int nvabi, nfabi;
	uint32_t stride, position;   /* bytes, from the vertex ABI */
} pipeline_t;

static pipeline_t gPipelines[MAX_PROGRAMS];
static int gNumPipelines;

/* Kernargs as LLVM lays them out: each at its own alignment. */
typedef struct {
	uint8_t bytes[RDNA4_MAX_KERNARG];
	uint32_t at;
	int full;
} kernargs_t;

static void put(kernargs_t *k, const void *v, uint32_t size)
{
	k->at = (k->at + size - 1) & ~(size - 1);

	if (k->at + size > sizeof(k->bytes)) {
		k->full = 1;
		return;
	}

	memcpy(k->bytes + k->at, v, size);
	k->at += size;
}

static void put32(kernargs_t *k, uint32_t v)
{
	put(k, &v, 4);
}

static void put64(kernargs_t *k, uint64_t v)
{
	put(k, &v, 8);
}

/* An ABI file of compile-pipeline.sh, one entry a line. */
static int readLines(const char *dir, const char *file, char lines[][48], int *n)
{
	char path[512], line[96];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, file);
	f = fopen(path, "r");
	if (!f)
		return 0;

	*n = 0;
	while (*n < MAX_KERNARGS && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = 0;
		snprintf(lines[(*n)++], 48, "%s", line);
	}

	fclose(f);

	return *n > 0;
}

/* A code object of the pipeline's directory, loaded by its entry point. */
static int loadKernel(rdna4_t *gpu, const char *dir, const char *what, rdna4_program_t *prog)
{
	char path[512], name[128] = { 0 };
	size_t bytes, nameBytes;
	void *elf, *txt;
	kern_return_t kr;

	snprintf(path, sizeof(path), "%s/%s.name", dir, what);
	txt = readFile(path, &nameBytes);
	if (!txt)
		return 0;

	snprintf(name, sizeof(name), "%.*s", (int)nameBytes, (const char *)txt);
	name[strcspn(name, "\n")] = 0;
	free(txt);

	snprintf(path, sizeof(path), "%s/%s.hsaco", dir, what);
	elf = readFile(path, &bytes);
	if (!elf)
		return 0;

	kr = rdna4_load(gpu, elf, bytes, name, prog);
	free(elf);
	if (kr)
		fprintf(stderr, "rdna4-pvexec: %s %s: load: %s\n", dir, what, rdna4_error(kr));

	return kr == 0;
}

/* compile-pipeline.sh VS FS DIR, beside the compute script; true when both code objects are there. */
static int compilePipeline(const char *dir)
{
	char script[512], vs[512], fs[512], hsaco[512];
	const char *slash = strrchr(gScript, '/');
	struct stat st;
	pid_t pid;
	int status;

	snprintf(hsaco, sizeof(hsaco), "%s/fragment.hsaco", dir);
	if (stat(hsaco, &st) == 0)
		return 1;

	snprintf(script, sizeof(script), "%.*scompile-pipeline.sh", slash ? (int)(slash - gScript + 1) : 0, gScript);
	snprintf(vs, sizeof(vs), "%s/vs.metallib", dir);
	snprintf(fs, sizeof(fs), "%s/fs.metallib", dir);

	pid = fork();
	if (pid == 0) {
		execl("/bin/sh", "sh", script, vs, fs, dir, (char *)NULL);
		_exit(127);
	}

	if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status))
		return 0;

	return stat(hsaco, &st) == 0;
}

/* Read `bytes` of the client's memory at `va` into a buffer. */
static uint8_t *guestCopy(rdna4_t *gpu, uint32_t task, uint64_t va, uint32_t bytes)
{
	uint8_t *p = bytes ? malloc(bytes) : NULL;

	if (p && rdna4_pv_access(gpu, task, va, p, bytes, 0)) {
		free(p);
		return NULL;
	}

	return p;
}

/* The vertex descriptor as compile-pipeline.sh reads it (vertex.desc): a line per attribute; nothing without one. */
static int writeVertexDesc(const char *dir, const RDNA4PvJob *job)
{
	char path[512];
	FILE *f;

	if (!job->attributeMask)
		return 1;

	snprintf(path, sizeof(path), "%s/vertex.desc", dir);
	f = fopen(path, "w");
	if (!f)
		return 0;

	for (uint32_t i = 0; i < RDNA4_PVJOB_MAX_ATTRIBUTES; i++) {
		const RDNA4PvAttribute *a = &job->attributes[i];
		const RDNA4PvLayout *l;

		if (!(job->attributeMask & 1u << i) || a->buffer >= RDNA4_PVJOB_MAX_ATTRIBUTES)
			continue;

		l = &job->layouts[a->buffer];
		fprintf(f, "attribute %u %u %u %u %u %u %u\n", i, a->format, a->buffer, a->offset, l->stride, l->stepFunction,
		        l->stepRate);
	}

	return fclose(f) == 0;
}

/* The job's render pipeline, compiled (or found) and loaded. */
static pipeline_t *pipeline(rdna4_t *gpu, const RDNA4PvJob *job)
{
	uint8_t *vs = guestCopy(gpu, job->task, job->codeVa, job->codeBytes);
	uint8_t *fs = guestCopy(gpu, job->task, job->fragmentVa, job->fragmentBytes);
	char key[56], dir[512], path[512];
	pipeline_t *p = NULL;

	if (!vs || !fs)
		goto out;

	/* the vertex descriptor is compiled into the vertex kernel */
	snprintf(key, sizeof(key), "pipe-%016" PRIx64 "-%016" PRIx64 "-%08" PRIx32, fnv1a(vs, job->codeBytes),
	         fnv1a(fs, job->fragmentBytes),
	         job->attributeMask ? (uint32_t)fnv1a((const uint8_t *)&job->attributeMask,
	                                              sizeof(job->attributeMask) + sizeof(job->attributes) + sizeof(job->layouts)) : 0);

	for (int i = 0; i < gNumPipelines; i++) {
		if (!strcmp(gPipelines[i].key, key)) {
			p = &gPipelines[i];
			goto out;
		}
	}

	if (gNumPipelines == MAX_PROGRAMS)
		goto out;

	snprintf(dir, sizeof(dir), "%s/%s", gCache, key);
	mkdir(gCache, 0755);
	mkdir(dir, 0755);
	snprintf(path, sizeof(path), "%s/vs.metallib", dir);
	if (!writeFile(path, vs, job->codeBytes))
		goto out;

	snprintf(path, sizeof(path), "%s/fs.metallib", dir);
	if (!writeFile(path, fs, job->fragmentBytes) || !writeVertexDesc(dir, job) || !compilePipeline(dir)) {
		fprintf(stderr, "rdna4-pvexec: %s: does not compile (%s)\n", key, dir);
		goto out;
	}

	p = &gPipelines[gNumPipelines];
	memset(p, 0, sizeof(*p));
	if (!readLines(dir, "vertex.abi", p->vabi, &p->nvabi) || !readLines(dir, "fragment.abi", p->fabi, &p->nfabi) ||
	    !loadKernel(gpu, dir, "vertex", &p->vertex) || !loadKernel(gpu, dir, "fragment", &p->fragment)) {
		p = NULL;
		goto out;
	}

	for (int i = 0; i < p->nvabi; i++) {
		sscanf(p->vabi[i], "stride %u", &p->stride);
		sscanf(p->vabi[i], "position %u", &p->position);
	}

	snprintf(p->key, sizeof(p->key), "%s", key);
	gNumPipelines++;
	if (gVerbose)
		printf("rdna4-pvexec: %s: vertex records of %u bytes\n", key, p->stride);

out:
	free(vs);
	free(fs);

	return p;
}

static const RDNA4PvBuffer *stageBuffer(const RDNA4PvJob *job, uint32_t stage, uint32_t slot)
{
	for (uint32_t i = 0; i < job->bufferCount; i++) {
		if (job->buffers[i].stage == stage && job->buffers[i].slot == slot)
			return &job->buffers[i];
	}

	return NULL;
}

/* A stage's "buffer SLOT" kernarg: the GPU copy of the buffer bound there. */
static int putBuffer(kernargs_t *k, const char *line, const RDNA4PvJob *job, uint32_t stage, const rdna4_buffer_t *bufs)
{
	unsigned slot;
	const RDNA4PvBuffer *b;

	if (sscanf(line, "buffer %u", &slot) != 1)
		return 0;

	b = stageBuffer(job, stage, slot);
	if (!b)
		return 0;

	put64(k, bufs[b - job->buffers].gpu);

	return 1;
}

static int vertexArgs(const pipeline_t *p, const RDNA4PvJob *job, const rdna4_buffer_t *bufs, uint64_t out,
                      uint32_t instance, kernargs_t *k)
{
	for (int i = 0; i < p->nvabi; i++) {
		const char *l = p->vabi[i];

		if (!strncmp(l, "buffer ", 7)) {
			if (!putBuffer(k, l, job, RDNA4_PVJOB_VERTEX, bufs))
				return 0;
		} else if (!strcmp(l, "vertex")) {
			put32(k, job->vertexStart);
			put32(k, job->vertexCount);
			put32(k, instance);
		} else if (!strcmp(l, "out")) {
			put64(k, out);
		} else if (!strcmp(l, "indices")) {
			const RDNA4PvBuffer *ix = stageBuffer(job, RDNA4_PVJOB_INDEX, 0);

			if (job->indexType && !ix)
				return 0;

			put64(k, ix ? bufs[ix - job->buffers].gpu : 0);
			put32(k, job->indexType);
		}
	}

	return !k->full;
}

/* A fragment function's "texture SLOT" or "sampler SLOT" kernarg: the descriptor uploaded for the slot. */
static int putObject(kernargs_t *k, const char *line, const char *kind, const uint32_t *slots, uint32_t n,
                     const rdna4_buffer_t *descs)
{
	unsigned slot;
	char fmt[32];

	snprintf(fmt, sizeof(fmt), "%s %%u", kind);
	if (sscanf(line, fmt, &slot) != 1)
		return 0;

	for (uint32_t i = 0; i < n; i++) {
		if (slots[i] == slot) {
			put64(k, descs[i].gpu);
			return 1;
		}
	}

	return 0;
}

static int fragmentArgs(const pipeline_t *p, const RDNA4PvJob *job, const rdna4_buffer_t *bufs,
                        const rdna4_buffer_t *texDescs, const rdna4_buffer_t *samplers, uint64_t verts,
                        uint64_t target, kernargs_t *k)
{
	uint32_t texSlots[RDNA4_PVJOB_MAX_TEXTURES], samplerSlots[RDNA4_PVJOB_MAX_SAMPLERS];

	for (uint32_t i = 0; i < job->textureCount; i++)
		texSlots[i] = job->textures[i].slot;

	for (uint32_t i = 0; i < job->samplerCount; i++)
		samplerSlots[i] = job->samplers[i].slot;

	for (int i = 0; i < p->nfabi; i++) {
		const char *l = p->fabi[i];

		if (!strncmp(l, "buffer ", 7)) {
			if (!putBuffer(k, l, job, RDNA4_PVJOB_FRAGMENT, bufs))
				return 0;
		} else if (!strncmp(l, "texture ", 8)) {
			if (!putObject(k, l, "texture", texSlots, job->textureCount, texDescs))
				return 0;
		} else if (!strncmp(l, "sampler ", 8)) {
			if (!putObject(k, l, "sampler", samplerSlots, job->samplerCount, samplers))
				return 0;
		} else if (!strcmp(l, "fragment")) {
			put64(k, verts);
			put32(k, p->stride / 4);
			put32(k, p->position / 4);
			put32(k, job->topology);
			put32(k, job->vertexCount);
			put64(k, target);
			put32(k, job->targetPitch);
			put32(k, job->targetWidth);
			put32(k, job->targetHeight);
			put32(k, job->targetFormat);
			for (int v = 0; v < 4; v++)
				put32(k, job->viewport[v]);
			put32(k, job->blend);
			for (int v = 0; v < 4; v++)
				put32(k, job->scissor[v]);
		}
	}

	return !k->full;
}

/* `bytes` of the client's memory at `va` (or the IOSurface) into a new VRAM buffer, through `host`. */
static kern_return_t upload(rdna4_t *gpu, uint32_t task, uint64_t va, uint32_t flags, void *host, uint64_t bytes,
                            rdna4_buffer_t *out)
{
	kern_return_t kr = rdna4_alloc(gpu, bytes, out);

	if (!kr)
		kr = rdna4_pv_access(gpu, task, va, host, bytes, flags);
	if (!kr)
		kr = rdna4_write(gpu, out, 0, host, bytes);

	return kr;
}

/* rdna4raster.c's struct rdna4_texture. */
typedef struct {
	uint64_t data;
	uint32_t width, height, pitch, format;
} gpu_texture_t;

/*
 * A texture of the draw in VRAM (its texels and their descriptor), and a
 * sampler's state word, as the fragment kernel reads them.
 */
static kern_return_t uploadTextures(rdna4_t *gpu, const RDNA4PvJob *job, rdna4_buffer_t *texels,
                                    rdna4_buffer_t *descs, rdna4_buffer_t *samplers)
{
	kern_return_t kr = 0;

	for (uint32_t i = 0; i < job->textureCount && !kr; i++) {
		const RDNA4PvTexture *t = &job->textures[i];
		const uint64_t bytes = (uint64_t)t->pitch * t->height;
		void *host = bytes && bytes <= MAX_BUFFER_BYTES ? malloc(bytes) : NULL;
		gpu_texture_t d;

		if (!host)
			return kIOReturnNoMemory;

		kr = t->surface ? upload(gpu, t->surface, 0, RDNA4_PVACCESS_SURFACE, host, bytes, &texels[i])
		                : upload(gpu, job->task, t->va, 0, host, bytes, &texels[i]);
		free(host);

		d = (gpu_texture_t){ texels[i].gpu, t->width, t->height, t->pitch, t->format };
		if (!kr)
			kr = rdna4_alloc(gpu, sizeof(d), &descs[i]);
		if (!kr)
			kr = rdna4_write(gpu, &descs[i], 0, &d, sizeof(d));
	}

	for (uint32_t i = 0; i < job->samplerCount && !kr; i++) {
		kr = rdna4_alloc(gpu, 16, &samplers[i]);
		if (!kr)
			kr = rdna4_write(gpu, &samplers[i], 0, &job->samplers[i].word, 4);
	}

	return kr;
}

static void freeAll(rdna4_t *gpu, rdna4_buffer_t *bufs, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		if (bufs[i].handle)
			rdna4_free(gpu, &bufs[i]);
	}
}

/*
 * One draw: the bound buffers and the render target copied into VRAM (by the
 * copy engine), per instance the vertex kernel over the vertices, then the
 * fragment kernel over the target; the target back into the client.
 */
static uint32_t runDraw(rdna4_t *gpu, const RDNA4PvJob *job)
{
	rdna4_buffer_t bufs[RDNA4_PVJOB_MAX_BUFFERS] = { { 0 } }, verts = { 0 }, target = { 0 };
	rdna4_buffer_t texels[RDNA4_PVJOB_MAX_TEXTURES] = { { 0 } }, texDescs[RDNA4_PVJOB_MAX_TEXTURES] = { { 0 } };
	rdna4_buffer_t samplers[RDNA4_PVJOB_MAX_SAMPLERS] = { { 0 } };
	const uint64_t targetBytes = (uint64_t)job->targetPitch * job->targetHeight;
	const uint32_t vgroups[3] = { (job->vertexCount + 63) / 64, 1, 1 }, vsize[3] = { 64, 1, 1 };
	const uint32_t fgroups[3] = { (job->targetWidth + 7) / 8, (job->targetHeight + 7) / 8, 1 }, fsize[3] = { 8, 8, 1 };
	pipeline_t *p = pipeline(gpu, job);
	const uint32_t targetFlags = job->targetSurface ? RDNA4_PVACCESS_SURFACE : 0;
	const uint32_t targetTask = job->targetSurface ? job->targetSurface : job->task;
	const uint64_t targetVa = job->targetSurface ? 0 : job->targetVa;
	void *host = NULL;
	uint64_t hostBytes = targetBytes;
	uint32_t status = 1;
	uint64_t us = 0, t0 = nowUs(), tIn = 0, tRun = 0;
	kern_return_t kr;

	if (!p || !targetBytes || targetBytes > MAX_BUFFER_BYTES) {
		fprintf(stderr, "rdna4-pvexec: job %u: no pipeline, or a target of %" PRIu64 " bytes\n", job->id, targetBytes);
		return status;
	}

	for (uint32_t i = 0; i < job->bufferCount; i++) {
		if (job->buffers[i].length > hostBytes)
			hostBytes = job->buffers[i].length;
	}

	host = hostBytes <= MAX_BUFFER_BYTES ? malloc(hostBytes) : NULL;
	if (!host)
		goto out;

	for (uint32_t i = 0; i < job->bufferCount; i++) {
		const RDNA4PvBuffer *b = &job->buffers[i];

		kr = upload(gpu, job->task, b->va, 0, host, b->length, &bufs[i]);
		if (kr) {
			fprintf(stderr, "rdna4-pvexec: job %u: buffer %u (%" PRIu64 " bytes): %s\n", job->id, b->slot, b->length,
			        rdna4_error(kr));
			goto out;
		}
	}

	kr = uploadTextures(gpu, job, texels, texDescs, samplers);
	if (kr) {
		fprintf(stderr, "rdna4-pvexec: job %u: textures or samplers: %s\n", job->id, rdna4_error(kr));
		goto out;
	}

	kr = rdna4_alloc(gpu, (uint64_t)p->stride * job->vertexCount, &verts);
	if (!kr)
		kr = upload(gpu, targetTask, targetVa, targetFlags, host, targetBytes, &target);
	if (kr) {
		fprintf(stderr, "rdna4-pvexec: job %u: vertices or target (%" PRIu64 " bytes): %s\n", job->id, targetBytes,
		        rdna4_error(kr));
		goto out;
	}

	tIn = nowUs();

	for (uint32_t instance = 0; instance < job->instanceCount; instance++) {
		kernargs_t va = { .at = 0 }, fa = { .at = 0 };

		if (!vertexArgs(p, job, bufs, verts.gpu, instance, &va) ||
		    !fragmentArgs(p, job, bufs, texDescs, samplers, verts.gpu, target.gpu, &fa)) {
			fprintf(stderr, "rdna4-pvexec: job %u: a kernel argument has no buffer bound\n", job->id);
			goto out;
		}

		kr = rdna4_dispatch(gpu, &p->vertex, vgroups, vsize, va.bytes, va.at, DISPATCH_TIMEOUT_MS, &us);
		if (!kr)
			kr = rdna4_dispatch(gpu, &p->fragment, fgroups, fsize, fa.bytes, fa.at, DISPATCH_TIMEOUT_MS, &us);
		if (kr) {
			fprintf(stderr, "rdna4-pvexec: job %u: draw: %s\n", job->id, rdna4_error(kr));
			goto out;
		}
	}

	tRun = nowUs();
	kr = rdna4_read(gpu, &target, 0, host, targetBytes);
	if (!kr)
		kr = rdna4_pv_access(gpu, targetTask, targetVa, host, targetBytes, targetFlags | RDNA4_PVACCESS_WRITE);
	if (kr)
		fprintf(stderr, "rdna4-pvexec: job %u: target back: %s\n", job->id, rdna4_error(kr));
	else
		status = 0;

	/* where a draw's time goes: copies in (buffers, textures, target), the kernels, the target back */
	if (gVerbose)
		printf("rdna4-pvexec: job %u: %s, %u vertices into %ux%u (%s), %" PRIu64 " us on the GPU; in %" PRIu64
		       " us, kernels %" PRIu64 " us, back %" PRIu64 " us\n", job->id, p->key, job->vertexCount, job->targetWidth,
		       job->targetHeight, job->targetSurface ? "IOSurface" : "texture", us, tIn - t0, tRun - tIn, nowUs() - tRun);

out:
	freeAll(gpu, bufs, job->bufferCount);
	freeAll(gpu, texels, RDNA4_PVJOB_MAX_TEXTURES);
	freeAll(gpu, texDescs, RDNA4_PVJOB_MAX_TEXTURES);
	freeAll(gpu, samplers, RDNA4_PVJOB_MAX_SAMPLERS);
	freeAll(gpu, &verts, 1);
	freeAll(gpu, &target, 1);
	free(host);

	return status;
}

/* One job: 0 when it ran. */
static uint32_t runJob(rdna4_t *gpu, const RDNA4PvJob *job)
{
	rdna4_buffer_t gpuBufs[RDNA4_PVJOB_MAX_BUFFERS] = { { 0 } };
	uint8_t args[RDNA4_MAX_KERNARG] = { 0 };
	uint32_t argBytes = 0, groups[3];
	uint8_t *code = NULL;
	program_t *p = NULL;
	uint64_t us = 0;
	uint32_t status = 1;
	kern_return_t kr;

	if (job->kind == RDNA4_PVJOB_DRAW)
		return runDraw(gpu, job);

	if (job->codeBytes && job->codeBytes <= RDNA4_PVJOB_MAX_CODE)
		code = malloc(job->codeBytes);

	if (code && !rdna4_pv_access(gpu, job->task, job->codeVa, code, job->codeBytes, 0))
		p = program(gpu, job, code);

	free(code);
	if (!p)
		return status;

	for (uint32_t i = 0; i < job->bufferCount; i++) {
		if (job->buffers[i].length > MAX_BUFFER_BYTES ||
		    rdna4_alloc(gpu, job->buffers[i].length, &gpuBufs[i]))
			goto out;
	}

	if (!kernargs(p, job, gpuBufs, args, &argBytes) || !moveBuffers(gpu, job, gpuBufs, 1))
		goto out;

	for (int d = 0; d < 3; d++)
		groups[d] = (job->threads[d] + job->groupSize[d] - 1) / job->groupSize[d];

	kr = rdna4_dispatch(gpu, &p->prog, groups, job->groupSize, args, argBytes, DISPATCH_TIMEOUT_MS, &us);
	if (kr) {
		fprintf(stderr, "rdna4-pvexec: job %u: dispatch: %s\n", job->id, rdna4_error(kr));
		goto out;
	}

	if (moveBuffers(gpu, job, gpuBufs, 0))
		status = 0;

	if (gVerbose)
		printf("rdna4-pvexec: job %u: %s over %ux%ux%u threads, %u buffer(s), %" PRIu64 " us\n", job->id,
		       p->key, job->threads[0], job->threads[1], job->threads[2], job->bufferCount, us);

out:
	for (uint32_t i = 0; i < job->bufferCount; i++) {
		if (gpuBufs[i].handle)
			rdna4_free(gpu, &gpuBufs[i]);
	}

	return status;
}

/* Take jobs until a signal or the runtime goes away; the reason, for the log. */
static const char *serve(rdna4_t *gpu)
{
	int told = 0;

	for (;;) {
		RDNA4PvJob job;
		kern_return_t kr;

		if (gStop)
			return "stopped";

		kr = rdna4_pv_next(gpu, 1000, &job);
		if (kr == kIOReturnUnsupported)
			return "the kext is not a paravirtual build";

		if (kr && kr != kIOReturnTimeout)
			return rdna4_error(kr);

		if (!told) {
			printf("rdna4-pvexec: waiting for compute dispatches\n");
			told = 1;
		}

		if (kr == kIOReturnTimeout)
			continue;

		rdna4_pv_done(gpu, job.id, runJob(gpu, &job));
	}
}

/* Wait out the retry interval; false when a signal came. */
static int waitRetry(void)
{
	for (int i = 0; i < RETRY_SECONDS && !gStop; i++)
		sleep(1);

	return !gStop;
}

int main(int argc, char **argv)
{
	const char *was = NULL;
	int opt;

	while ((opt = getopt(argc, argv, "s:c:v")) != -1) {
		switch (opt) {
		case 's':
			gScript = optarg;
			break;
		case 'c':
			gCache = optarg;
			break;
		case 'v':
			gVerbose = 1;
			break;
		default:
			fprintf(stderr, "usage: %s [-s compile-metallib.sh] [-c cache dir] [-v]\n", argv[0]);
			return 2;
		}
	}

	signal(SIGINT, onSignal);
	signal(SIGTERM, onSignal);
	setvbuf(stdout, NULL, _IOLBF, 0);

	/* Said once per change, not once a minute. */
	do {
		rdna4_t gpu;
		const char *why;
		kern_return_t kr = rdna4_open(&gpu);

		if (kr) {
			why = "no compute runtime (rdna4-compute=7)";
		} else {
			why = serve(&gpu);
			rdna4_close(&gpu);
			gNumPrograms = 0;
		}

		if (!was || strcmp(was, why))
			printf("rdna4-pvexec: %s; trying again every %d s\n", why, RETRY_SECONDS);

		was = why;
	} while (!gStop && waitRetry());

	return 0;
}
