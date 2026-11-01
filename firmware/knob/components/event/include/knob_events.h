#pragma once

/**
 * @file knob_events.h
 * @brief 旋钮事件类型、应用模式与事件载荷定义
 *
 * 各子系统通过 event_bus 传递 knob_event_t；app 层统一分发到 UI、haptics、BLE 等。
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 旋钮/系统事件类型 */
typedef enum {
    KNOB_EVT_ROTATION = 0,      /**< 编码器档位变化，data.rotation.delta_steps */
    KNOB_EVT_PRESS,             /**< 应变片按下边沿 */
    KNOB_EVT_RELEASE,           /**< 应变片释放边沿 */
    KNOB_EVT_LONG_PRESS,        /**< 长按 */
    KNOB_EVT_MODE_ENTER,        /**< 进入子模式，data.mode_change.mode */
    KNOB_EVT_MODE_EXIT,         /**< 返回主菜单（无载荷，由 app 处理） */
    KNOB_EVT_HOST_STATS,        /**< host→device 0x80：CPU/GPU/RAM % → 仪表三环 */
    KNOB_EVT_KEYBOARD_POWER,    /**< host→device 0x82：键盘母线 mV + 电流 mA → Power 屏 */
    KNOB_EVT_KEYBOARD_AUDIO,    /**< host→device 0x84：键盘 sound_on，仅刷新 Audio switch */
    KNOB_EVT_POWER_PREVIEW,     /**< host→device 0x83：键盘右键临时切 Power 屏 */
    KNOB_EVT_BLE_CONNECTED,
    KNOB_EVT_BLE_DISCONNECTED,
    KNOB_EVT_REMOTE_CMD,        /**< UI 远程控制命令转发给 host_link */
    KNOB_EVT_BATTERY_LOW,       /**< 低电量：关制动、显示低电 UI */
    KNOB_EVT_BATTERY_OK,
} knob_event_type_t;

/** 应用 UI/触觉 模式（与菜单项、子屏对应） */
typedef enum {
    APP_MODE_MENU = 0,
    APP_MODE_CONNECTION,
    APP_MODE_LED,
    APP_MODE_LED_COLOR,
    APP_MODE_AUDIO,
    APP_MODE_STANDBY,
    APP_MODE_INSTRUMENT,
    APP_MODE_ASTRONAUT,
    APP_MODE_SURFACE_DIAL,
} app_mode_t;

/** 经 UART 发给键盘的远程命令（映射为 HOST_MSG_*，见 proto.md） */
typedef enum {
    REMOTE_LINK_USB = 0,
    REMOTE_LINK_BLE,
    REMOTE_LINK_WIFI,
    REMOTE_LINK_ESPNOW,
    REMOTE_LED_HSV,
    REMOTE_LED_BREATH,
    REMOTE_LED_RHYTHM,
    REMOTE_AUDIO_MUTE,
    REMOTE_AUDIO_UNMUTE,
    REMOTE_LED_HSV_COMMIT,      /**< HSV 停旋后写 NVS */
} remote_cmd_t;

/** 统一事件结构：type + 按类型解释的 data 联合体 */
typedef struct {
    knob_event_type_t type;
    union {
        struct {
            int32_t delta_steps;
        } rotation;
        struct {
            app_mode_t mode;
        } mode_change;
        struct {
            uint8_t cpu_pct;
            uint8_t gpu_pct;
            uint8_t ram_pct;
        } host_stats;
        struct {
            uint16_t bus_mv;
            int16_t current_ma;
        } keyboard_power;
        struct {
            bool sound_on;
        } keyboard_audio;
        struct {
            bool pressed;
        } power_preview;
        struct {
            remote_cmd_t cmd;
            uint8_t arg0;
            uint8_t arg1;
            uint8_t arg2;
        } remote;
    } data;
} knob_event_t;

#ifdef __cplusplus
}
#endif
