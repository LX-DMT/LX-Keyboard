/**
 * @file haptics_engine.cpp
 * @brief SimpleFOC 电机任务：FOC 对中、SmartKnob 棘轮、仪表跟踪、编码器缓存
 *
 * core 0 主循环 ~1ms：未对中或制动关闭时零力矩并刷新编码器角；
 * 已对中后执行 smart_knob_step 或 instrument_step。
 * 首次开机可 defer FOC 对中至应变校准末段。
 */

#include "haptics.h"

#include "as5048a.h"
#include "motor_foc_nvs.h"

#include "board_pins.h"

#include "esp_log.h"

#include "esp_simplefoc.h"

#include "esp_timer.h"

#include "event_bus.h"

#include "freertos/FreeRTOS.h"

#include "freertos/task.h"

#include "haptic_profiles.h"

#include "knob_events.h"



#include <cmath>



static const char *TAG = "haptics";



static AS5048a s_sensor(BOARD_ENCODER_SPI_HOST,

                        (gpio_num_t)BOARD_ENCODER_PIN_SCLK,

                        (gpio_num_t)BOARD_ENCODER_PIN_MISO,

                        (gpio_num_t)BOARD_ENCODER_PIN_MOSI,

                        (gpio_num_t)BOARD_ENCODER_PIN_CS);

static BLDCDriver6PWM s_driver(BOARD_MOTOR_PIN_UH, BOARD_MOTOR_PIN_UL,

                               BOARD_MOTOR_PIN_VH, BOARD_MOTOR_PIN_VL,

                               BOARD_MOTOR_PIN_WH, BOARD_MOTOR_PIN_WL);

static BLDCMotor s_motor(7);



static haptic_profile_t s_config;

static float s_current_detent_center = 0;

static bool s_endstop_hold = false;

static int8_t s_endstop_hold_side = 0;

static float s_endstop_limit_center_rad = 0;

static float s_idle_check_velocity_ewma = 0;

static float s_spin_velocity_ewma = 0;

static uint32_t s_last_idle_start_ms = 0;

static int32_t s_position = 0;

static volatile bool s_instrument_active = false;

static volatile bool s_instrument_hold = false;

static volatile bool s_instrument_reinit = false;

static volatile float s_cpu_load_target = 0;

static volatile float s_instrument_cpu_arc = 20.0f;

static float s_inst_baseline_angle = 0.0f;
static float s_inst_baseline_arc = 20.0f;
static volatile bool s_inst_baseline_valid = false;
static volatile bool s_inst_latch_pending = false;

static constexpr float kInstArcMin = 20.0f;
static constexpr float kInstArcSpan = 60.0f;
static constexpr float kInstrumentPidP = 0.5f;
static constexpr float kInstrumentVoltageLimit = 3.8f;
static constexpr float kInstTargetMaxStepRad = 0.01f;
static constexpr float kDefaultVoltageLimit = 3.0f;

static volatile uint8_t s_haptic_pulse = 0;

static uint32_t s_last_haptic_exec_ms = 0;

static constexpr uint32_t kMinHapticPulseGapMs = 100;

static uint32_t s_detent_quiet_until_ms = 0;

static constexpr uint32_t kDetentQuietAfterPulseMs = 100;

static volatile bool s_motor_ready = false;

static volatile bool s_brake_enabled = true;

static volatile bool s_foc_cal_requested = false;
/** FOC 校准轮询状态：0 空闲 1 运行 2 成功 3 失败 */
static volatile int s_foc_cal_poll = 0;

/** 供 input_strain 读取的编码器角缓存（rad） */
static volatile float s_encoder_angle_rad = 0.0f;

/** 是否已完成物理 FOC 对中（未完成前主循环零力矩） */
static volatile bool s_foc_physically_calibrated = false;

static volatile int s_pending_profile = -1;
static volatile int32_t s_pending_position = INT32_MIN;
static volatile bool s_pending_menu_detents = false;
static volatile int32_t s_pending_menu_position = 0;
static volatile int32_t s_pending_menu_item_count = 0;

