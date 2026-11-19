/*
 * rtlib.c
 * AMDRadeonRDNA4
 *
 * Ray queries for the native Metal device (docs/native-metal-spike.md, gate 5): what air2amdgpu.cpp links into a kernel that calls
 * Metal's intersect (AIR air.intersect.triangle_data). Compiled to gfx1201 bitcode by tools/metal/m3/build.sh (rtlib.h); plain C, no
 * libraries.
 *
 * The acceleration structure is the device's own (tools/metal/native/RDNA4MTLDevice.m buildPrimitive), built by the CPU:
 *   +0   u32 magic 'RAS1', u32 node count, u32 triangle count, u32 0
 *   +16  nodes, 32 bytes each, depth first: f32 lo[3], u32 escape, f32 hi[3], u32 tris (0: an inner node; else first << 4 | count)
 *   then triangles, 48 bytes each: f32 v0[3], v1[3], v2[3], u32 primitive id, u32 geometry index, u32 0
 * A node's escape is the node after its subtree: the traversal is stackless (kernels get no scratch memory for a stack). A ray that
 * meets a node's box goes on to the next node (an inner node's first child) after testing a leaf's triangles; one that misses goes to
 * the escape.
 *
 * Returned, as eight floats (ints as their bits): intersection type (0 none, 1 triangle), distance, primitive id, geometry index,
 * barycentrics u, v (vertex 1's and 2's weights), front facing (clockwise, Metal's default winding), 0.
 * ponytail: cull modes, opacity, masks and intersection functions are not looked at.
 */

typedef float float8 __attribute__((ext_vector_type(8)));
typedef __attribute__((address_space(1))) const float gfloat;
typedef __attribute__((address_space(1))) const unsigned guint;

static float bits(unsigned u) { return __builtin_bit_cast(float, u); }
static float lesser(float a, float b) { return a < b ? a : b; }
static float greater(float a, float b) { return a > b ? a : b; }

float8 rdna4_rt_intersect(float ox, float oy, float oz, float dx, float dy, float dz, float tmin, float tmax, guint *as, int any)
{
	float8 r = { 0, 0, 0, 0, 0, 0, 0, 0 };

	if (!as || as[0] != 0x31534152u)   // 'RAS1'
		return r;
	const unsigned nodes = as[1];
	guint *node = as + 4;
	gfloat *tri = (gfloat *)(node + 8 * nodes);
	const float ix = 1.0f / dx, iy = 1.0f / dy, iz = 1.0f / dz;
	float best = tmax;

	for (unsigned i = 0; i < nodes;) {
		gfloat *n = (gfloat *)(node + 8 * i);
		// the slab test; a division by 0 gives infinities, which the comparisons take as they should
		const float ax = (n[0] - ox) * ix, bx = (n[4] - ox) * ix, ay = (n[1] - oy) * iy, by = (n[5] - oy) * iy;
		const float az = (n[2] - oz) * iz, bz = (n[6] - oz) * iz;
		const float near = greater(greater(lesser(ax, bx), lesser(ay, by)), greater(lesser(az, bz), tmin));
		const float far = lesser(lesser(greater(ax, bx), greater(ay, by)), lesser(greater(az, bz), best));
		const unsigned escape = node[8 * i + 3], tris = node[8 * i + 7];

		if (!(near <= far)) {
			i = escape;
			continue;
		}
		for (unsigned k = tris >> 4, end = (tris >> 4) + (tris & 15); tris && k < end; k++) {
			gfloat *t = tri + 12 * k;
			// Moller-Trumbore
			const float e1x = t[3] - t[0], e1y = t[4] - t[1], e1z = t[5] - t[2];
			const float e2x = t[6] - t[0], e2y = t[7] - t[1], e2z = t[8] - t[2];
			const float px = dy * e2z - dz * e2y, py = dz * e2x - dx * e2z, pz = dx * e2y - dy * e2x;
			const float det = e1x * px + e1y * py + e1z * pz;
			if (det > -1e-12f && det < 1e-12f)
				continue;
			const float inv = 1.0f / det;
			const float sx = ox - t[0], sy = oy - t[1], sz = oz - t[2];
			const float u = (sx * px + sy * py + sz * pz) * inv;
			if (u < 0 || u > 1)
				continue;
			const float qx = sy * e1z - sz * e1y, qy = sz * e1x - sx * e1z, qz = sx * e1y - sy * e1x;
			const float v = (dx * qx + dy * qy + dz * qz) * inv;
			if (v < 0 || u + v > 1)
				continue;
			const float d = (e2x * qx + e2y * qy + e2z * qz) * inv;
			if (d < tmin || d > best)
				continue;
			best = d;
			guint *ids = (guint *)(t + 9);
			r = (float8) { bits(1), d, bits(ids[0]), bits(ids[1]), u, v, bits(det < 0), 0 };
			if (any)
				return r;
		}
		i = tris ? escape : i + 1;
	}
	return r;
}
