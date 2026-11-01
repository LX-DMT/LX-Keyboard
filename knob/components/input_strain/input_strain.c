/**
 * @file input_strain.c
 * @brief 应变片（HX711）采样、按压检测与校准状态机
 *
 * 任务 input_strain（core 1，20ms）：未校准时仅轮询旋转启动；
 * 校准链：旋转≥20° → 按压 3s → 释放 3s → 电机 FOC → 完成。
 * 通过 event_bus 发布 KNOB_EVT_PRESS/RELEASE/LONG_PRESS。
 */

#include "input_strain.h"
#include "strain_cal_text.h"

#include "board_pins.h"
#include "build_id.h"
#include "esp_log.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "haptics.h"
#include "hx711.h"
#include "knob_events.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "input_strain";

#define NVS_NS "strain"
#define NVS_KEY_MAGIC "magic"
#define NVS_KEY_P_AVG "p_avg"
#define NVS_KEY_R_AVG "r_avg"
#define NVS_KEY_BUILD_ID "b_id"
#define STRAIN_NVS_MAGIC 0x53545241u

#define HX711_POLL_MS 20
/* Wall-clock timing kept equivalent to former 50 ms poll */
#define LONG_PRESS_TICKS_DEFAULT 70
#define PRESS_DEBOUNCE_MS 100
#define PRESS_STREAK_TICKS ((PRESS_DEBOUNCE_MS + HX711_POLL_MS - 1) / HX711_POLL_MS)
#define RELEASE_STREAK_TICKS 10
#define RELEASE_STREAK_LONG_TICKS 20
#define PRESS_COOLDOWN_MS 50
#define RELEASE_HAPTIC_MIN_HOLD_MS 150
#define RELEASE_HAPTIC_STREAK_TICKS 3
#define STRAIN_RELEASE_HAPTIC_RATIO 5
/* EMA: filtered += (raw - filtered) >> SHIFT */
#define STRAIN_READING_EWMA_SHIFT 2

#define CAL_SCREEN_COUNTDOWN_SEC 3
#define CAL_SCREEN_COUNTDOWN_MS (CAL_SCREEN_COUNTDOWN_SEC * 1000U)
#define CAL_START_DWELL_MS 1500
#define CAL_ROTATE_TRIGGER_RAD (20.0f * (float)M_PI / 180.0f)
#define CAL_ROTATE_SETTLE_MS 500U
#define CAL_ROTATE_STABLE_MS 400U
#define CAL_ROTATE_MIN_STEP_RAD (2.0f * (float)M_PI / 180.0f)
#define CAL_ROTATE_MAX_STEP_RAD (10.0f * (float)M_PI / 180.0f)
#define CAL_STEP_TIMEOUT_MS 30000
#define CAL_SAMPLE_MS HX711_POLL_MS
#define CAL_HX711_RETRY 40

/** 应变校准主状态机 */
typedef enum {
    CAL_ST_IDLE = 0,
    CAL_ST_PRESS_PROMPT,
    CAL_ST_PRESS_COUNTDOWN,
    CAL_ST_RELEASE_PROMPT,
    CAL_ST_RELEASE_COUNTDOWN,
    CAL_ST_MOTOR_PROMPT,
    CAL_ST_MOTOR_RUN,
} cal_state_t;

/** 按压触觉脉冲相位（边沿触发，避免持续按压重复脉冲） */
typedef enum {
    HAPTIC_PHASE_IDLE = 0,
    HAPTIC_PHASE_HELD,
    HAPTIC_PHASE_UNLOADED,
} haptic_phase_t;

/* --- 运行时按压检测状态 --- */
static int s_strain_press_flag;
static int s_strain_hold_count;
static int s_strain_press_streak;
static int s_strain_release_streak;
static int s_strain_haptic_press_streak;
static int s_strain_release_haptic_streak;
static uint32_t s_strain_press_cooldown_until_ms;
static uint32_t s_strain_press_edge_ms;
static haptic_phase_t s_strain_haptic_phase = HAPTIC_PHASE_IDLE;
static bool s_strain_long_press_sent;
static uint16_t s_long_press_ticks = LONG_PRESS_TICKS_DEFAULT;

