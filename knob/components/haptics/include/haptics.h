#pragma once

/**
 * @file haptics.h
 * @brief 触觉反馈与 FOC 电机控制 API
 *
 * 运行在独立 haptics 任务（core 0）：SimpleFOC + SmartKnob 棘轮算法。
 * 其他任务通过本头文件异步设置 profile、制动、仪表模式等。
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t haptics_init(void);
esp_err_t haptics_start(void);
void haptics_set_profile(uint8_t profile_id);
/** @brief 立即应用 profile 与档位（UI 切屏用），清除异步 pending */
void haptics_apply_profile_and_position(uint8_t profile_id, int32_t position);
/** @brief 菜单多档棘轮：每项一档，首尾可循环（max_position=-1） */
void haptics_apply_menu_detents(int32_t position, int32_t item_count);
void haptics_set_instrument_active(bool active);
/** @brief false=仪表介绍阶段零力矩；true=跟踪 CPU 弧位置 */
void haptics_set_instrument_hold(bool hold);
/** @brief 锁定当前编码器角与 CPU 弧为仪表基准（需先 set arc） */
void haptics_instrument_latch_live_origin(void);
void haptics_set_cpu_load(float cpu_pct);
/** @brief CPU 环弧位置 20–80，电机跟踪以保持与 UI 同步 */
void haptics_set_instrument_cpu_arc(float arc_pos);
void haptics_on_press_edge(bool pressed);
void haptics_on_release_edge(void);
/** @brief false=零力矩且无棘轮脉冲（如低电量、校准等待旋转） */
void haptics_set_brake_enabled(bool enabled);
bool haptics_brake_enabled(void);
int32_t haptics_get_position(void);
void haptics_set_position(int32_t position);

/** @brief 请求完整 initFOC 对中（轴微动），在 haptics 任务执行 */
void haptics_request_foc_calibration(void);
/** @brief FOC 校准轮询：0=空闲 1=进行中 2=成功 3=失败 */
int haptics_foc_calibration_poll(void);

/** @brief 磁编码器轴角（rad），仅 haptics 任务更新缓存 */
float haptics_get_encoder_angle_rad(void);

#ifdef __cplusplus
}
#endif