static constexpr float kMenuPositionWidthDeg = 15.0f;
static constexpr float kMenuDetentStrength = 1.5f;
static constexpr float kMenuEndstopStrength = 1.0f;
static constexpr float kMenuSnapPoint = 0.8f;



static constexpr float kDeadZoneDetentPercent = 0.2f;

static constexpr float kDeadZoneRad = 1.0f * PI / 180.0f;

static constexpr float kIdleVelocityEwmaAlpha = 0.001f;

static constexpr float kIdleVelocityRadPerSec = 0.05f;

static constexpr uint32_t kIdleCorrectionDelayMs = 500;

static constexpr float kIdleCorrectionMaxAngleRad = 5.0f * PI / 180.0f;

static constexpr float kIdleCorrectionRateAlpha = 0.0005f;

static constexpr float kSpinVelocityEwmaAlpha = 0.12f;

static constexpr float kSpinHardCutoffRadPerSec = 60.0f;

static constexpr float kSpinTorqueScaleRadPerSec = 12.0f;

static constexpr float kEndstopVelDamping = 0.3f;

static constexpr float kEndstopTorqueMax = 5.5f;

static constexpr float kEndstopSpringGainScale = 4.5f;

static constexpr float kEndstopExitWidthFrac = 0.3f;

static constexpr float kEndstopCoggingAmp = 0.4f;

static constexpr float kEndstopCoggingPhase = 0.0f;

static constexpr float kEndstopVoltageLimit = 2.5f;

static constexpr int kMotorPolePairs = 7;

static bool haptics_foc_resume_from_nvs(float zero, int8_t dir)
{
    s_motor.zero_electric_angle = zero;
    s_motor.sensor_direction = (Direction)dir;
    s_sensor.update();
    s_motor.shaft_angle = s_motor.shaftAngle();
    s_motor.shaft_velocity = s_motor.shaftVelocity();
    s_motor.motor_status = FOCMotorStatus::motor_ready;
    s_motor.enabled = 1;
    for (int i = 0; i < 2; i++) {
        s_motor.loopFOC();
    }
    return true;
}

static int haptics_run_full_foc_align(void)
{
    /* Boot may have loaded NVS into s_motor; clear so alignSensor() runs (shaft wiggle). */
    s_motor.zero_electric_angle = NOT_SET;
    s_motor.sensor_direction = NOT_SET;
    ESP_LOGI(TAG, "FOC: forcing full alignSensor (clear cached offsets)");
    if (s_motor.initFOC() == 0) {
        return 0;
    }

    esp_err_t err = motor_foc_nvs_save(s_motor.zero_electric_angle, (int8_t)s_motor.sensor_direction);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "FOC cal NVS save failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "FOC cal saved zero=%.4f dir=%d",
                 (double)s_motor.zero_electric_angle, (int)s_motor.sensor_direction);
    }
    s_foc_physically_calibrated = true;
    return 1;
}

static int haptics_init_foc(void)
{
    float zero = NOT_SET;
    int8_t dir = Direction::UNKNOWN;

    if (motor_foc_nvs_load(&zero, &dir)) {
        ESP_LOGI(TAG, "FOC cal loaded from NVS (zero=%.4f dir=%d)", (double)zero, (int)dir);
        if (haptics_foc_resume_from_nvs(zero, dir)) {
            s_foc_physically_calibrated = true;
            return 1;
        }
        ESP_LOGW(TAG, "FOC NVS resume failed, defer align until strain cal motor phase");
    } else {
        ESP_LOGI(TAG, "FOC: no stored cal, defer align until strain cal motor phase");
    }

    s_motor.zero_electric_angle = NOT_SET;
    s_motor.sensor_direction = NOT_SET;
    s_sensor.update();
    s_encoder_angle_rad = s_sensor.getSensorAngle();
    s_foc_physically_calibrated = false;
    return 1;
}

static uint32_t millis_now(void)

{

    return (uint32_t)(esp_timer_get_time() / 1000ULL);

}

static void haptics_cache_encoder_angle(void)
{
    s_encoder_angle_rad = s_sensor.getSensorAngle();
}



static float clampf(float value, float low, float high)

{

    return value < low ? low : (value > high ? high : value);

}