/* --- NVS 阈值与滤波读数 --- */
static int32_t s_pressed_avg;
static int32_t s_released_avg;
static bool s_strain_thresholds_valid;
static int32_t s_reading_filtered;
static bool s_reading_filter_init;

/* --- 校准流程与旋转启动检测 --- */
static volatile bool s_cal_active = false;
static bool s_cal_await_rotate;
static bool s_cal_start_scheduled;
static uint32_t s_cal_start_at_ms;
static cal_state_t s_cal_state = CAL_ST_IDLE;
static uint32_t s_cal_step_start_ms = 0;
static uint8_t s_cal_last_display_sec = 0;

static int32_t s_cal_p1 = 0;
static int32_t s_cal_r1 = 0;

static char s_cal_ui_buf[32];

static void (*s_cal_on_prompt)(const char *text) = NULL;
static void (*s_cal_on_done)(bool ok) = NULL;

static bool s_cal_brake_override;
static bool s_cal_brake_was_enabled;
static bool s_cal_rotate_have_prev;
static float s_cal_rotate_prev_rad;
static float s_cal_rotate_signed_accum_rad;
static uint32_t s_cal_rotate_armed_at_ms;
static uint32_t s_cal_rotate_stable_since_ms;
static bool s_cal_rotate_tracking;

static bool strain_is_inverted(void)
{
    return s_pressed_avg < s_released_avg;
}

static int32_t strain_span_abs(void)
{
    int32_t d = s_pressed_avg - s_released_avg;
    return d < 0 ? -d : d;
}

static int32_t strain_press_threshold(void)
{
    return s_pressed_avg;
}

static int32_t strain_release_threshold(void)
{
    int32_t half = strain_span_abs() / 2;
    if (strain_is_inverted()) {
        return s_released_avg - half;
    }
    return s_released_avg + half;
}

static int32_t strain_release_haptic_threshold(void)
{
    int32_t delta = strain_span_abs() * STRAIN_RELEASE_HAPTIC_RATIO / 10;
    if (strain_is_inverted()) {
        return s_released_avg - delta;
    }
    return s_released_avg + delta;
}

static bool strain_reading_pressed(int32_t reading, int32_t press_th)
{
    return strain_is_inverted() ? (reading < press_th) : (reading > press_th);
}

static bool strain_reading_released(int32_t reading, int32_t release_th)
{
    return strain_is_inverted() ? (reading > release_th) : (reading < release_th);
}

static bool strain_reading_still_pressed(int32_t reading, int32_t haptic_th)
{
    return strain_is_inverted() ? (reading <= haptic_th) : (reading >= haptic_th);
}

static void strain_filter_reset(void)
{
    s_reading_filter_init = false;
}

static int32_t strain_filter_update(int32_t raw)
{
    if (!s_reading_filter_init) {
        s_reading_filtered = raw;
        s_reading_filter_init = true;
    } else {
        s_reading_filtered += (raw - s_reading_filtered) >> STRAIN_READING_EWMA_SHIFT;
    }
    return s_reading_filtered;
}

static void cal_prompt(const char *text)
{
    if (s_cal_on_prompt != NULL && text != NULL) {
        s_cal_on_prompt(text);
    }
}

static void cal_brake_restore_after_rotate(void)
{
    if (!s_cal_brake_override) {
        return;
    }
    haptics_set_brake_enabled(s_cal_brake_was_enabled);
    s_cal_brake_override = false;
}

static void cal_brake_disable_for_rotate(void)
{
    if (s_cal_brake_override) {
        return;
    }
    s_cal_brake_was_enabled = haptics_brake_enabled();
    haptics_set_brake_enabled(false);
    s_cal_brake_override = true;
}

static void cal_rotate_reset_accum(void)
{
    s_cal_rotate_have_prev = false;
    s_cal_rotate_signed_accum_rad = 0.0f;
    s_cal_rotate_stable_since_ms = 0;
    s_cal_rotate_tracking = false;
}

