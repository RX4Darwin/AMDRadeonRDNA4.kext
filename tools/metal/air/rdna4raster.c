/*
 * rdna4raster.c
 * AMDRadeonRDNA4
 *
 * The fixed-function half of a Metal draw, done in a compute kernel: what
 * the fragment kernel air2amdgcn builds around a fragment function calls
 * (air2amdgcn fragment mode, docs/m0-pvgpu.md). One thread is one pixel of
 * the render target; for every triangle of the draw in order it tests the
 * pixel's centre against the triangle, works out the barycentrics, the
 * generated code interpolates the fragment inputs and calls the fragment
 * function, and the colour is blended into the pixel. The pixel is written
 * once, at the end.
 *
 * Vertex records are what the vertex kernel wrote: `stride` dwords each,
 * the clip-space position at dword `pos`. The render target is linear, 32
 * bits a pixel (RGBA8 or BGRA8 unorm).
 *
 * Textures are sampled here too, in software: a texture argument of the
 * fragment function points at an rdna4_texture the service builds, a
 * sampler argument at the guest's packed sampler state.
 *
 * Built into rdna4raster.ll by build-raster.sh with clang for gfx1201; the
 * compiler service links that with each fragment kernel.
 */

typedef unsigned int uint;

/* Every entry point is inlined into the generated kernel: the kernels run without a stack. */
#define ENTRY __attribute__((always_inline))
typedef float float4 __attribute__((ext_vector_type(4)));
typedef __attribute__((address_space(1))) const float gfloat;
typedef __attribute__((address_space(1))) uint guint;
typedef float float2 __attribute__((ext_vector_type(2)));

/* A texture as the service describes it to a fragment kernel: level 0 of a linear 2D texture. */
struct rdna4_texture {
	guint *data;
	uint width, height, pitch, format;
};

/*
 * The guest's sampler state (the bundle's newSamplerStateWithDescriptor:):
 * min filter in bits 0-1, mag filter 2-3, s and t address modes 5-7 and
 * 8-10 (MTLSamplerMinMagFilter, MTLSamplerAddressMode), normalised
 * coordinates in bit 31.
 */
struct rdna4_sampler {
	uint word;
};

typedef __attribute__((address_space(1))) const struct rdna4_texture gtexture;
typedef __attribute__((address_space(4))) const struct rdna4_sampler csampler;

/* Topologies (MTLPrimitiveType). */
enum {
	kTriangle = 3,
	kTriangleStrip = 4,
};

/* Pixel formats the store knows (MTLPixelFormat). */
enum {
	kRGBA8Unorm = 70,
	kRGBA8UnormSrgb = 71,
	kBGRA8Unorm = 80,
	kBGRA8UnormSrgb = 81,
};

/* Interpolation of a fragment input. */
enum {
	kPerspective = 0,
	kLinear = 1,
	kFlat = 2,
};

/* Blend state of colour attachment 0 (MTLBlendFactor, MTLBlendOperation is add). */
enum {
	kZero = 0,
	kOne = 1,
	kSourceAlpha = 4,
	kOneMinusSourceAlpha = 5,
	kDestinationAlpha = 8,
	kOneMinusDestinationAlpha = 9,
};

/* What one draw's fragment kernel knows, filled by the generated code from its kernargs. */
struct rdna4_draw {
	gfloat *verts;
	uint stride, pos;
	uint topology, count;      /* vertices */
	guint *rt;
	uint pitch, width, height, format;
	float vx, vy, vw, vh;      /* viewport, pixels */
	uint blend;                /* 0 off; else src | dst << 8 | srcAlpha << 16 | dstAlpha << 24 */
};

/* One pixel's state across the triangles. */
struct rdna4_px {
	uint x, y;
	float4 dst;
	int covered;
};

/* A covered triangle: the three records and the pixel's weights. */
struct rdna4_tri {
	gfloat *v[3];
	float p[3];                /* perspective-correct */
	float l[3];                /* screen-space linear */
	float z, w;
	int front;
};

/* The generated kernel keeps these on its stack in blocks of these sizes. */
_Static_assert(sizeof(struct rdna4_draw) <= 128, "rdna4_draw fits its block");
_Static_assert(sizeof(struct rdna4_px) <= 64, "rdna4_px fits its block");
_Static_assert(sizeof(struct rdna4_tri) <= 96, "rdna4_tri fits its block");

/* The draw, from the fragment kernel's kernargs. */
ENTRY void rdna4_draw_init(struct rdna4_draw *d, gfloat *verts, uint stride, uint pos, uint topology, uint count,
		     guint *rt, uint pitch, uint width, uint height, uint format, float vx, float vy, float vw,
		     float vh, uint blend)
{
	d->verts = verts;
	d->stride = stride;
	d->pos = pos;
	d->topology = topology;
	d->count = count;
	d->rt = rt;
	d->pitch = pitch;
	d->width = width;
	d->height = height;
	d->format = format;
	d->vx = vx;
	d->vy = vy;
	d->vw = vw;
	d->vh = vh;
	d->blend = blend;
}

