#include "esp_idf_version.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "chat_official.h"
#include "chat_client.h"
#include "activation_audio.h"
#include "activation_ui.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network_link.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_chip_info.h"
#include "esp_netif_sntp.h"
#include "cJSON.h"

/* Public 78/xiaozhi-esp32 OTA bootstrap protocol, Activation-Version 1.
 * Only bind and obtain service settings; firmware/assets fields are ignored. */
#define OFFICIAL_URL "https://api.tenclass.net/xiaozhi/ota/"
#define RESPONSE_CAP 8192
typedef struct { char *data;size_t used;bool overflow; } response_t;
typedef struct { char code[7];bool pending,demo;const char *uri,*token;unsigned version; } bootstrap_t;

static esp_err_t parse(const cJSON *root,bootstrap_t *out)
{
    memset(out,0,sizeof(*out));out->version=1;
    if(!cJSON_IsObject(root)) return ESP_ERR_INVALID_RESPONSE;
    const cJSON *activation=cJSON_GetObjectItemCaseSensitive(root,"activation");
    const cJSON *code=cJSON_GetObjectItemCaseSensitive(activation,"code");
    const cJSON *challenge=cJSON_GetObjectItemCaseSensitive(activation,"challenge");
    if(cJSON_IsString(code) && code->valuestring[0]) {
        size_t n=strlen(code->valuestring);
        if(n!=6) return ESP_ERR_INVALID_SIZE;
        for(size_t i=0;i<n;i++) if(code->valuestring[i]<'0' || code->valuestring[i]>'9') return ESP_ERR_INVALID_RESPONSE;
        memcpy(out->code,code->valuestring,n+1);out->pending=true;
    }
    if(cJSON_IsString(challenge) && challenge->valuestring[0]) out->pending=true;
    /* Provisioning may include an unbound websocket; don't mark it ready yet. */
    if(out->pending) return ESP_OK;
    const cJSON *ws=cJSON_GetObjectItemCaseSensitive(root,"websocket");
    const cJSON *uri=cJSON_GetObjectItemCaseSensitive(ws,"url");
    const cJSON *token=cJSON_GetObjectItemCaseSensitive(ws,"token");
    const cJSON *mqtt=cJSON_GetObjectItemCaseSensitive(root,"mqtt");
    const cJSON *client=cJSON_GetObjectItemCaseSensitive(mqtt,"client_id");
    out->demo=(cJSON_IsString(token) && !strcmp(token->valuestring,"test-token")) ||
        (cJSON_IsString(client) && !strncmp(client->valuestring,"GID_test",8));
    const cJSON *version=cJSON_GetObjectItemCaseSensitive(ws,"version");
    if(version) {
        if(!cJSON_IsNumber(version) || version->valuedouble!=version->valueint || version->valueint<1 || version->valueint>3) return ESP_ERR_NOT_SUPPORTED;
        out->version=version->valueint;
    }
    if(!cJSON_IsString(uri) || strncmp(uri->valuestring,"wss://",6)) return ESP_ERR_NOT_SUPPORTED;
    if(token && !cJSON_IsString(token)) return ESP_ERR_INVALID_RESPONSE;
    out->uri=uri->valuestring;out->token=token?token->valuestring:"";
    if(strlen(out->uri)>=CHAT_URI_CAP || strlen(out->token)>=CHAT_TOKEN_CAP) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}
