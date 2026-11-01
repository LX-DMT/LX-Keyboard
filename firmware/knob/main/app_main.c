/**
 * @file app_main.c
 * @brief ESP-IDF 固件入口
 *
 * 负责 NVS 初始化后调用 app_init / app_start 启动各子系统。
 * 不包含业务逻辑，具体编排见 components/app/app.c。
 */

#include "app.h"
#include "board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "nvs_flash.h"

static const char *TAG = "main";

/** @brief ESP-IDF 入口：NVS → 应用初始化 → 应用启动 */
void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    esp_rom_printf("LX_KNOB_BOOT\n");
    ESP_LOGI(TAG, "LX Knob IDF firmware starting");

    ESP_ERROR_CHECK(app_init());
    ESP_ERROR_CHECK(app_start());
    ESP_LOGI(TAG, "All subsystems started");
}
