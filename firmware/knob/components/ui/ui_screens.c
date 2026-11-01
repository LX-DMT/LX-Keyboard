/**
 * @file ui_screens.c
 * @brief 主屏路由：环形菜单、Connection、开机 Welcome、校准入口
 *
 * 管理 UI_SCR_* 切换；键盘 Power 预览覆盖（0x83）与 host→UI 同步入口。
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ui_screens.h"
#include "ui_screens_internal.h"
#include "ui_assets.h"

#include "display.h"
#include "esp_log.h"
#include "event_bus.h"
#include "haptics.h"
#include "input_strain.h"
#include "knob_events.h"
#include "ui.h"
#include "ui_i18n.h"
#include "ui_nav.h"

static lv_obj_t *s_low_power_panel;
static lv_obj_t *s_low_power_lbl;
static lv_obj_t *s_boot_splash_panel;
static lv_obj_t *s_boot_splash_lbl;
static lv_timer_t *s_boot_splash_timer;
static bool s_boot_splash_pending;
static bool s_boot_splash_active;
static bool s_boot_splash_kicked;

static const char *TAG = "ui_screens";

static lv_obj_t *s_menu;
static lv_obj_t *s_menu_caption;
static lv_obj_t *s_connection;
static lv_obj_t *s_dial;
static lv_obj_t *s_title;
static lv_obj_t *s_conn_btns[4];
static ui_screen_t s_screen = UI_SCR_MENU;
static bool s_surface_dial;
static bool s_kb_power_preview;
static ui_screen_t s_kb_power_preview_saved_screen;
static bool s_kb_power_preview_saved_dial;
static uint8_t s_menu_focus_idx;
static uint8_t s_menu_return_idx;
static bool s_menu_bind_active;
static uint8_t s_menu_caption_pending_idx;
static uint8_t s_conn_focus_idx;
static uint32_t s_menu_input_guard_start_ms;
static uint32_t s_menu_input_guard_duration_ms;

static lv_timer_t *s_menu_cal_timer;
static uint8_t s_menu_cal_cd_step;
static uint8_t s_menu_cal_phase;
static bool s_menu_cal_countdown_active;
static char s_menu_caption_custom[48];

static const uint8_t kMenuIdxCal = 5;
static const uint8_t kMenuIdxDial = 4;
static const uint32_t kMenuCalCountdownTickMs = 1000;
static const uint32_t kMenuCalCaptionDwellMs = 2000;
static const uint8_t kMenuCalCountdownSecs = 5;

static bool s_menu_cal_suppress_auto;

static const uint32_t kBootSplashFadeMs = 500;
static const uint32_t kBootSplashHoldMs = 2000;

static const char *const kBootSplashWelcome = "Welcome!";

enum {
    CAL_MENU_PHASE_CAPTION = 0,
    CAL_MENU_PHASE_COUNTDOWN,
};

static const int kMenuCenterBtnSize = 100;
static const int kMenuNearBtnSize = 50;
static const int kMenuFarBtnSize = 32;
static const int kMenuImgSrcSize = 128;
static const int kMenuScrollPad = (240 - kMenuCenterBtnSize) / 2;
static const uint32_t kMenuSizeAnimMs = 300;
static const uint32_t kMenuCaptionFadeMs = 200;
static const int kMenuCaptionYOffset = 78;

static const int kConnIconSize = 54;
static const int kConnIconFocusSize = 70;
static const int kConnBtnSlotSize = 74;
static const int kConnImgSrcSize = 128;
static const uint32_t kConnFocusAnimMs = 200;

static const char *const kMenuCaptions[] = {
    "Remote Link",
    "LED Light",
    "Audio",
    "Standby",
    "Dial",
    "Strain Cal",
};

static int32_t ui_menu_visual_size_for_distance(uint32_t dist)
{
    if (dist == 0) {
        return kMenuCenterBtnSize;
    }
    if (dist == 1) {
        return kMenuNearBtnSize;
    }
    return kMenuFarBtnSize;
}

static uint32_t ui_menu_scale_for_visual(int32_t visual_size)
{
    uint32_t scale = (uint32_t)(visual_size * 9 / 10 * LV_IMG_ZOOM_NONE / kMenuImgSrcSize);
    return scale == 0 ? 1 : scale;
}

static void ui_menu_apply_visual_size(lv_obj_t *btn, int32_t visual_size)
{
    lv_obj_t *img = lv_obj_get_child(btn, 0);
    if (img == NULL) {
        return;
    }

    lv_img_set_zoom(img, ui_menu_scale_for_visual(visual_size));
    lv_obj_center(img);
}

static int32_t ui_menu_visual_from_btn(lv_obj_t *btn)
{
    lv_obj_t *img = lv_obj_get_child(btn, 0);
    if (img == NULL) {
        return kMenuCenterBtnSize;
    }

    uint32_t scale = lv_img_get_zoom(img);
    return (int32_t)((int64_t)scale * kMenuImgSrcSize * 10 / LV_IMG_ZOOM_NONE / 9);
}

static void ui_menu_visual_anim_cb(void *var, int32_t v)
{
    ui_menu_apply_visual_size(var, v);
}

static void ui_menu_caption_opa_anim_cb(void *var, int32_t v)
{
    lv_obj_set_style_opa(var, (lv_opa_t)v, LV_PART_MAIN);
}

static void ui_menu_caption_fade_in(void)
{
    lv_anim_del(s_menu_caption, ui_menu_caption_opa_anim_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_menu_caption);
    lv_anim_set_exec_cb(&a, ui_menu_caption_opa_anim_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&a, kMenuCaptionFadeMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void ui_menu_caption_fade_out_ready_cb(lv_anim_t *a)
{
    lv_obj_t *caption = a->var;
    lv_label_set_text(caption, kMenuCaptions[s_menu_caption_pending_idx]);
    ui_menu_caption_fade_in();
}

static void ui_menu_caption_fade_out_ready_cb_custom(lv_anim_t *a)
{
    lv_obj_t *caption = a->var;
    lv_label_set_text(caption, s_menu_caption_custom);
    lv_obj_set_style_text_font(caption, UI_FONT_MENU_CAPTION, LV_PART_MAIN);
    ui_menu_caption_fade_in();
}

static void ui_menu_caption_show_text(const char *text, lv_anim_enable_t anim)
{
    if (s_menu_caption == NULL || text == NULL) {
        return;
    }

    strncpy(s_menu_caption_custom, text, sizeof(s_menu_caption_custom) - 1);
    s_menu_caption_custom[sizeof(s_menu_caption_custom) - 1] = '\0';

    lv_obj_clear_flag(s_menu_caption, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_menu_caption, LV_ALIGN_CENTER, 0, kMenuCaptionYOffset);

    if (anim == LV_ANIM_OFF) {
        lv_anim_del(s_menu_caption, ui_menu_caption_opa_anim_cb);
        lv_label_set_text(s_menu_caption, s_menu_caption_custom);
        lv_obj_set_style_text_font(s_menu_caption, UI_FONT_MENU_CAPTION, LV_PART_MAIN);
        lv_obj_set_style_opa(s_menu_caption, LV_OPA_COVER, LV_PART_MAIN);
        return;
    }

    const char *cur_text = lv_label_get_text(s_menu_caption);
    if (cur_text != NULL && strcmp(cur_text, s_menu_caption_custom) == 0 &&
        lv_obj_get_style_opa(s_menu_caption, LV_PART_MAIN) == LV_OPA_COVER) {
        return;
    }

    lv_anim_del(s_menu_caption, ui_menu_caption_opa_anim_cb);
    lv_opa_t cur_opa = lv_obj_get_style_opa(s_menu_caption, LV_PART_MAIN);
    if (cur_opa == LV_OPA_TRANSP) {
        lv_label_set_text(s_menu_caption, s_menu_caption_custom);
        lv_obj_set_style_text_font(s_menu_caption, UI_FONT_MENU_CAPTION, LV_PART_MAIN);
        ui_menu_caption_fade_in();
        return;
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_menu_caption);
    lv_anim_set_exec_cb(&a, ui_menu_caption_opa_anim_cb);
    lv_anim_set_values(&a, cur_opa, LV_OPA_TRANSP);
    lv_anim_set_time(&a, kMenuCaptionFadeMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_ready_cb(&a, ui_menu_caption_fade_out_ready_cb_custom);
    lv_anim_start(&a);
}

/* ========== 菜单项「Strain Cal」入口：标题 → 5s 倒计时 → 进校准屏 ========== */