static esp_err_t event(esp_http_client_event_t *e)
{
    response_t *r=e->user_data;
    if(e->event_id==HTTP_EVENT_ON_DATA && e->data_len>0) {
        if((size_t)e->data_len>=RESPONSE_CAP-r->used) { r->overflow=true;return ESP_ERR_INVALID_SIZE; }
        memcpy(r->data+r->used,e->data,e->data_len);r->used+=e->data_len;r->data[r->used]=0;
    }
    return ESP_OK;
}
static esp_err_t post(bool activate,bool espressif,const char *uuid,const char *mac,char *response,int *status)
{
    response_t r={.data=response};response[0]=0;
    esp_http_client_config_t config={.url=activate?OFFICIAL_URL "activate":OFFICIAL_URL,
        .timeout_ms=12000,.event_handler=event,.user_data=&r,
        .crt_bundle_attach=esp_crt_bundle_attach,.disable_auto_redirect=true};
    esp_http_client_handle_t http=esp_http_client_init(&config);if(!http) return ESP_ERR_NO_MEM;
    char body[768];esp_chip_info_t chip;esp_chip_info(&chip);
    if(activate) strcpy(body,"{}");
    else snprintf(body,sizeof(body),
        "{\"version\":2,\"language\":\"zh-CN\",\"mac_address\":\"%s\",\"uuid\":\"%s\","
        "\"chip_model_name\":\"esp32s3\",\"flash_size\":16777216,"
        "\"chip_info\":{\"model\":%d,\"cores\":%d,\"revision\":%d,\"features\":%lu},"
        "\"application\":{\"name\":\"lx_keyboard\",\"version\":\"1.0.0\",\"idf_version\":\"" IDF_VER "\"},"
        "\"board\":{\"type\":\"lx-keyboard\",\"name\":\"lx-keyboard\"}}",
        mac,uuid,chip.model,chip.cores,chip.revision,(unsigned long)chip.features);
    esp_http_client_set_method(http,HTTP_METHOD_POST);
    esp_http_client_set_header(http,"Content-Type","application/json");
    /* Espressif esp_xiaozhi omits Activation-Version on bootstrap and uses
     * the chip target as User-Agent. Preserve our actual MAC/UUID/board data. */
    if(!espressif || activate) esp_http_client_set_header(http,"Activation-Version","1");
    esp_http_client_set_header(http,"Device-Id",mac);
    esp_http_client_set_header(http,"Client-Id",uuid);
    esp_http_client_set_header(http,"User-Agent",espressif?"esp32s3":"lx-keyboard/1.0.0");
    esp_http_client_set_header(http,"Accept-Language","zh-CN");
    esp_http_client_set_post_field(http,body,strlen(body));
    esp_err_t rc=esp_http_client_perform(http);*status=esp_http_client_get_status_code(http);
    esp_http_client_cleanup(http);
    if(r.overflow) rc=ESP_ERR_INVALID_SIZE;
    return rc;
}
esp_err_t chat_official_setup(bool activate,bool (*cancelled)(void))
{
    if(!network_ready()) { puts("LX_CHAT_OFFICIAL waiting=wifi");return ESP_ERR_INVALID_STATE; }
    if(time(NULL)<1700000000) {
        esp_netif_sntp_start();
        esp_netif_sntp_sync_wait(pdMS_TO_TICKS(8000));
        if(time(NULL)<1700000000) { puts("LX_CHAT_OFFICIAL waiting=time_sync");return ESP_ERR_INVALID_STATE; }
    }
    char uuid[37],mac[18];uint8_t bytes[6];
    esp_err_t rc=chat_client_id(uuid);if(rc!=ESP_OK) return rc;
    esp_read_mac(bytes,ESP_MAC_WIFI_STA);
    snprintf(mac,sizeof(mac),"%02x:%02x:%02x:%02x:%02x:%02x",bytes[0],bytes[1],bytes[2],bytes[3],bytes[4],bytes[5]);
    char *response=heap_caps_calloc(1,RESPONSE_CAP,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!response) return ESP_ERR_NO_MEM;
    int status=0;char code[7]={0};bool espressif=false;int64_t until=esp_timer_get_time()+120000000;
retry:
    if(cancelled()) { rc=ESP_ERR_INVALID_STATE;goto done; }
    if(activate) {
        rc=post(true,espressif,uuid,mac,response,&status);
        printf("LX_CHAT_OFFICIAL activation_http=%d result=%s\n",status,esp_err_to_name(rc));
        if(rc!=ESP_OK) goto done;
        if(status==202) { puts("LX_CHAT_OFFICIAL state=pending");goto pending; }
        if(status!=200) { rc=ESP_ERR_INVALID_RESPONSE;goto done; }
    }
    memset(response,0,RESPONSE_CAP);
    rc=post(false,espressif,uuid,mac,response,&status);
    printf("LX_CHAT_OFFICIAL bootstrap_http=%d result=%s headers=%s\n",status,esp_err_to_name(rc),espressif?"espressif":"activation_v1");
    if(rc==ESP_OK && status!=200) rc=ESP_ERR_INVALID_RESPONSE;
    if(rc==ESP_OK) {
        cJSON *root=cJSON_Parse(response);bootstrap_t parsed;
        const cJSON *activation=cJSON_GetObjectItemCaseSensitive(root,"activation");
        const cJSON *mq=cJSON_GetObjectItemCaseSensitive(root,"mqtt");
        const cJSON *mc=cJSON_GetObjectItemCaseSensitive(mq,"client_id");
        const cJSON *wt=cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root,"websocket"),"token");
        printf("LX_CHAT_OFFICIAL mqtt_object=%u mqtt_test=%u websocket_test=%u\n",cJSON_IsObject(mq),
            cJSON_IsString(mc) && !strncmp(mc->valuestring,"GID_test",8),cJSON_IsString(wt) && !strcmp(wt->valuestring,"test-token"));
        printf("LX_CHAT_OFFICIAL activation_object=%u websocket_object=%u\n",cJSON_IsObject(activation),
            cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(root,"websocket")));
        rc=parse(root,&parsed);
        if(rc==ESP_OK && parsed.pending) {
            printf("LX_CHAT_OFFICIAL state=pending code=%s\n",parsed.code);
            memcpy(code,parsed.code,sizeof(code));
            activation_ui_code(code);
            cJSON_Delete(root);activate=true;goto pending;
        } else if(rc==ESP_OK) {
            /* Official bootstrap can retain these markers after web binding.
             * They do not prove account ownership or invalidate a WSS profile.
             * A pending activation above must still never install credentials. */
            rc=cancelled()?ESP_ERR_INVALID_STATE:chat_store_profile(parsed.uri,parsed.token,parsed.version);
            if(rc==ESP_OK) printf("LX_CHAT_OFFICIAL state=ready protocol=%u credentials=saved_on_device test_marker=%u account_binding=unconfirmed connection=unverified\n",parsed.version,parsed.demo);
        }
        cJSON_Delete(root);
    }
    goto done;
