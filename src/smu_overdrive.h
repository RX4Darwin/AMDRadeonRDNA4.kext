/*
 * SMU 14.0.2 OverDrive offsets (fan control, docs/fan-control-plan.md), driver interface 0x2E.
 *
 * Byte offsets from Linux's smu14_driver_if_v14_0.h (OverDriveTable_t, OverDriveLimits_t, PPTable_t) and smu_v14_0_2_pptable.h
 * (struct smu_14_0_2_powerplay_table, the combo table), computed by compiling those headers (2026-11-25).
 */
#ifndef RDNA4_SMU_OVERDRIVE_H
#define RDNA4_SMU_OVERDRIVE_H

#define RDNA4_SMU_TABLE_COMBO_PPTABLE 1u      /* TransferTableSmu2Dram table ids */
#define RDNA4_SMU_TABLE_OVERDRIVE     8u
#define RDNA4_SMU_DRIVER_IF_VERSION   0x2eu   /* the layouts below are this interface's (Linux's header, Oct 2024) */
#define RDNA4_SMU_DRIVER_IF_RETAIL    0x33u   /* what retail RX 9070 firmware reports; amdgpu runs it with the 0x2E layouts */

/* the combo table: the PPTable at 1344; in it PFE_Settings.FeaturesToRun at 4, SkuTable at 28, and in that the basic OverDrive limits */
#define RDNA4_SMU_COMBO_PPTABLE       1344u
#define RDNA4_SMU_PP_FEATURES_TO_RUN  4u
#define RDNA4_SMU_PP_OD_LIMITS_MIN    (28u + 2720u)
#define RDNA4_SMU_PP_OD_LIMITS_MAX    (28u + 2816u)

/* SkuTable.MsgLimits.FanStopTempMin / Max (uint16_t, degrees C): the zero-RPM stop temperature's limits, from the PPTable */
#define RDNA4_SMU_PP_FAN_STOP_TEMP_MIN (28u + 2664u)
#define RDNA4_SMU_PP_FAN_STOP_TEMP_MAX (28u + 2666u)

#define RDNA4_SMU_OD_TABLE_BYTES      156u    /* sizeof(OverDriveTable_t) */
#define RDNA4_SMU_ODL_BYTES           96u     /* sizeof(OverDriveLimits_t) */

/* OverDriveLimits_t (96 bytes) */
#define RDNA4_SMU_ODL_FEATURE_MASK    0u
#define RDNA4_SMU_ODL_FAN_PWM         36u     /* uint8_t [6] */
#define RDNA4_SMU_ODL_FAN_TEMP        42u     /* uint8_t [6] */
#define RDNA4_SMU_ODL_FAN_MIN_PWM     48u     /* uint16_t */
#define RDNA4_SMU_ODL_ACOUSTIC_TARGET 50u
#define RDNA4_SMU_ODL_ACOUSTIC_LIMIT  52u
#define RDNA4_SMU_ODL_FAN_TARGET_TEMP 54u
#define RDNA4_SMU_ODL_FAN_ZERO_RPM    56u     /* uint8_t */

/* OverDriveTable_t (156 bytes) */
#define RDNA4_SMU_OD_FEATURE_MASK     0u
#define RDNA4_SMU_OD_FAN_PWM          40u     /* uint8_t [6], percent */
#define RDNA4_SMU_OD_FAN_TEMP         46u     /* uint8_t [6], degrees C */
#define RDNA4_SMU_OD_FAN_MIN_PWM      52u     /* uint16_t */
#define RDNA4_SMU_OD_ACOUSTIC_TARGET  54u     /* uint16_t, RPM */
#define RDNA4_SMU_OD_ACOUSTIC_LIMIT   56u
#define RDNA4_SMU_OD_FAN_TARGET_TEMP  58u     /* uint16_t, degrees C */
#define RDNA4_SMU_OD_FAN_ZERO_RPM     60u     /* uint8_t */
#define RDNA4_SMU_OD_FAN_ZERO_RPM_STOP 61u    /* uint8_t, degrees C */
#define RDNA4_SMU_OD_FAN_MODE         62u     /* uint8_t: 0 auto, 1 manual linear (the curve) */
#define RDNA4_SMU_OD_MAX_OP_TEMP      63u

/* PP_OD_FEATURE_*_BIT in FeatureCtrlMask */
#define RDNA4_SMU_OD_FEATURE_FAN_CURVE  4u
#define RDNA4_SMU_OD_FEATURE_FAN_LEGACY 5u
#define RDNA4_SMU_OD_FEATURE_ZERO_FAN   11u

#endif
