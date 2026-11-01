#pragma once

/**
 * @file haptic_profiles.h
 * @brief SmartKnob 触觉 profile 参数结构
 */

#include <stdint.h>

/** 单档触觉配置：行程、棘轮宽度与强度等 */
typedef struct {
    int32_t position;
    int32_t min_position;
    int32_t max_position;
    float position_width_radians;
    float detent_strength_unit;
    float endstop_strength_unit;
    float snap_point;
    const char *label;
    uint32_t detent_positions_count;
    int32_t detent_positions[5];
    float snap_point_bias;
} haptic_profile_t;

#ifdef __cplusplus
extern "C" {
#endif

const haptic_profile_t *haptic_profiles_get(uint8_t index);
uint8_t haptic_profiles_count(void);

#ifdef __cplusplus
}
#endif