static void haptics_apply_profile(uint8_t profile_id)

{

    const haptic_profile_t *src = haptic_profiles_get(profile_id);

    s_config = *src;

    s_current_detent_center = s_motor.shaftAngle();

    s_spin_velocity_ewma = 0;

    s_endstop_hold = false;

    s_endstop_hold_side = 0;

    ESP_LOGI(TAG, "Profile %u: %s", profile_id, s_config.label);

}



static void haptics_apply_position_locked(int32_t position)

{

    if (s_config.max_position >= s_config.min_position) {

        if (position < s_config.min_position) {

            position = s_config.min_position;

        } else if (position > s_config.max_position) {

            position = s_config.max_position;

        }

    }

    s_config.position = position;

    s_position = position;

}



static void haptics_apply_menu_detents_locked(int32_t position, int32_t item_count)
{
    if (item_count < 1) {
        haptics_apply_profile(0);
        haptics_apply_position_locked(position);
        return;
    }

    s_config.min_position = 0;
    s_config.max_position = -1; /* unbounded: wrap at UI, no endstop at first/last */
    s_config.position_width_radians = kMenuPositionWidthDeg * PI / 180.0f;
    s_config.detent_strength_unit = kMenuDetentStrength;
    s_config.endstop_strength_unit = kMenuEndstopStrength;
    s_config.snap_point = kMenuSnapPoint;
    s_config.snap_point_bias = 0;
    s_config.detent_positions_count = 0;
    s_config.label = "Menu multi-detent";

    s_current_detent_center = s_motor.shaftAngle();
    s_spin_velocity_ewma = 0;
    s_endstop_hold = false;
    s_endstop_hold_side = 0;

    haptics_apply_position_locked(position);

    ESP_LOGI(TAG, "Menu detents items=%ld pos=%ld width=%.1fdeg",
             (long)item_count, (long)s_config.position,
             (double)(s_config.position_width_radians * 180.0f / PI));
}

static void haptics_apply_pending(void)

{

    if (s_pending_menu_detents) {
        s_pending_menu_detents = false;
        haptics_apply_menu_detents_locked(s_pending_menu_position, s_pending_menu_item_count);
        return;
    }

    int profile = s_pending_profile;

    if (profile >= 0) {

        s_pending_profile = -1;

        haptics_apply_profile((uint8_t)profile);

    }

    int32_t position = s_pending_position;

    if (position != INT32_MIN) {

        s_pending_position = INT32_MIN;

        haptics_apply_position_locked(position);

    }

}



static void haptics_begin_detent_quiet(void)
{
    s_sensor.update();
    s_motor.shaft_angle = s_motor.shaftAngle();
    s_current_detent_center = s_motor.shaft_angle;

    uint32_t until_ms = millis_now() + kDetentQuietAfterPulseMs;
    if (until_ms > s_detent_quiet_until_ms) {
        s_detent_quiet_until_ms = until_ms;
    }
}

static void haptic_pulse(float strength)

{
    haptics_begin_detent_quiet();

    s_motor.move(strength);

    for (uint8_t i = 0; i < 3; i++) {

        s_motor.loopFOC();

        vTaskDelay(1);

    }

    s_motor.move(-strength);

    for (uint8_t i = 0; i < 3; i++) {

        s_motor.loopFOC();

        vTaskDelay(1);

    }

    s_motor.move(0);

    s_motor.loopFOC();

    haptics_begin_detent_quiet();

}



static void smart_knob_step(void)

