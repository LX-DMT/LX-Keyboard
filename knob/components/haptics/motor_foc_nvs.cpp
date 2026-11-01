/**
 * @file motor_foc_nvs.cpp
 * @brief FOC 对中参数 NVS 持久化
 *
 * 存储 zero_electric_angle、sensor_direction 与 build_id；
 * build_id 为固定的校准兼容版本；只有校准格式/硬件不兼容时才需要重新对中。
 */

#include "motor_foc_nvs.h"

#include "build_id.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <cmath>
#include <cstring>

static const char *TAG = "motor_foc_nvs";

#define NVS_NS "motor_foc"
#define NVS_KEY_MAGIC "magic"
#define NVS_KEY_ZERO "z_elec"
#define NVS_KEY_DIR "s_dir"
#define NVS_KEY_BUILD_ID "b_id"
#define MOTOR_FOC_NVS_MAGIC 0x4D4F4346u

static bool motor_foc_angle_valid(float angle)
{
    if (!std::isfinite(angle)) {
        return false;
    }
    return angle >= -2.0f * static_cast<float>(M_PI) && angle <= 4.0f * static_cast<float>(M_PI);
}

static bool motor_foc_dir_valid(int8_t dir)
{
    return dir == 1 || dir == -1;
}

bool motor_foc_nvs_load(float *zero_electric_angle, int8_t *sensor_direction)
{
    if (zero_electric_angle == NULL || sensor_direction == NULL) {
        return false;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return false;
    }

    uint32_t magic = 0;
    float zero = 0.0f;
    int8_t dir = 0;
    uint32_t stored_build_id = 0;
    size_t zero_size = sizeof(zero);

    bool ok = nvs_get_u32(h, NVS_KEY_MAGIC, &magic) == ESP_OK && magic == MOTOR_FOC_NVS_MAGIC &&
              nvs_get_blob(h, NVS_KEY_ZERO, &zero, &zero_size) == ESP_OK &&
              zero_size == sizeof(zero) &&
              nvs_get_i8(h, NVS_KEY_DIR, &dir) == ESP_OK &&
              nvs_get_u32(h, NVS_KEY_BUILD_ID, &stored_build_id) == ESP_OK &&
              stored_build_id == g_build_id &&
              motor_foc_angle_valid(zero) && motor_foc_dir_valid(dir);

    nvs_close(h);

    if (!ok) {
        ESP_LOGI(TAG, "FOC NVS miss or build_id mismatch (current=0x%08lx), need alignment",
                 (unsigned long)g_build_id);
        return false;
    }

    *zero_electric_angle = zero;
    *sensor_direction = dir;
    ESP_LOGI(TAG, "loaded zero=%.4f dir=%d build_id=0x%08lx",
             (double)zero, (int)dir, (unsigned long)g_build_id);
    return true;
}

esp_err_t motor_foc_nvs_save(float zero_electric_angle, int8_t sensor_direction)
{
    if (!motor_foc_angle_valid(zero_electric_angle) || !motor_foc_dir_valid(sensor_direction)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u32(h, NVS_KEY_MAGIC, MOTOR_FOC_NVS_MAGIC);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY_ZERO, &zero_electric_angle, sizeof(zero_electric_angle));
    }
    if (err == ESP_OK) {
        err = nvs_set_i8(h, NVS_KEY_DIR, sensor_direction);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(h, NVS_KEY_BUILD_ID, g_build_id);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }

    nvs_close(h);
    return err;
}
