#include <stdio.h>
#include <string.h>
#include "usb_keys.h"
#include "mouse_queue.h"
#include "consumer_queue.h"
#include "tusb.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct { lx_key_report_t report; uint32_t sequence; int64_t event_us; } item_t;
typedef struct { uint32_t sequence; int64_t event_us,sent_us,done_us; } trace_t;
static item_t s_queue[32];
static unsigned s_head,s_count;
static TaskHandle_t s_task;
static portMUX_TYPE s_lock=portMUX_INITIALIZER_UNLOCKED;
static item_t s_latest,s_inflight;
static int64_t s_key_delivered_us;
static bool s_resync,s_pending;
static bool s_cancel_pending;
static volatile uint8_t s_leds;
static uint32_t s_sent,s_done,s_full;
static trace_t *s_trace;
static unsigned s_trace_count;
static int64_t s_sent_us;
static lx_mouse_queue_t s_mouse;
static lx_consumer_queue_t s_consumer;
void usb_consumer_submit(uint16_t usage)
{
    portENTER_CRITICAL(&s_lock);lx_consumer_push(&s_consumer,usage);portEXIT_CRITICAL(&s_lock);
    if(s_task) xTaskNotifyGive(s_task);
}
void usb_mouse_submit(const lx_mouse_sample_t *sample)
{
    portENTER_CRITICAL(&s_lock);lx_mouse_queue_push(&s_mouse,sample);portEXIT_CRITICAL(&s_lock);
    if(s_task) xTaskNotifyGive(s_task);
}

