/**
 * @file ble_dial.c
 * @brief Surface Dial 模式：将旋钮事件映射为 BLE HID 报告
 *
 * 懒加载 NimBLE 栈；激活时切换 haptics profile 4（回中手感）。
 */

#include "ble_dial.h"

#include "ble_dial_hid.h"
#include "esp_log.h"
#include "haptics.h"
#include "knob_events.h"

static const char *TAG = "ble_dial";

static bool s_active;
static bool s_stack_ready;

esp_err_t ble_dial_init(void)
{
    if (s_stack_ready) {
        return ESP_OK;
    }
    esp_err_t err = ble_dial_hid_init();
    if (err == ESP_OK) {
        s_stack_ready = true;
    }
    return err;
}

void ble_dial_set_active(bool active)
{
    if (active && !s_stack_ready) {
        if (ble_dial_init() != ESP_OK) {
            ESP_LOGE(TAG, "BLE init failed (haptics/exit still work)");
        }
    }

    s_active = active;
    ble_dial_hid_set_active(active);
    if (active) {
        haptics_set_profile(4);
        ESP_LOGI(TAG, "Surface Dial mode active — pair LX_Dial in Windows Bluetooth");
    }
}

bool ble_dial_is_connected(void)
{
    return ble_dial_hid_is_connected();
}

/** @brief 旋转→左右键，按压→按下/释放 HID 报告 */
void ble_dial_on_event(const knob_event_t *event)
{
    if (!event || !s_active) {
        return;
    }

    switch (event->type) {
    case KNOB_EVT_ROTATION:
        if (event->data.rotation.delta_steps > 0) {
            ble_dial_hid_send_report(BLE_DIAL_KEY_R);
        } else if (event->data.rotation.delta_steps < 0) {
            ble_dial_hid_send_report(BLE_DIAL_KEY_L);
        }
        break;
    case KNOB_EVT_PRESS:
        ble_dial_hid_send_report(BLE_DIAL_KEY_PRESS);
        break;
    case KNOB_EVT_RELEASE:
        ble_dial_hid_send_report(BLE_DIAL_KEY_RELEASE);
        break;
    case KNOB_EVT_LONG_PRESS:
        ble_dial_hid_send_report(BLE_DIAL_KEY_RELEASE);
        break;
    default:
        break;
    }
}
