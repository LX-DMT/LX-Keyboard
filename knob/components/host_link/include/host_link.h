#pragma once

/**
 * @file host_link.h
 * @brief 上位机 UART 链路协议与 API
 *
 * 帧格式：SOF(2) + VER(1) + TYPE(1) + LEN(1) + PAYLOAD + CRC8
 */

#include <stdint.h>

#include "esp_err.h"
#include "knob_events.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HOST_LINK_SOF0 0x4C
#define HOST_LINK_SOF1 0x58
#define HOST_LINK_VERSION 0x01

/** 主机 ↔ 设备 消息类型（完整定义见 docs/host_link/proto.md） */
typedef enum {
    /* device → host（旋钮 UI 操作） */
    HOST_MSG_REMOTE_LINK = 0x10,
    HOST_MSG_REMOTE_LED_COLOR = 0x20,
    HOST_MSG_REMOTE_LED_MODE = 0x21,
    HOST_MSG_REMOTE_AUDIO = 0x30,
    HOST_MSG_DEVICE_STATUS = 0x40, /**< 预留，未实现 */
    /* host → device（键盘/PC 下发） */
    HOST_MSG_HOST_STATS = 0x80,    /**< PC/工具：CPU/GPU/RAM % */
    HOST_MSG_HEARTBEAT = 0x81,     /**< 预留，RX 未解析 */
    HOST_MSG_POWER_STATS = 0x82,   /**< 键盘 INA219 母线 mV + 电流 mA */
    HOST_MSG_POWER_PREVIEW = 0x83, /**< 键盘右键预览 Power 屏 */
    HOST_MSG_AUDIO_STATUS = 0x84,  /**< 键盘静音状态 sound_on（仅同步 UI） */
} host_msg_type_t;

/** @brief 初始化 UART 并启动 RX 解析任务 */
esp_err_t host_link_init(void);
/** @brief 将 UI remote_cmd_t 映射为协议帧发送 */
esp_err_t host_link_send_remote_cmd(remote_cmd_t cmd, uint8_t arg0, uint8_t arg1, uint8_t arg2);
/** @brief 发送任意类型协议帧 */
esp_err_t host_link_send_frame(host_msg_type_t type, const uint8_t *payload, uint8_t len);

#ifdef __cplusplus
}
#endif
