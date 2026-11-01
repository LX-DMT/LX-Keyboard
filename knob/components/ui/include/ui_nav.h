#pragma once

/**
 * @file ui_nav.h
 * @brief LVGL 编码器导航与焦点组 API
 */

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_nav_init(void);
void ui_nav_clear_group(void);
void ui_nav_bind_object(lv_obj_t *obj);
void ui_nav_on_press(void);
void ui_nav_on_release(void);
void ui_nav_reset_input(void);

/** @brief 重绑焦点组后同步编码器基准，避免首帧大跳 */
void ui_nav_sync_position(void);
/** @brief 强制设定编码器逻辑位置（不读电机） */
void ui_nav_force_position(int32_t position);

#ifdef __cplusplus
}
#endif