static float clamp01(float v)
{
	return v < 0 ? 0 : v > 1 ? 1 : v;
}

static float4 unpack(uint v, uint format)
{
	const float4 c = { (v & 0xff) / 255.0f, ((v >> 8) & 0xff) / 255.0f, ((v >> 16) & 0xff) / 255.0f,
			   (v >> 24) / 255.0f };

	if (format == kBGRA8Unorm || format == kBGRA8UnormSrgb)
		return (float4){ c.z, c.y, c.x, c.w };

	return c;
}

static uint pack(float4 c, uint format)
{
	uint r = (uint)(clamp01(c.x) * 255.0f + 0.5f), g = (uint)(clamp01(c.y) * 255.0f + 0.5f);
	uint b = (uint)(clamp01(c.z) * 255.0f + 0.5f), a = (uint)(clamp01(c.w) * 255.0f + 0.5f);

	if (format == kBGRA8Unorm || format == kBGRA8UnormSrgb)
		return b | g << 8 | r << 16 | a << 24;

	return r | g << 8 | b << 16 | a << 24;
}

enum {
	kNearest = 0,
	kLinearFilter = 1,
	kClampToEdge = 0,
	kMirrorClampToEdge = 1,
	kRepeat = 2,
	kMirrorRepeat = 3,
	kClampToZero = 4,
};

/* A texel coordinate under an address mode; -1 for a texel that reads as zero. */
static int address(int i, int n, uint mode)
{
	switch (mode) {
	case kRepeat:
		i %= n;
		return i < 0 ? i + n : i;
	case kMirrorRepeat: {
		int m = i % (2 * n);

		if (m < 0)
			m += 2 * n;
		return m < n ? m : 2 * n - 1 - m;
	}
	case kMirrorClampToEdge:
		i = i < 0 ? -1 - i : i;
		return i >= n ? n - 1 : i;
	case kClampToZero:
		return i < 0 || i >= n ? -1 : i;
	default:
		return i < 0 ? 0 : i >= n ? n - 1 : i;
	}
}

static float4 texel(gtexture *t, int x, int y)
{
	if (x < 0 || y < 0)
		return (float4){ 0, 0, 0, 0 };

	return unpack(t->data[y * (t->pitch / 4) + x], t->format);
}

/* air.sample_texture_2d: level 0, nearest or bilinear. */
ENTRY float4 rdna4_sample_2d(gtexture *t, csampler *s, float2 uv)
{
	const uint w = s->word;
	const uint filter = (w >> 2) & 3;   /* the magnification filter for both: no mipmaps */
	const uint su = (w >> 5) & 7, sv = (w >> 8) & 7;
	const float x = (w >> 31) ? uv.x * t->width : uv.x, y = (w >> 31) ? uv.y * t->height : uv.y;

	if (!t->width || !t->height)
		return (float4){ 0, 0, 0, 0 };

	if (filter == kNearest)
		return texel(t, address((int)__builtin_floorf(x), t->width, su), address((int)__builtin_floorf(y), t->height, sv));

	const float fx = x - 0.5f, fy = y - 0.5f;
	const int x0 = (int)__builtin_floorf(fx), y0 = (int)__builtin_floorf(fy);
	const float ax = fx - x0, ay = fy - y0;
	const int u0 = address(x0, t->width, su), u1 = address(x0 + 1, t->width, su);
	const int v0 = address(y0, t->height, sv), v1 = address(y0 + 1, t->height, sv);
	const float4 top = texel(t, u0, v0) * (1 - ax) + texel(t, u1, v0) * ax;
	const float4 bottom = texel(t, u0, v1) * (1 - ax) + texel(t, u1, v1) * ax;

	return top * (1 - ay) + bottom * ay;
}

/* The pixel this thread shades, and its current colour; false outside the target. */
ENTRY int rdna4_px_begin(const struct rdna4_draw *d, uint x, uint y, struct rdna4_px *px)
{
	if (x >= d->width || y >= d->height)
		return 0;

	px->x = x;
	px->y = y;
	px->dst = unpack(d->rt[y * (d->pitch / 4) + x], d->format);
	px->covered = 0;

	return 1;
}

/* The triangles of the draw. */
ENTRY uint rdna4_tri_count(const struct rdna4_draw *d)
{
	if (d->topology == kTriangleStrip)
		return d->count >= 3 ? d->count - 2 : 0;

	return d->topology == kTriangle ? d->count / 3 : 0;
}

/* Clip space to pixels: x right, y down. */
static void toScreen(const struct rdna4_draw *d, gfloat *v, float *sx, float *sy, float *sz, float *rw)
{
	const float w = v[3];

	*rw = 1.0f / w;
	*sx = d->vx + (v[0] * *rw * 0.5f + 0.5f) * d->vw;
	*sy = d->vy + (0.5f - v[1] * *rw * 0.5f) * d->vh;
	*sz = v[2] * *rw;
}