static void ui_menu_cal_stop_countdown(void)
{
    s_menu_cal_countdown_active = false;
    s_menu_cal_cd_step = 0;
    s_menu_cal_phase = CAL_MENU_PHASE_CAPTION;
    if (s_menu_caption != NULL) {
        lv_obj_clear_flag(s_menu_caption, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_menu_cal_timer != NULL) {
        lv_timer_pause(s_menu_cal_timer);
    }
}

static void ui_menu_cal_update_countdown(uint8_t secs)
{
    if (s_menu_caption == NULL) {
        return;
    }
    display_lock();
    lv_obj_clear_flag(s_menu_caption, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(s_menu_caption, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_menu_caption, UI_FONT_MENU_CAPTION, LV_PART_MAIN);
    lv_label_set_text_fmt(s_menu_caption, UI_STR_CAL_COUNTDOWN, (unsigned)secs);
    display_unlock();
}

static void ui_menu_cal_start_countdown_after_caption_hide(void)
{
    if (s_menu_caption != NULL) {
        lv_obj_add_flag(s_menu_caption, LV_OBJ_FLAG_HIDDEN);
    }
    s_menu_cal_phase = CAL_MENU_PHASE_COUNTDOWN;
    s_menu_cal_countdown_active = true;
    s_menu_cal_cd_step = kMenuCalCountdownSecs;
    ui_menu_cal_update_countdown(s_menu_cal_cd_step);
    if (s_menu_cal_timer != NULL) {
        lv_timer_set_period(s_menu_cal_timer, kMenuCalCountdownTickMs);
        lv_timer_reset(s_menu_cal_timer);
        lv_timer_resume(s_menu_cal_timer);
    }
}

static void ui_menu_cal_caption_hidden_ready_cb(lv_anim_t *a)
{
    (void)a;
    ui_menu_cal_start_countdown_after_caption_hide();
}

static void ui_menu_cal_fade_out_caption(void)
{
    if (s_menu_caption == NULL) {
        return;
    }

    lv_anim_del(s_menu_caption, ui_menu_caption_opa_anim_cb);
    lv_opa_t cur_opa = lv_obj_get_style_opa(s_menu_caption, LV_PART_MAIN);
    if (cur_opa == LV_OPA_TRANSP) {
        ui_menu_cal_start_countdown_after_caption_hide();
        return;
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_menu_caption);
    lv_anim_set_exec_cb(&a, ui_menu_caption_opa_anim_cb);
    lv_anim_set_values(&a, cur_opa, LV_OPA_TRANSP);
    lv_anim_set_time(&a, kMenuCaptionFadeMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_ready_cb(&a, ui_menu_cal_caption_hidden_ready_cb);
    lv_anim_start(&a);
}

static void ui_menu_cal_enter_screen(void)
{
    ui_menu_cal_stop_countdown();
    ui_screens_show(UI_SCR_STRAIN_CAL);
    input_strain_cal_prepare();
}

static void ui_menu_cal_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_screen != UI_SCR_MENU || s_menu_focus_idx != kMenuIdxCal) {
        ui_menu_cal_stop_countdown();
        return;
    }

    if (s_menu_cal_phase == CAL_MENU_PHASE_CAPTION) {
        ui_menu_cal_fade_out_caption();
        return;
    }

    if (s_menu_cal_cd_step <= 1) {
        ui_menu_cal_enter_screen();
        return;
    }

    s_menu_cal_cd_step--;
    ui_menu_cal_update_countdown(s_menu_cal_cd_step);
}

/* ========== 首次未校准开机：Welcome 淡入/停留/淡出 → 自动进校准 ========== */

static void ui_boot_splash_opa_anim_cb(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, LV_PART_MAIN);
}