pending:
    if(esp_timer_get_time()>=until) { rc=ESP_ERR_TIMEOUT;goto done; }
    if(code[0]) {
        esp_err_t audio=activation_audio_play(code,cancelled);
        printf("LX_CHAT_OFFICIAL code_audio=%s\n",esp_err_to_name(audio));
    }
    /* Keep typing independent; only the low-priority voice worker waits. */
    for(unsigned i=0;i<30 && !cancelled();i++) vTaskDelay(pdMS_TO_TICKS(100));
    goto retry;
done:
    memset(response,0,RESPONSE_CAP);free(response);return rc;
}
unsigned chat_official_selftest(void)
{
    const char *cases[]={
        "{\"activation\":{\"code\":\"123456\",\"challenge\":\"test\"},\"websocket\":{\"url\":\"wss://test\"}}",
        "{\"websocket\":{\"url\":\"wss://test\",\"token\":\"test\"},\"firmware\":{\"url\":\"ignored\"}}",
        "{\"websocket\":{\"url\":\"ws://test\"}}", "[]",
        "{\"activation\":{\"code\":\"bad\\ncode\"}}",
        "{\"websocket\":{\"url\":\"wss://test\",\"token\":3}}"};
    unsigned errors=0;
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        cJSON *root=cJSON_Parse(cases[i]);bootstrap_t p;esp_err_t rc=parse(root,&p);
        if(i<2 ? rc!=ESP_OK : rc==ESP_OK) errors++;
        if(i==0 && (!p.pending || strcmp(p.code,"123456") || p.uri)) errors++;
        if(i==1 && (p.pending || !p.uri || strcmp(p.token,"test"))) errors++;
        cJSON_Delete(root);
    }
    for(unsigned v=0;v<=4;v++) {
        char json[96];snprintf(json,sizeof(json),"{\"websocket\":{\"url\":\"wss://test\",\"version\":%u}}",v);
        cJSON *root=cJSON_Parse(json);bootstrap_t p;esp_err_t rc=parse(root,&p);
        if(v>=1 && v<=3) { if(rc!=ESP_OK || p.version!=v) errors++; }
        else if(rc==ESP_OK) errors++;
        cJSON_Delete(root);
    }
    const char *extra[]={
        "{\"websocket\":{\"url\":\"wss://test\",\"token\":\"test-token\"}}",
        "{\"websocket\":{\"url\":\"wss://test\"},\"mqtt\":{\"client_id\":\"GID_test@@@example\"}}",
        "{\"activation\":{\"code\":\"12345\"}}",
        "{\"activation\":{\"code\":\"1234567\"}}"};
    for(unsigned i=0;i<4;i++) {
        cJSON *root=cJSON_Parse(extra[i]);bootstrap_t p;esp_err_t rc=parse(root,&p);
        if(i<2 ? (rc!=ESP_OK || !p.demo) : rc==ESP_OK) errors++;
        cJSON_Delete(root);
    }
    /* Marked profiles are usable settings, but activation always takes precedence. */
    cJSON *root=cJSON_Parse("{\"activation\":{\"code\":\"123456\"},\"websocket\":{\"url\":\"wss://test\",\"token\":\"test-token\"}}");
    bootstrap_t p;esp_err_t rc=parse(root,&p);
    if(rc!=ESP_OK || !p.pending || p.uri || strcmp(p.code,"123456")) errors++;
    cJSON_Delete(root);
    return errors;
}
