/**
 * @file app.c
 * @brief 应用编排：子系统 init/start 与全局事件分发
 *
 * app_dispatch_task（core 1）从 event_bus 取事件，按类型驱动 UI、haptics、
 * BLE Surface Dial、host_link、板级 RF 等。模式切换的副作用集中在此处理。
 */

#include "app.h"

#include "battery_monitor.h"
#include "ble_dial.h"
#include "board.h"
#include "esp_log.h"
#include "event_bus.h"
#include "haptics.h"
#include "host_link.h"
#include "input_strain.h"
#include "knob_events.h"
#include "led_ring.h"
#include "ui.h"
#include "ui_nav.h"
#include "ui_screens.h"
#include "ui_screens_internal.h"

static const char *TAG = "app";

/** 当前 UI/触觉模式，与 KNOB_EVT_MODE_ENTER 同步 */
static app_mode_t s_current_mode = APP_MODE_MENU;

/** 事件分发任务句柄 */
static TaskHandle_t s_dispatch_task;

/** @brief 应变校准文案回调，转给 UI 子屏 */
static void app_strain_cal_prompt(const char *text)
{
    ui_screen_sub_strain_cal_set_prompt(text);
}

/** @brief 应变校准结束回调 */
static void app_strain_cal_done(bool ok)
{
    ui_screen_sub_strain_cal_on_done(ok);
}

/** @brief 低电量时屏蔽按压类事件，避免误触 */
static bool app_battery_blocks_press(knob_event_type_t type)
{
    if (!battery_monitor_is_low()) {
        return false;
    }
    return type == KNOB_EVT_PRESS || type == KNOB_EVT_RELEASE || type == KNOB_EVT_LONG_PRESS;
}

/** @brief 开机 Welcome 闪屏期间屏蔽按压，避免打断启动流程 */
static bool app_boot_splash_blocks_press(knob_event_type_t type)
{
    if (!ui_screens_boot_splash_active()) {
        return false;
    }
    return type == KNOB_EVT_PRESS || type == KNOB_EVT_RELEASE || type == KNOB_EVT_LONG_PRESS;
}

/**
 * @brief 单事件处理：模式切换、旋转/按压路由、host/键盘同步、远程命令
 *
 * Surface Dial 模式下旋转/短按交给 BLE。
 * host→device：HOST_STATS、POWER_STATS、AUDIO_STATUS、POWER_PREVIEW 转 UI。
 */