/* Whether triangle `t` covers the pixel's centre; its weights if so. */
ENTRY int rdna4_tri_cover(const struct rdna4_draw *d, const struct rdna4_px *px, uint t, struct rdna4_tri *tri)
{
	uint i0 = 3 * t, i1 = 3 * t + 1, i2 = 3 * t + 2;
	float x[3], y[3], z[3], rw[3], e[3], area;
	const float cx = px->x + 0.5f, cy = px->y + 0.5f;

	if (d->topology == kTriangleStrip) {
		/* every other triangle of a strip has its first two vertices swapped, to keep its winding */
		i0 = t + (t & 1);
		i1 = t + 1 - (t & 1);
		i2 = t + 2;
	}

	tri->v[0] = d->verts + i0 * d->stride;
	tri->v[1] = d->verts + i1 * d->stride;
	tri->v[2] = d->verts + i2 * d->stride;

	for (int k = 0; k < 3; k++) {
		if (tri->v[k][d->pos + 3] <= 0)
			return 0;   /* behind the eye: no clipping yet */

		toScreen(d, tri->v[k] + d->pos, &x[k], &y[k], &z[k], &rw[k]);
	}

	area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
	if (area == 0)
		return 0;

	e[0] = (x[2] - x[1]) * (cy - y[1]) - (y[2] - y[1]) * (cx - x[1]);
	e[1] = (x[0] - x[2]) * (cy - y[2]) - (y[0] - y[2]) * (cx - x[2]);
	e[2] = (x[1] - x[0]) * (cy - y[0]) - (y[1] - y[0]) * (cx - x[0]);

	/* both windings: the edge values share the area's sign inside */
	for (int k = 0; k < 3; k++) {
		e[k] /= area;
		if (e[k] < 0)
			return 0;
	}

	const float pw = e[0] * rw[0] + e[1] * rw[1] + e[2] * rw[2];

	for (int k = 0; k < 3; k++) {
		tri->l[k] = e[k];
		tri->p[k] = e[k] * rw[k] / pw;
	}

	tri->z = e[0] * z[0] + e[1] * z[1] + e[2] * z[2];
	tri->w = pw;
	/* counter-clockwise on screen (y down: negative area) is front, Metal's default winding */
	tri->front = area < 0;

	return 1;
}

/* Fragment input at dword `off` of the records, `n` <= 4 components. */
ENTRY float4 rdna4_interp(const struct rdna4_tri *tri, uint off, uint n, uint mode)
{
	float4 r = { 0, 0, 0, 0 };

	for (uint c = 0; c < n && c < 4; c++) {
		const float a = tri->v[0][off + c], b = tri->v[1][off + c], v = tri->v[2][off + c];
		float x;

		if (mode == kFlat)
			x = a;
		else if (mode == kLinear)
			x = a * tri->l[0] + b * tri->l[1] + v * tri->l[2];
		else
			x = a * tri->p[0] + b * tri->p[1] + v * tri->p[2];

		r[c] = x;
	}

	return r;
}

/* [[position]] in a fragment function: the pixel's centre, depth and 1/w. */
ENTRY float4 rdna4_frag_position(const struct rdna4_px *px, const struct rdna4_tri *tri)
{
	return (float4){ px->x + 0.5f, px->y + 0.5f, tri->z, tri->w };
}

ENTRY int rdna4_front_facing(const struct rdna4_tri *tri)
{
	return tri->front;
}

static float factor(uint f, float4 src, float4 dst, int alpha, int c)
{
	switch (f) {
	case kZero:
		return 0;
	case kSourceAlpha:
		return src.w;
	case kOneMinusSourceAlpha:
		return 1 - src.w;
	case kDestinationAlpha:
		return dst.w;
	case kOneMinusDestinationAlpha:
		return 1 - dst.w;
	default:
		return 1;
	}
}

/* The fragment function's colour, blended into the pixel. */
ENTRY void rdna4_px_blend(const struct rdna4_draw *d, struct rdna4_px *px, float4 src)
{
	float4 out;

	px->covered = 1;

	if (!d->blend) {
		px->dst = src;
		return;
	}

	for (int c = 0; c < 4; c++) {
		const uint sf = (d->blend >> (c == 3 ? 16 : 0)) & 0xff, df = (d->blend >> (c == 3 ? 24 : 8)) & 0xff;

		out[c] = src[c] * factor(sf, src, px->dst, c == 3, c) + px->dst[c] * factor(df, src, px->dst, c == 3, c);
	}

	px->dst = out;
}

ENTRY void rdna4_px_end(const struct rdna4_draw *d, const struct rdna4_px *px)
{
	if (px->covered)
		d->rt[px->y * (d->pitch / 4) + px->x] = pack(px->dst, d->format);
}