{

    s_motor.shaft_angle = s_motor.shaftAngle();

    s_motor.shaft_velocity = s_motor.shaftVelocity();

    if (millis_now() < s_detent_quiet_until_ms) {
        s_motor.move(0);
        return;
    }



    const float abs_velocity = std::fabs(s_motor.shaft_velocity);

    s_spin_velocity_ewma = abs_velocity * kSpinVelocityEwmaAlpha +

                           s_spin_velocity_ewma * (1.0f - kSpinVelocityEwmaAlpha);



    s_idle_check_velocity_ewma = s_motor.shaft_velocity * kIdleVelocityEwmaAlpha +

                                 s_idle_check_velocity_ewma * (1.0f - kIdleVelocityEwmaAlpha);



    if (std::fabs(s_idle_check_velocity_ewma) > kIdleVelocityRadPerSec) {

        s_last_idle_start_ms = 0;

    } else if (s_last_idle_start_ms == 0) {

        s_last_idle_start_ms = millis_now();

    }



    if (!s_endstop_hold &&

        s_last_idle_start_ms > 0 &&

        millis_now() - s_last_idle_start_ms > kIdleCorrectionDelayMs &&

        std::fabs(s_motor.shaft_angle - s_current_detent_center) < kIdleCorrectionMaxAngleRad) {

        s_current_detent_center = s_motor.shaft_angle * kIdleCorrectionRateAlpha +

                                  s_current_detent_center * (1.0f - kIdleCorrectionRateAlpha);

    }



    float angle_to_detent_center = s_motor.shaft_angle - s_current_detent_center;

    const float snap_point_radians = s_config.position_width_radians * s_config.snap_point;

    const float bias_radians = s_config.position_width_radians * s_config.snap_point_bias;

    const float snap_point_radians_decrease =

        snap_point_radians + (s_config.position <= 0 ? bias_radians : -bias_radians);

    const float snap_point_radians_increase =

        -snap_point_radians + (s_config.position >= 0 ? -bias_radians : bias_radians);



    const int32_t prev_position = s_config.position;

    const int32_t num_positions = s_config.max_position - s_config.min_position + 1;

    const bool bounded = num_positions > 0;

    const bool oob_min = bounded && s_config.position == s_config.min_position && angle_to_detent_center > 0;

    const bool oob_max = bounded && s_config.position == s_config.max_position && angle_to_detent_center < 0;

    const bool out_of_bounds = oob_min || oob_max;

    const float exit_rad = kEndstopExitWidthFrac * s_config.position_width_radians;



    if (out_of_bounds) {

        if (!s_endstop_hold) {

            s_endstop_hold = true;

            s_endstop_hold_side = oob_min ? (int8_t)-1 : (int8_t)1;

            s_endstop_limit_center_rad = s_current_detent_center;

        }

    } else if (s_endstop_hold) {

        if (s_endstop_hold_side < 0 && s_config.position == s_config.min_position &&

            angle_to_detent_center < exit_rad) {

            s_endstop_hold = false;

            s_endstop_hold_side = 0;

        } else if (s_endstop_hold_side > 0 && s_config.position == s_config.max_position &&

                   angle_to_detent_center > -exit_rad) {

            s_endstop_hold = false;

            s_endstop_hold_side = 0;

        }

    }



    if (s_endstop_hold) {

        if (s_endstop_hold_side < 0) {

            s_config.position = s_config.min_position;

        } else {

            s_config.position = s_config.max_position;

        }

    } else {

        while (angle_to_detent_center > snap_point_radians_decrease &&

               (num_positions <= 0 || s_config.position > s_config.min_position)) {

            s_current_detent_center += s_config.position_width_radians;

            angle_to_detent_center -= s_config.position_width_radians;

            s_config.position--;

        }

        while (angle_to_detent_center < snap_point_radians_increase &&

               (num_positions <= 0 || s_config.position < s_config.max_position)) {

            s_current_detent_center -= s_config.position_width_radians;

            angle_to_detent_center += s_config.position_width_radians;

            s_config.position++;

        }

    }



    s_position = s_config.position;



    if (s_config.position != prev_position) {

        knob_event_t evt = {};

        evt.type = KNOB_EVT_ROTATION;

        evt.data.rotation.delta_steps = s_config.position - prev_position;

        event_bus_publish(&evt, 0);

    }



    const bool endstop_active = out_of_bounds || s_endstop_hold;



    if (!endstop_active && s_spin_velocity_ewma > kSpinHardCutoffRadPerSec) {

        s_motor.PID_velocity.reset();

        s_motor.move(0);

        return;

    }



    if (endstop_active) {

        s_motor.voltage_limit = kEndstopVoltageLimit;

        const float deflection = s_motor.shaft_angle - s_endstop_limit_center_rad;

        const float spring_gain = s_config.endstop_strength_unit * kEndstopSpringGainScale;

        float torque = -spring_gain * deflection;

        if (deflection * s_motor.shaft_velocity > 0.0f) {

            torque -= kEndstopVelDamping * s_motor.shaft_velocity;

        }

        torque += kEndstopCoggingAmp *

                  sinf((float)kMotorPolePairs * s_motor.shaft_angle + kEndstopCoggingPhase);

        torque = clampf(torque, -kEndstopTorqueMax, kEndstopTorqueMax);

        s_motor.move(torque);

        return;

    }



    s_motor.PID_velocity.limit = 10;

    s_motor.PID_velocity.P = s_config.detent_strength_unit * 4;



    const float dead_zone_adjustment = clampf(

        angle_to_detent_center,

        std::fmax(-s_config.position_width_radians * kDeadZoneDetentPercent, -kDeadZoneRad),

        std::fmin(s_config.position_width_radians * kDeadZoneDetentPercent, kDeadZoneRad));

    float input = -angle_to_detent_center + dead_zone_adjustment;

    if (s_config.detent_positions_count > 0) {

        bool in_detent = false;

        for (uint32_t i = 0; i < s_config.detent_positions_count; i++) {

            if (s_config.detent_positions[i] == s_config.position) {

                in_detent = true;

                break;

            }

        }

        if (!in_detent) {

            input = 0;

        }

    }



    const float torque = s_motor.PID_velocity(input);

    const float vel_scale = 1.0f - clampf(s_spin_velocity_ewma / kSpinTorqueScaleRadPerSec, 0.0f, 0.55f);

    s_motor.move(torque * vel_scale);

}



