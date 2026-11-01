#pragma once

/**
 * @file battery_monitor.h
 * @brief 电池监测 API
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t battery_monitor_init(void);
/** @brief 启动 battery_mon 任务（core 0，500ms 轮询） */
esp_err_t battery_monitor_start(void);

/** @brief 是否处于低电量保护态 */
bool battery_monitor_is_low(void);
/** @brief 滤波后电池电压 mV */
uint32_t battery_monitor_voltage_mv(void);

#ifdef __cplusplus
}
#endif
