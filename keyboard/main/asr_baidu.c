#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "asr_baidu.h"
#include "network_link.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "nvs.h"

/* Fixed Baidu standard Mandarin short-ASR adapter. Endpoint/body definitions
 * checked against Baidu-AIP/speech-demo/rest-api-asr/python/asr_raw.py.
 * Our code uses HTTPS and never prints credentials, tokens or request URLs. */
static char s_token[512],s_key[65],s_secret[65];
static int64_t s_token_until;
typedef struct { char *bytes;size_t used,capacity;bool overflow; } response_t;
static esp_err_t response_event(esp_http_client_event_t *event)
{
    response_t *r=event->user_data;
    if(event->event_id==HTTP_EVENT_ON_DATA && event->data_len>0) {
        if((size_t)event->data_len>=r->capacity-r->used) { r->overflow=true;return ESP_ERR_INVALID_SIZE; }
        memcpy(r->bytes+r->used,event->data,event->data_len);r->used+=event->data_len;r->bytes[r->used]=0;
    }
    return ESP_OK;
}
static esp_err_t post(const char *url,const char *type,const void *body,size_t n,char *response,size_t cap)
{
    response_t r={.bytes=response,.capacity=cap};response[0]=0;
    esp_http_client_config_t config={.url=url,.timeout_ms=10000,.event_handler=response_event,
        .user_data=&r,.crt_bundle_attach=esp_crt_bundle_attach,.disable_auto_redirect=true};
    esp_http_client_handle_t client=esp_http_client_init(&config);if(!client) return ESP_ERR_NO_MEM;
    esp_http_client_set_method(client,HTTP_METHOD_POST);
    esp_http_client_set_header(client,"Content-Type",type);
    esp_http_client_set_post_field(client,body,n);
    esp_err_t result=esp_http_client_perform(client);
    int status=esp_http_client_get_status_code(client);esp_http_client_cleanup(client);
    if(result==ESP_OK && (status!=200 || r.overflow)) result=ESP_ERR_INVALID_RESPONSE;
    return result;
}
static bool credentials(char key[65],char secret[65])
{
    nvs_handle_t h;key[0]=secret[0]=0;
    if(nvs_open("lx_asr",NVS_READONLY,&h)!=ESP_OK) return false;
    size_t n=65;esp_err_t a=nvs_get_str(h,"key",key,&n);n=65;esp_err_t b=nvs_get_str(h,"secret",secret,&n);
    nvs_close(h);return a==ESP_OK && b==ESP_OK && key[0] && secret[0];
}
bool asr_baidu_configured(void)
{
    char key[65],secret[65];bool ready=credentials(key,secret);memset(secret,0,sizeof(secret));return ready;
}
esp_err_t asr_baidu_setting(uint8_t operation,const uint8_t *bytes,size_t n)
{
    if((operation!=8 && operation!=9) || n>64 || memchr(bytes,0,n)) return ESP_ERR_INVALID_ARG;
    for(size_t i=0;i<n;i++) if(bytes[i]<33 || bytes[i]>126) return ESP_ERR_INVALID_ARG;
    char value[65];memcpy(value,bytes,n);value[n]=0;
    nvs_handle_t h;esp_err_t result=nvs_open("lx_asr",NVS_READWRITE,&h);
    if(result==ESP_OK) {
        result=nvs_set_str(h,operation==8?"key":"secret",value);
        if(result==ESP_OK) result=nvs_commit(h);
        nvs_close(h);
    }
    memset(value,0,sizeof(value));return result;
}
static bool escape(const char *input,char *output,size_t capacity)
{
    const char hex[]="0123456789ABCDEF";size_t at=0;
    for(const unsigned char *p=(const unsigned char*)input;*p;p++) {
        bool plain=(*p>='a' && *p<='z') || (*p>='A' && *p<='Z') || (*p>='0' && *p<='9') || strchr("-._~",*p);
        if(at+(plain?1:3)>=capacity) return false;
        if(plain) output[at++]=*p;
        else { output[at++]='%';output[at++]=hex[*p>>4];output[at++]=hex[*p&15]; }
    }
    output[at]=0;return true;
}
static esp_err_t token(char *response,size_t cap)
{
    char key[65],secret[65];if(!credentials(key,secret)) return ESP_ERR_NOT_FOUND;
    if(!strcmp(key,s_key) && !strcmp(secret,s_secret) && s_token[0] && esp_timer_get_time()<s_token_until) return ESP_OK;
    s_token[0]=0;
    char escaped_key[193],escaped_secret[193],body[480];
    escape(key,escaped_key,sizeof(escaped_key));escape(secret,escaped_secret,sizeof(escaped_secret));
    snprintf(body,sizeof(body),"grant_type=client_credentials&client_id=%s&client_secret=%s",escaped_key,escaped_secret);
    esp_err_t result=post("https://aip.baidubce.com/oauth/2.0/token","application/x-www-form-urlencoded",body,strlen(body),response,cap);
    memset(body,0,sizeof(body));memset(escaped_secret,0,sizeof(escaped_secret));
    if(result==ESP_OK) {
        cJSON *json=cJSON_Parse(response);
        const cJSON *value=cJSON_GetObjectItemCaseSensitive(json,"access_token"),*expiry=cJSON_GetObjectItemCaseSensitive(json,"expires_in");
        if(cJSON_IsString(value) && value->valuestring[0] && strlen(value->valuestring)<sizeof(s_token) && cJSON_IsNumber(expiry) && expiry->valuedouble>60) {
            strcpy(s_token,value->valuestring);strcpy(s_key,key);strcpy(s_secret,secret);
            double seconds=expiry->valuedouble;if(seconds>2592000) seconds=2592000;
            s_token_until=esp_timer_get_time()+(int64_t)(seconds-60)*1000000;
        } else result=ESP_ERR_INVALID_RESPONSE;
        cJSON_Delete(json);
    }
    memset(secret,0,sizeof(secret));return result;
}
static esp_err_t transcript(const char *response,char *output,size_t capacity,int *service_code)
{
    if(!capacity) return ESP_ERR_INVALID_SIZE;
    output[0]=0;*service_code=-1;
    cJSON *json=cJSON_Parse(response);if(!json) return ESP_ERR_INVALID_RESPONSE;
    const cJSON *code=cJSON_GetObjectItemCaseSensitive(json,"err_no"),*list=cJSON_GetObjectItemCaseSensitive(json,"result");
    esp_err_t result=ESP_ERR_INVALID_RESPONSE;
    if(cJSON_IsNumber(code)) *service_code=code->valueint;
    if(*service_code==0 && cJSON_IsArray(list)) {
        size_t used=0;const cJSON *entry;
        result=ESP_OK;
        cJSON_ArrayForEach(entry,list) {
            if(!cJSON_IsString(entry) || strlen(entry->valuestring)>=capacity-used) { result=ESP_ERR_INVALID_SIZE;break; }
            size_t n=strlen(entry->valuestring);memcpy(output+used,entry->valuestring,n);used+=n;output[used]=0;
        }
        if(result==ESP_OK && !used) result=ESP_ERR_INVALID_RESPONSE;
    }
    if(result!=ESP_OK) output[0]=0;
    cJSON_Delete(json);return result;
}
esp_err_t asr_baidu_recognize(const int16_t *pcm,size_t samples,char *text,size_t capacity)
{
    if(!pcm || samples<3200 || samples>320000 || !capacity) return ESP_ERR_INVALID_ARG;
    text[0]=0;
    if(!asr_baidu_configured()) return ESP_ERR_NOT_FOUND;
    if(!network_ready() || time(NULL)<1700000000) return ESP_ERR_INVALID_STATE;
    char *response=heap_caps_malloc(4096,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!response) return ESP_ERR_NO_MEM;
    esp_err_t result=token(response,4096);
    if(result==ESP_OK) {
        char escaped_token[1536],url[1700];uint8_t mac[6];esp_read_mac(mac,ESP_MAC_WIFI_STA);
        if(!escape(s_token,escaped_token,sizeof(escaped_token))) result=ESP_ERR_INVALID_SIZE;
        else {
            snprintf(url,sizeof(url),"https://vop.baidu.com/server_api?dev_pid=1537&cuid=lx%02x%02x%02x%02x%02x%02x&token=%s",
                mac[0],mac[1],mac[2],mac[3],mac[4],mac[5],escaped_token);
            result=post(url,"audio/pcm; rate=16000",pcm,samples*sizeof(*pcm),response,4096);
            memset(url,0,sizeof(url));memset(escaped_token,0,sizeof(escaped_token));
            if(result==ESP_OK) {
                int code;result=transcript(response,text,capacity,&code);
                if(code==3302) s_token[0]=0;
                printf("LX_ASR service_code=%d result=%s\n",code,esp_err_to_name(result));
            }
        }
    }
    memset(response,0,4096);free(response);return result;
}
unsigned asr_baidu_selftest(void)
{
    char value[32];int code;unsigned errors=0;
    if(transcript("{\"err_no\":0,\"result\":[\"你好\"]}",value,sizeof(value),&code)!=ESP_OK || strcmp(value,"你好")) errors++;
    if(transcript("{\"err_no\":3301}",value,sizeof(value),&code)==ESP_OK || value[0]) errors++;
    if(transcript("{\"err_no\":0,\"result\":[\"123456\"]}",value,4,&code)!=ESP_ERR_INVALID_SIZE || value[0]) errors++;
    if(transcript("not JSON",value,sizeof(value),&code)==ESP_OK) errors++;
    if(!escape("a+b /",value,sizeof(value)) || strcmp(value,"a%2Bb%20%2F")) errors++;
    return errors;
}
