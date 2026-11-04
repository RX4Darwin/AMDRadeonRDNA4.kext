/*
 *  rdna4pvjob.h
 *  AMDRadeonRDNA4
 *
 *  A compute dispatch of a Metal client on the paravirtual GPU (PVGPU
 *  builds), as the kext's host hands it to the compiler service in user
 *  space (userspace/rdna4-pvexec) through RDNA4ComputeClient: PvNext gives
 *  the next one, PvAccess reads and writes the client's memory by the
 *  task's GPU addresses, PvDone ends it. The host waits for the job before
 *  it completes the client's command buffer.
 *
 *  A compute job's function is the metallib the client compiled (one AIR
 *  module); the service compiles it for the threadgroup size, runs it over
 *  the grid with the buffers bound by slot, and writes the buffers back.
 *
 *  A draw job carries the render pipeline's vertex function (code*) and
 *  fragment function (fragment*), the buffers of both stages, the vertices
 *  to draw and the render target: a texture in the client's memory or an
 *  IOSurface (a CAMetalLayer's drawable). The service runs the vertex
 *  function per vertex and rasterizes the triangles with the fragment
 *  function in compute kernels (tools/metal/air/rdna4raster.c), and writes
 *  the target back.
 */

#ifndef RDNA4PvJob_h
#define RDNA4PvJob_h

#include <stdint.h>

#define RDNA4_PVJOB_MAX_BUFFERS 31u          /* Metal's buffer slots */
#define RDNA4_PVJOB_MAX_TEXTURES 16u         /* of a draw's fragment function */
#define RDNA4_PVJOB_MAX_SAMPLERS 16u
#define RDNA4_PVJOB_MAX_ATTRIBUTES 31u       /* a vertex descriptor's attributes and layouts */
#define RDNA4_PVJOB_MAX_CODE    (1u << 20)   /* metallib bytes */

enum {
	RDNA4_PVJOB_COMPUTE = 0,
	RDNA4_PVJOB_DRAW = 1,
};

enum {
	RDNA4_PVJOB_VERTEX = 0,      /* a buffer of the vertex function (or the compute kernel) */
	RDNA4_PVJOB_FRAGMENT = 1,
	RDNA4_PVJOB_INDEX = 2,       /* a draw's indices (slot 0), vertexCount of them */
};

enum {
	RDNA4_PVJOB_NOT_INDEXED = 0,
	RDNA4_PVJOB_UINT16 = 1,
	RDNA4_PVJOB_UINT32 = 2,
};

/* PvAccess flags: write, and `task` naming an IOSurface by ID, `va` an offset in it. */
#define RDNA4_PVACCESS_WRITE   1u
#define RDNA4_PVACCESS_SURFACE 2u

typedef struct {
	uint32_t slot;          /* [[buffer(slot)]] */
	uint32_t stage;         /* RDNA4_PVJOB_VERTEX or _FRAGMENT */
	uint64_t va;            /* the bound offset, in the task's GPU space */
	uint64_t length;        /* from va to the end of the buffer */
} RDNA4PvBuffer;

/* A texture of a draw: level 0 of a linear 2D texture in the client's memory, or an IOSurface. */
typedef struct {
	uint32_t slot;          /* [[texture(slot)]] */
	uint32_t surface;       /* an IOSurface ID, or 0 */
	uint64_t va;            /* else the texture's memory in the client */
	uint32_t width, height;
	uint32_t pitch;         /* bytes per row */
	uint32_t format;        /* MTLPixelFormat; bits 16-31 a 3D texture's depth, its slices then the rows of `height` */
} RDNA4PvTexture;

/* A sampler of a draw: the guest's packed state (tools/metal/air/rdna4raster.c). */
typedef struct {
	uint32_t slot;          /* [[sampler(slot)]] */
	uint32_t word;
} RDNA4PvSampler;

/* A vertex attribute of the pipeline's vertex descriptor ([[attribute(index)]]): format 0 when unused. */
typedef struct {
	uint8_t format;         /* MTLVertexFormat */
	uint8_t buffer;         /* the buffer slot, its layout */
	uint16_t offset;        /* bytes into the element */
} RDNA4PvAttribute;

/* A buffer layout of the vertex descriptor, by buffer slot. */
typedef struct {
	uint32_t stride;
	uint16_t stepRate;
	uint8_t stepFunction;   /* MTLVertexStepFunction */
	uint8_t reserved;
} RDNA4PvLayout;

typedef struct {
	uint32_t id;
	uint32_t task;
	uint64_t codeVa;        /* the metallib (a draw's vertex function), in the task's GPU space */
	uint32_t codeBytes;
	uint32_t kind;          /* RDNA4_PVJOB_COMPUTE or _DRAW */
	uint32_t threads[3];    /* the grid, in threads */
	uint32_t groupSize[3];  /* threads per threadgroup */
	uint32_t bufferCount;
	uint32_t reserved2;
	RDNA4PvBuffer buffers[RDNA4_PVJOB_MAX_BUFFERS];

	/* draws */
	uint64_t fragmentVa;    /* the fragment function's metallib */
	uint32_t fragmentBytes;
	uint32_t topology;      /* MTLPrimitiveType */
	uint32_t vertexStart, vertexCount;
	uint32_t instanceCount;
	uint32_t indexType;     /* RDNA4_PVJOB_NOT_INDEXED, _UINT16 or _UINT32 */
	uint64_t targetVa;      /* a texture's memory in the client, or 0 */
	uint32_t targetSurface; /* else an IOSurface ID */
	uint32_t targetFormat;  /* MTLPixelFormat */
	uint32_t targetWidth, targetHeight;
	uint32_t targetPitch;   /* bytes per row */
	uint32_t blend;         /* rdna4raster.c: 0 off, else the enable bit and the factors */
	uint32_t viewport[4];   /* x, y, w, h: float bits */
	uint32_t scissor[4];    /* x, y, w, h: pixels */
	uint32_t textureCount, samplerCount;
	RDNA4PvTexture textures[RDNA4_PVJOB_MAX_TEXTURES];
	RDNA4PvSampler samplers[RDNA4_PVJOB_MAX_SAMPLERS];
	uint32_t attributeMask; /* the vertex descriptor's attributes, a bit each; 0 without one */
	RDNA4PvAttribute attributes[RDNA4_PVJOB_MAX_ATTRIBUTES];
	RDNA4PvLayout layouts[RDNA4_PVJOB_MAX_ATTRIBUTES];
} RDNA4PvJob;

#define RDNA4_PVJOB_BYTES (56 + RDNA4_PVJOB_MAX_BUFFERS * 24 + 104 + RDNA4_PVJOB_MAX_TEXTURES * 32 + \
			   RDNA4_PVJOB_MAX_SAMPLERS * 8 + 4 + RDNA4_PVJOB_MAX_ATTRIBUTES * 12)
#ifdef __cplusplus
static_assert(sizeof(RDNA4PvJob) == RDNA4_PVJOB_BYTES, "RDNA4PvJob layout");
#else
_Static_assert(sizeof(RDNA4PvJob) == RDNA4_PVJOB_BYTES, "RDNA4PvJob layout");
#endif

#endif /* RDNA4PvJob_h */
