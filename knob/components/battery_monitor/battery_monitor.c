/**
 * @file battery_monitor.c
 * @brief 电池电压 ADC 采样与低电量保护
 *
 * GPIO7 分压采样，EMA 滤波；低于 3550mV 发布 KNOB_EVT_BATTERY_LOW（关制动）。
 * 需连续有效读数后才 arm 保护，避免上电瞬态误触发。
 */

#include "battery_monitor.h"

#include "board_pins.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_check.h"
#include "esp_log.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "knob_events.h"

static const char *TAG = "battery";

#define BATTERY_POLL_MS 500
#define BATTERY_TRIP_MV 3550
#define BATTERY_RECOVER_MV 3600
#define BATTERY_DIVIDER_RATIO 2.0f
#define BATTERY_DEBOUNCE_SAMPLES 6
#define BATTERY_EMA_SHIFT 2

#define BATTERY_PIN_MV_MIN 0
#define BATTERY_PIN_MV_MAX 2400
#define BATTERY_BAT_MV_MIN 2500
#define BATTERY_BAT_MV_MAX 5000
#define BATTERY_ARM_MV_MIN 3200
#define BATTERY_ARM_MV_MAX 4500
#define BATTERY_ARM_STREAK 4

#define BATTERY_RAW_SAMPLES 8
#define BATTERY_WARMUP_DISCARD 8

static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t s_adc_cali;
static bool s_adc_cali_ok;
static adc_channel_t s_adc_channel = ADC_CHANNEL_6;
static TaskHandle_t s_task;

/** 低电量标志与滤波后电压（mV），供其他任务只读 */
static volatile bool s_battery_low;
static volatile uint32_t s_battery_mv;
static int32_t s_filtered_mv;
static bool s_filter_ready;
static bool s_protection_armed;
static uint8_t s_arm_streak;

static bool adc_read_pin_mv(int *pin_mv, int *raw_out)
{
    if (pin_mv == NULL) {
        return false;
    }

    int raw = 0;
    if (adc_oneshot_read(s_adc_handle, s_adc_channel, &raw) != ESP_OK) {
        return false;
    }
    if (raw_out != NULL) {
        *raw_out = raw;
    }

    int mv = 0;
    if (s_adc_cali_ok) {
        if (adc_cali_raw_to_voltage(s_adc_cali, raw, &mv) != ESP_OK) {
            return false;
        }
    } else {
        mv = (raw * 3300) / 4095;
    }

    if (mv < BATTERY_PIN_MV_MIN || mv > BATTERY_PIN_MV_MAX) {
        ESP_LOGD(TAG, "reject pin mv=%d raw=%d", mv, raw);
        return false;
    }

    *pin_mv = mv;
    return true;
}

