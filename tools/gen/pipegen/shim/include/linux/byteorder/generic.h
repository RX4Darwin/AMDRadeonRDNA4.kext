/* dscgen shim: drm_dsc_helper.c packs the PPS big-endian */
#include <linux/types.h>
#define cpu_to_be16(x) __builtin_bswap16(x)
