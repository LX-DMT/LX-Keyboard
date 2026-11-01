#pragma once

/**
 * @file motor_foc_nvs.h
 * @brief 电机 FOC 校准结果 NVS 读写
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 从 NVS 加载 FOC 参数；build_id 不匹配返回 false */
bool motor_foc_nvs_load(float *zero_electric_angle, int8_t *sensor_direction);
/** @brief 保存 FOC 对中结果并写入当前 build_id */
esp_err_t motor_foc_nvs_save(float zero_electric_angle, int8_t sensor_direction);

#ifdef __cplusplus
}
#endif
