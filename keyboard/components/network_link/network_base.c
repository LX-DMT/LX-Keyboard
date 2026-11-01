#include <stdio.h>
#include <string.h>
#include <time.h>
#include "network_link.h"
#include "device_setup.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

static bool s_started,s_join_requested;
static bool s_power_save;
static volatile bool s_ready;
static esp_netif_t *s_netif;
static uint8_t s_channel=6;
static uint32_t s_receiver;
static uint32_t s_discovered_receiver;
static bool s_sntp_started;
static volatile bool s_auto_retry,s_connecting,s_radio_seen;
static volatile TickType_t s_last_radio;
static volatile unsigned s_disconnect_reason,s_retry_count;
void network_auto_retry(bool enable) { s_auto_retry=enable; }
void network_radio_activity(void) { s_last_radio=xTaskGetTickCount();s_radio_seen=true; }
esp_err_t network_disconnect_test(void) { return esp_wifi_disconnect(); }

static void retry_task(void *arg)
{
    (void)arg;
    unsigned ticks=0;
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(100));device_setup_poll();
        if(++ticks<50) continue;
        ticks=0;
        /* An active ESP-NOW keyboard sends heartbeats even between keystrokes.
         * Never start a router scan while that offline input path is in use. */
        bool radio_active=s_radio_seen && (TickType_t)(xTaskGetTickCount()-s_last_radio)<pdMS_TO_TICKS(2000);
        if(!device_setup_busy() && s_auto_retry && s_join_requested && !s_ready && !s_connecting && !radio_active && network_profile_present()) {
            s_retry_count++;
            esp_err_t result=network_retry();
            printf("LX_NETWORK auto_retry=%u result=%s\n",s_retry_count,esp_err_to_name(result));
        }
    }
}
bool network_profile_present(void)
{
    char ssid[33],password[64];bool configured=device_setup_profile(ssid,password);memset(password,0,sizeof(password));
    if(configured) return ssid[0]!=0;
    nvs_handle_t h;if(nvs_open("lx_net",NVS_READONLY,&h)!=ESP_OK) return false;
    size_t n=0;esp_err_t result=nvs_get_str(h,"ssid",NULL,&n);nvs_close(h);
    return result==ESP_OK && n>1;
}
static void load_peers(void)
{
    nvs_handle_t h;
    if(nvs_open("lx_net",NVS_READONLY,&h)!=ESP_OK) return;
    char value[16];size_t n=sizeof(value);
    if(nvs_get_str(h,"receiver",value,&n)==ESP_OK) inet_pton(AF_INET,value,&s_receiver);
    /* Standalone edition ignores legacy PC target without rewriting NVS. */
    nvs_get_u8(h,"channel",&s_channel);nvs_close(h);
    if(s_channel<1 || s_channel>13) s_channel=6;
}
esp_err_t network_setting(const char *key,const char *value)
{
    if(!strcmp(key,"pc") || (!strcmp(key,"target") && strcmp(value,"receiver"))) return ESP_ERR_NOT_SUPPORTED;
    if(device_setup_busy()) return ESP_ERR_INVALID_STATE;
    char ssid[33],password[64];bool configured=device_setup_profile(ssid,password);memset(password,0,sizeof(password));
    if(configured && (!strcmp(key,"ssid") || !strcmp(key,"pass"))) return ESP_ERR_NOT_SUPPORTED;
    size_t n=strlen(value);
    bool valid=false;
    if(!strcmp(key,"ssid")) valid=n<=32;
    else if(!strcmp(key,"pass")) valid=n==0 || (n>=8 && n<=63);
    else if(!strcmp(key,"target")) valid=!strcmp(value,"receiver");
    else if(!strcmp(key,"receiver")) {
        uint32_t addr;valid=inet_pton(AF_INET,value,&addr)==1 && addr && addr!=0xffffffff;
    }
    if(!valid) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;esp_err_t result=nvs_open("lx_net",NVS_READWRITE,&h);
    if(result==ESP_OK) {
        result=nvs_set_str(h,key,value);if(result==ESP_OK) result=nvs_commit(h);nvs_close(h);
    }
    if(result==ESP_OK) load_peers();
    return result;
}
static void network_event(void *arg,esp_event_base_t base,int32_t id,void *data)
{
    (void)arg;
    if(base==IP_EVENT && id==IP_EVENT_STA_GOT_IP) {
        s_ready=true;s_connecting=false;ip_event_got_ip_t *event=data;
        if(!s_sntp_started) {
            esp_sntp_config_t config=ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
            s_sntp_started=esp_netif_sntp_init(&config)==ESP_OK;
        }
        printf("LX_NETWORK ip=" IPSTR " channel=%u\n",IP2STR(&event->ip_info.ip),s_channel);
    } else if(base==WIFI_EVENT && id==WIFI_EVENT_STA_CONNECTED) {
        wifi_event_sta_connected_t *event=data;s_channel=event->channel;
        nvs_handle_t h;
        if(!device_setup_busy() && nvs_open("lx_net",NVS_READWRITE,&h)==ESP_OK) {
            nvs_set_u8(h,"channel",s_channel);nvs_commit(h);nvs_close(h);
        }
    } else if(base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED) {
        s_ready=false;s_connecting=false;
        s_disconnect_reason=((wifi_event_sta_disconnected_t*)data)->reason;
        /* Preserve the last router channel for the paired offline ESP-NOW path.
         * No repeated full-channel scan while the user is typing. */
        esp_wifi_set_channel(s_channel,WIFI_SECOND_CHAN_NONE);
        printf("LX_NETWORK disconnected reason=%u offline_channel=%u auto_retry=%u\n",s_disconnect_reason,s_channel,s_auto_retry);
    }
}
esp_err_t network_retry(void)
{
    if(!s_started) return ESP_ERR_INVALID_STATE;
    nvs_handle_t h;wifi_config_t config={0};
    char ssid[33]={0},password[64]={0};size_t n=sizeof(ssid);
    esp_err_t result=ESP_OK;
    if(!device_setup_profile(ssid,password)) {
        if(nvs_open("lx_net",NVS_READONLY,&h)!=ESP_OK) return ESP_ERR_NOT_FOUND;
        result=nvs_get_str(h,"ssid",ssid,&n);n=sizeof(password);
        nvs_get_str(h,"pass",password,&n);nvs_close(h);
    }
    if(result!=ESP_OK || !ssid[0]) return ESP_ERR_NOT_FOUND;
    memcpy(config.sta.ssid,ssid,strlen(ssid));memcpy(config.sta.password,password,strlen(password));
    config.sta.threshold.authmode=password[0]?WIFI_AUTH_WPA2_PSK:WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable=true;
    result=esp_wifi_set_config(WIFI_IF_STA,&config);
    memset(password,0,sizeof(password));memset(&config,0,sizeof(config));
    if(result==ESP_OK) {
        s_connecting=true;result=esp_wifi_connect();
        if(result!=ESP_OK) s_connecting=false;
    }
    return result;
}
esp_err_t network_start(bool join_router)
{
    if(!s_started) {
        load_peers();
        ESP_ERROR_CHECK(esp_netif_init());ESP_ERROR_CHECK(esp_event_loop_create_default());
        s_netif=esp_netif_create_default_wifi_sta();if(!s_netif) return ESP_ERR_NO_MEM;
        wifi_init_config_t config=WIFI_INIT_CONFIG_DEFAULT();ESP_ERROR_CHECK(esp_wifi_init(&config));
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,network_event,NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,network_event,NULL));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));ESP_ERROR_CHECK(esp_wifi_start());
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
        ESP_ERROR_CHECK(esp_wifi_set_channel(s_channel,WIFI_SECOND_CHAN_NONE));s_started=true;
        /* Keyboard builds execute flash code/rodata from PSRAM, including NVS
         * operations here. Keep this background stack out of audio's RAM budget.
         * Receiver builds without that configuration retain an internal stack. */