static float instrument_arc_to_target_angle(float arc_pos, float angle_init)
{
    if (s_inst_baseline_valid) {
        float delta_arc = arc_pos - s_inst_baseline_arc;
        float pct_delta = delta_arc * (100.0f / kInstArcSpan);
        return s_inst_baseline_angle - pct_delta * 0.02f;
    }
    float pct = (arc_pos - kInstArcMin) * (100.0f / kInstArcSpan);
    if (pct < 0.0f) {
        pct = 0.0f;
    } else if (pct > 100.0f) {
        pct = 100.0f;
    }
    return angle_init - pct * 0.02f;
}

static void instrument_step(float *last_angle, float *current_angle_init, bool *initialized)

{

    if (!s_instrument_hold) {

        s_motor.loopFOC();

        s_motor.controller = MotionControlType::torque;

        s_motor.PID_velocity.P = 1;

        s_motor.PID_velocity.I = 0;

        s_motor.PID_velocity.D = 0;

        s_motor.move(0);

        return;

    }

    if (s_inst_latch_pending) {
        s_inst_latch_pending = false;
        s_inst_baseline_angle = s_sensor.getSensorAngle();
        s_inst_baseline_arc = s_instrument_cpu_arc;
        s_inst_baseline_valid = true;
        *initialized = true;
        *current_angle_init = s_inst_baseline_angle;
        *last_angle = s_inst_baseline_angle;

        s_motor.loopFOC();
        s_motor.controller = MotionControlType::angle;
        s_motor.PID_velocity.P = kInstrumentPidP;
        s_motor.PID_velocity.I = 0;
        s_motor.PID_velocity.D = 0;
        s_motor.voltage_limit = kInstrumentVoltageLimit;
        s_motor.move(s_inst_baseline_angle);
        return;
    }

    if (!*initialized) {

        s_motor.loopFOC();

        s_motor.controller = MotionControlType::angle;

        s_motor.PID_velocity.P = 1;

        s_motor.PID_velocity.I = 1;

        s_motor.PID_velocity.D = 0;

        *current_angle_init = s_sensor.getSensorAngle();

        *initialized = true;

        *last_angle = *current_angle_init;

        s_motor.move(*current_angle_init);

        return;

    }



    s_motor.loopFOC();

    s_motor.controller = MotionControlType::angle;

    s_motor.PID_velocity.P = kInstrumentPidP;

    s_motor.PID_velocity.I = 0;

    s_motor.PID_velocity.D = 0;

    s_motor.voltage_limit = kInstrumentVoltageLimit;

    float target = instrument_arc_to_target_angle(s_instrument_cpu_arc, *current_angle_init);
    if (*last_angle < target) {
        *last_angle = fminf(*last_angle + kInstTargetMaxStepRad, target);
    } else if (*last_angle > target) {
        *last_angle = fmaxf(*last_angle - kInstTargetMaxStepRad, target);
    }
    s_motor.move(*last_angle);

}



