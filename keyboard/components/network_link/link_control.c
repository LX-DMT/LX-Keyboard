#include <string.h>
#include "link_control.h"
#include "network_link.h"
#include "bringup_usb.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

typedef struct { unsigned route;uint8_t type;uint16_t length;uint32_t sequence,session;uint8_t data[32]; } message_t;
static QueueHandle_t s_rx;
static portMUX_TYPE s_lock=portMUX_INITIALIZER_UNLOCKED;
static bool s_receiver,s_out_valid,s_pending;
static uint8_t s_mode;
static message_t s_out,s_request;
static int64_t s_out_until,s_request_until;
static void (*s_stats_handler)(const uint8_t values[3]);
void link_control_set_stats_handler(void (*handler)(const uint8_t values[3])) { s_stats_handler=handler; }

esp_err_t link_control_apply(const uint8_t *data,size_t length,uint8_t reply[10])
{
    esp_err_t result=ESP_OK;
    uint8_t mac[6];esp_read_mac(mac,ESP_MAC_WIFI_STA);
    if(s_receiver) result=ESP_ERR_NOT_SUPPORTED;
    else if(length && data[0]==33) {
        if(length!=10 || data[1]>100 || data[2]>100 || data[3]>100) result=ESP_ERR_INVALID_ARG;
        else if(memcmp(data+4,mac,6)) result=ESP_ERR_INVALID_STATE;
        else if(!s_stats_handler) result=ESP_ERR_NOT_SUPPORTED;
        else s_stats_handler(data+1); /* Latest-value queue; never block HID or BLE callbacks. */
    }
    else if(length) result=network_apply_mode(data,length);
    memset(reply,0,10);reply[0]=result;reply[1]=result>>8;
    reply[2]=s_mode;reply[3]=network_target_pc();
    memcpy(reply+4,mac,6);
    return result;
}
static void usb_error(const message_t *request,esp_err_t error)
{
    uint8_t status[2]={(uint8_t)error,(uint8_t)(error>>8)};
    bringup_usb_packet(LX_MSG_CONTROL_REPLY,request->sequence,status,2);
}
void link_control_receive(unsigned route,const lx_packet_t *packet)
{
    if(!s_rx || packet->length>32 ||
       (packet->type!=LX_MSG_CONTROL && packet->type!=LX_MSG_CONTROL_REPLY)) return;
    message_t item={.route=route,.type=packet->type,.sequence=packet->sequence,.session=packet->session,.length=packet->length};
    if(item.length) memcpy(item.data,packet->payload,item.length);
    xQueueSend(s_rx,&item,0); /* Never block USB, Wi-Fi, or the radio callback. */
}
void link_control_forward(unsigned route,const lx_packet_t *packet)
{
    message_t request={.route=route,.type=LX_MSG_CONTROL,.sequence=packet->sequence,.session=packet->session,.length=packet->length};
    if(!s_receiver || packet->length>32 || !route) { usb_error(&request,ESP_ERR_INVALID_STATE);return; }
    if(request.length) memcpy(request.data,packet->payload,request.length);
    portENTER_CRITICAL(&s_lock);
    bool accept=!s_pending && !s_out_valid;
    if(accept) {
        s_request=s_out=request;s_pending=s_out_valid=true;
        s_request_until=s_out_until=esp_timer_get_time()+1000000;
    }
    portEXIT_CRITICAL(&s_lock);
    if(!accept) usb_error(&request,ESP_ERR_INVALID_STATE);
}
size_t link_control_take(unsigned route,uint8_t bytes[LX_PACKET_MAX])
{
    message_t item;
    portENTER_CRITICAL(&s_lock);
    if(s_out_valid && esp_timer_get_time()>=s_out_until) s_out_valid=false;
    bool take=s_out_valid && s_out.route==route;
    if(take) { item=s_out;s_out_valid=false; }
    portEXIT_CRITICAL(&s_lock);
    return take?lx_packet_encode(bytes,LX_PACKET_MAX,item.type,item.sequence,item.session,item.data,item.length):0;
}
void link_control_drop(unsigned route)
{
    portENTER_CRITICAL(&s_lock);
    if(s_out_valid && s_out.route==route) s_out_valid=false;
    if(s_pending && s_request.route==route) s_request_until=0;
    portEXIT_CRITICAL(&s_lock);
}
static void control_task(void *arg)
{
    (void)arg;
    while(1) {
        message_t item;
        if(xQueueReceive(s_rx,&item,pdMS_TO_TICKS(10))==pdTRUE) {
            if(s_receiver && item.type==LX_MSG_CONTROL_REPLY) {
                portENTER_CRITICAL(&s_lock);
                bool match=s_pending && item.route==s_request.route && item.sequence==s_request.sequence && item.session==s_request.session;
                if(match) s_pending=false;
                portEXIT_CRITICAL(&s_lock);
                if(match) bringup_usb_packet(item.type,item.sequence,item.data,item.length);
            } else if(!s_receiver && item.type==LX_MSG_CONTROL) {
                bool route_ok=(item.route==LX_CONTROL_USB && s_mode==0) ||
                    (item.route==LX_CONTROL_RADIO && s_mode==3) || (item.route==LX_CONTROL_WIFI && s_mode==2);
                portENTER_CRITICAL(&s_lock);bool room=!s_out_valid;portEXIT_CRITICAL(&s_lock);
                if(route_ok && room) {
                    uint8_t reply[10];link_control_apply(item.data,item.length,reply);
                    if(item.route==LX_CONTROL_USB) bringup_usb_packet(LX_MSG_CONTROL_REPLY,item.sequence,reply,sizeof(reply));
                    else {
                        memcpy(item.data,reply,sizeof(reply));item.length=sizeof(reply);item.type=LX_MSG_CONTROL_REPLY;
                        portENTER_CRITICAL(&s_lock);s_out=item;s_out_valid=true;s_out_until=esp_timer_get_time()+1000000;portEXIT_CRITICAL(&s_lock);
                    }
                }
            }
        }
        portENTER_CRITICAL(&s_lock);
        bool timeout=s_pending && esp_timer_get_time()>=s_request_until;
        if(timeout) { item=s_request;s_pending=false;s_out_valid=false; }
        portEXIT_CRITICAL(&s_lock);
        if(timeout) usb_error(&item,ESP_ERR_TIMEOUT);
    }
}
esp_err_t link_control_start(bool receiver,uint8_t mode)
{
    s_receiver=receiver;s_mode=mode;s_rx=xQueueCreate(2,sizeof(message_t));
    if(!s_rx) return ESP_ERR_NO_MEM;
#if CONFIG_SPIRAM_FETCH_INSTRUCTIONS && CONFIG_SPIRAM_RODATA
    return xTaskCreatePinnedToCoreWithCaps(control_task,"link_control",3072,NULL,3,NULL,0,
        MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;
#else
    return xTaskCreatePinnedToCore(control_task,"link_control",3072,NULL,3,NULL,0)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;
#endif
}
