#include <stdio.h>
#include <string.h>
#include "custom_keys.h"
#include "bringup_usb.h"
#include "device_setup.h"
#include "power_policy.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static uint8_t s_actions[LX_CUSTOM_KEY_COUNT]={0x4c,LX_CUSTOM_POWER_PREVIEW};
static portMUX_TYPE s_lock=portMUX_INITIALIZER_UNLOCKED;
static StaticSemaphore_t s_write_state;
static SemaphoreHandle_t s_write;

void custom_keys_get(uint8_t actions[LX_CUSTOM_KEY_COUNT])
{
    portENTER_CRITICAL(&s_lock);memcpy(actions,s_actions,LX_CUSTOM_KEY_COUNT);portEXIT_CRITICAL(&s_lock);
}
void custom_keys_init(void)
{
    s_write=xSemaphoreCreateMutexStatic(&s_write_state);
    nvs_handle_t h;
    esp_err_t rc=nvs_open("lx_dev",NVS_READONLY,&h);
    if(rc==ESP_OK) {
        uint8_t saved[3];size_t size=sizeof(saved);
        rc=nvs_get_blob(h,"custom_keys",saved,&size);nvs_close(h);
        if(rc==ESP_OK && size==3 && saved[0]==1 && lx_custom_key_valid(saved[1]) && lx_custom_key_valid(saved[2]))
            memcpy(s_actions,saved+1,LX_CUSTOM_KEY_COUNT);
        else if(rc!=ESP_ERR_NVS_NOT_FOUND) printf("LX_CUSTOM_KEYS load=invalid defaults=1\n");
    }
}
esp_err_t custom_keys_save(const uint8_t actions[LX_CUSTOM_KEY_COUNT])
{
    if(!actions || !lx_custom_key_valid(actions[0]) || !lx_custom_key_valid(actions[1])) return ESP_ERR_INVALID_ARG;
    if(device_setup_busy() || power_policy_shutting_down() || !s_write || xSemaphoreTake(s_write,0)!=pdTRUE) return ESP_ERR_INVALID_STATE;
    uint8_t current[2];custom_keys_get(current);
    esp_err_t rc=ESP_OK;
    if(memcmp(current,actions,2)) {
        nvs_handle_t h;rc=nvs_open("lx_dev",NVS_READWRITE,&h);
        if(rc==ESP_OK) {
            uint8_t saved[]={1,actions[0],actions[1]};
            rc=nvs_set_blob(h,"custom_keys",saved,sizeof(saved));
            if(rc==ESP_OK) rc=nvs_commit(h);
            nvs_close(h);
        }
        if(rc==ESP_OK) {
            portENTER_CRITICAL(&s_lock);memcpy(s_actions,actions,2);portEXIT_CRITICAL(&s_lock);
        }
    }
    xSemaphoreGive(s_write);return rc;
}
void custom_keys_receive(const lx_packet_t *packet)
{
    /* Only the keyboard's product USB host_packet dispatches this message.
     * Version, operation, then both actions in one atomic persisted record. */
    const uint8_t *p=packet->payload;uint8_t op=packet->length>=2?p[1]:255;
    esp_err_t rc=ESP_ERR_INVALID_ARG;
    if(packet->length>=2 && p[0]==1) {
        if(op==0 && packet->length==2) rc=ESP_OK;
        else if(op==1 && packet->length==4) rc=custom_keys_save(p+2);
    }
    uint8_t reply[6]={1,op,(uint8_t)rc,(uint8_t)(rc>>8)};
    custom_keys_get(reply+4);
    bringup_usb_packet(LX_MSG_CUSTOM_KEYS_REPLY,packet->sequence,reply,sizeof(reply));
}
void custom_keys_print(void)
{
    uint8_t actions[2];custom_keys_get(actions);
    printf("LX_CUSTOM_KEYS version=1 bit82=%u bit84=%u power_action=0\n",actions[0],actions[1]);
}
