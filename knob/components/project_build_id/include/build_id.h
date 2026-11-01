#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Calibration compatibility version, not a firmware build timestamp.
 * Version 0 preserves the calibration in the validated original firmware.
 * Change only when hardware/calibration representation becomes incompatible.
 */
#define LX_CALIBRATION_SCHEMA_VERSION 0u

/** Legacy name retained for existing NVS keys and callers. Stable across builds. */
extern const uint32_t g_build_id;

#ifdef __cplusplus
}
#endif
