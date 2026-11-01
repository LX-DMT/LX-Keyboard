#pragma once

/**
 * @file input_strain.h
 * @brief 应变片按压检测与校准流程 API
 */

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t input_strain_init(void);
esp_err_t input_strain_start(void);

/** @brief NVS 中已有当前 build 的按压/释放阈值 */
bool input_strain_is_calibrated(void);

/** @brief 注册校准文案与完成回调（由 app 接到 UI） */
void input_strain_cal_set_ui_hooks(void (*on_prompt)(const char *text), void (*on_done)(bool ok));
/** @brief 进入「旋转启动」等待态，关制动并显示提示 */
void input_strain_cal_prepare(void);
void input_strain_cal_start(void);
void input_strain_cal_cancel(void);
bool input_strain_cal_active(void);
/** @brief 是否在等待旋转或已调度 Starting */
bool input_strain_cal_armed(void);

/** @brief 运行时长按阈值（轮询周期 HX711_POLL_MS=20ms） */
void input_strain_set_long_press_ms(uint32_t ms);
/** @brief 模式切换后清零长按计数 */
void input_strain_reset_long_press_progress(void);

#ifdef __cplusplus
}
#endif