static void ui_boot_splash_stop_timer(void)
{
    if (s_boot_splash_timer != NULL) {
        lv_timer_pause(s_boot_splash_timer);
    }
}

static void ui_boot_splash_show_welcome(void)
{
    if (s_boot_splash_lbl == NULL) {
        return;
    }
    lv_label_set_text(s_boot_splash_lbl, kBootSplashWelcome);
    lv_obj_clear_flag(s_boot_splash_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(s_boot_splash_lbl, LV_OPA_TRANSP, LV_PART_MAIN);
}

static void ui_boot_splash_fade_in(void);
static void ui_boot_splash_fade_out(void);
static void ui_boot_splash_finish(void);

static void ui_boot_splash_hold_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    ui_boot_splash_stop_timer();
    if (!s_boot_splash_active) {
        return;
    }
    ui_boot_splash_fade_out();
}

static void ui_boot_splash_fade_in_ready_cb(lv_anim_t *a)
{
    (void)a;
    if (!s_boot_splash_active) {
        return;
    }
    if (s_boot_splash_timer == NULL) {
        s_boot_splash_timer = lv_timer_create(ui_boot_splash_hold_timer_cb, kBootSplashHoldMs, NULL);
    } else {
        lv_timer_set_cb(s_boot_splash_timer, ui_boot_splash_hold_timer_cb);
        lv_timer_set_period(s_boot_splash_timer, kBootSplashHoldMs);
    }
    lv_timer_reset(s_boot_splash_timer);
    lv_timer_resume(s_boot_splash_timer);
}

static void ui_boot_splash_fade_out_ready_cb(lv_anim_t *a)
{
    (void)a;
    if (!s_boot_splash_active) {
        return;
    }
    if (s_boot_splash_lbl != NULL) {
        lv_obj_add_flag(s_boot_splash_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(s_boot_splash_lbl, LV_OPA_TRANSP, LV_PART_MAIN);
    }
    ui_boot_splash_finish();
}

static void ui_boot_splash_fade_in(void)
{
    if (s_boot_splash_lbl == NULL) {
        return;
    }
    lv_anim_del(s_boot_splash_lbl, ui_boot_splash_opa_anim_cb);
    lv_obj_clear_flag(s_boot_splash_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_boot_splash_lbl);
    lv_anim_set_exec_cb(&anim, ui_boot_splash_opa_anim_cb);
    lv_anim_set_values(&anim, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&anim, kBootSplashFadeMs);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_set_ready_cb(&anim, ui_boot_splash_fade_in_ready_cb);
    lv_anim_start(&anim);
}

static void ui_boot_splash_fade_out(void)
{
    if (s_boot_splash_lbl == NULL) {
        return;
    }
    lv_anim_del(s_boot_splash_lbl, ui_boot_splash_opa_anim_cb);
    lv_opa_t cur = lv_obj_get_style_opa(s_boot_splash_lbl, LV_PART_MAIN);
    if (cur == LV_OPA_TRANSP) {
        ui_boot_splash_fade_out_ready_cb(NULL);
        return;
    }
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_boot_splash_lbl);
    lv_anim_set_exec_cb(&anim, ui_boot_splash_opa_anim_cb);
    lv_anim_set_values(&anim, cur, LV_OPA_TRANSP);
    lv_anim_set_time(&anim, kBootSplashFadeMs);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_set_ready_cb(&anim, ui_boot_splash_fade_out_ready_cb);
    lv_anim_start(&anim);
}