static void app_handle_event(const knob_event_t *event)
{
    switch (event->type) {
    case KNOB_EVT_MODE_ENTER:
        s_current_mode = event->data.mode_change.mode;
        ESP_LOGI(TAG, "Enter mode %d", (int)s_current_mode);
        board_set_rf_enable(s_current_mode == APP_MODE_SURFACE_DIAL);
        led_ring_set_dial_active(s_current_mode == APP_MODE_SURFACE_DIAL);
        ble_dial_set_active(s_current_mode == APP_MODE_SURFACE_DIAL);
        haptics_set_instrument_active(s_current_mode == APP_MODE_INSTRUMENT);
        if (s_current_mode == APP_MODE_SURFACE_DIAL) {
            haptics_set_profile(4);
            input_strain_set_long_press_ms(3000);
            input_strain_reset_long_press_progress();
        }
        ui_on_event(event);
        break;
    case KNOB_EVT_MODE_EXIT:
        {
            bool from_dial = (s_current_mode == APP_MODE_SURFACE_DIAL);
            s_current_mode = APP_MODE_MENU;
            board_set_rf_enable(false);
            led_ring_set_dial_active(false);
            haptics_set_instrument_active(false);
            if (from_dial) {
                ui_screens_on_dial_mode_exit();
                input_strain_set_long_press_ms(1400);
                input_strain_reset_long_press_progress();
            }
            /* 先同步恢复菜单档位，避免异步 set_position 导致首帧旋转跳到图标 0 */
            ui_screens_apply_menu_haptics();
            /* 先恢复菜单 UI 再关 BLE，降低峰值堆占用 */
            ui_request_on_mode(APP_MODE_MENU);
            ble_dial_set_active(false);
            ESP_LOGI(TAG, "Back to menu");
        }
        break;
    case KNOB_EVT_REMOTE_CMD: {
        esp_err_t err = host_link_send_remote_cmd(event->data.remote.cmd,
                                                  event->data.remote.arg0,
                                                  event->data.remote.arg1,
                                                  event->data.remote.arg2);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "remote cmd %u tx failed: %s",
                     (unsigned)event->data.remote.cmd, esp_err_to_name(err));
        }
        break;
    }
    case KNOB_EVT_HOST_STATS:
        haptics_set_cpu_load((float)event->data.host_stats.cpu_pct);
        ui_screens_on_host_stats(event->data.host_stats.cpu_pct,
                                 event->data.host_stats.gpu_pct,
                                 event->data.host_stats.ram_pct);
        break;
    case KNOB_EVT_KEYBOARD_POWER:
        ui_screens_on_keyboard_power(event->data.keyboard_power.bus_mv,
                                     event->data.keyboard_power.current_ma);
        break;
    case KNOB_EVT_KEYBOARD_AUDIO:
        ui_screens_on_keyboard_audio(event->data.keyboard_audio.sound_on);
        break;
    case KNOB_EVT_POWER_PREVIEW:
        ui_screens_power_preview(event->data.power_preview.pressed);
        break;
    case KNOB_EVT_ROTATION:
        if (s_current_mode != APP_MODE_SURFACE_DIAL) {
            ui_on_event(event);
        }
        ble_dial_on_event(event);
        break;
    case KNOB_EVT_PRESS:
        if (app_battery_blocks_press(event->type) || app_boot_splash_blocks_press(event->type)) {
            break;
        }
        if (s_current_mode != APP_MODE_SURFACE_DIAL) {
            ui_on_event(event);
        }
        ble_dial_on_event(event);
        break;
    case KNOB_EVT_RELEASE:
        if (app_battery_blocks_press(event->type) || app_boot_splash_blocks_press(event->type)) {
            break;
        }
        if (s_current_mode != APP_MODE_SURFACE_DIAL) {
            ui_on_event(event);
        }
        ble_dial_on_event(event);
        break;
    case KNOB_EVT_LONG_PRESS:
        if (app_battery_blocks_press(event->type) || app_boot_splash_blocks_press(event->type)) {
            break;
        }
        if (s_current_mode == APP_MODE_SURFACE_DIAL) {
            ESP_LOGI(TAG, "Dial long press -> exit");
            knob_event_t exit_evt = { .type = KNOB_EVT_MODE_EXIT };
            app_handle_event(&exit_evt);
        } else {
            ui_on_event(event);
        }
        ble_dial_on_event(event);
        break;
    case KNOB_EVT_BATTERY_LOW:
        haptics_set_brake_enabled(false);
        ui_screens_set_low_power(true);
        break;
    case KNOB_EVT_BATTERY_OK:
        haptics_set_brake_enabled(true);
        ui_screens_set_low_power(false);
        break;
    default:
        break;
    }
}

/** @brief 阻塞读取 event_bus 并分发 */
static void app_dispatch_task(void *arg)
{
    knob_event_t event;
    (void)arg;

    while (true) {
        if (event_bus_subscribe(&event, portMAX_DELAY)) {
            app_handle_event(&event);
        }
    }
}

esp_err_t app_init(void)
{
    ESP_ERROR_CHECK(event_bus_init());
    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(battery_monitor_init());
    ESP_ERROR_CHECK(haptics_init());
    ESP_ERROR_CHECK(haptics_start());
    ESP_ERROR_CHECK(host_link_init());
    ESP_ERROR_CHECK(input_strain_init());
    ESP_ERROR_CHECK(led_ring_init());
    ESP_ERROR_CHECK(ui_init());
    input_strain_cal_set_ui_hooks(app_strain_cal_prompt, app_strain_cal_done);
    return ESP_OK;
}

esp_err_t app_start(void)
{
    ESP_ERROR_CHECK(ui_start());
    ESP_ERROR_CHECK(battery_monitor_start());
    ESP_ERROR_CHECK(input_strain_start());
    ESP_ERROR_CHECK(led_ring_start());
    BaseType_t ok = xTaskCreatePinnedToCore(
        app_dispatch_task, "app_dispatch", 8192, NULL, 5, &s_dispatch_task, 1);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Application dispatcher started");
    return ESP_OK;
}

app_mode_t app_get_mode(void)
{
    return s_current_mode;
}
