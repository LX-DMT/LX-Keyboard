#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "chat_uplink.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

/* 1.92 seconds of encoded audio, bounded independently of the 600 ms ADC
 * buffer. Never block capture or downlink decoding on a TLS write. */
#define TX_FRAMES 32
#define TX_BYTES 1536
#define TX_WAIT_MS 1500
typedef struct { size_t n;uint8_t bytes[TX_BYTES]; } tx_frame_t;
struct chat_uplink {
    esp_websocket_client_handle_t ws;
    QueueHandle_t queue;StaticQueue_t queue_state;
    uint8_t storage[TX_FRAMES*sizeof(tx_frame_t)];
    tx_frame_t sending;
    SemaphoreHandle_t done;
    atomic_bool stop;
    atomic_int error;
    unsigned sent,peak,wait_max_ms,slow,full,stall_ms;
};
static void sender(void *arg)
{
    chat_uplink_t *u=arg;
    while(!atomic_load(&u->stop)) {
        if(xQueueReceive(u->queue,&u->sending,pdMS_TO_TICKS(20))!=pdTRUE) continue;
        int64_t started=esp_timer_get_time();
        /* Fault injection is reachable only through the explicit local test.
         * Delay both initial upload and upload during echoed playback. */
        if(u->stall_ms && (u->sent==0 || u->sent==10)) {
            printf("LX_CHAT_TX_TEST stall_ms=%u after_frames=%u\n",u->stall_ms,u->sent);
            int64_t until=started+(int64_t)u->stall_ms*1000;
            while(!atomic_load(&u->stop) && esp_timer_get_time()<until) vTaskDelay(pdMS_TO_TICKS(10));
        }
        if(atomic_load(&u->stop)) break;
        int n=esp_websocket_client_send_bin(u->ws,(const char*)u->sending.bytes,u->sending.n,pdMS_TO_TICKS(TX_WAIT_MS));
        unsigned elapsed=(esp_timer_get_time()-started)/1000;
        if(elapsed>u->wait_max_ms) u->wait_max_ms=elapsed;
        if(elapsed>=300) u->slow++;
        if(n!=(int)u->sending.n) {
            /* A partial websocket write cannot safely be retried. */
            printf("LX_CHAT_TX failed=1 bytes=%u written=%d wait_ms=%u\n",(unsigned)u->sending.n,n,elapsed);
            atomic_store(&u->error,ESP_FAIL);break;
        }
        u->sent++;
    }
    xSemaphoreGive(u->done);vTaskDeleteWithCaps(NULL);
}
chat_uplink_t *chat_uplink_start(esp_websocket_client_handle_t ws,unsigned stall_ms)
{
    chat_uplink_t *u=heap_caps_calloc(1,sizeof(*u),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!u) return NULL;
    u->ws=ws;u->stall_ms=stall_ms;atomic_init(&u->stop,false);atomic_init(&u->error,ESP_OK);
    u->queue=xQueueCreateStatic(TX_FRAMES,sizeof(tx_frame_t),u->storage,&u->queue_state);
    u->done=xSemaphoreCreateBinary();
    if(u->queue && u->done && xTaskCreatePinnedToCoreWithCaps(sender,"chat_uplink",6144,u,4,NULL,0,
            MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)==pdPASS) return u;
    if(u->queue) vQueueDelete(u->queue);
    if(u->done) vSemaphoreDelete(u->done);
    free(u);return NULL;
}
esp_err_t chat_uplink_push(chat_uplink_t *u,const void *bytes,size_t n)
{
    esp_err_t rc=atomic_load(&u->error);if(rc!=ESP_OK) return rc;
    if(!n || n>TX_BYTES) return ESP_ERR_INVALID_SIZE;
    tx_frame_t frame={.n=n};memcpy(frame.bytes,bytes,n);
    if(xQueueSend(u->queue,&frame,0)!=pdTRUE) {
        u->full++;atomic_store(&u->error,ESP_ERR_TIMEOUT);return ESP_ERR_TIMEOUT;
    }
    unsigned queued=uxQueueMessagesWaiting(u->queue);if(queued>u->peak)u->peak=queued;
    return ESP_OK;
}
esp_err_t chat_uplink_result(chat_uplink_t *u) { return atomic_load(&u->error); }
void chat_uplink_stop(chat_uplink_t *u)
{
    if(!u) return;
    atomic_store(&u->stop,true);xSemaphoreTake(u->done,portMAX_DELAY);
    printf("LX_CHAT_TX sent=%u queue_peak=%u full=%u wait_max_ms=%u slow=%u result=%s\n",
        u->sent,u->peak,u->full,u->wait_max_ms,u->slow,esp_err_to_name(atomic_load(&u->error)));
    vQueueDelete(u->queue);vSemaphoreDelete(u->done);free(u);
}