static void haptics_task(void *arg)

{

    (void)arg;



    s_sensor.init();

    s_motor.linkSensor(&s_sensor);

    s_driver.voltage_power_supply = 4;

    if (s_driver.init() == 0) {

        ESP_LOGE(TAG, "BLDC 6PWM driver init failed");

        vTaskDelete(NULL);

        return;

    }

    s_motor.linkDriver(&s_driver);

    s_motor.controller = MotionControlType::torque;

    s_motor.PID_velocity.P = 1;

    s_motor.PID_velocity.I = 0;

    s_motor.PID_velocity.D = 0.148f;

    s_motor.PID_velocity.output_ramp = 5000;

    s_motor.PID_velocity.limit = 3;

    s_motor.voltage_limit = 3;

    s_motor.voltage_sensor_align = 2.5f;

    s_motor.LPF_velocity.Tf = 0.025f;

    s_motor.LPF_angle.Tf = 0.01f;

    s_motor.init();

    if (haptics_init_foc() == 0) {

        ESP_LOGE(TAG, "FOC init failed — check AS5047 SPI wiring (MOSI=11 MISO=13 SCLK=12 CS=10)");

        s_driver.disable();

        while (true) {

            vTaskDelay(pdMS_TO_TICKS(1000));

        }

    }

    s_motor_ready = true;

    if (!s_pending_menu_detents && s_pending_profile < 0) {

        s_pending_profile = 0;

    }

    haptics_apply_pending();

    ESP_LOGI(TAG, "Motor FOC ready");



    float last_angle = 0;

    float current_angle_init = 0;

    bool instrument_initialized = false;



    while (true) {

        haptics_apply_pending();

        if (s_foc_cal_requested) {
            s_foc_cal_requested = false;
            s_foc_cal_poll = 1;
            ESP_LOGI(TAG, "FOC calibration requested (strain cal)");
            int foc_ok = haptics_run_full_foc_align();
            s_foc_cal_poll = foc_ok ? 2 : 3;
            vTaskDelay(1);
            continue;
        }

        if (!s_brake_enabled || !s_foc_physically_calibrated) {
            s_sensor.update();
            haptics_cache_encoder_angle();
            s_motor.loopFOC();
            s_motor.controller = MotionControlType::torque;
            s_motor.PID_velocity.P = 1;
            s_motor.PID_velocity.I = 0;
            s_motor.PID_velocity.D = 0;
            s_motor.move(0);
            vTaskDelay(1);
            continue;
        }

        if (s_haptic_pulse != 0) {
            uint32_t now_ms = millis_now();
            if (now_ms - s_last_haptic_exec_ms >= kMinHapticPulseGapMs) {
                if (s_haptic_pulse == 1) {
                    haptic_pulse(s_config.max_position < 0 ? 8.0f : 5.0f);
                } else if (s_haptic_pulse == 2) {
                    haptic_pulse(s_config.max_position < 0 ? 3.0f : 1.5f);
                }
                s_last_haptic_exec_ms = now_ms;
                s_haptic_pulse = 0;
            }
        }

        if (s_instrument_reinit) {

            s_instrument_reinit = false;

            instrument_initialized = false;

        }



        if (s_instrument_active) {

            s_sensor.update();
            haptics_cache_encoder_angle();

            instrument_step(&last_angle, &current_angle_init, &instrument_initialized);

        } else {

            instrument_initialized = false;

            s_motor.voltage_limit = kDefaultVoltageLimit;

            s_sensor.update();
            haptics_cache_encoder_angle();

            s_motor.loopFOC();

            s_motor.controller = MotionControlType::torque;

            s_motor.PID_velocity.P = 1;

            s_motor.PID_velocity.I = 0;

            s_motor.PID_velocity.D = 0.148f;

            smart_knob_step();

        }



        vTaskDelay(1);

    }

}



