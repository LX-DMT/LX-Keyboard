#pragma once

/**
 * @file ui_boot_nvs.h
 * @brief 新固件首次开机是否展示校准引导
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 当前 build_id 是否尚未展示过 Cal 引导 */
bool ui_boot_needs_cal_hint(void);

/** @brief 记录本 build 已展示引导，后续开机进菜单图标 0 */
void ui_boot_mark_hint_shown(void);

#ifdef __cplusplus
}
#endif
