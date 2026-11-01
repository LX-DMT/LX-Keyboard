#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "knob_link.h"
#include "board.h"
#include "battery_led.h"
#include "user_controls.h"
#include "power_policy.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"

/* Reimplemented from the unchanged knob's public LX v1 protocol document. */
static QueueHandle_t s_stats_queue;
static QueueHandle_t s_preview_queue;
static uint8_t s_preview_sent=2;
static uint32_t s_preview_written;
typedef struct { uint8_t size,bytes[38]; } injection_t;
static QueueHandle_t s_inject;
static esp_err_t (*s_handler)(uint8_t,const uint8_t *,size_t);
static uint32_t s_written,s_received,s_bad,s_applied,s_rejected,s_power_written,s_power_errors;
static uint16_t s_mv;
static int16_t s_ma;
static uint8_t s_values[3];
static uint32_t s_power_changes,s_switch_errors;
static uint8_t crc8(const uint8_t *bytes,size_t n)
{
    uint8_t crc=0;
    while(n--) { crc^=*bytes++;for(unsigned i=0;i<8;i++) crc=crc&0x80?(crc<<1)^7:crc<<1; }
    return crc;
}
void knob_set_handler(esp_err_t (*handler)(uint8_t,const uint8_t *,size_t)) { s_handler=handler; }
esp_err_t knob_inject(const uint8_t *bytes,size_t n)
{
    if(!n || n>38) return ESP_ERR_INVALID_ARG;
    injection_t item={.size=n};memcpy(item.bytes,bytes,n);
    return xQueueSend(s_inject,&item,0)==pdTRUE?ESP_OK:ESP_ERR_TIMEOUT;
}
static bool send_frame(uint8_t type,const uint8_t *payload,size_t n)
{
    uint8_t frame[38]={0x4c,0x58,1,type,n};memcpy(frame+5,payload,n);
    frame[5+n]=crc8(frame+2,n+3);
    return board_knob_write(frame,n+6)==n+6;
}
void knob_host_stats(const uint8_t values[3])
{
    if(values[0]>100 || values[1]>100 || values[2]>100) return;
    if(s_stats_queue) xQueueOverwrite(s_stats_queue,values);
}
void knob_power_preview(bool pressed)
{
    uint8_t value=pressed;
    /* Scan never waits on UART; the knob task forwards the latest held state. */
    if(s_preview_queue) xQueueOverwrite(s_preview_queue,&value);
}
static void knob_task(void *arg)
{
    (void)arg;uint8_t buffer[80];size_t used=0;int64_t next_power=0,next_audio=0;
    uint8_t preview=0,desired[3]={0};bool stats_pending=false;
    int64_t ready_at=0,resync_at=0;
    while(1) {
        int64_t now=esp_timer_get_time();
        bool on=!power_policy_dark() && !power_policy_shutting_down();
        if(on!=board_knob_powered()) {
            esp_err_t rc=board_knob_power(on);
            if(rc==ESP_OK) {
                s_power_changes++;used=0;uart_flush_input(UART_NUM_1);
                /* Let the restarted screen boot; repeat the full state once
                 * more for slow display startup, without blocking sampling. */
                ready_at=now+800000;resync_at=on?now+3000000:0;
                stats_pending=true;s_preview_sent=2;next_audio=0;
                printf("LX_KNOB_POWER on=%u gpio39=%d\n",on,board_knob_power_level());
            } else s_switch_errors++;
        }
        bool ready=on && board_knob_powered() && now>=ready_at;
        if(resync_at && now>=resync_at) {
            resync_at=0;stats_pending=true;s_preview_sent=2;next_audio=0;next_power=0;
        }
        xQueueReceive(s_preview_queue,&preview,0);
        if(ready && preview!=s_preview_sent && send_frame(0x83,&preview,1)) {
            s_preview_sent=preview;s_preview_written++;
        }
        uint8_t values[3];
        if(xQueueReceive(s_stats_queue,values,0)==pdTRUE) {
            memcpy(desired,values,3);
            stats_pending|=!s_written || memcmp(values,s_values,3)!=0;
        }
        if(ready && stats_pending && send_frame(0x80,desired,3)) {
            s_written++;memcpy(s_values,desired,3);stats_pending=false;
        }
        if(now>=next_power) {
            next_power=now+500000;
            if(board_power_read(&s_mv,&s_ma)==ESP_OK) {
                battery_led_sample(true,s_mv,s_ma);
                uint8_t p[]={s_mv,s_mv>>8,(uint16_t)s_ma,(uint16_t)s_ma>>8};
                if(ready && send_frame(0x82,p,4)) s_power_written++;
            } else { s_power_errors++;battery_led_sample(false,0,0); }
        }
        if(ready && now>=next_audio) {
            next_audio=now+1000000;uint8_t sound=user_controls_sound_on();send_frame(0x84,&sound,1);
        }
        injection_t item;
        if(sizeof(buffer)-used>=38 && xQueueReceive(s_inject,&item,0)==pdTRUE) {
            memcpy(buffer+used,item.bytes,item.size);used+=item.size;
        }
        int n=0;
        if(ready)n=uart_read_bytes(UART_NUM_1,buffer+used,sizeof(buffer)-used,pdMS_TO_TICKS(20));
        else { uart_flush_input(UART_NUM_1);vTaskDelay(pdMS_TO_TICKS(20)); }
        if(n>0) used+=n;
        while(used>=6) {
            size_t total=6+buffer[4];
            bool header=buffer[0]==0x4c && buffer[1]==0x58 && buffer[2]==1 && buffer[4]<=32;
            if(header && used<total) break;
            size_t consumed=1;
            if(header && crc8(buffer+2,total-3)==buffer[total-1]) {
                s_received++;consumed=total;
                esp_err_t result=s_handler?s_handler(buffer[3],buffer+5,buffer[4]):ESP_ERR_NOT_SUPPORTED;
                if(result==ESP_OK) s_applied++;else s_rejected++;
                printf("LX_KNOB rx_type=%u length=%u result=%s\n",buffer[3],buffer[4],esp_err_to_name(result));
            } else s_bad++;
            memmove(buffer,buffer+consumed,used-consumed);used-=consumed;
        }
    }
}
esp_err_t knob_link_start(void)
{
    s_stats_queue=xQueueCreate(1,3);if(!s_stats_queue) return ESP_ERR_NO_MEM;
    s_preview_queue=xQueueCreate(1,1);if(!s_preview_queue) return ESP_ERR_NO_MEM;
    s_inject=xQueueCreate(4,sizeof(injection_t));if(!s_inject) return ESP_ERR_NO_MEM;
    return xTaskCreatePinnedToCoreWithCaps(knob_task,"knob_link",3072,NULL,3,NULL,0,
        MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;
}
void knob_link_stats(void)
{
    printf("LX_KNOB written=%lu received=%lu bad_bytes=%lu cpu=%u gpu=%u ram=%u display=not_observed\n",
        (unsigned long)s_written,(unsigned long)s_received,(unsigned long)s_bad,s_values[0],s_values[1],s_values[2]);
    printf("LX_KNOB_CONTROL accepted=%lu rejected=%lu power_written=%lu power_errors=%lu bus_mv=%u raw_ma=%d\n",
        (unsigned long)s_applied,(unsigned long)s_rejected,(unsigned long)s_power_written,(unsigned long)s_power_errors,s_mv,s_ma);
    printf("LX_KNOB_PREVIEW sent=%u written=%lu\n",s_preview_sent,(unsigned long)s_preview_written);
    printf("LX_KNOB_POWER on=%u gpio39=%d changes=%lu errors=%lu physical=not_measured\n",
        board_knob_powered(),board_knob_power_level(),(unsigned long)s_power_changes,(unsigned long)s_switch_errors);
}