#if CONFIG_SPIRAM_FETCH_INSTRUCTIONS && CONFIG_SPIRAM_RODATA
        if(xTaskCreatePinnedToCoreWithCaps(retry_task,"net_retry",5120,NULL,3,NULL,0,
            MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS) return ESP_ERR_NO_MEM;
#else
        if(xTaskCreatePinnedToCore(retry_task,"net_retry",5120,NULL,3,NULL,0)!=pdPASS) return ESP_ERR_NO_MEM;
#endif
    }
    if(join_router && !s_join_requested) {
        s_join_requested=true;esp_err_t result=network_retry();
        printf("LX_NETWORK join=%s\n",esp_err_to_name(result));
    }
    return network_control_start();
}
bool network_ready(void) { return s_ready; }
void network_status(uint8_t out[8])
{
    esp_netif_ip_info_t ip={0};if(s_netif) esp_netif_get_ip_info(s_netif,&ip);
    out[0]=s_ready;out[1]=s_channel;out[2]=s_disconnect_reason;out[3]=s_disconnect_reason>>8;
    memcpy(out+4,&ip.ip.addr,4);
}
esp_err_t network_trial(const char *ssid,const char *password)
{
    if(!s_started) return ESP_ERR_INVALID_STATE;
    esp_wifi_disconnect();vTaskDelay(pdMS_TO_TICKS(100));s_ready=false;
    wifi_config_t config={0};memcpy(config.sta.ssid,ssid,strlen(ssid));memcpy(config.sta.password,password,strlen(password));
    config.sta.threshold.authmode=password[0]?WIFI_AUTH_WPA2_PSK:WIFI_AUTH_OPEN;config.sta.pmf_cfg.capable=true;
    esp_err_t rc=esp_wifi_set_config(WIFI_IF_STA,&config);memset(&config,0,sizeof(config));
    if(rc==ESP_OK) rc=esp_wifi_connect();
    if(rc!=ESP_OK) return rc;
    for(unsigned i=0;i<150;i++) { if(s_ready) return ESP_OK;vTaskDelay(pdMS_TO_TICKS(100)); }
    return ESP_ERR_TIMEOUT;
}
void network_trial_end(void)
{
    esp_wifi_disconnect();vTaskDelay(pdMS_TO_TICKS(100));s_ready=false;network_retry();
}
esp_err_t network_power_save(bool enabled)
{
    if(!s_started) return ESP_ERR_INVALID_STATE;
    esp_err_t rc=esp_wifi_set_ps(enabled?WIFI_PS_MIN_MODEM:WIFI_PS_NONE);
    if(rc==ESP_OK) s_power_save=enabled;
    return rc;
}
bool network_power_save_enabled(void) { return s_power_save; }
bool network_target_pc(void) { return false; }
void network_set_discovered_receiver(uint32_t address) { s_discovered_receiver=address; }
uint32_t network_peer_address(void) { return s_discovered_receiver?s_discovered_receiver:s_receiver; }
void network_print(void)
{
    esp_netif_ip_info_t info={0};if(s_netif) esp_netif_get_ip_info(s_netif,&info);
    esp_ip4_addr_t peer={.addr=network_peer_address()};
    printf("LX_NETWORK ready=%u ip=" IPSTR " channel=%u target=%s peer_configured=%u peer=" IPSTR " discovered=%u\n",
        s_ready,IP2STR(&info.ip),s_channel,"receiver",peer.addr!=0,IP2STR(&peer),s_discovered_receiver!=0);
    wifi_ap_record_t ap={0};esp_err_t ap_status=esp_wifi_sta_get_ap_info(&ap);
    printf("LX_NETWORK_ROUTE mask=" IPSTR " gateway=" IPSTR " ap=%02x:%02x:%02x:%02x:%02x:%02x rssi=%d ap_status=%s internal_free=%u internal_largest=%u\n",
        IP2STR(&info.netmask),IP2STR(&info.gw),ap.bssid[0],ap.bssid[1],ap.bssid[2],ap.bssid[3],ap.bssid[4],ap.bssid[5],ap.rssi,
        esp_err_to_name(ap_status),(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    printf("LX_NETWORK_TIME unix=%lld sntp_started=%u\n",(long long)time(NULL),s_sntp_started);
    printf("LX_NETWORK_RETRY enabled=%u connecting=%u retries=%u last_reason=%u radio_active=%u\n",s_auto_retry,s_connecting,s_retry_count,s_disconnect_reason,
        s_radio_seen && (TickType_t)(xTaskGetTickCount()-s_last_radio)<pdMS_TO_TICKS(2000));
}
