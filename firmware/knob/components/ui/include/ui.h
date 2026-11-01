#pragma once

/**
 * @file ui.h
 * @brief UI 子系统入口：初始化、事件转发、模式请求
 */

#include "esp_err.h"
#include "knob_events.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ui_init(void);
esp_err_t ui_start(void);
/** @brief 由 app 分发旋转/按压等事件到当前屏 */
void ui_on_event(const knob_event_t *event);
/** @brief 异步请求切换应用模式（入队到 ui_task） */
void ui_request_on_mode(app_mode_t mode);
void ui_post_remote_cmd(remote_cmd_t cmd, uint8_t arg0, uint8_t arg1, uint8_t arg2);

#ifdef __cplusplus
}
#endif