static bool adc_read_battery_mv(int32_t *out_bat_mv, int *out_pin_mv, int *out_raw)
{
    if (out_bat_mv == NULL) {
        return false;
    }

    int samples[BATTERY_RAW_SAMPLES];
    int count = 0;

    for (int i = 0; i < BATTERY_RAW_SAMPLES; i++) {
        int pin_mv = 0;
        int raw = 0;
        if (adc_read_pin_mv(&pin_mv, &raw)) {
            samples[count++] = pin_mv;
            if (out_raw != NULL && i == BATTERY_RAW_SAMPLES - 1) {
                *out_raw = raw;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (count < 4) {
        return false;
    }

    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (samples[j] < samples[i]) {
                int tmp = samples[i];
                samples[i] = samples[j];
                samples[j] = tmp;
            }
        }
    }

    int32_t sum = 0;
    const int trim = 1;
    for (int i = trim; i < count - trim; i++) {
        sum += samples[i];
    }
    const int avg_count = count - 2 * trim;
    const int32_t pin_avg = sum / avg_count;
    const int32_t bat_mv = (int32_t)(pin_avg * BATTERY_DIVIDER_RATIO);

    if (bat_mv < BATTERY_BAT_MV_MIN || bat_mv > BATTERY_BAT_MV_MAX) {
        ESP_LOGD(TAG, "reject bat mv=%ld pin=%ld", (long)bat_mv, (long)pin_avg);
        return false;
    }

    *out_bat_mv = bat_mv;
    if (out_pin_mv != NULL) {
        *out_pin_mv = (int)pin_avg;
    }
    return true;
}

static void battery_publish(knob_event_type_t type)
{
    knob_event_t evt = { .type = type };
    event_bus_publish(&evt, 0);
}

static void battery_monitor_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(10));
    for (int i = 0; i < BATTERY_WARMUP_DISCARD; i++) {
        int pin_mv = 0;
        adc_read_pin_mv(&pin_mv, NULL);
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    bool low = false;
    uint8_t low_streak = 0;
    uint8_t ok_streak = 0;

    while (true) {
        int32_t sample_mv = 0;
        int pin_mv = 0;
        int raw = 0;
        if (adc_read_battery_mv(&sample_mv, &pin_mv, &raw)) {
            if (!s_filter_ready) {
                s_filtered_mv = sample_mv;
                s_filter_ready = true;
            } else {
                s_filtered_mv += (sample_mv - s_filtered_mv) >> BATTERY_EMA_SHIFT;
            }
            if (s_filtered_mv < 0) {
                s_filtered_mv = sample_mv;
            }
            s_battery_mv = (uint32_t)s_filtered_mv;

            ESP_LOGI(TAG, "battery %ld mV (pin %d, raw %d)", (long)s_filtered_mv, pin_mv, raw);

            if (!s_protection_armed) {
                if (s_filtered_mv >= BATTERY_ARM_MV_MIN && s_filtered_mv <= BATTERY_ARM_MV_MAX) {
                    s_arm_streak++;
                    if (s_arm_streak >= BATTERY_ARM_STREAK) {
                        s_protection_armed = true;
                        ESP_LOGI(TAG, "protection armed");
                    }
                } else {
                    s_arm_streak = 0;
                }
            }

            if (s_protection_armed) {
                if (!low) {
                    if (s_filtered_mv < BATTERY_TRIP_MV) {
                        low_streak++;
                        ok_streak = 0;
                        if (low_streak >= BATTERY_DEBOUNCE_SAMPLES) {
                            low = true;
                            s_battery_low = true;
                            low_streak = 0;
                            ESP_LOGW(TAG, "low power enter (%ld mV)", (long)s_filtered_mv);
                            battery_publish(KNOB_EVT_BATTERY_LOW);
                        }
                    } else {
                        low_streak = 0;
                    }
                } else {
                    if (s_filtered_mv >= BATTERY_RECOVER_MV) {
                        ok_streak++;
                        low_streak = 0;
                        if (ok_streak >= BATTERY_DEBOUNCE_SAMPLES) {
                            low = false;
                            s_battery_low = false;
                            ok_streak = 0;
                            ESP_LOGI(TAG, "low power exit (%ld mV)", (long)s_filtered_mv);
                            battery_publish(KNOB_EVT_BATTERY_OK);
                        }
                    } else {
                        ok_streak = 0;
                    }
                }
            }
        } else {
            ESP_LOGW(TAG, "ADC sample rejected");
        }

        vTaskDelay(pdMS_TO_TICKS(BATTERY_POLL_MS));
    }
}

esp_err_t battery_monitor_init(void)
{
    adc_unit_t unit = ADC_UNIT_1;
    adc_channel_t channel = ADC_CHANNEL_6;
    ESP_RETURN_ON_ERROR(adc_oneshot_io_to_channel(BOARD_BAT_ADC_GPIO, &unit, &channel), TAG, "gpio to adc");

    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = unit,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&init_cfg, &s_adc_handle), TAG, "adc unit");

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc_handle, channel, &chan_cfg), TAG, "adc chan");
    s_adc_channel = channel;

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = unit,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    s_adc_cali_ok = (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali) == ESP_OK);

    s_battery_low = false;
    s_battery_mv = 0;
    s_filtered_mv = 0;
    s_filter_ready = false;
    s_protection_armed = false;
    s_arm_streak = 0;
    ESP_LOGI(TAG, "ADC GPIO%d, trip=%dmV recover=%dmV", BOARD_BAT_ADC_GPIO, BATTERY_TRIP_MV, BATTERY_RECOVER_MV);
    return ESP_OK;
}

esp_err_t battery_monitor_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(battery_monitor_task, "battery_mon", 3072, NULL, 3, &s_task, 0);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool battery_monitor_is_low(void)
{
    return s_battery_low;
}

uint32_t battery_monitor_voltage_mv(void)
{
    return s_battery_mv;
}