static uint32_t cal_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static float cal_angle_delta_wrapped(float prev, float cur)
{
    float delta = cur - prev;
    while (delta > (float)M_PI) {
        delta -= 2.0f * (float)M_PI;
    }
    while (delta < -(float)M_PI) {
        delta += 2.0f * (float)M_PI;
    }
    return delta;
}

static void cal_rotate_trigger_start(void)
{
    if (!s_cal_await_rotate) {
        return;
    }
    s_cal_await_rotate = false;
    cal_prompt(STRAIN_CAL_STARTING);
    s_cal_start_scheduled = true;
    s_cal_start_at_ms = cal_now_ms() + CAL_START_DWELL_MS;
    ESP_LOGI(TAG, "strain calibration rotate detected (signed=%.3f rad), start in %ums",
             (double)s_cal_rotate_signed_accum_rad, (unsigned)CAL_START_DWELL_MS);
}

static void cal_rotate_poll(void)
{
    float cur = haptics_get_encoder_angle_rad();
    if (!s_cal_rotate_have_prev) {
        s_cal_rotate_prev_rad = cur;
        s_cal_rotate_have_prev = true;
        return;
    }

    if ((cal_now_ms() - s_cal_rotate_armed_at_ms) < CAL_ROTATE_SETTLE_MS) {
        s_cal_rotate_prev_rad = cur;
        s_cal_rotate_signed_accum_rad = 0.0f;
        s_cal_rotate_stable_since_ms = 0;
        s_cal_rotate_tracking = false;
        return;
    }

    float delta = cal_angle_delta_wrapped(s_cal_rotate_prev_rad, cur);
    s_cal_rotate_prev_rad = cur;

    if (fabsf(delta) > CAL_ROTATE_MAX_STEP_RAD) {
        s_cal_rotate_stable_since_ms = 0;
        s_cal_rotate_tracking = false;
        s_cal_rotate_signed_accum_rad = 0.0f;
        return;
    }

    if (!s_cal_rotate_tracking) {
        if (fabsf(delta) <= CAL_ROTATE_MIN_STEP_RAD) {
            if (s_cal_rotate_stable_since_ms == 0) {
                s_cal_rotate_stable_since_ms = cal_now_ms();
            } else if ((cal_now_ms() - s_cal_rotate_stable_since_ms) >= CAL_ROTATE_STABLE_MS) {
                s_cal_rotate_tracking = true;
                s_cal_rotate_signed_accum_rad = 0.0f;
            }
        } else {
            s_cal_rotate_stable_since_ms = 0;
        }
        return;
    }

    if (fabsf(delta) < CAL_ROTATE_MIN_STEP_RAD) {
        return;
    }

    s_cal_rotate_signed_accum_rad += delta;

    if (fabsf(s_cal_rotate_signed_accum_rad) >= CAL_ROTATE_TRIGGER_RAD) {
        cal_rotate_trigger_start();
    }
}

static void cal_clear_arm_state(void)
{
    s_cal_await_rotate = false;
    s_cal_start_scheduled = false;
    s_cal_start_at_ms = 0;
}

static void cal_finish(bool ok)
{
    s_cal_active = false;
    s_cal_state = CAL_ST_IDLE;
    s_cal_last_display_sec = 0;
    cal_clear_arm_state();
    cal_brake_restore_after_rotate();
    if (s_cal_on_done != NULL) {
        s_cal_on_done(ok);
    }
}

