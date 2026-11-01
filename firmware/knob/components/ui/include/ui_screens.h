#pragma once

/**
 * @file ui_screens.h
 * @brief 屏幕切换与菜单/开机/低电等全局 UI 行为
 */

#include "knob_events.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_screens_init(void);
void ui_screens_show_menu(void);
void ui_screens_back(void);
void ui_screens_on_host_stats(uint8_t cpu, uint8_t gpu, uint8_t ram);
/** @brief 键盘 0x82：更新 Power 屏母线电压与电流 */
void ui_screens_on_keyboard_power(uint16_t bus_mv, int16_t current_ma);
/** @brief 键盘 0x84：同步 Audio switch（不发 REMOTE_AUDIO_*） */
void ui_screens_on_keyboard_audio(bool sound_on);
/** @brief 键盘 0x83：右键按住临时切 Power 屏，松开恢复 */
void ui_screens_power_preview(bool pressed);
bool ui_screens_power_preview_active(void);
void ui_screens_on_mode(app_mode_t mode);
uint8_t ui_screens_get_menu_return_idx(void);
void ui_screens_set_menu_return_idx(uint8_t idx);
/** @brief 按 menu_return_idx 恢复菜单多档棘轮 */
void ui_screens_apply_menu_haptics(void);
void ui_screens_on_dial_mode_exit(void);
/** @brief 切屏后短暂忽略编码器，防止惯性误触 */
void ui_screens_arm_menu_input_guard(uint32_t duration_ms);
bool ui_screens_menu_input_guarded(void);
void ui_screens_set_low_power(bool on);
bool ui_screens_boot_splash_active(void);
void ui_screens_boot_splash_kick_if_pending(void);
void ui_screens_menu_cal_enter(void);

#ifdef __cplusplus
}
#endif
