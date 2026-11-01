#pragma once

/**
 * @file app.h
 * @brief 应用层编排：子系统初始化与事件分发
 */

#include "esp_err.h"
#include "knob_events.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 初始化 event_bus、板级、触觉、UI、应变等子系统 */
esp_err_t app_init(void);

/** @brief 启动 UI/电池/应变/灯环任务及 app_dispatch 事件循环 */
esp_err_t app_start(void);

/** @brief 当前应用模式（菜单或某子模式） */
app_mode_t app_get_mode(void);

#ifdef __cplusplus
}
#endif