static void ui_boot_splash_finish(void)
{
    ui_boot_splash_stop_timer();
    if (s_boot_splash_lbl != NULL) {
        lv_anim_del(s_boot_splash_lbl, ui_boot_splash_opa_anim_cb);
        lv_obj_add_flag(s_boot_splash_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_boot_splash_panel != NULL) {
        lv_obj_add_flag(s_boot_splash_panel, LV_OBJ_FLAG_HIDDEN);
    }
    s_boot_splash_active = false;
    s_boot_splash_pending = false;
    s_menu_cal_suppress_auto = true;
    ESP_LOGI(TAG, "boot splash done -> strain cal");
    ui_menu_cal_enter_screen();
}

void ui_screens_boot_splash_kick_if_pending(void)
{
    if (s_boot_splash_kicked || !s_boot_splash_pending) {
        return;
    }
    s_boot_splash_kicked = true;
    s_boot_splash_active = true;
    if (s_boot_splash_panel != NULL) {
        lv_obj_clear_flag(s_boot_splash_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_boot_splash_panel);
    }
    ui_boot_splash_show_welcome();
    ESP_LOGI(TAG, "boot splash start");
    ui_boot_splash_fade_in();
}

bool ui_screens_boot_splash_active(void)
{
    return s_boot_splash_active || s_boot_splash_pending;
}

static void ui_menu_begin_cal_focus(lv_anim_enable_t anim)
{
    ui_menu_cal_stop_countdown();
    s_menu_cal_phase = CAL_MENU_PHASE_CAPTION;
    s_menu_cal_countdown_active = false;
    if (s_menu_caption != NULL) {
        lv_obj_clear_flag(s_menu_caption, LV_OBJ_FLAG_HIDDEN);
    }
    ui_menu_caption_show_text(UI_STR_CAL_CAPTION, anim);
    if (s_menu_cal_timer == NULL) {
        s_menu_cal_timer = lv_timer_create(ui_menu_cal_timer_cb, kMenuCalCaptionDwellMs, NULL);
    } else {
        lv_timer_set_period(s_menu_cal_timer, kMenuCalCaptionDwellMs);
    }
    lv_timer_reset(s_menu_cal_timer);
    lv_timer_resume(s_menu_cal_timer);
}

bool ui_screens_menu_cal_focused(void)
{
    return s_screen == UI_SCR_MENU && s_menu_focus_idx == kMenuIdxCal;
}

void ui_screens_menu_cal_enter(void)
{
    if (s_screen != UI_SCR_MENU || s_menu_focus_idx != kMenuIdxCal) {
        return;
    }
    ui_menu_cal_enter_screen();
}

void ui_screens_menu_cal_suppress_auto_enter(void)
{
    s_menu_cal_suppress_auto = true;
}

static void ui_menu_caption_show(uint8_t idx, lv_anim_enable_t anim)
{
    if (s_menu_caption == NULL || idx >= sizeof(kMenuCaptions) / sizeof(kMenuCaptions[0])) {
        return;
    }

    if (s_menu_cal_countdown_active && idx == kMenuIdxCal) {
        return;
    }

    s_menu_caption_pending_idx = idx;
    lv_obj_clear_flag(s_menu_caption, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_menu_caption, LV_ALIGN_CENTER, 0, kMenuCaptionYOffset);

    if (anim == LV_ANIM_OFF) {
        lv_anim_del(s_menu_caption, ui_menu_caption_opa_anim_cb);
        lv_label_set_text(s_menu_caption, kMenuCaptions[idx]);
        lv_obj_set_style_opa(s_menu_caption, LV_OPA_COVER, LV_PART_MAIN);
        return;
    }

    const char *cur_text = lv_label_get_text(s_menu_caption);
    if (cur_text != NULL && strcmp(cur_text, kMenuCaptions[idx]) == 0 &&
        lv_obj_get_style_opa(s_menu_caption, LV_PART_MAIN) == LV_OPA_COVER) {
        return;
    }

    lv_anim_del(s_menu_caption, ui_menu_caption_opa_anim_cb);
    lv_opa_t cur_opa = lv_obj_get_style_opa(s_menu_caption, LV_PART_MAIN);
    if (cur_opa == LV_OPA_TRANSP) {
        lv_label_set_text(s_menu_caption, kMenuCaptions[idx]);
        ui_menu_caption_fade_in();
        return;
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_menu_caption);
    lv_anim_set_exec_cb(&a, ui_menu_caption_opa_anim_cb);
    lv_anim_set_values(&a, cur_opa, LV_OPA_TRANSP);
    lv_anim_set_time(&a, kMenuCaptionFadeMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_ready_cb(&a, ui_menu_caption_fade_out_ready_cb);
    lv_anim_start(&a);
}

static void ui_menu_set_btn_visual(lv_obj_t *btn, int32_t visual_size, lv_anim_enable_t anim)
{
    lv_obj_set_size(btn, kMenuCenterBtnSize, kMenuCenterBtnSize);

    if (anim == LV_ANIM_OFF) {
        lv_anim_del(btn, ui_menu_visual_anim_cb);
        ui_menu_apply_visual_size(btn, visual_size);
        return;
    }

    int32_t cur = ui_menu_visual_from_btn(btn);
    if (cur == visual_size) {
        ui_menu_apply_visual_size(btn, visual_size);
        return;
    }

    lv_anim_del(btn, ui_menu_visual_anim_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, btn);
    lv_anim_set_exec_cb(&a, ui_menu_visual_anim_cb);
    lv_anim_set_values(&a, cur, visual_size);
    lv_anim_set_time(&a, kMenuSizeAnimMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static uint32_t ui_menu_circular_distance(uint32_t a, uint32_t b, uint32_t cnt)
{
    uint32_t diff = (a > b) ? (a - b) : (b - a);
    if (diff > cnt / 2) {
        diff = cnt - diff;
    }
    return diff;
}

static void ui_menu_apply_focus(lv_obj_t *focused, lv_anim_enable_t anim)
{
    uint32_t cnt = lv_obj_get_child_cnt(s_menu);
    if (cnt == 0 || focused == NULL) {
        return;
    }

    uint32_t focus_idx = lv_obj_get_index(focused);
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t dist = ui_menu_circular_distance(i, focus_idx, cnt);
        int32_t visual = ui_menu_visual_size_for_distance(dist);
        ui_menu_set_btn_visual(lv_obj_get_child(s_menu, i), visual, anim);
    }

    lv_obj_update_layout(s_menu);
    lv_obj_scroll_to_view(focused, anim);

    if (focus_idx == kMenuIdxCal) {
        if (s_menu_cal_suppress_auto) {
            ui_menu_cal_stop_countdown();
            ui_menu_caption_show_text(UI_STR_CAL_CAPTION, anim);
        } else {
            ui_menu_begin_cal_focus(anim);
        }
    } else {
        s_menu_cal_suppress_auto = false;
        ui_menu_cal_stop_countdown();
        lv_obj_set_style_text_font(s_menu_caption, UI_FONT_MENU_CAPTION, LV_PART_MAIN);
        ui_menu_caption_show((uint8_t)focus_idx, anim);
    }
}

static void ui_menu_focus_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_FOCUSED || s_menu_bind_active) {
        return;
    }
    if (s_surface_dial || lv_obj_has_flag(s_menu, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    lv_obj_t *focused = lv_event_get_target(e);
    uint8_t idx = (uint8_t)lv_obj_get_index(focused);
    s_menu_focus_idx = idx;
    s_menu_return_idx = idx;
    ui_menu_apply_focus(focused, LV_ANIM_ON);
}

static void ui_screens_bind_menu_items(void)
{
    uint8_t restore_idx = s_menu_return_idx;

    s_menu_bind_active = true;
    ui_nav_clear_group();
    ui_nav_sync_position();
    lv_group_set_wrap(lv_group_get_default(), true);
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(s_menu); i++) {
        ui_nav_bind_object(lv_obj_get_child(s_menu, i));
    }

    uint32_t cnt = lv_obj_get_child_cnt(s_menu);
    if (cnt == 0) {
        s_menu_bind_active = false;
        return;
    }

    if (restore_idx >= cnt) {
        restore_idx = 0;
    }

    s_menu_focus_idx = restore_idx;
    s_menu_return_idx = restore_idx;
    lv_obj_t *focus_btn = lv_obj_get_child(s_menu, restore_idx);
    lv_group_focus_obj(focus_btn);
    ui_menu_apply_focus(focus_btn, LV_ANIM_OFF);
    s_menu_bind_active = false;
}

void ui_screens_menu_on_encoder(int32_t delta)
{
    if (delta == 0 || s_screen != UI_SCR_MENU || ui_screens_menu_input_guarded() ||
        ui_screens_boot_splash_active()) {
        return;
    }

    uint32_t cnt = lv_obj_get_child_cnt(s_menu);
    if (cnt == 0) {
        return;
    }

    int32_t idx = (int32_t)s_menu_focus_idx + delta;
    while (idx < 0) {
        idx += (int32_t)cnt;
    }
    while (idx >= (int32_t)cnt) {
        idx -= (int32_t)cnt;
    }

    s_menu_focus_idx = (uint8_t)idx;
    s_menu_return_idx = (uint8_t)idx;
    lv_obj_t *focus_btn = lv_obj_get_child(s_menu, s_menu_focus_idx);
    lv_group_focus_obj(focus_btn);
    ui_menu_apply_focus(focus_btn, LV_ANIM_ON);
}

static uint32_t ui_conn_scale_for_visual(int32_t visual_size)
{
    uint32_t scale = (uint32_t)(visual_size * 9 / 10 * LV_IMG_ZOOM_NONE / kConnImgSrcSize);
    return scale == 0 ? 1 : scale;
}

static void ui_conn_apply_visual_size(lv_obj_t *btn, int32_t visual_size)
{
    lv_obj_t *img = lv_obj_get_child(btn, 0);
    if (img == NULL) {
        return;
    }

    lv_obj_set_size(btn, kConnBtnSlotSize, kConnBtnSlotSize);
    lv_img_set_zoom(img, ui_conn_scale_for_visual(visual_size));
    lv_obj_center(img);
}

static int32_t ui_conn_visual_from_btn(lv_obj_t *btn)
{
    lv_obj_t *img = lv_obj_get_child(btn, 0);
    if (img == NULL) {
        return kConnIconSize;
    }

    uint32_t scale = lv_img_get_zoom(img);
    return (int32_t)((int64_t)scale * kConnImgSrcSize * 10 / LV_IMG_ZOOM_NONE / 9);
}

static void ui_conn_visual_anim_cb(void *var, int32_t v)
{
    ui_conn_apply_visual_size(var, v);
}

static void ui_conn_set_focus_visual(lv_obj_t *btn, bool focused, lv_anim_enable_t anim)
{
    int32_t target = focused ? kConnIconFocusSize : kConnIconSize;

    if (anim == LV_ANIM_OFF) {
        lv_anim_del(btn, ui_conn_visual_anim_cb);
        ui_conn_apply_visual_size(btn, target);
        return;
    }

    int32_t cur = ui_conn_visual_from_btn(btn);
    if (cur == target) {
        ui_conn_apply_visual_size(btn, target);
        return;
    }

    lv_anim_del(btn, ui_conn_visual_anim_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, btn);
    lv_anim_set_exec_cb(&a, ui_conn_visual_anim_cb);
    lv_anim_set_values(&a, cur, target);
    lv_anim_set_time(&a, kConnFocusAnimMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void ui_conn_apply_focus_idx(uint8_t idx, lv_anim_enable_t anim)
{
    for (size_t i = 0; i < sizeof(s_conn_btns) / sizeof(s_conn_btns[0]); i++) {
        ui_conn_set_focus_visual(s_conn_btns[i], i == idx, anim);
    }
}

static void ui_screens_bind_connection_items(void)
{
    ui_nav_clear_group();
    ui_nav_sync_position();
    lv_group_set_wrap(lv_group_get_default(), true);
    for (size_t i = 0; i < sizeof(s_conn_btns) / sizeof(s_conn_btns[0]); i++) {
        ui_nav_bind_object(s_conn_btns[i]);
    }
    s_conn_focus_idx = 0;
    lv_group_focus_obj(s_conn_btns[0]);
    ui_conn_apply_focus_idx(0, LV_ANIM_OFF);
}

void ui_screens_connection_on_encoder(int32_t delta)
{
    if (delta == 0 || s_screen != UI_SCR_CONNECTION) {
        return;
    }

    const uint32_t cnt = sizeof(s_conn_btns) / sizeof(s_conn_btns[0]);
    int32_t idx = (int32_t)s_conn_focus_idx + delta;
    while (idx < 0) {
        idx += (int32_t)cnt;
    }
    while (idx >= (int32_t)cnt) {
        idx -= (int32_t)cnt;
    }

    if ((uint8_t)idx == s_conn_focus_idx) {
        return;
    }

    s_conn_focus_idx = (uint8_t)idx;
    lv_group_focus_obj(s_conn_btns[s_conn_focus_idx]);
    ui_conn_apply_focus_idx(s_conn_focus_idx, LV_ANIM_ON);
}

static lv_obj_t *ui_create_full_panel(void)
{
    lv_obj_t *scr = lv_obj_create(lv_scr_act());
    lv_obj_set_size(scr, 240, 240);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_HIDDEN);
    return scr;
}

void ui_screens_arm_menu_input_guard(uint32_t duration_ms)
{
    s_menu_input_guard_start_ms = lv_tick_get();
    s_menu_input_guard_duration_ms = duration_ms;
}

bool ui_screens_menu_input_guarded(void)
{
    if (s_menu_input_guard_duration_ms == 0) {
        return false;
    }
    return lv_tick_elaps(s_menu_input_guard_start_ms) < s_menu_input_guard_duration_ms;
}

static void ui_menu_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (ui_screens_menu_input_guarded()) {
        return;
    }
    lv_obj_t *btn = lv_event_get_target(e);
    s_menu_focus_idx = (uint8_t)lv_obj_get_index(btn);
    s_menu_return_idx = s_menu_focus_idx;

    intptr_t id = (intptr_t)lv_event_get_user_data(e);
    if (id == 1) {
        ui_screens_show(UI_SCR_CONNECTION);
    } else if (id == 2) {
        ui_screens_show(UI_SCR_LED);
    } else if (id == 4) {
        ui_screens_show(UI_SCR_AUDIO);
    } else if (id == 5) {
        ui_screens_show(UI_SCR_STANDBY);
    } else if (id == 6) {
        s_menu_focus_idx = kMenuIdxDial;
        s_menu_return_idx = kMenuIdxDial;
        knob_event_t evt = {};
        evt.type = KNOB_EVT_MODE_ENTER;
        evt.data.mode_change.mode = APP_MODE_SURFACE_DIAL;
        event_bus_publish(&evt, 0);
    } else if (id == 7) {
        ui_screens_menu_cal_enter();
    }
}

static void ui_remote_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    remote_cmd_t cmd = (remote_cmd_t)(intptr_t)lv_event_get_user_data(e);
    ui_post_remote_cmd(cmd, 0, 0, 0);
    ui_screens_show_menu();
}

static void ui_fit_icon(lv_obj_t *img, int size)
{
    lv_obj_set_size(img, size, size);
    lv_obj_center(img);
}

static lv_obj_t *ui_add_conn_icon_btn(lv_obj_t *parent, const lv_img_dsc_t *icon,
                                      lv_align_t align, int16_t x_ofs, int16_t y_ofs,
                                      remote_cmd_t cmd)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, kConnBtnSlotSize, kConnBtnSlotSize);
    lv_obj_remove_style_all(btn);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(btn, align, x_ofs, y_ofs);
    lv_obj_add_event_cb(btn, ui_remote_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)cmd);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(btn, 0, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(btn, 0, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);

    lv_obj_t *img = lv_img_create(btn);
    lv_img_set_src(img, icon);
    lv_obj_center(img);
    ui_conn_apply_visual_size(btn, kConnIconSize);
    return btn;
}