void tud_hid_report_complete_cb(uint8_t instance,uint8_t const *report,uint16_t length)
{
    (void)instance;
    if(length && report[0]==1) {
        int64_t now=esp_timer_get_time();
        portENTER_CRITICAL(&s_lock);
        if(s_pending) {
            if(s_trace && s_trace_count<1200) s_trace[s_trace_count++]=(trace_t){
                s_inflight.sequence,s_inflight.event_us,s_sent_us,now};
            s_key_delivered_us=s_inflight.event_us;
            s_pending=false; s_done++;
        }
        portEXIT_CRITICAL(&s_lock);
    }
    if(s_task) xTaskNotifyGive(s_task);
}
void tud_hid_set_report_cb(uint8_t instance,uint8_t id,hid_report_type_t type,
                          uint8_t const *buffer,uint16_t length)
{
    (void)instance;
    if(type==HID_REPORT_TYPE_OUTPUT && id==1 && length) s_leds=buffer[0];
}
uint8_t usb_keys_leds(void) { return s_leds; }
bool usb_keys_text_room(bool *pending)
{
    portENTER_CRITICAL(&s_lock);bool room=s_count<2;
    *pending=s_count || s_pending || s_latest.event_us>s_key_delivered_us;
    portEXIT_CRITICAL(&s_lock);return room;
}
void usb_keys_cancel_pending(void)
{
    item_t release={.event_us=esp_timer_get_time()};
    portENTER_CRITICAL(&s_lock);
    s_head=0;s_count=1;s_queue[0]=s_latest=release;s_resync=false;s_cancel_pending=true;
    portEXIT_CRITICAL(&s_lock);
    if(s_task) xTaskNotifyGive(s_task);
}
void usb_keys_submit(const lx_key_report_t *report,uint32_t sequence,int64_t event_us)
{
    if(!s_task) return;
    item_t item={.report=*report,.sequence=sequence,.event_us=event_us};
    portENTER_CRITICAL(&s_lock);
    s_latest=item;
    if(s_count<32) { s_queue[(s_head+s_count)%32]=item; s_count++; }
    else { s_resync=true; s_full++; }
    portEXIT_CRITICAL(&s_lock);
    xTaskNotifyGive(s_task);
}
static bool send(const item_t *item)
{
    portENTER_CRITICAL(&s_lock);
    s_inflight=*item; s_sent_us=esp_timer_get_time(); s_pending=true;
    portEXIT_CRITICAL(&s_lock);
    if(!tud_hid_keyboard_report(1,item->report.modifiers,item->report.keys)) {
        portENTER_CRITICAL(&s_lock); s_pending=false; portEXIT_CRITICAL(&s_lock);
        return false;
    }
    portENTER_CRITICAL(&s_lock); s_sent++; portEXIT_CRITICAL(&s_lock);
    return true;
}
static void output_task(void *arg)
{
    (void)arg;
    bool connected=false,have=false,release_first=false;
    item_t item={0},snapshot={0};
    lx_mouse_report_t mouse={0};bool mouse_have=false;
    uint16_t consumer=0;bool consumer_have=false;
    while(1) {
        bool mounted=tud_mounted();
        if(!mounted) {
            connected=false; have=false;mouse_have=false;consumer_have=false;
            portENTER_CRITICAL(&s_lock);
            s_count=0; s_head=0; s_pending=false;s_mouse.count=0;s_consumer.count=0;
            portEXIT_CRITICAL(&s_lock);
        } else {
            portENTER_CRITICAL(&s_lock);
            bool resync=s_resync;
            if(s_cancel_pending) { have=false;release_first=false;s_cancel_pending=false; }
            s_resync=false;
            if(!connected || resync) {
                s_count=0; s_head=0; snapshot=s_latest;
                lx_mouse_queue_resync(&s_mouse);mouse_have=false;
                lx_consumer_resync(&s_consumer);consumer_have=false;
            }
            portEXIT_CRITICAL(&s_lock);
            if(!connected || resync) {
                connected=true; release_first=true; have=false;
            }
            if(tud_hid_ready()) {
                if(release_first) {
                    item_t release={.event_us=esp_timer_get_time()};
                    if(send(&release)) {
                        release_first=false;
                        item=snapshot;
                        have=true;
                    }
                } else {
                    if(!have) {
                        portENTER_CRITICAL(&s_lock);
                        if(s_count) { item=s_queue[s_head]; s_head=(s_head+1)%32; s_count--; have=true; }
                        portEXIT_CRITICAL(&s_lock);
                    }
                    if(have) { if(send(&item)) have=false; }
                    else {
                        portENTER_CRITICAL(&s_lock);
                        if(!consumer_have) consumer_have=lx_consumer_take(&s_consumer,&consumer);
                        portEXIT_CRITICAL(&s_lock);
                        if(consumer_have) {
                            if(tud_hid_report(3,&consumer,2)) consumer_have=false;
                        } else {
                        if(!mouse_have) {
                            portENTER_CRITICAL(&s_lock);mouse_have=lx_mouse_queue_take(&s_mouse,&mouse);portEXIT_CRITICAL(&s_lock);
                        }
                        if(mouse_have && tud_hid_report(2,&mouse,sizeof(mouse))) mouse_have=false;
                        }
                    }
                }
            }
        }
        /* Wake immediately on new input/completion, or retry after one tick. */
        ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(1));
    }
}
esp_err_t usb_keys_start(void)
{
    if(s_task) return ESP_OK;
    if(xTaskCreatePinnedToCore(output_task,"key_output",4096,NULL,11,&s_task,1)!=pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
void usb_keys_print_stats(void)
{
    portENTER_CRITICAL(&s_lock);
    uint32_t sent=s_sent,done=s_done,full=s_full,count=s_trace_count;
    portEXIT_CRITICAL(&s_lock);
    printf("LX_INPUT sent=%lu completed=%lu queue_full=%lu trace=%lu leds=%u mounted=%u\n",
        (unsigned long)sent,(unsigned long)done,(unsigned long)full,(unsigned long)count,s_leds,tud_mounted());
}
void usb_keys_trace_clear(void)
{
    if(!s_trace) { trace_t *trace=heap_caps_calloc(1200,sizeof(trace_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        portENTER_CRITICAL(&s_lock);s_trace=trace;portEXIT_CRITICAL(&s_lock); }
    portENTER_CRITICAL(&s_lock); s_trace_count=0; portEXIT_CRITICAL(&s_lock);
}
void usb_keys_trace_print(void)
{
    portENTER_CRITICAL(&s_lock); unsigned count=s_trace_count; portEXIT_CRITICAL(&s_lock);
    for(unsigned i=0;i<count;i++) {
        portENTER_CRITICAL(&s_lock); trace_t t=s_trace[i]; portEXIT_CRITICAL(&s_lock);
        printf("LX_TRACE %lu %lld %lld %lld\n",(unsigned long)t.sequence,
            (long long)t.event_us,(long long)t.sent_us,(long long)t.done_us);
    }
    printf("LX_TRACE_END %u\n",count);
}
