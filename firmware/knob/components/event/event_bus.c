/**
 * @file event_bus.c
 * @brief 旋钮全局事件队列（发布/订阅）
 *
 * 单例 FreeRTOS 队列，容量 64，元素为 knob_event_t。
 * 生产者：input_strain、haptics、host_link、ble_dial、battery 等；
 * 消费者：app_dispatch_task（core 1）。
 */

#include "knob_events.h"

#include "esp_log.h"

static const char *TAG = "event";

/** 全局事件队列句柄，event_bus_init 后有效 */
static QueueHandle_t s_event_queue;

/** @brief 创建事件队列（幂等） */
esp_err_t event_bus_init(void)
{
    if (s_event_queue != NULL) {
        return ESP_OK;
    }

    s_event_queue = xQueueCreate(64, sizeof(knob_event_t));
    if (s_event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create event queue");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Event bus ready");
    return ESP_OK;
}

/** @brief 获取底层队列句柄（高级用法） */
QueueHandle_t event_bus_get_queue(void)
{
    return s_event_queue;
}

/**
 * @brief 向总线投递事件
 * @param timeout_ticks 队列满时等待 tick 数，0 表示不阻塞
 */
esp_err_t event_bus_publish(const knob_event_t *event, TickType_t timeout_ticks)
{
    if (s_event_queue == NULL || event == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueSend(s_event_queue, event, timeout_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

/**
 * @brief 从总线取事件（阻塞或超时）
 * @return true 取到事件；false 超时或队列未初始化
 */
bool event_bus_subscribe(knob_event_t *event, TickType_t timeout_ticks)
{
    if (s_event_queue == NULL || event == NULL) {
        return false;
    }
    return xQueueReceive(s_event_queue, event, timeout_ticks) == pdTRUE;
}
