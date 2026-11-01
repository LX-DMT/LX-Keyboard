#include "ui_screens.h"
/**
 * @file ui_screen_sub.c
 * @brief 子屏 LVGL：LED/HSV、Audio、Standby、仪表、Power、应变校准
 *
 * Connection 子屏在 ui_screens.c。本文件负责：
 * - Standby 轮播（Astronaut / Instrument / Power）与 Power 电压电流 UI
 * - Audio 静音 switch（编码器发 REMOTE_AUDIO_*；0x84 仅同步显示）
 * - Instrument 三环与 HOST_STATS；应变校准圆环倒计时
 */

#include "ui_screens_internal.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "display.h"
#include "esp_log.h"
#include "event_bus.h"
#include "haptics.h"
#include "host_link.h"
#include "input_strain.h"
#include "knob_events.h"
#include "ui_i18n.h"
#include "ui.h"
#include "ui_nav.h"
#include "battery_monitor.h"
#include "ui_assets.h"

static const char *TAG = "ui_sub";

static lv_obj_t *s_led;
static lv_obj_t *s_led_caption;
static lv_obj_t *s_led_btns[3];
static lv_obj_t *s_led_hsv;
static lv_obj_t *s_cw_h;
static lv_obj_t *s_cw_s;
static lv_obj_t *s_cw_v;

static const int kHsvWheelSizeH = 200;
static const int kHsvWheelSizeS = 150;
static const int kHsvWheelSizeV = 100;
static const int kHsvArcWidth = 10;

/* Main menu carousel index (Connection=0, LED=1, Audio=2, Standby=3, ...). */
static const uint8_t kMenuIdxLed = 1;
static const uint8_t kMenuIdxStandby = 3;

static lv_obj_t *s_audio;
static lv_obj_t *s_audio_switch;
static bool s_audio_muted;
static lv_obj_t *s_standby;
static lv_obj_t *s_standby_btns[3];
static uint8_t s_standby_focus_idx;
static uint8_t s_standby_return_idx;
static bool s_standby_bind_active;
static lv_obj_t *s_standby_caption;
static uint8_t s_standby_caption_pending_idx;

static const char *const kStandbyCaptions[] = {
    "Astronaut",
    "Dashboard",
    "Power",
};

static lv_obj_t *s_power;
static lv_obj_t *s_power_kb_v_lbl;
static lv_obj_t *s_power_kb_i_lbl;
static lv_obj_t *s_power_knob_v_lbl;
static lv_timer_t *s_power_timer;
static uint16_t s_power_kb_bus_mv;
static int16_t s_power_kb_current_ma;
static bool s_power_kb_valid;

#define POWER_CURRENT_INVERT      1
#define POWER_I_COLOR_CHARGE      0x88E888
#define POWER_I_COLOR_DISCHARGE   0xF09090
static lv_obj_t *s_instrument;
static lv_obj_t *s_instrument_arcs[3];
static lv_obj_t *s_instrument_cpu_row;
static lv_obj_t *s_instrument_cpu_val_lbl;
static lv_obj_t *s_instrument_cpu_pct_lbl;
static lv_obj_t *s_instrument_gpu_row;
static lv_obj_t *s_instrument_gpu_val_lbl;
static lv_obj_t *s_instrument_ram_row;
static lv_obj_t *s_instrument_ram_val_lbl;
static lv_obj_t *s_astronaut;
static lv_obj_t *s_strain_cal;
static lv_obj_t *s_strain_cal_prompt_lbl;
static lv_obj_t *s_strain_cal_countdown_arc;
static lv_timer_t *s_strain_cal_done_timer;
static bool s_sub_inited;

#define STRAIN_CAL_PROMPT_Y 0
static const int kStrainCalArcSize = 220;
static const uint32_t kStrainCalCountdownMs = 3000;

static bool strain_cal_text_is_countdown(const char *text)
{
    if (text == NULL || text[0] == '\0' || text[1] != '\0') {
        return false;
    }
    return text[0] >= '1' && text[0] <= '9';
}

static void ui_sub_strain_cal_arc_anim_cb(void *var, int32_t v)
{
    lv_arc_set_value((lv_obj_t *)var, v);
}

static void ui_sub_strain_cal_countdown_stop(void)
{
    if (s_strain_cal_countdown_arc == NULL) {
        return;
    }
    lv_anim_del(s_strain_cal_countdown_arc, ui_sub_strain_cal_arc_anim_cb);
    lv_arc_set_value(s_strain_cal_countdown_arc, 0);
    lv_obj_add_flag(s_strain_cal_countdown_arc, LV_OBJ_FLAG_HIDDEN);
}

static void ui_sub_strain_cal_countdown_start(uint32_t duration_ms)
{
    if (s_strain_cal_countdown_arc == NULL) {
        return;
    }
    lv_anim_del(s_strain_cal_countdown_arc, ui_sub_strain_cal_arc_anim_cb);
    lv_arc_set_value(s_strain_cal_countdown_arc, 0);
    lv_obj_clear_flag(s_strain_cal_countdown_arc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_strain_cal_countdown_arc);

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_strain_cal_countdown_arc);
    lv_anim_set_exec_cb(&anim, ui_sub_strain_cal_arc_anim_cb);
    lv_anim_set_values(&anim, 0, 360);
    lv_anim_set_time(&anim, duration_ms);
    lv_anim_set_path_cb(&anim, lv_anim_path_linear);
    lv_anim_start(&anim);
}

typedef enum {
    INST_SWEEP_UP,
    INST_SWEEP_DOWN,
    INST_LABEL_FADE_IN,
    INST_LIVE,
} inst_phase_t;

static uint8_t s_inst_target_pct[3];
static float s_inst_arc_pos[3] = {20.0f, 20.0f, 20.0f};
static float s_inst_arc_vel[3];
static int32_t s_inst_arc_goal[3] = {20, 20, 20};
static inst_phase_t s_inst_phase = INST_LIVE;
static lv_timer_t *s_inst_smooth_timer;

static const uint32_t kInstSmoothPeriodMs = 30;
static const uint32_t kInstLabelFadeMs = 250;
static const int kInstArcMin = 20;
static const int kInstArcMax = 80;
static const float kInstArcVmax = 55.0f;
static const float kInstArcAmax = 280.0f;
static const float kInstArcDtS = kInstSmoothPeriodMs / 1000.0f;
static const lv_coord_t kInstCpuRowW = 130;
static const lv_coord_t kInstCpuRowX = 15;
static const lv_coord_t kInstCpuValW = 72;
static const lv_coord_t kInstCpuPctX = 10;
static const lv_coord_t kInstCpuValGap = 4;
static const lv_coord_t kInstSideRowW = 74;
static const lv_coord_t kInstSideValW = 38;
static const lv_coord_t kInstGpuRowY = 55;
static const lv_coord_t kInstRamRowY = 70;

