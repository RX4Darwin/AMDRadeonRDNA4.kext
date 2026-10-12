/*
 * SMU 14.0 metrics table offsets.
 *
 * These are byte offsets in SmuMetrics_t from smu14_driver_if_v14_0.h.
 * Keep the values in one C-compatible header so the kext, emulator, and
 * host checks cannot silently drift together.
 */
#ifndef RDNA4_SMU_METRICS_H
#define RDNA4_SMU_METRICS_H

#define RDNA4_SMU_METRICS_AVG_GFXCLK_POST_DS 48u
#define RDNA4_SMU_METRICS_AVG_MEMCLK_POST_DS 56u
#define RDNA4_SMU_METRICS_AVG_SOCKET_POWER  136u
#define RDNA4_SMU_METRICS_AVG_TEMPERATURE   140u
#define RDNA4_SMU_METRICS_AVG_FAN_RPM       170u

/* Fields for the power-management readout (rdna4-run sensors); offsets from
 * the SmuMetrics_t layout at smu14_driver_if_v14_0.h:1649-1727 with
 * PPCLK_COUNT 11 (:456-467), SVI_PLANE_COUNT 4 (:558-563), TEMP_COUNT 12 and
 * THROTTLER_COUNT 21 (:216). */
#define RDNA4_SMU_METRICS_CURR_CLOCK           0u    /* uint32_t[PPCLK_COUNT] */
#define RDNA4_SMU_METRICS_PPCLK_GFXCLK         0u    /* PPCLK_GFXCLK (:456) */
#define RDNA4_SMU_METRICS_AVG_GFXCLK_PRE_DS   46u
#define RDNA4_SMU_METRICS_MOVING_AVG_GFX_ACT  90u
#define RDNA4_SMU_METRICS_COUNTER            104u    /* uint32_t */
#define RDNA4_SMU_METRICS_AVG_VOLTAGE        108u    /* uint16_t[SVI_PLANE_COUNT], mV */
#define RDNA4_SMU_METRICS_SVI_VDD_GFX          0u    /* SVI_PLANE_VDD_GFX (:559) */
#define RDNA4_SMU_METRICS_AVG_CURRENT        116u    /* uint16_t[SVI_PLANE_COUNT], A */
#define RDNA4_SMU_METRICS_AVG_GFX_ACTIVITY   124u
#define RDNA4_SMU_METRICS_AVG_UCLK_ACTIVITY  126u
#define RDNA4_SMU_METRICS_THROTTLING_PCT     172u    /* uint8_t[THROTTLER_COUNT] */
#define RDNA4_SMU_METRICS_THROTTLER_COUNT     21u

#endif
