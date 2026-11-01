#include <stdio.h>
#include <string.h>
#include "user_controls.h"
#include "board.h"
#include "voice_capture.h"
#include "power_policy.h"
#include "battery_led.h"
#include "activation_ui.h"
#include "nvs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

typedef struct { uint8_t mode,h,s,v,mute; } preferences_t;
typedef struct { uint8_t type,p[4]; } command_t;
static preferences_t s_p={.s=255,.mute=1};
static QueueHandle_t s_commands;
static SemaphoreHandle_t s_lock;
static uint8_t s_speed=60;
static uint32_t s_applied,s_saved,s_errors;
static int64_t s_last_audio;
bool user_controls_sound_on(void) { return !s_p.mute; }
esp_err_t user_controls_command(uint8_t type,const uint8_t *p,size_t n)
{
    if(power_policy_shutting_down()) return ESP_ERR_INVALID_STATE;
    if(!((type==0x20 && n==4 && (p[0]==4 || p[0]==9)) ||
         (type==0x21 && n==3 && (p[0]==5 || p[0]==6)) ||
         (type==0x30 && n==3 && (p[0]==7 || p[0]==8)))) return ESP_ERR_INVALID_ARG;
    command_t command={.type=type};memcpy(command.p,p,n);
    return xQueueSend(s_commands,&command,0)==pdTRUE?ESP_OK:ESP_ERR_TIMEOUT;
}
static esp_err_t save(const preferences_t *prefs,uint8_t speed)
{
    nvs_handle_t h;esp_err_t result=nvs_open("lx_ui",NVS_READWRITE,&h);
    if(result==ESP_OK) {
        result=nvs_set_blob(h,"prefs",prefs,sizeof(*prefs));
        if(result==ESP_OK) result=nvs_set_u8(h,"led_speed",speed);
        if(result==ESP_OK) result=nvs_commit(h);
        nvs_close(h);
    }
    if(result==ESP_OK) s_saved++;else s_errors++;
    return result;
}
esp_err_t user_controls_lighting_get(user_lighting_t *out)
{
    if(!out || !s_lock || xSemaphoreTake(s_lock,pdMS_TO_TICKS(1000))!=pdTRUE)return ESP_ERR_INVALID_STATE;
    *out=(user_lighting_t){s_p.mode,s_p.h,s_p.s,s_p.v,s_speed};
    xSemaphoreGive(s_lock);return ESP_OK;
}
esp_err_t user_controls_lighting_set(const user_lighting_t *value)
{
    if(!value || value->mode>5 || value->speed<1 || value->speed>100)return ESP_ERR_INVALID_ARG;
    if(power_policy_shutting_down() || !s_lock || xSemaphoreTake(s_lock,pdMS_TO_TICKS(1000))!=pdTRUE)return ESP_ERR_INVALID_STATE;
    preferences_t next=s_p;
    next.mode=value->mode;next.h=value->h;next.s=value->s;next.v=value->v;
    esp_err_t rc=ESP_OK;
    if(memcmp(&next,&s_p,sizeof(next)) || s_speed!=value->speed) {
        rc=save(&next,value->speed);
        if(rc==ESP_OK) { s_p=next;s_speed=value->speed;s_applied++; }
    }
    xSemaphoreGive(s_lock);
    if(rc==ESP_OK)power_policy_activity();
    return rc;
}
static void rgb(unsigned h,unsigned s,unsigned v,uint8_t out[3])
{
    unsigned sector=h*6/256,f=h*6%256;
    unsigned a=v*(255-s)/255,b=v*(255-s*f/255)/255,c=v*(255-s*(255-f)/255)/255;
    const uint8_t colors[6][3]={{v,c,a},{b,v,a},{a,v,c},{a,b,v},{c,a,v},{v,a,b}};
    memcpy(out,colors[sector],3);
}
static void controls_task(void *arg)
{
    (void)arg;int64_t dirty=0;uint8_t previous[3]={255,255,255};bool dark=false,off=false;uint32_t activation_rev=0;
    while(1) {
        command_t command;int64_t now=esp_timer_get_time();
        xSemaphoreTake(s_lock,portMAX_DELAY);
        while(xQueueReceive(s_commands,&command,0)==pdTRUE) {
            power_policy_activity();
            preferences_t before=s_p;
            if(command.type==0x20) { s_p.mode=0;s_p.h=command.p[1];s_p.s=command.p[2];s_p.v=command.p[3]; }
            else if(command.type==0x21) s_p.mode=command.p[0]==5?1:2;
            else { s_p.mute=command.p[0]==7;voice_audio_enable(!s_p.mute); }
            s_applied++;
            if(memcmp(&before,&s_p,sizeof(s_p))) dirty=now+500000;
            if(command.p[0]==9 && dirty) dirty=now;
        }
        if(dirty && now>=dirty) { save(&s_p,s_speed);dirty=0; }
        preferences_t p=s_p;uint8_t speed=s_speed;
        xSemaphoreGive(s_lock);
        power_policy_tick();
        uint32_t next_activation_rev=activation_ui_revision();
        bool next_dark=power_policy_dark();
        uint8_t peak[3];rgb(p.h,p.s,p.v*128/255,peak);
        /* Rainbow can reach a two-channel peak regardless of its starting hue. */
        if(p.mode==4) { peak[0]=p.v*128/255;peak[1]=peak[0];peak[2]=peak[0]*(255-p.s)/255; }
        uint32_t setting=((uint32_t)p.mode<<24)|((uint32_t)p.h<<16)|((uint32_t)p.s<<8)|p.v;
        battery_led_tick(!next_dark && !power_policy_shutting_down(),setting,(unsigned)peak[0]+peak[1]+peak[2]);
        board_side_led_tick();
        if(power_policy_shutting_down()) {
            if(!off) { if(board_lights_off()==ESP_OK)off=true;else s_errors++; }
            vTaskDelay(pdMS_TO_TICKS(20));continue;
        }
        if(next_dark) {
            if(!dark || battery_led_refresh_needed() || now-s_last_audio>=500000) {
                if(board_led_rgb(0,0,0,p.v*128/255)==ESP_OK)dark=true;else s_errors++;
                s_last_audio=now;
            }
            vTaskDelay(pdMS_TO_TICKS(20));continue;
        }
        if(dark) { memset(previous,255,sizeof(previous));s_last_audio=0;dark=false; }
        unsigned value=p.v*128/255,hue=p.h;
        unsigned period_ms=6000-speed*50;
        unsigned phase=(now/1000)%period_ms;
        if(p.mode==1) {
            unsigned factor=phase<period_ms/2?phase:period_ms-phase;
            value=value*factor/(period_ms/2);
        } else if(p.mode==2) {
            value=value*board_audio_level()/255;
        } else if(p.mode==3) {
            if(phase>=period_ms/2)value=0;
        } else if(p.mode==4) {
            hue=(p.h+phase*256/period_ms)%256;
        } else if(p.mode==5) {
            value=0;
        }
        uint8_t color[3];rgb(hue,p.s,value,color);
        /* Also restore the selected effect after a temporary LED diagnostic. */
        if(activation_rev!=next_activation_rev || battery_led_refresh_needed() || memcmp(previous,color,3) || now-s_last_audio>=500000) {
            if(board_led_rgb(color[0],color[1],color[2],p.v*128/255)==ESP_OK) { memcpy(previous,color,3);activation_rev=next_activation_rev; }
            else s_errors++;
            s_last_audio=now;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
esp_err_t user_controls_start(void)
{
    nvs_handle_t h;
    if(nvs_open("lx_ui",NVS_READONLY,&h)==ESP_OK) {
        preferences_t p;size_t n=sizeof(p);
        if(nvs_get_blob(h,"prefs",&p,&n)==ESP_OK && n==sizeof(p) && p.mode<=5 && p.mute<=1) s_p=p;
        uint8_t speed=60;
        if(nvs_get_u8(h,"led_speed",&speed)==ESP_OK && speed>=1 && speed<=100)s_speed=speed;
        nvs_close(h);
    }
    voice_audio_enable(!s_p.mute);
    s_lock=xSemaphoreCreateMutex();if(!s_lock)return ESP_ERR_NO_MEM;
    s_commands=xQueueCreate(8,sizeof(command_t));if(!s_commands) return ESP_ERR_NO_MEM;
    return xTaskCreatePinnedToCore(controls_task,"user_controls",3072,NULL,2,NULL,0)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;
}
void user_controls_stats(void)
{
    printf("LX_CONTROLS mode=%u hsv=%u,%u,%u sound=%u applied=%lu saved=%lu errors=%lu\n",
        s_p.mode,s_p.h,s_p.s,s_p.v,!s_p.mute,(unsigned long)s_applied,(unsigned long)s_saved,(unsigned long)s_errors);
}