static lv_obj_t *ui_add_menu_btn(lv_obj_t *parent, const lv_img_dsc_t *icon, intptr_t id)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, kMenuCenterBtnSize, kMenuCenterBtnSize);
    lv_obj_remove_style_all(btn);
    lv_obj_add_event_cb(btn, ui_menu_btn_cb, LV_EVENT_CLICKED, (void *)id);
    lv_obj_add_event_cb(btn, ui_menu_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(btn, 0, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(btn, 0, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);

    lv_obj_t *img = lv_img_create(btn);
    lv_img_set_src(img, icon);
    lv_obj_center(img);
    return btn;
}

ui_screen_t ui_screens_current(void)
{
    return s_screen;
}

bool ui_screens_is_surface_dial(void)
{
    return s_surface_dial;
}

uint8_t ui_screens_get_menu_return_idx(void)
{
    return s_menu_return_idx;
}

void ui_screens_set_menu_return_idx(uint8_t idx)
{
    s_menu_return_idx = idx;
    s_menu_focus_idx = idx;
}

void ui_screens_on_dial_mode_exit(void)
{
    s_menu_focus_idx = kMenuIdxDial;
    s_menu_return_idx = kMenuIdxDial;
}

bool ui_screens_menu_nav_active(void)
{
    return s_screen == UI_SCR_MENU && !s_surface_dial;
}

lv_obj_t *ui_screens_get_title(void)
{
    return s_title;
}

void ui_screens_hide_all(void)
{
    lv_obj_add_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
    if (s_menu_caption != NULL) {
        lv_obj_add_flag(s_menu_caption, LV_OBJ_FLAG_HIDDEN);
        lv_anim_del(s_menu_caption, ui_menu_caption_opa_anim_cb);
    }
    lv_obj_add_flag(s_connection, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_dial, LV_OBJ_FLAG_HIDDEN);
    ui_screen_sub_hide_all();
}

static void ui_screens_apply_menu_haptics_locked(int32_t position)
{
    uint32_t cnt = lv_obj_get_child_cnt(s_menu);
    if (cnt == 0) {
        cnt = 1;
    }
    if (position >= (int32_t)cnt) {
        position = (int32_t)cnt - 1;
    }
    if (position < 0) {
        position = 0;
    }
    haptics_apply_menu_detents(position, (int32_t)cnt);
}

void ui_screens_apply_menu_haptics(void)
{
    display_lock();
    ui_screens_apply_menu_haptics_locked((int32_t)s_menu_return_idx);
    display_unlock();
}

static void ui_screens_show_menu_locked(void)
{
    ui_screens_apply_menu_haptics_locked((int32_t)s_menu_return_idx);

    s_screen = UI_SCR_MENU;
    ui_screens_hide_all();
    lv_label_set_text(s_title, "LX Dial");
    lv_obj_add_flag(s_title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
    if (s_menu_caption != NULL) {
        lv_obj_clear_flag(s_menu_caption, LV_OBJ_FLAG_HIDDEN);
    }
    ui_nav_reset_input();
    ui_screens_bind_menu_items();
    ui_nav_force_position((int32_t)s_menu_return_idx);
    lv_obj_move_foreground(s_menu);
    if (s_menu_caption != NULL) {
        lv_obj_move_foreground(s_menu_caption);
    }
}

static void ui_screens_hide_surface_dial_locked(void)
{
    lv_obj_add_flag(s_dial, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_img_opa(s_dial, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_move_background(s_dial);
}

/* Dial exit: hide overlay only — do not re-bind menu (avoids SPI/LVGL heap spike). */
static void ui_screens_exit_dial_overlay_locked(void)
{
    s_surface_dial = false;
    s_screen = UI_SCR_MENU;
    ui_screens_hide_surface_dial_locked();
    lv_obj_clear_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
    if (s_menu_caption != NULL) {
        lv_obj_clear_flag(s_menu_caption, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_move_foreground(s_menu);
    if (s_menu_caption != NULL) {
        lv_obj_move_foreground(s_menu_caption);
    }

    uint32_t cnt = lv_obj_get_child_cnt(s_menu);
    if (cnt > 0) {
        uint8_t idx = kMenuIdxDial;
        if (idx >= cnt) {
            idx = 0;
        }
        s_menu_focus_idx = idx;
        s_menu_return_idx = idx;
        lv_obj_t *focus_btn = lv_obj_get_child(s_menu, idx);
        if (focus_btn != NULL) {
            lv_group_focus_obj(focus_btn);
            ui_menu_apply_focus(focus_btn, LV_ANIM_OFF);
        }
    }
}

static void ui_screens_show_surface_dial_locked(void)
{
    ui_screens_hide_all();
    lv_label_set_text(s_title, "Surface Dial");
    lv_obj_add_flag(s_title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_img_opa(s_dial, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_dial, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_dial);
    s_screen = UI_SCR_MENU;
}

void ui_screens_show(ui_screen_t screen)
{
    display_lock();
    if (s_screen == UI_SCR_MENU && screen != UI_SCR_MENU) {
        lv_obj_t *focused = lv_group_get_focused(lv_group_get_default());
        if (focused != NULL && lv_obj_get_parent(focused) == s_menu) {
            s_menu_return_idx = (uint8_t)lv_obj_get_index(focused);
            s_menu_focus_idx = s_menu_return_idx;
        }
    }

    switch (screen) {
    case UI_SCR_MENU:
        ui_menu_cal_stop_countdown();
        ui_screens_show_menu_locked();
        break;
    case UI_SCR_CONNECTION:
        s_screen = screen;
        s_surface_dial = false;
        ui_screens_hide_surface_dial_locked();
        ui_screens_hide_all();
        lv_obj_clear_flag(s_title, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_title, "Remote Link");
        lv_obj_clear_flag(s_connection, LV_OBJ_FLAG_HIDDEN);
        ui_screens_bind_connection_items();
        break;
    default:
        s_screen = screen;
        s_surface_dial = false;
        ui_screens_hide_surface_dial_locked();
        ui_screens_hide_all();
        switch (screen) {
        case UI_SCR_LED:
            lv_obj_clear_flag(s_title, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_title, "LED");
            break;
        case UI_SCR_LED_HSV:
            lv_obj_clear_flag(s_title, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_title, "LED HSV");
            break;
        case UI_SCR_AUDIO:
            lv_obj_clear_flag(s_title, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_title, "Audio");
            break;
        case UI_SCR_STANDBY:
            lv_obj_clear_flag(s_title, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_title, "Standby");
            break;
        case UI_SCR_INSTRUMENT:
        case UI_SCR_ASTRONAUT:
        case UI_SCR_POWER:
            lv_obj_add_flag(s_title, LV_OBJ_FLAG_HIDDEN);
            break;
        case UI_SCR_STRAIN_CAL:
            lv_obj_add_flag(s_title, LV_OBJ_FLAG_HIDDEN);
            break;
        default:
            lv_obj_clear_flag(s_title, LV_OBJ_FLAG_HIDDEN);
            break;
        }
        if (s_screen == UI_SCR_INSTRUMENT && screen != UI_SCR_INSTRUMENT) {
            ui_screen_sub_instrument_smooth_stop();
        }
        lv_obj_t *panel = ui_screen_sub_get_panel(screen);
        if (panel != NULL) {
            lv_obj_clear_flag(panel, LV_OBJ_FLAG_HIDDEN);
            ui_screen_sub_bind_nav(screen);
        }
        if (screen == UI_SCR_INSTRUMENT) {
            ui_screen_sub_instrument_smooth_start();
        }
        break;
    }
    display_unlock();
}

void ui_screens_init(void)
{
    display_lock();

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    s_title = lv_label_create(scr);
    lv_label_set_text(s_title, "LX Dial");
    lv_obj_set_style_text_color(s_title, lv_color_hex(0x00BFFF), LV_PART_MAIN);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 8);

    s_menu = lv_obj_create(scr);
    lv_obj_set_size(s_menu, 240, 240);
    lv_obj_set_style_bg_color(s_menu, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_menu, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_menu, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_menu, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(s_menu, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_left(s_menu, kMenuScrollPad, LV_PART_MAIN);
    lv_obj_set_style_pad_right(s_menu, kMenuScrollPad, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_menu, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_menu, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(s_menu, LV_DIR_HOR);
    lv_obj_add_flag(s_menu, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_snap_x(s_menu, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(s_menu, LV_SCROLLBAR_MODE_OFF);
    lv_obj_center(s_menu);

    ui_add_menu_btn(s_menu, &alpha_menu_con_pic, 1);
    ui_add_menu_btn(s_menu, &alpha_menu_led_pic, 2);
    ui_add_menu_btn(s_menu, &alpha_menu_aucfg_pic, 4);
    ui_add_menu_btn(s_menu, &alpha_menu_standby_pic, 5);
    ui_add_menu_btn(s_menu, &alpha_menu_dial_pic_02, 6);
    ui_add_menu_btn(s_menu, &alpha_menu_cal_pic, 7);

    s_menu_caption = lv_label_create(scr);
    lv_label_set_text(s_menu_caption, kMenuCaptions[0]);
    lv_obj_set_width(s_menu_caption, 220);
    lv_obj_set_style_text_color(s_menu_caption, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_menu_caption, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_menu_caption, UI_FONT_MENU_CAPTION, LV_PART_MAIN);
    lv_obj_align(s_menu_caption, LV_ALIGN_CENTER, 0, kMenuCaptionYOffset);
    lv_obj_add_flag(s_menu_caption, LV_OBJ_FLAG_HIDDEN);

    s_connection = ui_create_full_panel();

    const int conn_inset = 18;
    s_conn_btns[0] = ui_add_conn_icon_btn(s_connection, &alpha_con_usb_pic, LV_ALIGN_TOP_MID, 0, conn_inset,
                                          REMOTE_LINK_USB);
    s_conn_btns[1] = ui_add_conn_icon_btn(s_connection, &alpha_con_ble_pic, LV_ALIGN_RIGHT_MID, -conn_inset, 0,
                                          REMOTE_LINK_BLE);
    s_conn_btns[2] = ui_add_conn_icon_btn(s_connection, &alpha_con_wifi_pic, LV_ALIGN_BOTTOM_MID, 0, -conn_inset,
                                          REMOTE_LINK_WIFI);
    s_conn_btns[3] = ui_add_conn_icon_btn(s_connection, &alpha_con_espnow_pic, LV_ALIGN_LEFT_MID, conn_inset, 0,
                                          REMOTE_LINK_ESPNOW);

    s_dial = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_dial, 240, 240);
    lv_obj_set_style_bg_opa(s_dial, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_img_src(s_dial, &alpha_menu_dial_pic_normal, LV_PART_MAIN);
    lv_obj_set_style_bg_img_opa(s_dial, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_dial, 0, LV_PART_MAIN);
    lv_obj_center(s_dial);
    lv_obj_add_flag(s_dial, LV_OBJ_FLAG_HIDDEN);

    s_low_power_panel = lv_obj_create(scr);
    lv_obj_set_size(s_low_power_panel, 240, 240);
    lv_obj_center(s_low_power_panel);
    lv_obj_set_style_bg_color(s_low_power_panel, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_low_power_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_low_power_panel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_low_power_panel, 0, LV_PART_MAIN);
    lv_obj_add_flag(s_low_power_panel, LV_OBJ_FLAG_HIDDEN);

    s_low_power_lbl = lv_label_create(s_low_power_panel);
    lv_label_set_text(s_low_power_lbl, "Low Power");
    lv_obj_set_style_text_color(s_low_power_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_low_power_lbl, UI_FONT_CAL_PROMPT, LV_PART_MAIN);
    lv_obj_center(s_low_power_lbl);

    s_boot_splash_panel = lv_obj_create(scr);
    lv_obj_set_size(s_boot_splash_panel, 240, 240);
    lv_obj_center(s_boot_splash_panel);
    lv_obj_set_style_bg_color(s_boot_splash_panel, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_boot_splash_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_boot_splash_panel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_boot_splash_panel, 0, LV_PART_MAIN);
    lv_obj_add_flag(s_boot_splash_panel, LV_OBJ_FLAG_HIDDEN);

    s_boot_splash_lbl = lv_label_create(s_boot_splash_panel);
    lv_obj_set_width(s_boot_splash_lbl, 200);
    lv_label_set_long_mode(s_boot_splash_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_boot_splash_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_boot_splash_lbl, UI_FONT_CAL_PROMPT, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_boot_splash_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_center(s_boot_splash_lbl);
    lv_obj_add_flag(s_boot_splash_lbl, LV_OBJ_FLAG_HIDDEN);

    s_boot_splash_pending = false;
    s_boot_splash_active = false;
    s_boot_splash_kicked = false;

    if (!input_strain_is_calibrated()) {
        s_menu_focus_idx = kMenuIdxCal;
        s_menu_return_idx = kMenuIdxCal;
        s_menu_cal_suppress_auto = false;
        s_boot_splash_pending = true;
        lv_obj_add_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_boot_splash_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_boot_splash_panel);
        ESP_LOGI(TAG, "boot: strain uncalibrated, welcome splash then strain cal");
    } else {
        s_menu_focus_idx = 0;
        s_menu_return_idx = 0;
        ui_screens_show(UI_SCR_MENU);
        ESP_LOGI(TAG, "boot: strain calibrated, focus menu icon 0");
    }

    display_unlock();
    ESP_LOGI(TAG, "UI screens ready");
}

void ui_screens_show_menu(void)
{
    ui_screens_show(UI_SCR_MENU);
}

void ui_screens_set_low_power(bool on)
{
    if (s_low_power_panel == NULL) {
        return;
    }
    display_lock();
    if (on) {
        lv_obj_clear_flag(s_low_power_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_low_power_panel);
    } else {
        lv_obj_add_flag(s_low_power_panel, LV_OBJ_FLAG_HIDDEN);
    }
    display_unlock();
}

void ui_screens_back(void)
{
    if (s_kb_power_preview && s_screen == UI_SCR_POWER) {
        return;
    }
    switch (s_screen) {
    case UI_SCR_MENU:
        break;
    case UI_SCR_CONNECTION:
    case UI_SCR_LED:
    case UI_SCR_AUDIO:
    case UI_SCR_STANDBY:
        ui_screens_show(UI_SCR_MENU);
        break;
    case UI_SCR_LED_HSV:
        ui_screen_sub_hsv_exit_to_menu();
        break;
    case UI_SCR_INSTRUMENT:
        ui_screen_sub_leave_instrument();
        ui_screens_show(UI_SCR_STANDBY);
        break;
    case UI_SCR_ASTRONAUT:
        ui_screens_show(UI_SCR_STANDBY);
        break;
    case UI_SCR_POWER:
        ui_screens_show(UI_SCR_STANDBY);
        break;
    case UI_SCR_STRAIN_CAL:
        ui_screen_sub_strain_cal_leave();
        ui_screens_show(UI_SCR_MENU);
        break;
    default:
        ui_screens_show(UI_SCR_MENU);
        break;
    }
}

void ui_screens_on_host_stats(uint8_t cpu, uint8_t gpu, uint8_t ram)
{
    ui_screen_sub_on_host_stats(cpu, gpu, ram);
}

void ui_screens_on_keyboard_power(uint16_t bus_mv, int16_t current_ma)
{
    ui_screen_sub_on_keyboard_power(bus_mv, current_ma);
}

void ui_screens_on_keyboard_audio(bool sound_on)
{
    ui_screen_sub_on_keyboard_audio(sound_on);
}

bool ui_screens_power_preview_active(void)
{
    return s_kb_power_preview;
}

void ui_screens_power_preview(bool pressed)
{
    if (pressed) {
        if (s_kb_power_preview) {
            return;
        }
        s_kb_power_preview_saved_screen = s_screen;
        s_kb_power_preview_saved_dial = s_surface_dial;
        s_kb_power_preview = true;
        ui_screens_show(UI_SCR_POWER);
        return;
    }

    if (!s_kb_power_preview) {
        return;
    }
    s_kb_power_preview = false;
    if (s_kb_power_preview_saved_dial) {
        ui_screens_on_mode(APP_MODE_SURFACE_DIAL);
    } else {
        ui_screens_show(s_kb_power_preview_saved_screen);
    }
}

void ui_screens_on_mode(app_mode_t mode)
{
    display_lock();
    if (mode == APP_MODE_SURFACE_DIAL) {
        s_menu_focus_idx = kMenuIdxDial;
        s_menu_return_idx = kMenuIdxDial;
        s_surface_dial = true;
        ui_screens_show_surface_dial_locked();
    } else if (mode == APP_MODE_MENU) {
        if (s_surface_dial) {
            ui_screens_exit_dial_overlay_locked();
            /* Block stray press/encoder after dial exit from re-entering dial or jumping focus. */
            ui_screens_arm_menu_input_guard(400);
            ui_nav_force_position((int32_t)kMenuIdxDial);
            ui_nav_reset_input();
        } else {
            ui_screens_show_menu_locked();
        }
    }
    display_unlock();
}
