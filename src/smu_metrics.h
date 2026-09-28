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

#endif
