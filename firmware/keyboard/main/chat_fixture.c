#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "chat_fixture.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "mbedtls/sha1.h"
#include "mbedtls/base64.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
struct chat_fixture { int server;SemaphoreHandle_t done;unsigned echoed;uint16_t length[10];uint8_t audio[10][1536]; };
static bool transfer(int fd,void *bytes,size_t n,bool writing)
{
    size_t at=0;while(at<n) { int rc=writing?send(fd,(char*)bytes+at,n-at,0):recv(fd,(char*)bytes+at,n-at,0);if(rc<=0) return false;at+=rc; }return true;
}
static bool send_frame(int fd,unsigned opcode,const void *data,size_t n)
{
    uint8_t header[4]={opcode,n<126?n:126,n>>8,n&255};
    return transfer(fd,header,n<126?2:4,true) && transfer(fd,(void*)data,n,true);
}
static bool text_frame(int fd,const char *text) { return send_frame(fd,0x81,text,strlen(text)); }
static void serve(void *arg)
{
    chat_fixture_t *s=arg;int fd=accept(s->server,NULL,NULL);char *buffer=malloc(2049);
    if(fd<0 || !buffer) goto done;
    struct timeval timeout={.tv_sec=3};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    size_t used=0;
    while(used<2048) {
        if(!transfer(fd,buffer+used,1,false)) goto done;
        buffer[++used]=0;if(strstr(buffer,"\r\n\r\n")) break;
    }
    char *key=strstr(buffer,"Sec-WebSocket-Key:");if(!key) goto done;
    key+=18;while(*key==' ') key++;char *end=strstr(key,"\r\n");if(!end || end-key>60) goto done;*end=0;
    char joined[100],accept_key[32];uint8_t digest[20];size_t length=0;
    snprintf(joined,sizeof(joined),"%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11",key);
    mbedtls_sha1((const uint8_t*)joined,strlen(joined),digest);
    mbedtls_base64_encode((uint8_t*)accept_key,sizeof(accept_key)-1,&length,digest,20);accept_key[length]=0;
    used=snprintf(buffer,2049,"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n",accept_key);
    if(!transfer(fd,buffer,used,true)) goto done;
    unsigned captured=0;bool hello=false,detected=false,listening=false,manual=false;
    while(1) {
        uint8_t h[2],mask[4];if(!transfer(fd,h,2,false)) break;
        if(!(h[0]&128) || !(h[1]&128)) break;
        size_t n=h[1]&127;
        if(n==126) { uint8_t size[2];if(!transfer(fd,size,2,false)) break;n=((unsigned)size[0]<<8)|size[1]; }
        else if(n==127) break;
        if(n>1536 || !transfer(fd,mask,4,false) || !transfer(fd,buffer,n,false)) break;
        for(size_t i=0;i<n;i++) buffer[i]^=mask[i%4];
        buffer[n]=0;
        if((h[0]&15)==1 && !hello) {
            if(!strstr(buffer,"\"hello\"")) break;
            /* Force both multi-event payload chunks and continuation frames. */
            strcpy(buffer,"{\"type\":\"stt\",\"text\":\"");size_t start=strlen(buffer);memset(buffer+start,'x',1900);strcpy(buffer+start+1900,"\"}");
            if(!text_frame(fd,buffer)) break;
            const char *reply="{\"type\":\"hello\",\"transport\":\"websocket\",\"session_id\":\"local-fixture\",\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,\"channels\":1,\"frame_duration\":60}}";
            if(!send_frame(fd,0x01,reply,31) || !send_frame(fd,0x80,reply+31,strlen(reply)-31)) break;
            hello=true;
        } else if((h[0]&15)==1 && strstr(buffer,"\"listen\"")) {
            cJSON *j=cJSON_Parse(buffer);
            const cJSON *session=cJSON_GetObjectItemCaseSensitive(j,"session_id");
            const cJSON *type=cJSON_GetObjectItemCaseSensitive(j,"type");
            const cJSON *state=cJSON_GetObjectItemCaseSensitive(j,"state");
            const cJSON *text=cJSON_GetObjectItemCaseSensitive(j,"text");
            const cJSON *mode=cJSON_GetObjectItemCaseSensitive(j,"mode");
            bool valid=cJSON_IsString(session) && !strcmp(session->valuestring,"local-fixture") &&
                cJSON_IsString(type) && !strcmp(type->valuestring,"listen") && cJSON_IsString(state);
            if(valid && !strcmp(state->valuestring,"detect")) {
                valid=!detected && !listening && cJSON_IsString(text) && !strcmp(text->valuestring,"你好小智");
                if(valid) detected=true;
            } else if(valid && !strcmp(state->valuestring,"start")) {
                manual=!detected && cJSON_IsString(mode) && !strcmp(mode->valuestring,"manual");
                valid=!listening && (manual || (detected && cJSON_IsString(mode) && !strcmp(mode->valuestring,"realtime")));
                if(valid) listening=true;
            } else if(valid && !strcmp(state->valuestring,"stop") && manual) {
                valid=listening && captured==12;
                if(valid) {
                    /* An unwanted server reply must never reach the DAC. */
                    text_frame(fd,"{\"type\":\"tts\",\"state\":\"start\"}");
                    send_frame(fd,0x82,"ignored-audio",13);
                    valid=text_frame(fd,"{\"type\":\"stt\",\"text\":\"你好，测试123。\"}");
                    if(valid) { s->echoed=captured;puts("LX_ASR_TEST manual_order=PASS"); }
                    listening=false;
                }
            } else valid=false;
            cJSON_Delete(j);
            if(!valid) { puts("LX_CHAT_TEST wake_order=FAIL");break; }
        } else if((h[0]&15)==2 && manual) {
            if(!listening || !n || ++captured>12) break;
        } else if((h[0]&15)==2 && captured<10) {
            if(!detected || !listening) { puts("LX_CHAT_TEST wake_order=FAIL");break; }
            s->length[captured]=n;memcpy(s->audio[captured],buffer,n);captured++;
            if(captured!=10) continue;
            puts("LX_CHAT_TEST wake_order=PASS");
            if(!text_frame(fd,"{\"type\":\"tts\",\"state\":\"start\"}")) break;
            for(unsigned i=0;i<10;i++) { if(!send_frame(fd,0x82,s->audio[i],s->length[i])) goto done;s->echoed++; }
            if(!text_frame(fd,"{\"type\":\"tts\",\"state\":\"stop\"}") || !text_frame(fd,"{\"type\":\"goodbye\"}")) break;
            /* Remain connected until the client drains queued playback and aborts. */
        } else if((h[0]&15)==1 && strstr(buffer,"\"abort\"")) break;
        else if((h[0]&15)==8) break;
    }
done:
    if(fd>=0) { shutdown(fd,SHUT_RDWR);close(fd); }
    free(buffer);xSemaphoreGive(s->done);vTaskDelete(NULL);
}
chat_fixture_t *chat_fixture_start(void)
{
    if(esp_netif_init()!=ESP_OK) return NULL;
    chat_fixture_t *s=heap_caps_calloc(1,sizeof(*s),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(!s) return NULL;
    s->server=socket(AF_INET,SOCK_STREAM,IPPROTO_IP);s->done=xSemaphoreCreateBinary();
    if(s->server<0 || !s->done) goto fail;
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(39572),.sin_addr={.s_addr=htonl(INADDR_LOOPBACK)}};
    int yes=1;setsockopt(s->server,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
    struct timeval timeout={.tv_sec=3};setsockopt(s->server,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    if(bind(s->server,(struct sockaddr*)&address,sizeof(address)) || listen(s->server,1)) goto fail;
    if(xTaskCreatePinnedToCore(serve,"chat_fixture",4096,s,3,NULL,0)!=pdPASS) goto fail;
    return s;
fail:
    if(s->server>=0) close(s->server);
    if(s->done) vSemaphoreDelete(s->done);
    free(s);return NULL;
}
unsigned chat_fixture_stop(chat_fixture_t *s)
{
    shutdown(s->server,SHUT_RDWR);close(s->server);
    xSemaphoreTake(s->done,portMAX_DELAY);unsigned count=s->echoed;vSemaphoreDelete(s->done);free(s);return count;
}
