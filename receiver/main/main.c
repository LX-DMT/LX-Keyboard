#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_flash.h"
#include "esp_idf_version.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bringup_usb.h"
#include "usb_keys.h"
#include "radio_keys.h"
#include "lx_protocol.h"
#include "mouse_queue.h"
#include "network_link.h"
#include "device_setup.h"
#include "link_control.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "status_led.h"
static void receiver_require(esp_err_t result,const char *stage)
{
    if(result==ESP_OK)return;
    printf("LX_RECEIVER_INIT stage=%s error=%s\n",stage,esp_err_to_name(result));
    status_led_fault();
    /* Leave the low-priority indicator running when startup cannot finish. */
    while(1)vTaskDelay(pdMS_TO_TICKS(1000));
}
static void restart_task(void *arg) { (void)arg;vTaskDelay(pdMS_TO_TICKS(200));esp_restart(); }
static esp_err_t request_mode(uint8_t mode)
{
    if(mode!=255) return ESP_ERR_NOT_SUPPORTED;
    return xTaskCreate(restart_task,"restart",2048,NULL,3,NULL)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;
}

static void host_packet(const lx_packet_t *packet,void *arg)
{
    (void)arg;
    if(packet->type==LX_MSG_CONTROL) link_control_forward(wifi_input_active()?LX_CONTROL_WIFI:radio_input_active()?LX_CONTROL_RADIO:0,packet);
    else if(packet->type==LX_MSG_HOST_STATS && packet->length==3 &&
        packet->payload[0]<=100 && packet->payload[1]<=100 && packet->payload[2]<=100) {
        radio_host_stats(packet->payload);wifi_host_stats(packet->payload);bringup_usb_packet(LX_MSG_ACK,packet->sequence,NULL,0);
    } else if(packet->type==LX_MSG_SETUP) {
        device_setup_receive(packet);
    } else if(packet->type==LX_MSG_CONFIG) {
        esp_err_t result=network_apply_config(packet->payload,packet->length);
        uint8_t status[2]={(uint8_t)result,(uint8_t)(result>>8)};bringup_usb_packet(LX_MSG_ACK,packet->sequence,status,2);
    }
}
static void info(void)
{
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    printf("LX_M1 role=receiver idf=%s flash=%lu psram=%u usb=%u\n",
           esp_get_idf_version(), (unsigned long)flash, (unsigned)esp_psram_get_size(),
           bringup_usb_mounted());
    fflush(stdout);
}
void app_main(void)
{
    esp_err_t led_result=status_led_start();
    if(led_result!=ESP_OK)printf("LX_STATUS_LED init=%s\n",esp_err_to_name(led_result));
    network_set_mode_handler(request_mode);
    receiver_require(link_control_start(true,255),"link_control");
    bringup_usb_set_handler(host_packet,NULL);
    receiver_require(bringup_usb_start(true),"usb");
    receiver_require(usb_keys_start(),"usb_keys");
    /* New-project NVS lives after our app, outside the old config region. */
    esp_err_t nvs=nvs_flash_init();
    if(nvs==ESP_ERR_NVS_NO_FREE_PAGES || nvs==ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase()); nvs=nvs_flash_init();
    }
    receiver_require(nvs,"nvs");
    device_setup_init(true);
    receiver_require(radio_keys_start(true),"radio");
    receiver_require(wifi_keys_start(true),"wifi");
    receiver_require(uart_driver_install(UART_NUM_0,512,0,0,NULL,0),"console");
    uart_vfs_dev_use_driver(UART_NUM_0);
    status_led_ready();
    info();
    char line[128];
    while (1) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        line[strcspn(line,"\r\n")]=0;
        if(!strcmp(line,"input")) { usb_keys_print_stats(); radio_keys_stats();wifi_keys_stats(); }
        else if(!strcmp(line,"selftest")) printf("LX_SELFTEST failures=%u\n",network_discovery_selftest()+lx_protocol_selftest()+lx_mouse_selftest()+lx_mouse_queue_selftest());
        else if(!strcmp(line,"trace clear")) { usb_keys_trace_clear(); puts("LX_TRACE_CLEARED"); }
        else if(!strcmp(line,"trace")) usb_keys_trace_print();
        else if(!strcmp(line,"network")) network_print();
        else if(!strncmp(line,"radio rate ",11)) {
            unsigned rate=0;printf("LX_RADIO_RATE %s\n",esp_err_to_name(sscanf(line+11,"%u",&rate)==1?radio_keys_rate(rate):ESP_ERR_INVALID_ARG));
        }
        else if(!strcmp(line,"wifi disconnect")) printf("LX_NETWORK disconnect_test=%s\n",esp_err_to_name(network_disconnect_test()));
        else if(!strcmp(line,"wifi retry")) printf("LX_NETWORK retry=%s\n",esp_err_to_name(network_retry()));
        else if(!strncmp(line,"wifi ",5)) {
            char *value=strchr(line+5,' ');
            if(value) { *value++=0;printf("LX_NETWORK setting=%s result=%s apply=restart\n",line+5,esp_err_to_name(network_setting(line+5,value))); }
        }
        else if(!strcmp(line,"led"))status_led_print();
        else info();
        fflush(stdout);
    }
}
