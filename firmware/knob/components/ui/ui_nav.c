/**
 * @file ui_nav.c
 * @brief LVGL 编码器输入：菜单 NEXT/PREV，子屏专用旋转处理
 *
 * 默认将 haptics 档位映射为 LVGL 导航；以下子屏在 ui_nav 内直接分发编码器增量：
 * Connection / LED 轮播 / Audio（静音）/ Standby 轮播 / LED HSV 独占。
 */

#include "ui_nav.h"

#include "display.h"
#include "haptics.h"
#include "knob_events.h"
#include "ui_screens.h"
#include "ui_screens_internal.h"

static lv_indev_drv_t s_indev_drv;
static lv_indev_t *s_indev;
static lv_group_t *s_group;
static int32_t s_last_position;
static bool s_hsv_encoder_active;

typedef enum {
    NAV_KEY_NONE = 0,
    NAV_KEY_ENTER_PRESSED,
    NAV_KEY_ENTER_RELEASED,
} nav_key_pulse_t;

#define NAV_KEY_Q_SIZE 8
static nav_key_pulse_t s_key_q[NAV_KEY_Q_SIZE];
static volatile uint8_t s_key_q_head;
static volatile uint8_t s_key_q_tail;

static void nav_key_enqueue(nav_key_pulse_t pulse)
{
    uint8_t next = (uint8_t)((s_key_q_tail + 1U) % NAV_KEY_Q_SIZE);
    if (next == s_key_q_head) {
        return;
    }
    s_key_q[s_key_q_tail] = pulse;
    s_key_q_tail = next;
}

static bool nav_key_dequeue(nav_key_pulse_t *pulse)
{
    if (s_key_q_head == s_key_q_tail) {
        return false;
    }
    *pulse = s_key_q[s_key_q_head];
    s_key_q_head = (uint8_t)((s_key_q_head + 1U) % NAV_KEY_Q_SIZE);
    return true;
}

void ui_nav_reset_input(void)
{
    s_key_q_head = 0;
    s_key_q_tail = 0;
}

static void ui_indev_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data)
{
    (void)indev_drv;

    nav_key_pulse_t pulse;
    if (nav_key_dequeue(&pulse)) {
        if (pulse == NAV_KEY_ENTER_PRESSED) {
            data->key = LV_KEY_ENTER;
            data->state = LV_INDEV_STATE_PRESSED;
            return;
        }
        if (pulse == NAV_KEY_ENTER_RELEASED) {
            data->key = LV_KEY_ENTER;
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }
    }

    int32_t pos = haptics_get_position();
    lv_obj_t *focused = lv_group_get_focused(s_group);
    int32_t delta = pos - s_last_position;

    if (focused != NULL && lv_obj_check_type(focused, &lv_slider_class)) {
        if (delta != 0) {
            if (ui_screen_sub_handle_encoder(focused, delta)) {
                s_last_position = pos;
                s_hsv_encoder_active = true;
                data->key = 0;
                data->state = LV_INDEV_STATE_RELEASED;
                return;
            }

            if (delta > 0) {
                data->key = LV_KEY_RIGHT;
                data->state = LV_INDEV_STATE_PRESSED;
                s_last_position++;
            } else {
                data->key = LV_KEY_LEFT;
                data->state = LV_INDEV_STATE_PRESSED;
                s_last_position--;
            }
            return;
        }

        if (s_hsv_encoder_active) {
            s_hsv_encoder_active = false;
            ui_screen_sub_hsv_flush();
        }

        data->key = 0;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    if (s_hsv_encoder_active) {
        s_hsv_encoder_active = false;
        ui_screen_sub_hsv_flush();
    }

    if (ui_screens_current() == UI_SCR_LED_HSV) {
        ui_screen_sub_hsv_poll();
        s_last_position = haptics_get_position();
        data->key = 0;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    if (ui_screens_is_surface_dial()) {
        s_last_position = pos;
        data->key = 0;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    if (ui_screens_menu_nav_active()) {
        if (delta != 0) {
            if (!ui_screens_menu_input_guarded()) {
                ui_screens_menu_on_encoder(delta);
            }
            s_last_position = pos;
        }
        data->key = 0;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    if (ui_screens_current() == UI_SCR_CONNECTION && delta != 0) {
        ui_screens_connection_on_encoder(delta);
        s_last_position = pos;
        data->key = 0;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    if (ui_screens_current() == UI_SCR_LED && delta != 0) {
        ui_screen_sub_led_on_encoder(delta);
        s_last_position = pos;
        data->key = 0;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    if (ui_screens_current() == UI_SCR_AUDIO && delta != 0) {
        ui_screen_sub_audio_on_encoder(delta);
        s_last_position = pos;
        data->key = 0;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    if (ui_screens_current() == UI_SCR_STANDBY && delta != 0) {
        ui_screen_sub_standby_on_encoder(delta);
        s_last_position = pos;
        data->key = 0;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    if (delta > 0) {
        data->key = LV_KEY_NEXT;
        data->state = LV_INDEV_STATE_PRESSED;
        s_last_position++;
    } else if (delta < 0) {
        data->key = LV_KEY_PREV;
        data->state = LV_INDEV_STATE_PRESSED;
        s_last_position--;
    } else {
        data->key = 0;
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

void ui_nav_init(void)
{
    display_lock();
    ui_nav_reset_input();
    s_group = lv_group_create();
    lv_group_set_default(s_group);

    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type = LV_INDEV_TYPE_KEYPAD;
    s_indev_drv.read_cb = ui_indev_read;
    s_indev = lv_indev_drv_register(&s_indev_drv);
    lv_indev_set_group(s_indev, s_group);
    display_unlock();
}

void ui_nav_clear_group(void)
{
    display_lock();
    lv_group_remove_all_objs(s_group);
    display_unlock();
}

void ui_nav_bind_object(lv_obj_t *obj)
{
    display_lock();
    lv_group_add_obj(s_group, obj);
    display_unlock();
}

void ui_nav_on_press(void)
{
    if (ui_screens_menu_input_guarded()) {
        return;
    }
    nav_key_enqueue(NAV_KEY_ENTER_PRESSED);
}

void ui_nav_on_release(void)
{
    if (ui_screens_menu_input_guarded()) {
        return;
    }
    nav_key_enqueue(NAV_KEY_ENTER_RELEASED);
}

void ui_nav_sync_position(void)
{
    s_last_position = haptics_get_position();
}

void ui_nav_force_position(int32_t position)
{
    s_last_position = position;
}
