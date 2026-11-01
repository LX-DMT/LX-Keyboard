#include "lighting_settings.h"
#include "battery_led.h"
#include "bringup_usb.h"
#include "device_setup.h"
#include "power_policy.h"
#include "user_controls.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static StaticSemaphore_t s_write_state;
static SemaphoreHandle_t s_write;

void lighting_settings_init(void)
{
    s_write=xSemaphoreCreateMutexStatic(&s_write_state);
    /* Missing/invalid setting keeps the factory default enabled. */
    bool enabled=true;nvs_handle_t h;
    if(nvs_open("lx_dev",NVS_READONLY,&h)==ESP_OK) {
        uint8_t value=1;
        if(nvs_get_u8(h,"low_light",&value)==ESP_OK && value<=1)enabled=value;
        nvs_close(h);
    }
    battery_led_protection_set(enabled);
}
static esp_err_t save(uint8_t enabled)
{
    if(enabled>1)return ESP_ERR_INVALID_ARG;
    if(device_setup_busy() || power_policy_shutting_down() || !s_write || xSemaphoreTake(s_write,0)!=pdTRUE)return ESP_ERR_INVALID_STATE;
    esp_err_t rc=ESP_OK;
    if(battery_led_protection_enabled()!=(bool)enabled) {
        nvs_handle_t h;rc=nvs_open("lx_dev",NVS_READWRITE,&h);
        if(rc==ESP_OK) {
            rc=nvs_set_u8(h,"low_light",enabled);
            if(rc==ESP_OK)rc=nvs_commit(h);
            nvs_close(h);
        }
        if(rc==ESP_OK)battery_led_protection_set(enabled);
    }
    xSemaphoreGive(s_write);return rc;
}
void lighting_settings_receive(const lx_packet_t *packet)
{
    const uint8_t *p=packet->payload;uint8_t op=packet->length>=2?p[1]:255;
    esp_err_t rc=ESP_ERR_INVALID_ARG;
    if(op==2 || op==3) {
        user_lighting_t value={0};
        if(p[0]==1) {
            if(op==2 && packet->length==2)rc=user_controls_lighting_get(&value);
            else if(op==3 && packet->length==7 && !device_setup_busy()) {
                value=(user_lighting_t){p[2],p[3],p[4],p[5],p[6]};
                rc=user_controls_lighting_set(&value);
                if(rc==ESP_OK)rc=user_controls_lighting_get(&value);
            }
        }
        uint8_t reply[]={1,op,(uint8_t)rc,(uint8_t)(rc>>8),value.mode,value.h,value.s,value.v,value.speed};
        bringup_usb_packet(LX_MSG_LIGHTING_REPLY,packet->sequence,reply,sizeof(reply));return;
    }
    if(packet->length>=2 && p[0]==1) {
        if(op==0 && packet->length==2)rc=ESP_OK;
        else if(op==1 && packet->length==3)rc=save(p[2]);
    }
    uint8_t reply[]={1,op,(uint8_t)rc,(uint8_t)(rc>>8),battery_led_protection_enabled()};
    bringup_usb_packet(LX_MSG_LIGHTING_REPLY,packet->sequence,reply,sizeof(reply));
}
