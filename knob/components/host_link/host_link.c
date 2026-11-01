/**
 * @file host_link.c
 * @brief 与键盘/上位机 UART 二进制协议（LX Host Link v1）
 *
 * UART0 GPIO43/44 @ 115200。
 * - RX：解析 host→device 帧（0x80 HOST_STATS、0x82 POWER_STATS、0x83 POWER_PREVIEW、0x84 AUDIO_STATUS）
 * - TX：转发 UI remote_cmd（0x10–0x30）到键盘
 * CRC8 校验，SOF 0x4C 0x58。详见 docs/host_link/proto.md。
 */

#include "host_link.h"

#include "board_pins.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "knob_events.h"

#include <string.h>

static const char *TAG = "host_link";
static SemaphoreHandle_t s_uart_mtx;

static uint8_t host_link_crc8(const uint8_t *data, uint8_t len)
{
    uint8_t crc = 0;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static void host_link_publish_stats(uint8_t cpu, uint8_t gpu, uint8_t ram)
{
    knob_event_t evt = {};
    evt.type = KNOB_EVT_HOST_STATS;
    evt.data.host_stats.cpu_pct = cpu;
    evt.data.host_stats.gpu_pct = gpu;
    evt.data.host_stats.ram_pct = ram;
    event_bus_publish(&evt, 0);
}

static void host_link_publish_power(uint16_t bus_mv, int16_t current_ma)
{
    knob_event_t evt = {};
    evt.type = KNOB_EVT_KEYBOARD_POWER;
    evt.data.keyboard_power.bus_mv = bus_mv;
    evt.data.keyboard_power.current_ma = current_ma;
    event_bus_publish(&evt, 0);
}

static void host_link_publish_power_preview(bool pressed)
{
    knob_event_t evt = {};
    evt.type = KNOB_EVT_POWER_PREVIEW;
    evt.data.power_preview.pressed = pressed;
    event_bus_publish(&evt, 0);
}

static void host_link_publish_audio_status(bool sound_on)
{
    knob_event_t evt = {};
    evt.type = KNOB_EVT_KEYBOARD_AUDIO;
    evt.data.keyboard_audio.sound_on = sound_on;
    event_bus_publish(&evt, 0);
}

static void host_link_parse_rx(const uint8_t *frame, uint8_t len)
{
    if (len < 5) {
        return;
    }
    if (frame[0] != HOST_LINK_SOF0 || frame[1] != HOST_LINK_SOF1) {
        return;
    }
    if (frame[2] != HOST_LINK_VERSION) {
        return;
    }
    uint8_t type = frame[3];
    uint8_t plen = frame[4];
    if ((uint8_t)(5 + plen + 1) > len) {
        return;
    }
    uint8_t crc = host_link_crc8(&frame[2], (uint8_t)(3 + plen));
    if (crc != frame[5 + plen]) {
        ESP_LOGW(TAG, "CRC mismatch type=0x%02X", type);
        return;
    }
    const uint8_t *payload = &frame[5];
    /* host → device：解析后发布到 event_bus，由 app → UI 刷新 */
    if (type == HOST_MSG_HOST_STATS && plen >= 3) {
        host_link_publish_stats(payload[0], payload[1], payload[2]);
    } else if (type == HOST_MSG_POWER_STATS && plen >= 4) {
        uint16_t bus_mv = (uint16_t)(payload[0] | ((uint16_t)payload[1] << 8));
        int16_t current_ma = (int16_t)(payload[2] | ((uint16_t)payload[3] << 8));
        host_link_publish_power(bus_mv, current_ma);
    } else if (type == HOST_MSG_POWER_PREVIEW && plen >= 1) {
        host_link_publish_power_preview(payload[0] != 0);
    } else if (type == HOST_MSG_AUDIO_STATUS && plen >= 1) {
        host_link_publish_audio_status(payload[0] != 0);
    }
}

static void host_link_rx_task(void *arg)
{
    uint8_t buf[64];
    uint8_t frame[40];
    uint8_t frame_len = 0;
    (void)arg;

    while (true) {
        int n = uart_read_bytes(BOARD_HOST_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (n <= 0) {
            continue;
        }
        for (int i = 0; i < n; i++) {
            if (frame_len == 0 && buf[i] != HOST_LINK_SOF0) {
                continue;
            }
            if (frame_len == 1 && buf[i] != HOST_LINK_SOF1) {
                frame_len = 0;
                if (buf[i] == HOST_LINK_SOF0) {
                    frame[frame_len++] = buf[i];
                }
                continue;
            }
            frame[frame_len++] = buf[i];
            if (frame_len >= 5) {
                uint8_t plen = frame[4];
                if (frame_len >= 5 + plen + 1) {
                    host_link_parse_rx(frame, frame_len);
                    frame_len = 0;
                }
            }
            if (frame_len >= sizeof(frame)) {
                frame_len = 0;
            }
        }
    }
}

esp_err_t host_link_init(void)
{
    if (s_uart_mtx == NULL) {
        s_uart_mtx = xSemaphoreCreateMutex();
        if (s_uart_mtx == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    uart_config_t cfg = {
        .baud_rate = BOARD_HOST_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    /* UART0 与 IDF 控制台共用；启动时驱动可能已安装 */
    if (!uart_is_driver_installed(BOARD_HOST_UART_NUM)) {
        ESP_ERROR_CHECK(uart_driver_install(BOARD_HOST_UART_NUM, 1024, 512, 0, NULL, 0));
    }
    ESP_ERROR_CHECK(uart_param_config(BOARD_HOST_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(BOARD_HOST_UART_NUM,
                                 BOARD_HOST_UART_TX,
                                 BOARD_HOST_UART_RX,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));

    BaseType_t ok = xTaskCreate(host_link_rx_task, "host_rx", 4096, NULL, 4, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "UART%d host link @ %d baud (TX=%d RX=%d)",
             BOARD_HOST_UART_NUM, BOARD_HOST_UART_BAUD,
             BOARD_HOST_UART_TX, BOARD_HOST_UART_RX);
    return ESP_OK;
}

esp_err_t host_link_send_frame(host_msg_type_t type, const uint8_t *payload, uint8_t len)
{
    if (len > 32) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_uart_mtx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_uart_mtx, pdMS_TO_TICKS(10)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t frame[2 + 1 + 1 + 1 + 32 + 1];
    uint8_t idx = 0;
    frame[idx++] = HOST_LINK_SOF0;
    frame[idx++] = HOST_LINK_SOF1;
    frame[idx++] = HOST_LINK_VERSION;
    frame[idx++] = (uint8_t)type;
    frame[idx++] = len;
    for (uint8_t i = 0; i < len; i++) {
        frame[idx++] = payload ? payload[i] : 0;
    }
    frame[idx] = host_link_crc8(&frame[2], (uint8_t)(idx - 2));
    idx++;

    int written = uart_write_bytes(BOARD_HOST_UART_NUM, frame, idx);
    xSemaphoreGive(s_uart_mtx);
    return written == idx ? ESP_OK : ESP_FAIL;
}

static host_msg_type_t host_link_map_remote_type(remote_cmd_t cmd)
{
    switch (cmd) {
    case REMOTE_LINK_USB:
    case REMOTE_LINK_BLE:
    case REMOTE_LINK_WIFI:
    case REMOTE_LINK_ESPNOW:
        return HOST_MSG_REMOTE_LINK;
    case REMOTE_LED_HSV:
    case REMOTE_LED_HSV_COMMIT:
        return HOST_MSG_REMOTE_LED_COLOR;
    case REMOTE_LED_BREATH:
    case REMOTE_LED_RHYTHM:
        return HOST_MSG_REMOTE_LED_MODE;
    case REMOTE_AUDIO_MUTE:
    case REMOTE_AUDIO_UNMUTE:
        return HOST_MSG_REMOTE_AUDIO;
    default:
        return HOST_MSG_DEVICE_STATUS;
    }
}

esp_err_t host_link_send_remote_cmd(remote_cmd_t cmd, uint8_t arg0, uint8_t arg1, uint8_t arg2)
{
    host_msg_type_t type = host_link_map_remote_type(cmd);
    if (type == HOST_MSG_REMOTE_LED_COLOR) {
        uint8_t payload[4] = { (uint8_t)cmd, arg0, arg1, arg2 };
        return host_link_send_frame(type, payload, 4);
    }
    uint8_t payload[3] = { (uint8_t)cmd, arg0, arg1 };
    return host_link_send_frame(type, payload, 3);
}
