/**
 * @file ui.c
 * @brief UI 任务：LVGL 刷新与模式切换队列
 *
 * ui_task（core 1）周期调用 lv_timer_handler；模式变更经 s_mode_q 异步投递，
 * 避免在 app 事件上下文直接操作 LVGL。
 */

#include "ui.h"
#include "event_bus.h"

#include "display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "knob_events.h"
#include "input_strain.h"
#include "ui_nav.h"
#include "ui_screens.h"
#include "ui_screens_internal.h"

#include <sys/param.h>

static const char *TAG = "ui";

static QueueHandle_t s_mode_q;

static void ui_drain_mode_queue(void)
{
    app_mode_t mode;
    while (s_mode_q != NULL && xQueueReceive(s_mode_q, &mode, 0) == pdTRUE) {
        ui_screens_on_mode(mode);
    }
}

void ui_request_on_mode(app_mode_t mode)
{
    if (s_mode_q == NULL) {
        return;
    }
    BaseType_t ok;
    if (mode == APP_MODE_MENU) {
        ok = xQueueSendToFront(s_mode_q, &mode, pdMS_TO_TICKS(50));
    } else {
        ok = xQueueSend(s_mode_q, &mode, pdMS_TO_TICKS(50));
    }
    if (ok != pdTRUE) {
        ESP_LOGW(TAG, "mode queue full, drop mode %d", (int)mode);
    }
}

static void ui_task(void *arg)
{
    (void)arg;
    while (true) {
        ui_drain_mode_queue();
        display_lock();
        ui_screens_boot_splash_kick_if_pending();
        uint32_t delay_ms = lv_timer_handler();
        display_unlock();
        delay_ms = MAX(delay_ms, 5);
        if (ui_screens_current() == UI_SCR_LED_HSV) {
            delay_ms = MIN(delay_ms, 5);
        } else {
            delay_ms = MIN(delay_ms, 8);
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

esp_err_t ui_init(void)
{
    s_mode_q = xQueueCreate(8, sizeof(app_mode_t));
    if (s_mode_q == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_ERROR_CHECK(display_init());
    display_lock();
    ui_nav_init();
    ui_screens_init();
    display_unlock();
    return ESP_OK;
}

esp_err_t ui_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(ui_task, "ui", 20480, NULL, 5, NULL, 1);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "UI task started");
    return ESP_OK;
}

void ui_on_event(const knob_event_t *event)
{
    if (!event) {
        return;
    }
    switch (event->type) {
    case KNOB_EVT_MODE_ENTER:
        ui_request_on_mode(event->data.mode_change.mode);
        break;
    case KNOB_EVT_MODE_EXIT:
        ui_request_on_mode(APP_MODE_MENU);
        break;
    case KNOB_EVT_PRESS:
        if (ui_screens_boot_splash_active()) {
            break;
        }
        if (ui_screens_current() == UI_SCR_STRAIN_CAL) {
            break;
        }
        if (ui_screens_current() == UI_SCR_LED_HSV) {
            ui_screen_sub_hsv_on_press();
        } else if (ui_screens_current() == UI_SCR_AUDIO) {
            ui_screen_sub_audio_on_press();
        } else if (ui_screens_current() == UI_SCR_POWER) {
            if (!ui_screens_power_preview_active()) {
                ui_screen_sub_power_on_press();
            }
        } else if (ui_screens_current() == UI_SCR_ASTRONAUT ||
                   ui_screens_current() == UI_SCR_INSTRUMENT) {
            ui_screen_sub_standby_feature_on_press();
        } else if (ui_screens_menu_cal_focused() && !ui_screens_menu_input_guarded()) {
            ui_screens_menu_cal_enter();
        } else if (!ui_screens_menu_input_guarded() && !ui_screens_menu_cal_focused()) {
            ui_nav_on_press();
        }
        break;
    case KNOB_EVT_RELEASE:
        if (ui_screens_boot_splash_active()) {
            break;
        }
        if (ui_screens_current() == UI_SCR_STRAIN_CAL) {
            break;
        }
        if (ui_screen_sub_hsv_on_release()) {
            break;
        }
        if (!ui_screens_menu_cal_focused()) {
            ui_nav_on_release();
        }
        break;
    case KNOB_EVT_ROTATION:
        break;
    case KNOB_EVT_LONG_PRESS:
        if (ui_screens_boot_splash_active()) {
            break;
        }
        ui_screens_back();
        break;
    default:
        break;
    }
}

void ui_post_remote_cmd(remote_cmd_t cmd, uint8_t arg0, uint8_t arg1, uint8_t arg2)
{
    knob_event_t evt = {};
    evt.type = KNOB_EVT_REMOTE_CMD;
    evt.data.remote.cmd = cmd;
    evt.data.remote.arg0 = arg0;
    evt.data.remote.arg1 = arg1;
    evt.data.remote.arg2 = arg2;
    event_bus_publish(&evt, 0);
}