static void cal_reset_step_timer(void)
{
    s_cal_step_start_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void cal_start_capture(void)
{
    cal_brake_restore_after_rotate();
    s_cal_active = true;
    s_cal_p1 = 0;
    s_cal_r1 = 0;
    s_cal_last_display_sec = 0;
    cal_reset_step_timer();
    s_cal_state = CAL_ST_PRESS_PROMPT;
    cal_prompt(STRAIN_CAL_PRESS_PROMPT);
    ESP_LOGI(TAG, "strain calibration started (countdown capture, %us)", (unsigned)CAL_SCREEN_COUNTDOWN_SEC);
}

static bool cal_step_timed_out(void)
{
    return (cal_now_ms() - s_cal_step_start_ms) >= CAL_STEP_TIMEOUT_MS;
}

static void cal_prompt_countdown(uint8_t secs)
{
    snprintf(s_cal_ui_buf, sizeof(s_cal_ui_buf), STRAIN_CAL_SCREEN_COUNTDOWN_FMT, (unsigned)secs);
    cal_prompt(s_cal_ui_buf);
}

static uint8_t cal_countdown_display_sec(uint32_t elapsed_ms)
{
    if (elapsed_ms >= CAL_SCREEN_COUNTDOWN_MS) {
        return 0;
    }
    uint32_t idx = elapsed_ms / 1000U;
    if (idx >= CAL_SCREEN_COUNTDOWN_SEC) {
        return 1;
    }
    return (uint8_t)(CAL_SCREEN_COUNTDOWN_SEC - idx);
}

static bool cal_capture_reading(int32_t *out)
{
    for (int i = 0; i < CAL_HX711_RETRY; i++) {
        if (hx711_is_ready()) {
            *out = hx711_read_raw();
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

static void cal_begin_countdown_phase(void)
{
    cal_reset_step_timer();
    s_cal_last_display_sec = 0;
    cal_prompt_countdown(CAL_SCREEN_COUNTDOWN_SEC);
    s_cal_last_display_sec = CAL_SCREEN_COUNTDOWN_SEC;
}

static bool cal_run_countdown_phase(void)
{
    uint32_t elapsed = cal_now_ms() - s_cal_step_start_ms;
    uint8_t display_sec = cal_countdown_display_sec(elapsed);

    if (display_sec != 0 && display_sec != s_cal_last_display_sec) {
        cal_prompt_countdown(display_sec);
        s_cal_last_display_sec = display_sec;
    }

    return elapsed >= CAL_SCREEN_COUNTDOWN_MS;
}

static esp_err_t strain_nvs_load(void)
{
    s_strain_thresholds_valid = false;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "strain NVS missing, press detection disabled");
        return err;
    }

    uint32_t magic = 0;
    int32_t p_avg = 0;
    int32_t r_avg = 0;
    uint32_t stored_build_id = 0;

    bool ok = nvs_get_u32(h, NVS_KEY_MAGIC, &magic) == ESP_OK && magic == STRAIN_NVS_MAGIC &&
              nvs_get_i32(h, NVS_KEY_P_AVG, &p_avg) == ESP_OK &&
              nvs_get_i32(h, NVS_KEY_R_AVG, &r_avg) == ESP_OK &&
              nvs_get_u32(h, NVS_KEY_BUILD_ID, &stored_build_id) == ESP_OK &&
              stored_build_id == g_build_id;

    nvs_close(h);

    if (!ok) {
        ESP_LOGI(TAG, "strain cal invalid for build_id=0x%08lx, press disabled",
                 (unsigned long)g_build_id);
        return ESP_ERR_NOT_FOUND;
    }

    s_pressed_avg = p_avg;
    s_released_avg = r_avg;
    s_strain_thresholds_valid = true;
    strain_filter_reset();
    ESP_LOGI(TAG, "strain thresholds press=%ld release=%ld inverted=%d build_id=0x%08lx",
             (long)p_avg, (long)r_avg, (int)strain_is_inverted(), (unsigned long)g_build_id);
    return ESP_OK;
}

static esp_err_t strain_nvs_save(int32_t p_avg, int32_t r_avg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u32(h, NVS_KEY_MAGIC, STRAIN_NVS_MAGIC);
    if (err == ESP_OK) {
        err = nvs_set_i32(h, NVS_KEY_P_AVG, p_avg);
    }
    if (err == ESP_OK) {
        err = nvs_set_i32(h, NVS_KEY_R_AVG, r_avg);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(h, NVS_KEY_BUILD_ID, g_build_id);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        s_strain_thresholds_valid = true;
    }
    return err;
}

static void cal_begin_motor_phase(void)
{
    s_cal_state = CAL_ST_MOTOR_PROMPT;
    cal_reset_step_timer();
    cal_prompt(STRAIN_CAL_MOTOR_PROMPT);
    ESP_LOGI(TAG, "strain cal -> motor FOC phase");
}

static void cal_try_save(void)
{
    if (strain_nvs_save(s_cal_p1, s_cal_r1) != ESP_OK) {
        ESP_LOGW(TAG, "cal NVS save failed");
        cal_finish(false);
        return;
    }

    s_pressed_avg = s_cal_p1;
    s_released_avg = s_cal_r1;
    ESP_LOGI(TAG, "cal saved press=%ld release=%ld inverted=%d",
             (long)s_cal_p1, (long)s_cal_r1, (int)strain_is_inverted());
    cal_begin_motor_phase();
}

static void input_strain_cal_step(void)
{
    if (cal_step_timed_out()) {
        ESP_LOGW(TAG, "cal timeout state=%d", (int)s_cal_state);
        cal_finish(false);
        return;
    }

    switch (s_cal_state) {
    case CAL_ST_PRESS_PROMPT:
        s_cal_state = CAL_ST_PRESS_COUNTDOWN;
        cal_begin_countdown_phase();
        break;

    case CAL_ST_PRESS_COUNTDOWN:
        if (!cal_run_countdown_phase()) {
            break;
        }
        if (!cal_capture_reading(&s_cal_p1)) {
            ESP_LOGW(TAG, "cal HX711 read failed (press)");
            cal_finish(false);
            break;
        }
        ESP_LOGI(TAG, "cal captured press=%ld", (long)s_cal_p1);
        s_cal_state = CAL_ST_RELEASE_PROMPT;
        cal_prompt(STRAIN_CAL_RELEASE_PROMPT);
        break;

    case CAL_ST_RELEASE_PROMPT:
        s_cal_state = CAL_ST_RELEASE_COUNTDOWN;
        cal_begin_countdown_phase();
        break;

    case CAL_ST_RELEASE_COUNTDOWN:
        if (!cal_run_countdown_phase()) {
            break;
        }
        if (!cal_capture_reading(&s_cal_r1)) {
            ESP_LOGW(TAG, "cal HX711 read failed (release)");
            cal_finish(false);
            break;
        }
        ESP_LOGI(TAG, "cal captured release=%ld", (long)s_cal_r1);
        cal_try_save();
        break;

    case CAL_ST_MOTOR_PROMPT:
        if (cal_now_ms() - s_cal_step_start_ms < CAL_START_DWELL_MS) {
            break;
        }
        s_cal_state = CAL_ST_MOTOR_RUN;
        cal_reset_step_timer();
        haptics_request_foc_calibration();
        break;

    case CAL_ST_MOTOR_RUN: {
        int foc_poll = haptics_foc_calibration_poll();
        if (foc_poll == 1) {
            break;
        }
        if (foc_poll == 2) {
            ESP_LOGI(TAG, "motor FOC cal ok");
            cal_finish(true);
            break;
        }
        if (foc_poll == 3 || cal_step_timed_out()) {
            ESP_LOGW(TAG, "motor FOC cal failed (poll=%d timeout=%d)",
                     foc_poll, (int)cal_step_timed_out());
            cal_finish(false);
        }
        break;
    }

    default:
        break;
    }
}

static void strain_publish(knob_event_type_t type)
{
    knob_event_t evt = { .type = type };
    TickType_t wait = (type == KNOB_EVT_LONG_PRESS) ? pdMS_TO_TICKS(200) : pdMS_TO_TICKS(50);
    if (event_bus_publish(&evt, wait) != ESP_OK) {
        ESP_LOGW(TAG, "event %d dropped (queue full)", (int)type);
    }
}

static void input_strain_task(void *arg)
{
    (void)arg;

    while (true) {
        if (s_cal_start_scheduled && cal_now_ms() >= s_cal_start_at_ms) {
            s_cal_start_scheduled = false;
            s_cal_start_at_ms = 0;
            cal_start_capture();
        }

        if (s_cal_await_rotate) {
            cal_rotate_poll();
            vTaskDelay(pdMS_TO_TICKS(HX711_POLL_MS));
            continue;
        }

        if (s_cal_active) {
            input_strain_cal_step();
            vTaskDelay(pdMS_TO_TICKS(CAL_SAMPLE_MS));
            continue;
        }

        if (!s_strain_thresholds_valid) {
            vTaskDelay(pdMS_TO_TICKS(HX711_POLL_MS));
            continue;
        }

        if (hx711_is_ready()) {
            int32_t reading = strain_filter_update(hx711_read_raw());
            int32_t press_th = strain_press_threshold();
            int32_t release_th = strain_release_threshold();
            int32_t release_haptic_th = strain_release_haptic_threshold();

            bool press_cooldown = cal_now_ms() < s_strain_press_cooldown_until_ms;
            uint32_t now_ms = cal_now_ms();

            /* Haptic phase machine: edge-triggered press/release pulses only */
            if (strain_reading_pressed(reading, press_th)) {
                bool allow_haptic_press = (s_strain_haptic_phase == HAPTIC_PHASE_UNLOADED) ||
                    (s_strain_haptic_phase == HAPTIC_PHASE_IDLE && !press_cooldown);
                if (allow_haptic_press) {
                    s_strain_haptic_press_streak++;
                    if (s_strain_haptic_press_streak >= PRESS_STREAK_TICKS) {
                        s_strain_haptic_press_streak = 0;
                        s_strain_haptic_phase = HAPTIC_PHASE_HELD;
                        s_strain_press_edge_ms = now_ms;
                        haptics_on_press_edge(true);
                    }
                }
            } else {
                s_strain_haptic_press_streak = 0;
            }

            if (s_strain_haptic_phase == HAPTIC_PHASE_HELD &&
                strain_reading_released(reading, release_haptic_th)) {
                if (now_ms - s_strain_press_edge_ms >= RELEASE_HAPTIC_MIN_HOLD_MS) {
                    s_strain_release_haptic_streak++;
                    if (s_strain_release_haptic_streak >= RELEASE_HAPTIC_STREAK_TICKS) {
                        s_strain_release_haptic_streak = 0;
                        /* Silent unload: re-arm for next press without release pulse
                         * (avoids rotate-induced release + press double haptic). */
                        s_strain_haptic_phase = HAPTIC_PHASE_UNLOADED;
                    }
                }
            } else if (strain_reading_still_pressed(reading, release_haptic_th)) {
                s_strain_release_haptic_streak = 0;
            }

            if (s_strain_press_flag == 1 && strain_reading_released(reading, release_haptic_th)) {
                s_strain_hold_count = 0;
            }

            if (strain_reading_pressed(reading, press_th)) {
                s_strain_release_streak = 0;
                if (s_strain_press_flag == 0) {
                    if (press_cooldown) {
                        s_strain_press_streak = 0;
                    } else {
                        s_strain_press_streak++;
                        if (s_strain_press_streak >= PRESS_STREAK_TICKS) {
                            s_strain_press_flag = 1;
                            s_strain_press_streak = 0;
                            s_strain_hold_count = 0;
                            s_strain_long_press_sent = false;
                            strain_publish(KNOB_EVT_PRESS);
                        }
                    }
                }
            } else if (strain_reading_released(reading, release_th)) {
                s_strain_press_streak = 0;
                int need_release = s_strain_long_press_sent ? RELEASE_STREAK_LONG_TICKS : RELEASE_STREAK_TICKS;
                s_strain_release_streak++;
                if (s_strain_press_flag == 1 && s_strain_release_streak >= need_release) {
                    if (s_strain_haptic_phase == HAPTIC_PHASE_HELD ||
                        s_strain_haptic_phase == HAPTIC_PHASE_UNLOADED) {
                        haptics_on_release_edge();
                    }
                    s_strain_press_flag = 0;
                    s_strain_hold_count = 0;
                    s_strain_release_streak = 0;
                    s_strain_long_press_sent = false;
                    s_strain_haptic_phase = HAPTIC_PHASE_IDLE;
                    s_strain_haptic_press_streak = 0;
                    s_strain_release_haptic_streak = 0;
                    s_strain_press_cooldown_until_ms = now_ms + PRESS_COOLDOWN_MS;
                    strain_publish(KNOB_EVT_RELEASE);
                }
            } else {
                s_strain_press_streak = 0;
                s_strain_release_streak = 0;
            }

            if (s_strain_press_flag == 1 && strain_reading_still_pressed(reading, release_haptic_th)) {
                if (s_strain_hold_count >= s_long_press_ticks && !s_strain_long_press_sent) {
                    s_strain_long_press_sent = true;
                    ESP_LOGI(TAG, "long press");
                    strain_publish(KNOB_EVT_LONG_PRESS);
                } else {
                    s_strain_hold_count++;
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(HX711_POLL_MS));
    }
}

bool input_strain_is_calibrated(void)
{
    return s_strain_thresholds_valid;
}

esp_err_t input_strain_init(void)
{
    ESP_ERROR_CHECK(hx711_init(BOARD_STRAIN_PIN_DOUT, BOARD_STRAIN_PIN_SCK));
    strain_nvs_load();
    if (s_strain_thresholds_valid) {
        ESP_LOGI(TAG, "HX711 ready, press enabled press=%ld release=%ld",
                 (long)s_pressed_avg, (long)s_released_avg);
    } else {
        ESP_LOGI(TAG, "HX711 ready, press disabled until calibration");
    }
    return ESP_OK;
}

esp_err_t input_strain_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(input_strain_task, "input_strain", 6144, NULL, 4, NULL, 1);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void input_strain_cal_set_ui_hooks(void (*on_prompt)(const char *text), void (*on_done)(bool ok))
{
    s_cal_on_prompt = on_prompt;
    s_cal_on_done = on_done;
}

void input_strain_cal_prepare(void)
{
    cal_brake_restore_after_rotate();
    s_cal_await_rotate = false;
    s_cal_start_scheduled = false;
    s_cal_start_at_ms = 0;
    s_cal_active = false;
    s_cal_state = CAL_ST_IDLE;
    s_cal_p1 = 0;
    s_cal_r1 = 0;
    s_cal_last_display_sec = 0;
    cal_rotate_reset_accum();
    s_cal_rotate_armed_at_ms = cal_now_ms();
    s_cal_rotate_prev_rad = haptics_get_encoder_angle_rad();
    s_cal_rotate_have_prev = true;
    cal_brake_disable_for_rotate();
    s_cal_await_rotate = true;
    cal_prompt(STRAIN_CAL_ROTATE_TO_START);
    ESP_LOGI(TAG, "strain calibration armed (rotate >=20 deg to start, brake off, settle %ums stable %ums)",
             (unsigned)CAL_ROTATE_SETTLE_MS, (unsigned)CAL_ROTATE_STABLE_MS);
}

void input_strain_cal_start(void)
{
    cal_clear_arm_state();
    cal_start_capture();
}

void input_strain_cal_cancel(void)
{
    if (!s_cal_active && !s_cal_await_rotate && !s_cal_start_scheduled) {
        return;
    }
    if (s_cal_active && s_cal_state == CAL_ST_MOTOR_RUN && haptics_foc_calibration_poll() == 1) {
        ESP_LOGI(TAG, "strain calibration cancel deferred (FOC running)");
        return;
    }
    ESP_LOGI(TAG, "strain calibration cancelled");
    if (s_cal_active) {
        cal_finish(false);
    } else {
        cal_clear_arm_state();
        cal_brake_restore_after_rotate();
        s_cal_state = CAL_ST_IDLE;
    }
}

bool input_strain_cal_active(void)
{
    return s_cal_active;
}

bool input_strain_cal_armed(void)
{
    return s_cal_await_rotate || s_cal_start_scheduled;
}

void input_strain_set_long_press_ms(uint32_t ms)
{
    uint32_t ticks = (ms + HX711_POLL_MS - 1U) / HX711_POLL_MS;
    if (ticks < 1U) {
        ticks = 1U;
    }
    if (ticks > UINT16_MAX) {
        ticks = UINT16_MAX;
    }
    s_long_press_ticks = (uint16_t)ticks;
}

void input_strain_reset_long_press_progress(void)
{
    s_strain_hold_count = 0;
    s_strain_long_press_sent = false;
}
