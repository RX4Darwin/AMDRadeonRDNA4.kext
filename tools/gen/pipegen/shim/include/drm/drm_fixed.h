/* dscgen shim: drm_dsc_helper.c includes it for its debug print format only */
#define FXP_Q4_FMT "%d.%04d"
#define FXP_Q4_ARGS(q4) ((q4) >> 4), (((q4) & 0xf) * 625)
