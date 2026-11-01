/**
 * @file haptic_profiles.c
 * @brief SmartKnob 触觉 profile 预设表
 *
 * 各 profile 定义档位宽度、棘轮强度、行程上下限等，供 haptics_set_profile 选用。
 */

#include "haptic_profiles.h"

#include <math.h>

static const haptic_profile_t s_profiles[] = {
    {0, 0, -1, 10 * (float)M_PI / 180, 1, 1, 1.1f, "Unbounded\nNo detents", 0, {}, 0},
    {0, 0, -1, 1 * (float)M_PI / 180, 1, 1, 1.1f, "Hue\n1deg", 0, {}, 0},
    {0, 0, 99, (360.0f / 100.0f) * (float)M_PI / 180, 1, 1, 1.1f, "HSV Sat\n1rev", 0, {}, 0},
    {0, 0, 99, (360.0f / 100.0f) * (float)M_PI / 180, 1, 1, 1.1f, "HSV Val\n1rev", 0, {}, 0},
    {0, 0, -1, 3 * (float)M_PI / 180, 1, 1, 1.1f, "Return-to-center", 0, {}, 0},
    {127, 0, 255, 1 * (float)M_PI / 180, 0, 1, 1.1f, "Fine values\nNo detents", 0, {}, 0},
    {127, 0, 255, 1 * (float)M_PI / 180, 1, 1, 1.1f, "Fine values\nWith detents", 0, {}, 0},
    {0, 0, 31, 8.225806452f * (float)M_PI / 180, 2, 1, 1.1f, "Coarse values\nStrong detents", 0, {}, 0},
    {0, 0, 31, 8.225806452f * (float)M_PI / 180, 0.2f, 1, 1.1f, "Coarse values\nWeak detents", 0, {}, 0},
    {0, 0, 31, 7 * (float)M_PI / 180, 2.5f, 1, 0.7f, "Magnetic detents", 4, {2, 10, 21, 22}, 0},
    {0, -6, 6, 60 * (float)M_PI / 180, 1, 1, 0.55f, "Return-to-center\nwith detents", 0, {}, 0.4f},
};

const haptic_profile_t *haptic_profiles_get(uint8_t index)
{
    if (index >= (sizeof(s_profiles) / sizeof(s_profiles[0]))) {
        return &s_profiles[0];
    }
    return &s_profiles[index];
}

uint8_t haptic_profiles_count(void)
{
    return (uint8_t)(sizeof(s_profiles) / sizeof(s_profiles[0]));
}