static lv_obj_t *ui_sub_instrument_create_stat_row(lv_obj_t *parent, const char *prefix, lv_coord_t y_ofs,
                                                   lv_obj_t **val_lbl_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, kInstSideRowW, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *pfx = lv_label_create(row);
    lv_label_set_text(pfx, prefix);
    lv_obj_set_style_text_font(pfx, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(pfx, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(pfx, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *val = lv_label_create(row);
    lv_obj_set_width(val, kInstSideValW);
    lv_obj_set_height(val, LV_SIZE_CONTENT);
    lv_label_set_long_mode(val, LV_LABEL_LONG_CLIP);
    lv_label_set_text(val, "0%");
    lv_obj_set_style_text_font(val, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(val, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_align(val, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_align(row, LV_ALIGN_CENTER, 0, y_ofs);
    if (val_lbl_out != NULL) {
        *val_lbl_out = val;
    }
    return row;
}

static int32_t ui_sub_inst_pct_to_arc(uint8_t pct)
{
    return kInstArcMin + ((int32_t)pct * (kInstArcMax - kInstArcMin)) / 100;
}

static float inst_trap_step(float pos, float goal, float *vel, float dt_s, float v_max, float a_max)
{
    const float eps = 0.01f;
    float dist = goal - pos;
    if (fabsf(dist) < eps) {
        *vel = 0.0f;
        return goal;
    }
    float sign = (dist > 0.0f) ? 1.0f : -1.0f;
    float v = *vel;
    float v_stop = sqrtf(2.0f * a_max * fabsf(dist));
    if (v_stop > v_max) {
        v_stop = v_max;
    }
    float v_abs = fabsf(v);
    float target_v_abs;
    if (v_abs > v_stop + eps || sign * v < 0.0f) {
        target_v_abs = v_abs - a_max * dt_s;
        if (target_v_abs < 0.0f) {
            target_v_abs = 0.0f;
        }
    } else {
        target_v_abs = v_abs + a_max * dt_s;
        if (target_v_abs > v_stop) {
            target_v_abs = v_stop;
        }
        if (target_v_abs > v_max) {
            target_v_abs = v_max;
        }
    }
    v = sign * target_v_abs;
    *vel = v;
    float new_pos = pos + v * dt_s;
    if (sign > 0.0f && new_pos > goal) {
        new_pos = goal;
        *vel = 0.0f;
    } else if (sign < 0.0f && new_pos < goal) {
        new_pos = goal;
        *vel = 0.0f;
    }
    return new_pos;
}

static bool inst_arc_at_goal(float pos, float goal, float vel)
{
    return fabsf(pos - goal) < 0.01f && fabsf(vel) < 0.01f;
}

static void ui_sub_instrument_label_opa_anim_cb(void *var, int32_t v)
{
    lv_obj_set_style_opa(var, (lv_opa_t)v, LV_PART_MAIN);
}

static lv_obj_t *ui_sub_instrument_label_at(int i)
{
    switch (i) {
    case 0:
        return s_instrument_cpu_row;
    case 1:
        return s_instrument_gpu_row;
    default:
        return s_instrument_ram_row;
    }
}

static void ui_sub_instrument_labels_hide(void)
{
    for (int i = 0; i < 3; i++) {
        lv_obj_t *lbl = ui_sub_instrument_label_at(i);
        if (lbl == NULL) {
            continue;
        }
        lv_anim_del(lbl, ui_sub_instrument_label_opa_anim_cb);
        lv_obj_set_style_opa(lbl, LV_OPA_TRANSP, LV_PART_MAIN);
    }
}

static void ui_sub_instrument_apply_label_texts(void)
{
    if (s_instrument_cpu_val_lbl != NULL) {
        lv_label_set_text_fmt(s_instrument_cpu_val_lbl, "%u", (unsigned)s_inst_target_pct[0]);
    }
    if (s_instrument_gpu_val_lbl != NULL) {
        lv_label_set_text_fmt(s_instrument_gpu_val_lbl, "%u%%", (unsigned)s_inst_target_pct[1]);
    }
    if (s_instrument_ram_val_lbl != NULL) {
        lv_label_set_text_fmt(s_instrument_ram_val_lbl, "%u%%", (unsigned)s_inst_target_pct[2]);
    }
}

static void ui_sub_instrument_enter_live(void);

static void ui_sub_instrument_labels_fade_in_ready_cb(lv_anim_t *a)
{
    (void)a;
    ui_sub_instrument_enter_live();
}

static void ui_sub_instrument_labels_fade_in(void)
{
    for (int i = 0; i < 3; i++) {
        lv_obj_t *lbl = ui_sub_instrument_label_at(i);
        if (lbl == NULL) {
            continue;
        }
        lv_anim_del(lbl, ui_sub_instrument_label_opa_anim_cb);
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, lbl);
        lv_anim_set_exec_cb(&anim, ui_sub_instrument_label_opa_anim_cb);
        lv_anim_set_values(&anim, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_time(&anim, kInstLabelFadeMs);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
        if (i == 0) {
            lv_anim_set_ready_cb(&anim, ui_sub_instrument_labels_fade_in_ready_cb);
        }
        lv_anim_start(&anim);
    }
}

static void ui_sub_instrument_enter_live(void)
{
    s_inst_phase = INST_LIVE;
    for (int i = 0; i < 3; i++) {
        s_inst_arc_goal[i] = ui_sub_inst_pct_to_arc(s_inst_target_pct[i]);
        s_inst_arc_vel[i] = 0.0f;
    }
    haptics_set_instrument_cpu_arc(s_inst_arc_pos[0]);
    haptics_instrument_latch_live_origin();
}

static void ui_sub_instrument_intro_step_arcs(float goal)
{
    for (int i = 0; i < 3; i++) {
        s_inst_arc_pos[i] =
            inst_trap_step(s_inst_arc_pos[i], goal, &s_inst_arc_vel[i], kInstArcDtS, kInstArcVmax, kInstArcAmax);
        lv_arc_set_value(s_instrument_arcs[i], (int32_t)lrintf(s_inst_arc_pos[i]));
    }
}

static void ui_sub_instrument_smooth_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    display_lock();
    if (s_inst_phase == INST_SWEEP_UP) {
        ui_sub_instrument_intro_step_arcs((float)kInstArcMax);
        if (inst_arc_at_goal(s_inst_arc_pos[0], (float)kInstArcMax, s_inst_arc_vel[0])) {
            s_inst_phase = INST_SWEEP_DOWN;
        }
    } else if (s_inst_phase == INST_SWEEP_DOWN) {
        ui_sub_instrument_intro_step_arcs((float)kInstArcMin);
        if (inst_arc_at_goal(s_inst_arc_pos[0], (float)kInstArcMin, s_inst_arc_vel[0])) {
            s_inst_phase = INST_LABEL_FADE_IN;
            ui_sub_instrument_apply_label_texts();
            ui_sub_instrument_labels_fade_in();
        }
    } else if (s_inst_phase == INST_LIVE) {
        for (int i = 0; i < 3; i++) {
            s_inst_arc_goal[i] = ui_sub_inst_pct_to_arc(s_inst_target_pct[i]);
            s_inst_arc_pos[i] = inst_trap_step(s_inst_arc_pos[i], (float)s_inst_arc_goal[i], &s_inst_arc_vel[i],
                                               kInstArcDtS, kInstArcVmax, kInstArcAmax);
            lv_arc_set_value(s_instrument_arcs[i], (int32_t)lrintf(s_inst_arc_pos[i]));
        }
        haptics_set_instrument_cpu_arc(s_inst_arc_pos[0]);
    }
    display_unlock();
}

void ui_screen_sub_instrument_smooth_start(void)
{
    display_lock();
    haptics_set_instrument_hold(false);
    s_inst_phase = INST_SWEEP_UP;
    for (int i = 0; i < 3; i++) {
        s_inst_arc_pos[i] = (float)kInstArcMin;
        s_inst_arc_vel[i] = 0.0f;
        s_inst_arc_goal[i] = kInstArcMin;
        lv_arc_set_value(s_instrument_arcs[i], kInstArcMin);
    }
    ui_sub_instrument_labels_hide();
    if (s_inst_smooth_timer == NULL) {
        s_inst_smooth_timer = lv_timer_create(ui_sub_instrument_smooth_timer_cb, kInstSmoothPeriodMs, NULL);
    } else {
        lv_timer_resume(s_inst_smooth_timer);
    }
    display_unlock();
}

void ui_screen_sub_instrument_smooth_stop(void)
{
    haptics_set_instrument_hold(false);
    for (int i = 0; i < 3; i++) {
        lv_obj_t *lbl = ui_sub_instrument_label_at(i);
        if (lbl != NULL) {
            lv_anim_del(lbl, ui_sub_instrument_label_opa_anim_cb);
            lv_obj_set_style_opa(lbl, LV_OPA_COVER, LV_PART_MAIN);
        }
    }
    s_inst_phase = INST_LIVE;
    if (s_inst_smooth_timer != NULL) {
        lv_timer_pause(s_inst_smooth_timer);
    }
}

static lv_color_hsv_t s_hsv_color = {0, 99, 99};
static bool s_hsv_haptics_pending;
static bool s_hsv_channel_press_armed;
static uint8_t s_hsv_channel;
static bool s_hsv_channel_pending;
static int32_t s_hsv_last_pos = -1;

static const uint32_t kHsvTxIntervalMs = 50;
static uint32_t s_hsv_last_tx_ms;
static lv_timer_t *s_hsv_tx_timer;
static bool s_hsv_tx_dirty;

static const int kLedCarouselBtnSize = 100;
static const int kLedCarouselNearSize = 50;
static const int kLedCarouselFarSize = 32;
static const int kLedCarouselImgSrcSize = 128;
static const int kLedCarouselScrollPad = (240 - kLedCarouselBtnSize) / 2;
static const uint32_t kLedCarouselAnimMs = 300;
static const uint32_t kLedCaptionFadeMs = 200;
static const int kLedCaptionYOffset = 78;

static uint8_t s_led_focus_idx;
static uint8_t s_led_caption_pending_idx;

static const char *const kLedCaptions[] = {
    "Solid On",
    "Breathing",
    "Rhythm",
};

static int32_t ui_sub_hsv_wrap_hue(int32_t pos);
static int32_t ui_sub_hsv_position_for_channel(void);
static uint16_t ui_sub_hsv_h_display(void);
static lv_color_hsv_t ui_sub_hsv_for_wheels(void);
static void ui_sub_hsv_sync_sv_from_pos(int32_t pos);
static lv_obj_t *ui_sub_hsv_create_wheel(lv_obj_t *parent, int size, lv_colorwheel_mode_t mode);
static void ui_sub_hsv_recreate_sat_wheel(void);
static void ui_sub_hsv_set_h_wheel(void);
static void ui_sub_hsv_sync_hsv_to_wheels(void);

/* Three concentric lv_colorwheel widgets: ring = LV_PART_MAIN, selector dot = LV_PART_KNOB. */
static void ui_sub_hsv_sync_knob_channel(void)
{
    if (s_cw_h == NULL) {
        return;
    }

    lv_obj_set_style_opa(s_cw_h, s_hsv_channel == 0 ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_opa(s_cw_s, s_hsv_channel == 1 ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_opa(s_cw_v, s_hsv_channel == 2 ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_KNOB);

    /* Outermost ring at the back; active channel on top for the knob only. */
    lv_obj_move_background(s_cw_h);
    lv_obj_move_background(s_cw_s);

    lv_obj_t *active = s_cw_h;
    if (s_hsv_channel == 1) {
        active = s_cw_s;
    } else if (s_hsv_channel == 2) {
        active = s_cw_v;
    }
    lv_obj_move_foreground(active);
}

void ui_screen_sub_hsv_exit_to_menu(void)
{
    s_hsv_channel = 0;
    s_hsv_channel_pending = false;
    s_hsv_haptics_pending = false;
    s_hsv_channel_press_armed = false;
    ui_screens_set_menu_return_idx(kMenuIdxLed);
    ui_screens_show_menu();
    ui_screens_arm_menu_input_guard(500);
}

static void ui_sub_hsv_apply_channel_switch(void)
{
    if (s_hsv_channel >= 3) {
        ui_screen_sub_hsv_exit_to_menu();
        return;
    }

    int32_t pos = ui_sub_hsv_position_for_channel();
    if (s_hsv_channel == 0) {
        pos = ui_sub_hsv_wrap_hue(pos);
    }
    haptics_apply_profile_and_position((uint8_t)(s_hsv_channel + 1), pos);
    s_hsv_last_pos = pos;
    ui_nav_sync_position();
    ui_sub_hsv_sync_sv_from_pos(pos);

    switch (s_hsv_channel) {
    case 1:
        ui_sub_hsv_recreate_sat_wheel();
        ui_sub_hsv_set_h_wheel();
        lv_colorwheel_set_hsv(s_cw_s, ui_sub_hsv_for_wheels());
        lv_colorwheel_set_hsv(s_cw_v, ui_sub_hsv_for_wheels());
        break;
    case 2:
        lv_colorwheel_set_hsv(s_cw_v, ui_sub_hsv_for_wheels());
        break;
    default:
        break;
    }
    ui_sub_hsv_sync_knob_channel();
}

/* Motor/logic h = pos; on-screen hue mirrors so knob CW matches hand (same as Arduino hsv_temp.h). */
static uint16_t ui_sub_hsv_h_display(void)
{
    uint16_t h = (uint16_t)(360 - s_hsv_color.h);
    if (h >= 360) {
        h = 0;
    }
    return h;
}

static lv_color_hsv_t ui_sub_hsv_for_wheels(void)
{
    lv_color_hsv_t color = s_hsv_color;
    color.h = ui_sub_hsv_h_display();
    if (color.s > 99) {
        color.s = 99;
    }
    if (color.v > 99) {
        color.v = 99;
    }
    return color;
}

static void ui_sub_hsv_sync_sv_from_pos(int32_t pos)
{
    if (pos < 0) {
        pos = 0;
    } else if (pos > 99) {
        pos = 99;
    }
    switch (s_hsv_channel) {
    case 1:
        s_hsv_color.s = (uint8_t)(99 - pos);
        break;
    case 2:
        s_hsv_color.v = (uint8_t)(99 - pos);
        break;
    default:
        break;
    }
}

static void ui_sub_hsv_recreate_sat_wheel(void)
{
    if (s_cw_s != NULL) {
        lv_obj_del(s_cw_s);
    }
    s_cw_s = ui_sub_hsv_create_wheel(s_led_hsv, kHsvWheelSizeS, LV_COLORWHEEL_MODE_SATURATION);
}

/* Outermost hue ring always renders at full S/V (matches Arduino: cw_h not updated on S/V channels). */
static void ui_sub_hsv_set_h_wheel(void)
{
    lv_color_hsv_t hue_ring = ui_sub_hsv_for_wheels();
    hue_ring.s = 100;
    hue_ring.v = 100;
    lv_colorwheel_set_hsv(s_cw_h, hue_ring);
}

static void ui_sub_hsv_sync_hsv_to_wheels(void)
{
    ui_sub_hsv_set_h_wheel();
    lv_colorwheel_set_hsv(s_cw_s, ui_sub_hsv_for_wheels());
    lv_colorwheel_set_hsv(s_cw_v, ui_sub_hsv_for_wheels());
}

static void ui_sub_hsv_update_wheels(bool full_redraw)
{
    if (s_cw_h == NULL) {
        return;
    }

    if (full_redraw) {
        ui_sub_hsv_sync_hsv_to_wheels();
        return;
    }

    switch (s_hsv_channel) {
    case 0:
        ui_sub_hsv_set_h_wheel();
        lv_colorwheel_set_hsv(s_cw_s, ui_sub_hsv_for_wheels());
        lv_colorwheel_set_hsv(s_cw_v, ui_sub_hsv_for_wheels());
        break;
    case 1:
        lv_colorwheel_set_hsv(s_cw_s, ui_sub_hsv_for_wheels());
        lv_colorwheel_set_hsv(s_cw_v, ui_sub_hsv_for_wheels());
        break;
    case 2:
        lv_colorwheel_set_hsv(s_cw_v, ui_sub_hsv_for_wheels());
        break;
    default:
        break;
    }
}

static int32_t ui_sub_hsv_wrap_hue(int32_t pos)
{
    pos %= 360;
    if (pos < 0) {
        pos += 360;
    }
    return pos;
}

static int32_t ui_sub_hsv_position_for_channel(void)
{
    switch (s_hsv_channel) {
    case 0:
        return (int32_t)s_hsv_color.h;
    case 1:
        return 99 - (int32_t)s_hsv_color.s;
    case 2:
        return 99 - (int32_t)s_hsv_color.v;
    default:
        return 0;
    }
}

static void ui_sub_hsv_apply_position(int32_t pos)
{
    switch (s_hsv_channel) {
    case 0:
        s_hsv_color.h = (uint16_t)ui_sub_hsv_wrap_hue(pos);
        break;
    case 1:
        if (pos < 0) {
            pos = 0;
        } else if (pos > 99) {
            pos = 99;
        }
        s_hsv_color.s = (uint8_t)(99 - pos);
        break;
    case 2:
        if (pos < 0) {
            pos = 0;
        } else if (pos > 99) {
            pos = 99;
        }
        s_hsv_color.v = (uint8_t)(99 - pos);
        break;
    default:
        break;
    }

    ui_sub_hsv_update_wheels(false);
}

static void ui_sub_hsv_tx_now(void)
{
    s_hsv_tx_dirty = false;
    s_hsv_last_tx_ms = lv_tick_get();
    uint8_t send_h = (uint8_t)(ui_sub_hsv_h_display() / 1.4f);
    uint8_t send_s = (uint8_t)(s_hsv_color.s * 2.5f);
    uint8_t send_v = (uint8_t)(s_hsv_color.v * 2.5f);
    host_link_send_remote_cmd(REMOTE_LED_HSV, send_h, send_s, send_v);
}

static void ui_sub_hsv_tx_flush(lv_timer_t *timer)
{
    (void)timer;
    s_hsv_tx_timer = NULL;
    if (s_hsv_tx_dirty) {
        ui_sub_hsv_tx_now();
    }
}

static void ui_sub_send_hsv(void)
{
    s_hsv_tx_dirty = true;

    uint32_t now = lv_tick_get();
    uint32_t elapsed = now - s_hsv_last_tx_ms;
    if (elapsed >= kHsvTxIntervalMs) {
        if (s_hsv_tx_timer != NULL) {
            lv_timer_del(s_hsv_tx_timer);
            s_hsv_tx_timer = NULL;
        }
        ui_sub_hsv_tx_now();
        return;
    }

    if (s_hsv_tx_timer == NULL) {
        uint32_t delay = kHsvTxIntervalMs - elapsed;
        if (delay < 1) {
            delay = 1;
        }
        s_hsv_tx_timer = lv_timer_create(ui_sub_hsv_tx_flush, delay, NULL);
        lv_timer_set_repeat_count(s_hsv_tx_timer, 1);
    }
}

void ui_screen_sub_hsv_flush(void)
{
    if (!s_hsv_tx_dirty) {
        return;
    }
    if (s_hsv_tx_timer != NULL) {
        lv_timer_del(s_hsv_tx_timer);
        s_hsv_tx_timer = NULL;
    }
    ui_sub_hsv_tx_now();
}

static int32_t ui_sub_led_visual_size_for_distance(uint32_t dist)
{
    if (dist == 0) {
        return kLedCarouselBtnSize;
    }
    if (dist == 1) {
        return kLedCarouselNearSize;
    }
    return kLedCarouselFarSize;
}

static uint32_t ui_sub_led_scale_for_visual(int32_t visual_size)
{
    uint32_t scale = (uint32_t)(visual_size * 9 / 10 * LV_IMG_ZOOM_NONE / kLedCarouselImgSrcSize);
    return scale == 0 ? 1 : scale;
}

static void ui_sub_led_apply_visual_size(lv_obj_t *btn, int32_t visual_size)
{
    lv_obj_t *img = lv_obj_get_child(btn, 0);
    if (img == NULL) {
        return;
    }

    lv_img_set_zoom(img, ui_sub_led_scale_for_visual(visual_size));
    lv_obj_center(img);
}

static int32_t ui_sub_led_visual_from_btn(lv_obj_t *btn)
{
    lv_obj_t *img = lv_obj_get_child(btn, 0);
    if (img == NULL) {
        return kLedCarouselBtnSize;
    }

    uint32_t scale = lv_img_get_zoom(img);
    return (int32_t)((int64_t)scale * kLedCarouselImgSrcSize * 10 / LV_IMG_ZOOM_NONE / 9);
}

static void ui_sub_led_visual_anim_cb(void *var, int32_t v)
{
    ui_sub_led_apply_visual_size(var, v);
}

static void ui_sub_led_caption_opa_anim_cb(void *var, int32_t v)
{
    lv_obj_set_style_opa(var, (lv_opa_t)v, LV_PART_MAIN);
}

static void ui_sub_led_caption_fade_in(void)
{
    lv_anim_del(s_led_caption, ui_sub_led_caption_opa_anim_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_led_caption);
    lv_anim_set_exec_cb(&a, ui_sub_led_caption_opa_anim_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&a, kLedCaptionFadeMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void ui_sub_led_caption_fade_out_ready_cb(lv_anim_t *a)
{
    lv_obj_t *caption = a->var;
    lv_label_set_text(caption, kLedCaptions[s_led_caption_pending_idx]);
    ui_sub_led_caption_fade_in();
}

static void ui_sub_led_caption_show(uint8_t idx, lv_anim_enable_t anim)
{
    if (s_led_caption == NULL || idx >= sizeof(kLedCaptions) / sizeof(kLedCaptions[0])) {
        return;
    }

    s_led_caption_pending_idx = idx;
    lv_obj_clear_flag(s_led_caption, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_led_caption, LV_ALIGN_CENTER, 0, kLedCaptionYOffset);

    if (anim == LV_ANIM_OFF) {
        lv_anim_del(s_led_caption, ui_sub_led_caption_opa_anim_cb);
        lv_label_set_text(s_led_caption, kLedCaptions[idx]);
        lv_obj_set_style_opa(s_led_caption, LV_OPA_COVER, LV_PART_MAIN);
        return;
    }

    const char *cur_text = lv_label_get_text(s_led_caption);
    if (cur_text != NULL && strcmp(cur_text, kLedCaptions[idx]) == 0 &&
        lv_obj_get_style_opa(s_led_caption, LV_PART_MAIN) == LV_OPA_COVER) {
        return;
    }

    lv_anim_del(s_led_caption, ui_sub_led_caption_opa_anim_cb);
    lv_opa_t cur_opa = lv_obj_get_style_opa(s_led_caption, LV_PART_MAIN);
    if (cur_opa == LV_OPA_TRANSP) {
        lv_label_set_text(s_led_caption, kLedCaptions[idx]);
        ui_sub_led_caption_fade_in();
        return;
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_led_caption);
    lv_anim_set_exec_cb(&a, ui_sub_led_caption_opa_anim_cb);
    lv_anim_set_values(&a, cur_opa, LV_OPA_TRANSP);
    lv_anim_set_time(&a, kLedCaptionFadeMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_ready_cb(&a, ui_sub_led_caption_fade_out_ready_cb);
    lv_anim_start(&a);
}

static void ui_sub_led_set_btn_visual(lv_obj_t *btn, int32_t visual_size, lv_anim_enable_t anim)
{
    lv_obj_set_size(btn, kLedCarouselBtnSize, kLedCarouselBtnSize);

    if (anim == LV_ANIM_OFF) {
        lv_anim_del(btn, ui_sub_led_visual_anim_cb);
        ui_sub_led_apply_visual_size(btn, visual_size);
        return;
    }

    int32_t cur = ui_sub_led_visual_from_btn(btn);
    if (cur == visual_size) {
        ui_sub_led_apply_visual_size(btn, visual_size);
        return;
    }

    lv_anim_del(btn, ui_sub_led_visual_anim_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, btn);
    lv_anim_set_exec_cb(&a, ui_sub_led_visual_anim_cb);
    lv_anim_set_values(&a, cur, visual_size);
    lv_anim_set_time(&a, kLedCarouselAnimMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static uint32_t ui_sub_led_circular_distance(uint32_t a, uint32_t b, uint32_t cnt)
{
    uint32_t diff = (a > b) ? (a - b) : (b - a);
    if (diff > cnt / 2) {
        diff = cnt - diff;
    }
    return diff;
}

static void ui_sub_led_apply_focus(lv_obj_t *focused, lv_anim_enable_t anim)
{
    uint32_t cnt = lv_obj_get_child_cnt(s_led);
    if (cnt == 0 || focused == NULL) {
        return;
    }

    uint32_t focus_idx = lv_obj_get_index(focused);
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t dist = ui_sub_led_circular_distance(i, focus_idx, cnt);
        int32_t visual = ui_sub_led_visual_size_for_distance(dist);
        ui_sub_led_set_btn_visual(lv_obj_get_child(s_led, i), visual, anim);
    }

    lv_obj_update_layout(s_led);
    lv_obj_scroll_to_view(focused, anim);
    ui_sub_led_caption_show((uint8_t)focus_idx, anim);
}

static void ui_sub_led_focus_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_FOCUSED) {
        return;
    }

    lv_obj_t *focused = lv_event_get_target(e);
    s_led_focus_idx = (uint8_t)lv_obj_get_index(focused);
    ui_sub_led_apply_focus(focused, LV_ANIM_ON);
}

static lv_obj_t *ui_sub_led_add_btn(const lv_img_dsc_t *icon, lv_event_cb_t cb, lv_event_code_t trigger)
{
    lv_obj_t *btn = lv_btn_create(s_led);
    lv_obj_set_size(btn, kLedCarouselBtnSize, kLedCarouselBtnSize);
    lv_obj_remove_style_all(btn);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, cb, trigger, NULL);
    lv_obj_add_event_cb(btn, ui_sub_led_focus_cb, LV_EVENT_FOCUSED, NULL);
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

static void ui_sub_bind_led_items(void)
{
    ui_nav_clear_group();
    ui_nav_sync_position();
    lv_group_set_wrap(lv_group_get_default(), true);
    for (size_t i = 0; i < sizeof(s_led_btns) / sizeof(s_led_btns[0]); i++) {
        ui_nav_bind_object(s_led_btns[i]);
    }

    s_led_focus_idx = 0;
    lv_group_focus_obj(s_led_btns[0]);
    ui_sub_led_apply_focus(s_led_btns[0], LV_ANIM_OFF);
}

void ui_screen_sub_led_on_encoder(int32_t delta)
{
    if (delta == 0 || ui_screens_current() != UI_SCR_LED) {
        return;
    }

    const uint32_t cnt = sizeof(s_led_btns) / sizeof(s_led_btns[0]);
    int32_t idx = (int32_t)s_led_focus_idx + delta;
    while (idx < 0) {
        idx += (int32_t)cnt;
    }
    while (idx >= (int32_t)cnt) {
        idx -= (int32_t)cnt;
    }

    if ((uint8_t)idx == s_led_focus_idx) {
        return;
    }

    s_led_focus_idx = (uint8_t)idx;
    lv_group_focus_obj(s_led_btns[s_led_focus_idx]);
}

static lv_obj_t *ui_sub_create_panel(void)
{
    lv_obj_t *scr = lv_obj_create(lv_scr_act());
    lv_obj_set_size(scr, 240, 240);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_HIDDEN);
    return scr;
}

static lv_obj_t *ui_sub_hsv_create_wheel(lv_obj_t *parent, int size, lv_colorwheel_mode_t mode)
{
    /* knob_recolor=true: LV_PART_KNOB follows lv_colorwheel_get_rgb() */
    lv_obj_t *cw = lv_colorwheel_create(parent, true);
    lv_obj_set_size(cw, size, size);
    lv_obj_set_style_arc_width(cw, kHsvArcWidth, LV_PART_MAIN);
    lv_colorwheel_set_hsv(cw, ui_sub_hsv_for_wheels());
    lv_colorwheel_set_mode(cw, mode);
    lv_obj_center(cw);
    lv_colorwheel_set_mode_fixed(cw, true);
    lv_obj_set_style_bg_opa(cw, LV_OPA_TRANSP, LV_PART_MAIN);
    return cw;
}

static void ui_screen_sub_hsv_enter(void)
{
    s_hsv_color.h = 0;
    s_hsv_color.s = 99;
    s_hsv_color.v = 99;
    s_hsv_channel = 0;
    s_hsv_channel_pending = false;
    s_hsv_haptics_pending = true;
    s_hsv_channel_press_armed = false;

    ui_sub_hsv_update_wheels(true);
    ui_sub_hsv_sync_knob_channel();
    s_hsv_last_pos = ui_sub_hsv_position_for_channel();
}

void ui_screen_sub_hsv_on_press(void)
{
    if (!s_hsv_channel_press_armed) {
        return;
    }

    s_hsv_channel++;
    if (s_hsv_channel >= 3) {
        s_hsv_channel_pending = false;
        ui_screen_sub_hsv_flush();
        uint8_t send_h = (uint8_t)(ui_sub_hsv_h_display() / 1.4f);
        uint8_t send_s = (uint8_t)(s_hsv_color.s * 2.5f);
        uint8_t send_v = (uint8_t)(s_hsv_color.v * 2.5f);
        host_link_send_remote_cmd(REMOTE_LED_HSV_COMMIT, send_h, send_s, send_v);
        ui_screen_sub_hsv_exit_to_menu();
        return;
    }

    /* Defer profile/knob switch to poll (matches original my_gui.cpp timing). */
    s_hsv_channel_pending = true;
}

bool ui_screen_sub_hsv_on_release(void)
{
    if (ui_screens_current() != UI_SCR_LED_HSV) {
        return false;
    }

    if (s_hsv_haptics_pending) {
        s_hsv_haptics_pending = false;
        int32_t pos = ui_sub_hsv_position_for_channel();
        pos = ui_sub_hsv_wrap_hue(pos);
        haptics_apply_profile_and_position(1, pos);
        s_hsv_last_pos = pos;
        ui_nav_sync_position();
    }

    s_hsv_channel_press_armed = true;
    return true;
}

void ui_screen_sub_hsv_poll(void)
{
    if (ui_screens_current() != UI_SCR_LED_HSV) {
        return;
    }

    if (s_hsv_channel_pending) {
        s_hsv_channel_pending = false;
        ui_sub_hsv_apply_channel_switch();
    }

    int32_t pos = haptics_get_position();
    if (pos == s_hsv_last_pos) {
        return;
    }

    s_hsv_last_pos = pos;
    ui_sub_hsv_apply_position(pos);
    ui_sub_send_hsv();
}

bool ui_screen_sub_handle_encoder(lv_obj_t *focused, int32_t delta)
{
    /* 未使用：子屏编码器由 ui_nav.c 直接调用 ui_screen_sub_*_on_encoder() */
    (void)focused;
    (void)delta;
    return false;
}

static void ui_sub_led_color_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_PRESSED) {
        return;
    }
    ui_screens_set_menu_return_idx(kMenuIdxLed);
    ui_screens_show(UI_SCR_LED_HSV);
    ui_sub_hsv_tx_now();
}

static void ui_sub_led_breath_cb(lv_event_t *e)
{
    (void)e;
    ui_post_remote_cmd(REMOTE_LED_BREATH, 0, 0, 0);
    ui_screens_show(UI_SCR_MENU);
}

static void ui_sub_led_rhythm_cb(lv_event_t *e)
{
    (void)e;
    ui_post_remote_cmd(REMOTE_LED_RHYTHM, 0, 0, 0);
    ui_screens_show(UI_SCR_MENU);
}

/* ========== Audio：switch 亮=有声；编码器发 UART，0x84 仅同步显示 ========== */

static void ui_sub_audio_set_display(bool muted)
{
    s_audio_muted = muted;
    if (s_audio_switch == NULL) {
        return;
    }

    bool sound_on = !muted;
    bool cur = lv_obj_has_state(s_audio_switch, LV_STATE_CHECKED);
    if (cur == sound_on) {
        return;
    }

    if (sound_on) {
        lv_obj_add_state(s_audio_switch, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(s_audio_switch, LV_STATE_CHECKED);
    }
}

static void ui_sub_audio_apply_muted(bool muted)
{
    if (s_audio_muted == muted) {
        return;
    }
    ui_sub_audio_set_display(muted);
    ui_post_remote_cmd(muted ? REMOTE_AUDIO_MUTE : REMOTE_AUDIO_UNMUTE, 0, 0, 0);
}

void ui_screen_sub_audio_enter(void)
{
    ui_sub_audio_set_display(s_audio_muted);
}

void ui_screen_sub_on_keyboard_audio(bool sound_on)
{
    ui_sub_audio_set_display(!sound_on);
}

void ui_screen_sub_audio_on_encoder(int32_t delta)
{
    if (delta == 0 || ui_screens_current() != UI_SCR_AUDIO) {
        return;
    }

    /* Clockwise unmute (on), counter-clockwise mute (off). */
    ui_sub_audio_apply_muted(delta < 0);
}

void ui_screen_sub_audio_on_press(void)
{
    ui_screens_show_menu();
}

static uint32_t ui_sub_standby_scale_for_visual(int32_t visual_size)
{
    uint32_t scale = (uint32_t)(visual_size * 9 / 10 * LV_IMG_ZOOM_NONE / kLedCarouselImgSrcSize);
    return scale == 0 ? 1 : scale;
}

static int32_t ui_sub_standby_visual_size_for_distance(uint32_t dist)
{
    if (dist == 0) {
        return kLedCarouselBtnSize;
    }
    if (dist == 1) {
        return kLedCarouselNearSize;
    }
    return kLedCarouselFarSize;
}

static uint32_t ui_sub_standby_circular_distance(uint32_t a, uint32_t b, uint32_t cnt)
{
    uint32_t diff = (a > b) ? (a - b) : (b - a);
    if (diff > cnt / 2) {
        diff = cnt - diff;
    }
    return diff;
}

static void ui_sub_standby_apply_visual_size(lv_obj_t *btn, int32_t visual_size)
{
    lv_obj_t *img = lv_obj_get_child(btn, 0);
    if (img == NULL) {
        return;
    }

    lv_img_set_zoom(img, ui_sub_standby_scale_for_visual(visual_size));
    lv_obj_center(img);
}

static int32_t ui_sub_standby_visual_from_btn(lv_obj_t *btn)
{
    lv_obj_t *img = lv_obj_get_child(btn, 0);
    if (img == NULL) {
        return kLedCarouselBtnSize;
    }

    uint32_t scale = lv_img_get_zoom(img);
    return (int32_t)((int64_t)scale * kLedCarouselImgSrcSize * 10 / LV_IMG_ZOOM_NONE / 9);
}

static void ui_sub_standby_visual_anim_cb(void *var, int32_t v)
{
    ui_sub_standby_apply_visual_size(var, v);
}

static void ui_sub_standby_set_btn_visual(lv_obj_t *btn, int32_t visual_size, lv_anim_enable_t anim)
{
    lv_obj_set_size(btn, kLedCarouselBtnSize, kLedCarouselBtnSize);

    if (anim == LV_ANIM_OFF) {
        lv_anim_del(btn, ui_sub_standby_visual_anim_cb);
        ui_sub_standby_apply_visual_size(btn, visual_size);
        return;
    }

    int32_t cur = ui_sub_standby_visual_from_btn(btn);
    if (cur == visual_size) {
        ui_sub_standby_apply_visual_size(btn, visual_size);
        return;
    }

    lv_anim_del(btn, ui_sub_standby_visual_anim_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, btn);
    lv_anim_set_exec_cb(&a, ui_sub_standby_visual_anim_cb);
    lv_anim_set_values(&a, cur, visual_size);
    lv_anim_set_time(&a, kLedCarouselAnimMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void ui_sub_standby_caption_opa_anim_cb(void *var, int32_t v)
{
    lv_obj_set_style_opa(var, (lv_opa_t)v, LV_PART_MAIN);
}

static void ui_sub_standby_caption_fade_in(void)
{
    lv_anim_del(s_standby_caption, ui_sub_standby_caption_opa_anim_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_standby_caption);
    lv_anim_set_exec_cb(&a, ui_sub_standby_caption_opa_anim_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&a, kLedCaptionFadeMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void ui_sub_standby_caption_fade_out_ready_cb(lv_anim_t *a)
{
    lv_obj_t *caption = a->var;
    lv_label_set_text(caption, kStandbyCaptions[s_standby_caption_pending_idx]);
    ui_sub_standby_caption_fade_in();
}

static void ui_sub_standby_caption_show(uint8_t idx, lv_anim_enable_t anim)
{
    if (s_standby_caption == NULL || idx >= sizeof(kStandbyCaptions) / sizeof(kStandbyCaptions[0])) {
        return;
    }

    s_standby_caption_pending_idx = idx;
    lv_obj_clear_flag(s_standby_caption, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_standby_caption, LV_ALIGN_CENTER, 0, kLedCaptionYOffset);

    if (anim == LV_ANIM_OFF) {
        lv_anim_del(s_standby_caption, ui_sub_standby_caption_opa_anim_cb);
        lv_label_set_text(s_standby_caption, kStandbyCaptions[idx]);
        lv_obj_set_style_opa(s_standby_caption, LV_OPA_COVER, LV_PART_MAIN);
        return;
    }

    const char *cur_text = lv_label_get_text(s_standby_caption);
    if (cur_text != NULL && strcmp(cur_text, kStandbyCaptions[idx]) == 0 &&
        lv_obj_get_style_opa(s_standby_caption, LV_PART_MAIN) == LV_OPA_COVER) {
        return;
    }

    lv_anim_del(s_standby_caption, ui_sub_standby_caption_opa_anim_cb);
    lv_opa_t cur_opa = lv_obj_get_style_opa(s_standby_caption, LV_PART_MAIN);
    if (cur_opa == LV_OPA_TRANSP) {
        lv_label_set_text(s_standby_caption, kStandbyCaptions[idx]);
        ui_sub_standby_caption_fade_in();
        return;
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_standby_caption);
    lv_anim_set_exec_cb(&a, ui_sub_standby_caption_opa_anim_cb);
    lv_anim_set_values(&a, cur_opa, LV_OPA_TRANSP);
    lv_anim_set_time(&a, kLedCaptionFadeMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_ready_cb(&a, ui_sub_standby_caption_fade_out_ready_cb);
    lv_anim_start(&a);
}

static void ui_sub_standby_apply_focus(lv_obj_t *focused, lv_anim_enable_t anim)
{
    uint32_t cnt = lv_obj_get_child_cnt(s_standby);
    if (cnt == 0 || focused == NULL) {
        return;
    }

    uint32_t focus_idx = lv_obj_get_index(focused);
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t dist = ui_sub_standby_circular_distance(i, focus_idx, cnt);
        int32_t visual = ui_sub_standby_visual_size_for_distance(dist);
        ui_sub_standby_set_btn_visual(lv_obj_get_child(s_standby, i), visual, anim);
    }

    lv_obj_update_layout(s_standby);
    lv_obj_scroll_to_view(focused, anim);
    ui_sub_standby_caption_show((uint8_t)focus_idx, anim);
}

static void ui_sub_standby_focus_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_FOCUSED || s_standby_bind_active) {
        return;
    }

    lv_obj_t *focused = lv_event_get_target(e);
    s_standby_focus_idx = (uint8_t)lv_obj_get_index(focused);
    s_standby_return_idx = s_standby_focus_idx;
    ui_sub_standby_apply_focus(focused, LV_ANIM_ON);
}

static lv_obj_t *ui_sub_standby_add_btn(const lv_img_dsc_t *icon, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(s_standby);
    lv_obj_set_size(btn, kLedCarouselBtnSize, kLedCarouselBtnSize);
    lv_obj_remove_style_all(btn);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn, ui_sub_standby_focus_cb, LV_EVENT_FOCUSED, NULL);
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

static void ui_sub_bind_standby_items(void)
{
    uint8_t restore_idx = s_standby_return_idx;

    s_standby_bind_active = true;
    ui_nav_clear_group();
    ui_nav_sync_position();
    lv_group_set_wrap(lv_group_get_default(), true);
    for (size_t i = 0; i < sizeof(s_standby_btns) / sizeof(s_standby_btns[0]); i++) {
        ui_nav_bind_object(s_standby_btns[i]);
    }

    const uint32_t cnt = sizeof(s_standby_btns) / sizeof(s_standby_btns[0]);
    if (restore_idx >= cnt) {
        restore_idx = 0;
    }

    s_standby_focus_idx = restore_idx;
    s_standby_return_idx = restore_idx;
    lv_group_focus_obj(s_standby_btns[restore_idx]);
    ui_sub_standby_apply_focus(s_standby_btns[restore_idx], LV_ANIM_OFF);
    s_standby_bind_active = false;
}

void ui_screen_sub_standby_on_encoder(int32_t delta)
{
    if (delta == 0 || ui_screens_current() != UI_SCR_STANDBY) {
        return;
    }

    const uint32_t cnt = sizeof(s_standby_btns) / sizeof(s_standby_btns[0]);
    int32_t idx = (int32_t)s_standby_focus_idx + delta;
    while (idx < 0) {
        idx += (int32_t)cnt;
    }
    while (idx >= (int32_t)cnt) {
        idx -= (int32_t)cnt;
    }

    if ((uint8_t)idx == s_standby_focus_idx) {
        return;
    }

    s_standby_focus_idx = (uint8_t)idx;
    s_standby_return_idx = (uint8_t)idx;
    lv_group_focus_obj(s_standby_btns[s_standby_focus_idx]);
}

void ui_screen_sub_standby_feature_on_press(void)
{
    if (ui_screens_current() == UI_SCR_INSTRUMENT) {
        ui_screen_sub_leave_instrument();
        ui_screens_show_menu();
        return;
    }
    if (ui_screens_current() == UI_SCR_ASTRONAUT) {
        ui_screens_show_menu();
    }
}

static void ui_sub_standby_astronaut_cb(lv_event_t *e)
{
    (void)e;
    ui_screens_show(UI_SCR_ASTRONAUT);
}

static void ui_sub_standby_instrument_cb(lv_event_t *e)
{
    (void)e;
    knob_event_t evt = {};
    evt.type = KNOB_EVT_MODE_ENTER;
    evt.data.mode_change.mode = APP_MODE_INSTRUMENT;
    event_bus_publish(&evt, 0);
    ui_screens_show(UI_SCR_INSTRUMENT);
}

/* ========== Power 屏：键盘 0x82 电压/电流 + 旋钮本地电压；Standby 第三项入口 ========== */

static void ui_sub_power_format_value(lv_obj_t *lbl, uint32_t value_mv, const char *unit)
{
    lv_label_set_text_fmt(lbl, "%u.%02u %s",
                          (unsigned)(value_mv / 1000),
                          (unsigned)((value_mv % 1000) / 10),
                          unit);
}

static void ui_sub_power_format_current(lv_obj_t *lbl, int16_t current_ma)
{
#if POWER_CURRENT_INVERT
    current_ma = (int16_t)(-current_ma);
#endif
    uint32_t abs_ma = (uint32_t)(current_ma < 0 ? -current_ma : current_ma);
    if (current_ma > 0) {
        lv_label_set_text_fmt(lbl, "+%u.%02u A",
                              (unsigned)(abs_ma / 1000),
                              (unsigned)((abs_ma % 1000) / 10));
        lv_obj_set_style_text_color(lbl, lv_color_hex(POWER_I_COLOR_CHARGE), LV_PART_MAIN);
    } else if (current_ma < 0) {
        lv_label_set_text_fmt(lbl, "-%u.%02u A",
                              (unsigned)(abs_ma / 1000),
                              (unsigned)((abs_ma % 1000) / 10));
        lv_obj_set_style_text_color(lbl, lv_color_hex(POWER_I_COLOR_DISCHARGE), LV_PART_MAIN);
    } else {
        lv_label_set_text(lbl, "0.00 A");
        lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN);
    }
}

static void ui_sub_power_refresh_labels(void)
{
    if (s_power_kb_v_lbl == NULL || s_power_kb_i_lbl == NULL || s_power_knob_v_lbl == NULL) {
        return;
    }

    /* LVGL printf 不支持 %f，用整数拼出 X.XX */
    if (s_power_kb_valid) {
        ui_sub_power_format_value(s_power_kb_v_lbl, s_power_kb_bus_mv, "V");
        ui_sub_power_format_current(s_power_kb_i_lbl, s_power_kb_current_ma);
    } else {
        lv_label_set_text(s_power_kb_v_lbl, "-- V");
        lv_label_set_text(s_power_kb_i_lbl, "-- A");
        lv_obj_set_style_text_color(s_power_kb_i_lbl, lv_color_white(), LV_PART_MAIN);
    }

    uint32_t knob_mv = battery_monitor_voltage_mv();
    if (knob_mv > 0) {
        ui_sub_power_format_value(s_power_knob_v_lbl, knob_mv, "V");
    } else {
        lv_label_set_text(s_power_knob_v_lbl, "-- V");
    }
}

static void ui_sub_power_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    ui_sub_power_refresh_labels();
}

static void ui_sub_power_leave(void)
{
    if (s_power_timer != NULL) {
        lv_timer_del(s_power_timer);
        s_power_timer = NULL;
    }
}

static void ui_sub_power_enter(void)
{
    ui_sub_power_refresh_labels();
    if (s_power_timer == NULL) {
        s_power_timer = lv_timer_create(ui_sub_power_timer_cb, 500, NULL);
    }
}

void ui_screen_sub_power_on_press(void)
{
    ui_screens_set_menu_return_idx(kMenuIdxStandby);
    ui_screens_show_menu();
}

static void ui_sub_standby_power_cb(lv_event_t *e)
{
    (void)e;
    ui_screens_show(UI_SCR_POWER);
}

static lv_obj_t *ui_sub_power_add_line(lv_obj_t *parent, const char *text, int y_ofs)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, y_ofs);
    return lbl;
}

static void ui_screen_sub_build(void)
{
    s_led = ui_sub_create_panel();
    lv_obj_set_size(s_led, 240, 240);
    lv_obj_set_style_pad_column(s_led, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_left(s_led, kLedCarouselScrollPad, LV_PART_MAIN);
    lv_obj_set_style_pad_right(s_led, kLedCarouselScrollPad, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_led, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_led, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(s_led, LV_DIR_HOR);
    lv_obj_add_flag(s_led, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_snap_x(s_led, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(s_led, LV_SCROLLBAR_MODE_OFF);
    lv_obj_center(s_led);
    s_led_btns[0] = ui_sub_led_add_btn(&alpha_led_on_pic, ui_sub_led_color_cb, LV_EVENT_PRESSED);
    s_led_btns[1] = ui_sub_led_add_btn(&alpha_led_breathing_pic, ui_sub_led_breath_cb, LV_EVENT_CLICKED);
    s_led_btns[2] = ui_sub_led_add_btn(&alpha_led_rhythm_pic, ui_sub_led_rhythm_cb, LV_EVENT_CLICKED);

    s_led_caption = lv_label_create(lv_scr_act());
    lv_label_set_text(s_led_caption, kLedCaptions[0]);
    lv_obj_set_width(s_led_caption, 220);
    lv_obj_set_style_text_color(s_led_caption, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_led_caption, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_led_caption, LV_ALIGN_CENTER, 0, kLedCaptionYOffset);
    lv_obj_add_flag(s_led_caption, LV_OBJ_FLAG_HIDDEN);

    s_led_hsv = ui_sub_create_panel();
    s_cw_h = ui_sub_hsv_create_wheel(s_led_hsv, kHsvWheelSizeH, LV_COLORWHEEL_MODE_HUE);
    s_cw_s = ui_sub_hsv_create_wheel(s_led_hsv, kHsvWheelSizeS, LV_COLORWHEEL_MODE_SATURATION);
    s_cw_v = ui_sub_hsv_create_wheel(s_led_hsv, kHsvWheelSizeV, LV_COLORWHEEL_MODE_VALUE);
    ui_sub_hsv_sync_knob_channel();

    s_audio = ui_sub_create_panel();
    s_audio_switch = lv_switch_create(s_audio);
    lv_obj_set_size(s_audio_switch, 90, 44);
    lv_obj_center(s_audio_switch);
    lv_obj_clear_flag(s_audio_switch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *au_hint = lv_label_create(s_audio);
    lv_label_set_text(au_hint, "Mute / Unmute");
    lv_obj_align(au_hint, LV_ALIGN_BOTTOM_MID, 0, -24);

    s_standby = ui_sub_create_panel();
    lv_obj_set_size(s_standby, 240, 240);
    lv_obj_set_style_pad_column(s_standby, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_left(s_standby, kLedCarouselScrollPad, LV_PART_MAIN);
    lv_obj_set_style_pad_right(s_standby, kLedCarouselScrollPad, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_standby, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_standby, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(s_standby, LV_DIR_HOR);
    lv_obj_add_flag(s_standby, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_snap_x(s_standby, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(s_standby, LV_SCROLLBAR_MODE_OFF);
    lv_obj_center(s_standby);
    s_standby_btns[0] = ui_sub_standby_add_btn(&alpha_stdby_astronaut_pic, ui_sub_standby_astronaut_cb);
    s_standby_btns[1] = ui_sub_standby_add_btn(&alpha_stdby_instrument_pic, ui_sub_standby_instrument_cb);
    s_standby_btns[2] = ui_sub_standby_add_btn(&alpha_stdby_power_pic, ui_sub_standby_power_cb);

    s_standby_caption = lv_label_create(lv_scr_act());
    lv_label_set_text(s_standby_caption, kStandbyCaptions[0]);
    lv_obj_set_width(s_standby_caption, 220);
    lv_obj_set_style_text_color(s_standby_caption, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_standby_caption, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_standby_caption, LV_ALIGN_CENTER, 0, kLedCaptionYOffset);
    lv_obj_add_flag(s_standby_caption, LV_OBJ_FLAG_HIDDEN);

    s_instrument = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_instrument, 240, 240);
    lv_obj_set_style_bg_opa(s_instrument, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_img_src(s_instrument, &alpha_instrument_pic, LV_PART_MAIN);
    lv_obj_set_style_bg_img_opa(s_instrument, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_instrument, 0, LV_PART_MAIN);
    lv_obj_center(s_instrument);
    lv_obj_add_flag(s_instrument, LV_OBJ_FLAG_HIDDEN);
    const uint32_t arc_colors[] = { 0xFF8C00, 0x00BFFF, 0x9400D3 };
    const int arc_sizes[] = { 185, 163, 141 };
    for (int i = 0; i < 3; i++) {
        s_instrument_arcs[i] = lv_arc_create(s_instrument);
        lv_obj_set_size(s_instrument_arcs[i], arc_sizes[i], arc_sizes[i]);
        lv_obj_center(s_instrument_arcs[i]);
        lv_obj_set_style_bg_opa(s_instrument_arcs[i], LV_OPA_TRANSP, LV_PART_MAIN);
        lv_arc_set_range(s_instrument_arcs[i], 20, 80);
        lv_arc_set_value(s_instrument_arcs[i], kInstArcMin);
        lv_obj_clear_flag(s_instrument_arcs[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(s_instrument_arcs[i], 0, LV_PART_MAIN);
        lv_obj_set_style_arc_width(s_instrument_arcs[i], 12, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(s_instrument_arcs[i], lv_color_hex(arc_colors[i]), LV_PART_INDICATOR);
        lv_obj_remove_style(s_instrument_arcs[i], NULL, LV_PART_KNOB);
    }
    s_instrument_cpu_row = lv_obj_create(s_instrument);
    lv_obj_set_size(s_instrument_cpu_row, kInstCpuRowW, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_instrument_cpu_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_instrument_cpu_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_instrument_cpu_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_instrument_cpu_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_instrument_cpu_row, LV_ALIGN_CENTER, kInstCpuRowX, 0);

    s_instrument_cpu_pct_lbl = lv_label_create(s_instrument_cpu_row);
    lv_label_set_text(s_instrument_cpu_pct_lbl, "%");
    lv_obj_set_style_text_font(s_instrument_cpu_pct_lbl, &lv_font_montserrat_34, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_instrument_cpu_pct_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(s_instrument_cpu_pct_lbl, LV_ALIGN_CENTER, kInstCpuPctX, 0);

    s_instrument_cpu_val_lbl = lv_label_create(s_instrument_cpu_row);
    lv_obj_set_width(s_instrument_cpu_val_lbl, kInstCpuValW);
    lv_obj_set_height(s_instrument_cpu_val_lbl, LV_SIZE_CONTENT);
    lv_label_set_long_mode(s_instrument_cpu_val_lbl, LV_LABEL_LONG_CLIP);
    lv_label_set_text(s_instrument_cpu_val_lbl, "0");
    lv_obj_set_style_text_font(s_instrument_cpu_val_lbl, &lv_font_montserrat_34, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_instrument_cpu_val_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_instrument_cpu_val_lbl, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_align_to(s_instrument_cpu_val_lbl, s_instrument_cpu_pct_lbl, LV_ALIGN_OUT_LEFT_MID, -kInstCpuValGap, 0);
    s_instrument_gpu_row =
        ui_sub_instrument_create_stat_row(s_instrument, "GPU:", kInstGpuRowY, &s_instrument_gpu_val_lbl);
    s_instrument_ram_row =
        ui_sub_instrument_create_stat_row(s_instrument, "RAM:", kInstRamRowY, &s_instrument_ram_val_lbl);

    s_astronaut = lv_animimg_create(lv_scr_act());
    lv_obj_set_size(s_astronaut, 240, 240);
    lv_obj_center(s_astronaut);
    lv_obj_set_style_pad_all(s_astronaut, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_astronaut, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(s_astronaut, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_astronaut, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_clear_flag(s_astronaut, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_astronaut, LV_OBJ_FLAG_HIDDEN);

    static const lv_img_dsc_t *astro_frames[] = {
        &tkr01, &tkr02, &tkr03, &tkr04, &tkr05, &tkr06, &tkr07,
        &tkr08, &tkr09, &tkr10, &tkr11, &tkr12, &tkr13,
    };
    lv_animimg_set_src(s_astronaut, (const void **)astro_frames,
                       sizeof(astro_frames) / sizeof(astro_frames[0]));
    lv_animimg_set_duration(s_astronaut, 1000);
    lv_animimg_set_repeat_count(s_astronaut, LV_ANIM_REPEAT_INFINITE);
    lv_animimg_start(s_astronaut);

    s_power = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_power, 240, 240);
    lv_obj_center(s_power);
    lv_obj_set_style_bg_color(s_power, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_power, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_power, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_power, 0, LV_PART_MAIN);
    lv_obj_add_flag(s_power, LV_OBJ_FLAG_HIDDEN);

    ui_sub_power_add_line(s_power, "Keyboard", -72);
    s_power_kb_v_lbl = ui_sub_power_add_line(s_power, "-- V", -44);
    s_power_kb_i_lbl = ui_sub_power_add_line(s_power, "-- A", -16);
    ui_sub_power_add_line(s_power, "Knob", 24);
    s_power_knob_v_lbl = ui_sub_power_add_line(s_power, "-- V", 52);

    s_strain_cal = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_strain_cal, 240, 240);
    lv_obj_center(s_strain_cal);
    lv_obj_set_style_bg_color(s_strain_cal, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_strain_cal, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_strain_cal, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_strain_cal, 0, LV_PART_MAIN);
    lv_obj_add_flag(s_strain_cal, LV_OBJ_FLAG_HIDDEN);

    s_strain_cal_prompt_lbl = lv_label_create(s_strain_cal);
    lv_label_set_long_mode(s_strain_cal_prompt_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_strain_cal_prompt_lbl, 210);
    lv_obj_set_style_text_align(s_strain_cal_prompt_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_strain_cal_prompt_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_strain_cal_prompt_lbl, UI_FONT_CAL_PROMPT, LV_PART_MAIN);
    lv_label_set_text(s_strain_cal_prompt_lbl, "");
    lv_obj_align(s_strain_cal_prompt_lbl, LV_ALIGN_CENTER, 0, STRAIN_CAL_PROMPT_Y);

    s_strain_cal_countdown_arc = lv_arc_create(s_strain_cal);
    lv_obj_set_size(s_strain_cal_countdown_arc, kStrainCalArcSize, kStrainCalArcSize);
    lv_obj_center(s_strain_cal_countdown_arc);
    lv_obj_set_style_bg_opa(s_strain_cal_countdown_arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_arc_set_range(s_strain_cal_countdown_arc, 0, 360);
    lv_arc_set_bg_angles(s_strain_cal_countdown_arc, 0, 360);
    lv_arc_set_rotation(s_strain_cal_countdown_arc, 270);
    lv_arc_set_mode(s_strain_cal_countdown_arc, LV_ARC_MODE_NORMAL);
    lv_arc_set_value(s_strain_cal_countdown_arc, 0);
    lv_obj_clear_flag(s_strain_cal_countdown_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_strain_cal_countdown_arc, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_strain_cal_countdown_arc, lv_color_hex(0x404040), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_strain_cal_countdown_arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_strain_cal_countdown_arc, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_remove_style(s_strain_cal_countdown_arc, NULL, LV_PART_KNOB);
    lv_obj_add_flag(s_strain_cal_countdown_arc, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "Sub-screens ready");
}

void ui_screen_sub_ensure_init(void)
{
    if (s_sub_inited) {
        return;
    }
    display_lock();
    ui_screen_sub_build();
    s_sub_inited = true;
    display_unlock();
}

void ui_screen_sub_init(void)
{
    ui_screen_sub_ensure_init();
}

static void ui_sub_strain_cal_done_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    s_strain_cal_done_timer = NULL;
    ui_screens_menu_cal_suppress_auto_enter();
    ui_screens_show_menu();
}

void ui_screen_sub_strain_cal_set_prompt(const char *text)
{
    ui_screen_sub_ensure_init();
    if (s_strain_cal_prompt_lbl == NULL || text == NULL) {
        return;
    }
    display_lock();
    if (strain_cal_text_is_countdown(text)) {
        if (text[0] == '3') {
            ui_sub_strain_cal_countdown_start(kStrainCalCountdownMs);
        }
    } else {
        ui_sub_strain_cal_countdown_stop();
        lv_label_set_text(s_strain_cal_prompt_lbl, text);
        lv_obj_align(s_strain_cal_prompt_lbl, LV_ALIGN_CENTER, 0, STRAIN_CAL_PROMPT_Y);
        lv_obj_move_foreground(s_strain_cal_prompt_lbl);
    }
    display_unlock();
}

void ui_screen_sub_strain_cal_on_done(bool ok)
{
    ui_screen_sub_ensure_init();
    display_lock();
    if (s_strain_cal_done_timer != NULL) {
        lv_timer_del(s_strain_cal_done_timer);
        s_strain_cal_done_timer = NULL;
    }
    ui_sub_strain_cal_countdown_stop();
    lv_label_set_text(s_strain_cal_prompt_lbl, ok ? UI_STR_CAL_DONE_OK : UI_STR_CAL_DONE_FAIL);
    lv_obj_align(s_strain_cal_prompt_lbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_move_foreground(s_strain_cal_prompt_lbl);
    s_strain_cal_done_timer = lv_timer_create(ui_sub_strain_cal_done_timer_cb, 1500, NULL);
    lv_timer_set_repeat_count(s_strain_cal_done_timer, 1);
    display_unlock();
}

void ui_screen_sub_strain_cal_leave(void)
{
    if (s_strain_cal_done_timer != NULL) {
        lv_timer_del(s_strain_cal_done_timer);
        s_strain_cal_done_timer = NULL;
    }
    display_lock();
    ui_sub_strain_cal_countdown_stop();
    display_unlock();
    input_strain_cal_cancel();
}

void ui_screen_sub_bind_nav(ui_screen_t screen)
{
    switch (screen) {
    case UI_SCR_LED:
        ui_sub_bind_led_items();
        break;
    case UI_SCR_LED_HSV:
        ui_nav_clear_group();
        ui_screen_sub_hsv_enter();
        break;
    case UI_SCR_AUDIO:
        ui_nav_clear_group();
        ui_nav_sync_position();
        lv_group_set_wrap(lv_group_get_default(), false);
        ui_screen_sub_audio_enter();
        break;
    case UI_SCR_STANDBY:
        ui_sub_bind_standby_items();
        break;
    case UI_SCR_ASTRONAUT:
        ui_nav_clear_group();
        ui_nav_sync_position();
        lv_group_set_wrap(lv_group_get_default(), false);
        lv_animimg_start(s_astronaut);
        break;
    case UI_SCR_POWER:
        ui_nav_clear_group();
        ui_nav_sync_position();
        lv_group_set_wrap(lv_group_get_default(), false);
        ui_sub_power_enter();
        break;
    case UI_SCR_STRAIN_CAL:
        ui_nav_clear_group();
        ui_nav_sync_position();
        lv_group_set_wrap(lv_group_get_default(), false);
        break;
    default:
        break;
    }
}

void ui_screen_sub_hide_all(void)
{
    if (!s_sub_inited) {
        return;
    }
    lv_obj_add_flag(s_led, LV_OBJ_FLAG_HIDDEN);
    if (s_led_caption != NULL) {
        lv_obj_add_flag(s_led_caption, LV_OBJ_FLAG_HIDDEN);
        lv_anim_del(s_led_caption, ui_sub_led_caption_opa_anim_cb);
    }
    lv_obj_add_flag(s_led_hsv, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_audio, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_standby, LV_OBJ_FLAG_HIDDEN);
    if (s_standby_caption != NULL) {
        lv_obj_add_flag(s_standby_caption, LV_OBJ_FLAG_HIDDEN);
        lv_anim_del(s_standby_caption, ui_sub_standby_caption_opa_anim_cb);
    }
    ui_screen_sub_instrument_smooth_stop();
    lv_obj_add_flag(s_instrument, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_astronaut, LV_OBJ_FLAG_HIDDEN);
    ui_sub_power_leave();
    lv_obj_add_flag(s_power, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_strain_cal, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *ui_screen_sub_get_panel(ui_screen_t screen)
{
    ui_screen_sub_ensure_init();
    switch (screen) {
    case UI_SCR_LED:
        return s_led;
    case UI_SCR_LED_HSV:
        return s_led_hsv;
    case UI_SCR_AUDIO:
        return s_audio;
    case UI_SCR_STANDBY:
        return s_standby;
    case UI_SCR_INSTRUMENT:
        return s_instrument;
    case UI_SCR_ASTRONAUT:
        return s_astronaut;
    case UI_SCR_POWER:
        return s_power;
    case UI_SCR_STRAIN_CAL:
        return s_strain_cal;
    default:
        return NULL;
    }
}

void ui_screen_sub_on_keyboard_power(uint16_t bus_mv, int16_t current_ma)
{
    ui_screen_sub_ensure_init();
    s_power_kb_bus_mv = bus_mv;
    s_power_kb_current_ma = current_ma;
    s_power_kb_valid = true;
    /* s_power_timer (500ms) refreshes labels on ui_task — no LVGL from dispatch */
}

void ui_screen_sub_on_host_stats(uint8_t cpu, uint8_t gpu, uint8_t ram)
{
    ui_screen_sub_ensure_init();
    s_inst_target_pct[0] = cpu;
    s_inst_target_pct[1] = gpu;
    s_inst_target_pct[2] = ram;
    if (s_inst_phase == INST_LIVE) {
        display_lock();
        ui_sub_instrument_apply_label_texts();
        display_unlock();
    }
    if (s_inst_smooth_timer != NULL) {
        lv_timer_resume(s_inst_smooth_timer);
    }
}

void ui_screen_sub_leave_instrument(void)
{
    ui_screen_sub_instrument_smooth_stop();
    haptics_set_instrument_active(false);
    knob_event_t evt = {};
    evt.type = KNOB_EVT_MODE_EXIT;
    event_bus_publish(&evt, 0);
}
