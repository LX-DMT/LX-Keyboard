/**
 * @file ui_boot_nvs.c
 * @brief 开机校准提示 NVS：按 build_id 判断是否需要自动进 Cal
 */

#include "ui_boot_nvs.h"

#include "build_id.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "ui_boot_nvs";

#define NVS_NS "ui_boot"
#define NVS_KEY_SERVED_BUILD_ID "served_bid"

bool ui_boot_needs_cal_hint(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no ui_boot NVS, Cal hint for build_id=0x%08lx",
                 (unsigned long)g_build_id);
        return true;
    }

    uint32_t served = 0;
    err = nvs_get_u32(h, NVS_KEY_SERVED_BUILD_ID, &served);
    nvs_close(h);

    bool need = (err != ESP_OK) || (served != g_build_id);
    if (need) {
        ESP_LOGI(TAG, "Cal hint: served=0x%08lx current=0x%08lx",
                 (unsigned long)served, (unsigned long)g_build_id);
    }
    return need;
}

void ui_boot_mark_hint_shown(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mark hint open failed: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_u32(h, NVS_KEY_SERVED_BUILD_ID, g_build_id);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mark hint save failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Cal hint recorded for build_id=0x%08lx", (unsigned long)g_build_id);
    }
}