extern "C" {



esp_err_t haptics_init(void)

{

    return ESP_OK;

}



esp_err_t haptics_start(void)

{

    BaseType_t ok = xTaskCreatePinnedToCore(haptics_task, "haptics", 10240, NULL, 1, NULL, 0);

    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;

}



void haptics_set_profile(uint8_t profile_id)

{

    s_pending_menu_detents = false;
    s_pending_profile = (int)profile_id;

}



void haptics_apply_profile_and_position(uint8_t profile_id, int32_t position)

{

    if (!s_motor_ready) {

        s_pending_profile = (int)profile_id;

        s_pending_position = position;

        return;

    }

    s_pending_profile = -1;

    s_pending_position = INT32_MIN;
    s_pending_menu_detents = false;

    haptics_apply_profile(profile_id);

    haptics_apply_position_locked(position);

}

void haptics_apply_menu_detents(int32_t position, int32_t item_count)
{
    s_pending_profile = -1;
    s_pending_position = INT32_MIN;

    if (!s_motor_ready) {
        s_pending_menu_detents = true;
        s_pending_menu_position = position;
        s_pending_menu_item_count = item_count;
        return;
    }

    s_pending_menu_detents = false;
    haptics_apply_menu_detents_locked(position, item_count);
}

void haptics_set_instrument_active(bool active)

{

    s_instrument_active = active;

    if (active) {
        s_instrument_cpu_arc = kInstArcMin;
        s_instrument_hold = false;
        s_inst_baseline_valid = false;
        s_inst_latch_pending = false;
    } else {
        s_instrument_hold = false;
        s_inst_baseline_valid = false;
        s_inst_latch_pending = false;
        if (s_motor_ready) {
            s_motor.voltage_limit = kDefaultVoltageLimit;
        }
    }

}

void haptics_instrument_latch_live_origin(void)
{
    s_inst_latch_pending = true;
    s_instrument_hold = true;
}



void haptics_set_instrument_hold(bool hold)

{

    if (hold && !s_instrument_hold) {
        s_instrument_reinit = true;
    }

    s_instrument_hold = hold;

}

void haptics_set_cpu_load(float cpu_pct)

{

    if (cpu_pct < 0.0f) {
        cpu_pct = 0.0f;
    } else if (cpu_pct > 100.0f) {
        cpu_pct = 100.0f;
    }
    s_cpu_load_target = cpu_pct;

}



void haptics_set_instrument_cpu_arc(float arc_pos)

{

    s_instrument_cpu_arc = arc_pos;

}



void haptics_set_brake_enabled(bool enabled)

{

    s_brake_enabled = enabled;

    if (!enabled) {
        s_haptic_pulse = 0;
    }

}



bool haptics_brake_enabled(void)

{

    return s_brake_enabled;

}



void haptics_on_press_edge(bool pressed)

{

    (void)pressed;

    if (!s_motor_ready || !s_brake_enabled) {
        return;
    }

    s_haptic_pulse = 1;

}



void haptics_on_release_edge(void)

{

    if (!s_motor_ready || !s_brake_enabled) {
        return;
    }

    s_haptic_pulse = 2;

}



int32_t haptics_get_position(void)

{

    return s_position;

}



void haptics_set_position(int32_t position)

{

    s_pending_position = position;

}



void haptics_request_foc_calibration(void)

{

    if (!s_motor_ready) {
        ESP_LOGW(TAG, "FOC cal request ignored (motor not ready)");
        s_foc_cal_poll = 3;
        return;
    }

    if (s_foc_cal_poll == 1) {
        return;
    }

    s_foc_cal_poll = 0;
    s_foc_cal_requested = true;

}



int haptics_foc_calibration_poll(void)

{

    return s_foc_cal_poll;

}



float haptics_get_encoder_angle_rad(void)

{

    return s_encoder_angle_rad;

}



} /* extern "C" */


