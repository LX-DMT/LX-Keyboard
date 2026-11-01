#pragma once

/**
 * @file ui_screens_internal.h
 * @brief UI 内部屏枚举与子屏 API（ui 组件内使用）
 */

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

/** 逻辑屏幕 ID（Connection 在 ui_screens.c，其余子屏多在 ui_screen_sub.c） */
typedef enum {
    UI_SCR_MENU = 0,
    UI_SCR_CONNECTION,
    UI_SCR_LED,
    UI_SCR_LED_HSV,
    UI_SCR_AUDIO,
    UI_SCR_STANDBY,
    UI_SCR_INSTRUMENT,
    UI_SCR_ASTRONAUT,
    UI_SCR_POWER,       /**< Standby→Power 或键盘 0x83 预览；显示键盘电压/电流 */
    UI_SCR_STRAIN_CAL,
} ui_screen_t;

void ui_screens_hide_all(void);
void ui_screens_show(ui_screen_t screen);
ui_screen_t ui_screens_current(void);
lv_obj_t *ui_screens_get_title(void);

void ui_screen_sub_init(void);
void ui_screen_sub_ensure_init(void);
void ui_screen_sub_bind_nav(ui_screen_t screen);
void ui_screen_sub_hide_all(void);
lv_obj_t *ui_screen_sub_get_panel(ui_screen_t screen);
void ui_screen_sub_on_host_stats(uint8_t cpu, uint8_t gpu, uint8_t ram);
/** @brief 0x82：刷新 Power 屏电压/电流标签 */
void ui_screen_sub_on_keyboard_power(uint16_t bus_mv, int16_t current_ma);
/** @brief 0x84：更新 s_audio_muted 与 switch；进入 Audio 屏时 ui_screen_sub_audio_enter 再刷一次 */
void ui_screen_sub_on_keyboard_audio(bool sound_on);
void ui_screen_sub_power_on_press(void);
void ui_screen_sub_instrument_smooth_start(void);
void ui_screen_sub_instrument_smooth_stop(void);
void ui_screen_sub_leave_instrument(void);
void ui_screen_sub_strain_cal_set_prompt(const char *text);
void ui_screen_sub_strain_cal_on_done(bool ok);
void ui_screen_sub_strain_cal_leave(void);
bool ui_screens_menu_cal_focused(void);
void ui_screens_menu_cal_suppress_auto_enter(void);
bool ui_screen_sub_handle_encoder(lv_obj_t *focused, int32_t delta);
void ui_screen_sub_hsv_flush(void);
void ui_screen_sub_hsv_on_press(void);
/** @return true 表示 release 已被 HSV 消费（如返回菜单），跳过 ui_nav_on_release */
bool ui_screen_sub_hsv_on_release(void);
void ui_screen_sub_hsv_exit_to_menu(void);
void ui_screen_sub_hsv_poll(void);
bool ui_screens_is_surface_dial(void);
bool ui_screens_menu_nav_active(void);
void ui_screens_menu_on_encoder(int32_t delta);
void ui_screens_connection_on_encoder(int32_t delta);
void ui_screen_sub_led_on_encoder(int32_t delta);
void ui_screen_sub_audio_on_encoder(int32_t delta);
void ui_screen_sub_audio_on_press(void);
/** @brief 进入 Audio 屏时按 s_audio_muted 刷新 switch */
void ui_screen_sub_audio_enter(void);
void ui_screen_sub_standby_on_encoder(int32_t delta);
void ui_screen_sub_standby_feature_on_press(void);
