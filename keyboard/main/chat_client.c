#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "chat_client.h"
#include "chat_protocol.h"
#include "chat_fixture.h"
#include "chat_uplink.h"
#include "voice_opus.h"
#include "voice_duplex.h"
#include "board.h"
#include "network_link.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define PACKET_MAX 1536
#define RX_PACKETS 40
typedef struct { uint16_t n;uint8_t kind;uint8_t bytes[PACKET_MAX]; } packet_t;
typedef struct {
    chat_rx_t rx;
    QueueHandle_t queue;
    StaticQueue_t queue_control;
    uint8_t queue_bytes[RX_PACKETS*sizeof(packet_t)];
    volatile bool connected,failed;
    bool dictation;
    volatile bool accept_stt;
    unsigned version;
} session_t;
typedef struct {
    const int16_t *pcm;size_t samples;char *text;size_t capacity;
    chat_pcm_reader_t read;void *context;bool (*submit)(void *,const char *);
} dictation_t;
static struct { unsigned sessions,tx,rx,errors,overflows,encode_us,decode_us;esp_err_t last;bool active; } s_stats;
static bool profile(char uri[CHAT_URI_CAP],char token[CHAT_TOKEN_CAP],uint8_t *version)
{
    uri[0]=token[0]=0;nvs_handle_t h;
    if(nvs_open("lx_chat",NVS_READONLY,&h)!=ESP_OK) return false;
    size_t n=CHAT_URI_CAP;esp_err_t rc=nvs_get_str(h,"uri",uri,&n);n=CHAT_TOKEN_CAP;
    esp_err_t trc=nvs_get_str(h,"token",token,&n);
    uint8_t v=1;esp_err_t vrc=nvs_get_u8(h,"version",&v);nvs_close(h);
    if((vrc!=ESP_OK && vrc!=ESP_ERR_NVS_NOT_FOUND) || v<1 || v>3) return false;
    if(version) *version=v;
    if(trc!=ESP_OK && trc!=ESP_ERR_NVS_NOT_FOUND) return false;
    return rc==ESP_OK && uri[0];
}
bool chat_configured(void) { char uri[CHAT_URI_CAP],token[CHAT_TOKEN_CAP];bool ok=profile(uri,token,NULL);memset(token,0,sizeof(token));return ok; }
static bool cloud_enabled(void)
{
    nvs_handle_t h;uint8_t enabled=0;
    if(nvs_open("lx_chat",NVS_READONLY,&h)==ESP_OK) { nvs_get_u8(h,"enabled",&enabled);nvs_close(h); }
    return enabled==1;
}
esp_err_t chat_cloud_enable(bool enabled)
{
    nvs_handle_t h;esp_err_t rc=nvs_open("lx_chat",NVS_READWRITE,&h);if(rc!=ESP_OK) return rc;
    rc=nvs_set_u8(h,"enabled",enabled?1:0);if(rc==ESP_OK) rc=nvs_commit(h);nvs_close(h);return rc;
}
esp_err_t chat_store_profile(const char *uri,const char *token,unsigned version)
{
    if(token && !strncmp(token,"Bearer ",7)) token+=7;
    if(version<1 || version>3 || !uri || !token || strncmp(uri,"wss://",6) || strlen(uri)>=CHAT_URI_CAP || strlen(token)>=CHAT_TOKEN_CAP) return ESP_ERR_INVALID_ARG;
    for(const char *p=uri;*p;p++) if((unsigned char)*p<33 || (unsigned char)*p>126) return ESP_ERR_INVALID_ARG;
    for(const char *p=token;*p;p++) if((unsigned char)*p<33 || (unsigned char)*p>126) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;esp_err_t rc=nvs_open("lx_chat",NVS_READWRITE,&h);
    if(rc!=ESP_OK) return rc;
    rc=nvs_set_str(h,"uri",uri);
    if(rc==ESP_OK) rc=nvs_set_str(h,"token",token);
    if(rc==ESP_OK) rc=nvs_set_u8(h,"version",version);
    if(rc==ESP_OK) rc=nvs_commit(h);
    nvs_close(h);return rc;
}
esp_err_t chat_setting(uint8_t op,const uint8_t *data,size_t n)
{
    if((op!=10 && op!=11) || n>233) return ESP_ERR_INVALID_ARG;
    for(size_t i=0;i<n;i++) if(data[i]<33 || data[i]>126) return ESP_ERR_INVALID_ARG;
    char value[234];memcpy(value,data,n);value[n]=0;
    if(op==10 && n && strncmp(value,"wss://",6) && strncmp(value,"ws://",5)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;esp_err_t rc=nvs_open("lx_chat",NVS_READWRITE,&h);
    if(rc==ESP_OK) {
        rc=nvs_set_str(h,op==10?"uri":"token",value);
        if(rc==ESP_OK && op==10) rc=nvs_set_u8(h,"version",1);
        if(rc==ESP_OK) rc=nvs_commit(h);
        nvs_close(h);
    }
    memset(value,0,sizeof(value));return rc;
}
esp_err_t chat_client_id(char uuid[37])
{
    nvs_handle_t h;esp_err_t rc=nvs_open("lx_chat",NVS_READWRITE,&h);if(rc!=ESP_OK) return rc;
    size_t n=37;
    if(nvs_get_str(h,"client",uuid,&n)!=ESP_OK || n!=37) {
        uint8_t b[16];esp_fill_random(b,sizeof(b));b[6]=(b[6]&15)|64;b[8]=(b[8]&63)|128;
        snprintf(uuid,37,"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
        rc=nvs_set_str(h,"client",uuid);if(rc==ESP_OK) rc=nvs_commit(h);
    }
    nvs_close(h);return rc;
}
static void event(void *arg,esp_event_base_t base,int32_t id,void *value)
{
    (void)base;session_t *s=arg;esp_websocket_event_data_t *e=value;
    if(id==WEBSOCKET_EVENT_CONNECTED) { s->connected=true;return; }
    if(id==WEBSOCKET_EVENT_ERROR || id==WEBSOCKET_EVENT_DISCONNECTED || id==WEBSOCKET_EVENT_CLOSED) {
        printf("LX_CHAT_SOCKET event=%ld\n",(long)id);s->failed=true;return;
    }
    if(id!=WEBSOCKET_EVENT_DATA || s->failed) return;
    if(e->payload_len<0 || e->payload_offset<0 || e->data_len<0) { s->failed=true;return; }
    int complete=chat_rx_feed(&s->rx,e->op_code,e->fin,e->payload_len,e->payload_offset,e->data_ptr,e->data_len);
    if(complete<0) { s->failed=true;return; }
    if(!complete) return;
    packet_t packet={0};
    if(s->rx.opcode==2) {
        if(s->dictation) return; /* Never decode or play dictation replies. */
        const uint8_t *audio;size_t n;
        if(!chat_audio_unwrap(s->version,s->rx.bytes,s->rx.used,&audio,&n) || n>PACKET_MAX) { s->failed=true;return; }
        packet.kind=128;packet.n=n;memcpy(packet.bytes,audio,n);
    } else {
        chat_message_t m=chat_json((const char*)s->rx.bytes,s->rx.used);
        if(m.kind==CHAT_INVALID) { s->failed=true;return; }
        if(m.kind==CHAT_IGNORE) {
            /* Report protocol notices without logging recognized user speech. */
            cJSON *j=cJSON_ParseWithLength((const char*)s->rx.bytes,s->rx.used);
            const cJSON *type=cJSON_GetObjectItemCaseSensitive(j,"type");
            const cJSON *state=cJSON_GetObjectItemCaseSensitive(j,"state");
            const cJSON *text=cJSON_GetObjectItemCaseSensitive(j,"text");
            if(cJSON_IsString(type)) {
                printf("LX_CHAT_NOTICE type=%.32s state=%.32s text_bytes=%u\n",type->valuestring,
                    cJSON_IsString(state)?state->valuestring:"",cJSON_IsString(text)?(unsigned)strlen(text->valuestring):0);
                if(!strcmp(type->valuestring,"alert") || !strcmp(type->valuestring,"activation") || !strcmp(type->valuestring,"error")) {
                    const cJSON *message=cJSON_GetObjectItemCaseSensitive(j,"message");
                    if(cJSON_IsString(message)) printf("LX_CHAT_SERVICE_MESSAGE %.400s\n",message->valuestring);
                }
            }
            cJSON_Delete(j);return;
        }
        packet.kind=m.kind;
        if(m.kind==CHAT_HELLO) { packet.n=sizeof(m);memcpy(packet.bytes,&m,sizeof(m)); }
        if(s->dictation) {
            if(m.kind==CHAT_STT) {
                if(!s->accept_stt) return;
                int n=chat_stt_text((const char*)s->rx.bytes,s->rx.used,(char*)packet.bytes,1025);
                if(n<0) { s->failed=true;return; }
                if(!n) return;
                packet.n=n;
            } else if(m.kind==CHAT_TTS_START || m.kind==CHAT_TTS_STOP) return;
        }
    }
    if(xQueueSend(s->queue,&packet,0)!=pdTRUE) { s_stats.overflows++;s->failed=true; }
}
static esp_err_t send_text(esp_websocket_client_handle_t ws,const char *text)
{
    size_t n=strlen(text);return esp_websocket_client_send_text(ws,text,n,pdMS_TO_TICKS(1000))==(int)n?ESP_OK:ESP_FAIL;
}
static esp_err_t control_mode(esp_websocket_client_handle_t ws,const char *session,const char *type,const char *state,const char *mode)
{
    cJSON *j=cJSON_CreateObject();if(!j) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(j,"session_id",session);cJSON_AddStringToObject(j,"type",type);
    if(state) cJSON_AddStringToObject(j,"state",state);
    if(state && !strcmp(state,"detect")) cJSON_AddStringToObject(j,"text","你好小智");
    if(state && !strcmp(state,"start")) cJSON_AddStringToObject(j,"mode",mode);
    char *text=cJSON_PrintUnformatted(j);esp_err_t rc=text?send_text(ws,text):ESP_ERR_NO_MEM;
    free(text);cJSON_Delete(j);return rc;
}
static esp_err_t control(esp_websocket_client_handle_t ws,const char *session,const char *type,const char *state)
{
    return control_mode(ws,session,type,state,"realtime");
}
static esp_err_t dictate(esp_websocket_client_handle_t ws,session_t *s,const char *session,
    voice_opus_t *codec,int16_t *pcm,packet_t *packet,dictation_t *d,bool (*cancelled)(void))
{
    esp_err_t rc=control_mode(ws,session,"listen","start","manual");
    if(rc!=ESP_OK) return rc;
    /* Live capture runs independently during TLS setup. The reader blocks for
     * new samples; an initial backlog drains without replaying real-time delays. */
    size_t frames=0,offset=0;bool flushed=false;
    TickType_t tick=xTaskGetTickCount();
    while(!flushed) {
        if(cancelled()) return ESP_ERR_INVALID_STATE;
        if(s->failed) return ESP_FAIL;
        size_t count=0;
        memset(pcm,0,960*sizeof(*pcm));
        if(d->read) {
            rc=d->read(d->context,pcm,&count);if(rc!=ESP_OK) return rc;
            if(count>960) return ESP_ERR_INVALID_SIZE;
        } else {
            count=offset<d->samples?d->samples-offset:0;
            if(count>960) count=960;
            if(count) memcpy(pcm,d->pcm+offset,count*sizeof(*pcm));
        }
        offset+=count;
        if(!count) flushed=true; /* One silence frame flushes the Opus tail. */
        size_t h=chat_audio_header(s->version),n=0;
        rc=voice_opus_encode(codec,pcm,960,packet->bytes+h,PACKET_MAX-h,&n);
        if(rc!=ESP_OK) return rc;
        if(!chat_audio_wrap(s->version,packet->bytes,n,(uint32_t)(frames*60))) return ESP_ERR_INVALID_SIZE;
        n+=h;
        if(esp_websocket_client_send_bin(ws,(const char*)packet->bytes,n,pdMS_TO_TICKS(300))!=(int)n) return ESP_FAIL;
        s_stats.tx++;frames++;
        if(!d->read) vTaskDelayUntil(&tick,pdMS_TO_TICKS(60));
        else vTaskDelay(1);
    }
    if(cancelled()) return ESP_ERR_INVALID_STATE;
    s->accept_stt=true;
    int64_t stopped=esp_timer_get_time();
    rc=control(ws,session,"listen","stop");if(rc!=ESP_OK) return rc;
    puts("LX_ASR_STOP sent=1");
    int64_t until=esp_timer_get_time()+15000000;
    while(!cancelled() && esp_timer_get_time()<until) {
        if(xQueueReceive(s->queue,packet,pdMS_TO_TICKS(20))==pdTRUE) {
            if(packet->kind==CHAT_STT) {
                if(!packet->n || packet->n>=d->capacity) return ESP_ERR_INVALID_SIZE;
                memcpy(d->text,packet->bytes,packet->n);d->text[packet->n]=0;
                if(cancelled()) return ESP_ERR_INVALID_STATE;
                printf("LX_ASR_TIMING stop_to_stt_ms=%lld\n",(esp_timer_get_time()-stopped)/1000);
                if(d->submit) d->submit(d->context,d->text);
                printf("LX_ASR_XIAOZHI recognized_bytes=%u frames=%u playback=0\n",packet->n,(unsigned)frames);
                return ESP_OK;
            }
            if(packet->kind==CHAT_GOODBYE) return ESP_ERR_NOT_FOUND;
        } else if(s->failed) return ESP_FAIL;
    }
    return cancelled()?ESP_ERR_INVALID_STATE:ESP_ERR_TIMEOUT;
}
static esp_err_t run(const char *uri,const char *token,unsigned version,bool (*cancelled)(void),bool probe,chat_capture_t *diagnostic,dictation_t *dictation,unsigned test_stall_ms)
{
    char uuid[37],headers[CHAT_TOKEN_CAP+220];
    esp_err_t rc=chat_client_id(uuid);if(rc!=ESP_OK) return rc;
    uint8_t mac[6];esp_read_mac(mac,ESP_MAC_WIFI_STA);
    snprintf(headers,sizeof(headers),"Protocol-Version: %u\r\nDevice-Id: %02x:%02x:%02x:%02x:%02x:%02x\r\nClient-Id: %s\r\n%s%s%s",
        version,mac[0],mac[1],mac[2],mac[3],mac[4],mac[5],uuid,token[0]?"Authorization: Bearer ":"",token,token[0]?"\r\n":"");
    session_t *s=heap_caps_calloc(1,sizeof(*s),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!s) return ESP_ERR_NO_MEM;
    s->version=version;s->dictation=dictation!=NULL;
    s->queue=xQueueCreateStatic(RX_PACKETS,sizeof(packet_t),s->queue_bytes,&s->queue_control);
    voice_opus_t codec={0};esp_websocket_client_handle_t ws=NULL;
    int16_t *pcm=heap_caps_malloc((960+1920)*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    int16_t *decoded=pcm?pcm+960:NULL;
    packet_t *packet=heap_caps_malloc(sizeof(*packet),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    voice_duplex_t *duplex=NULL;chat_uplink_t *uplink=NULL;char session[97]={0};
    const char *reason="setup";
    unsigned tx_playing=0,interruptions=0;
    unsigned peak=0;uint64_t energy=0,samples_sent=0;
    if(!s->queue || !pcm || !packet) { rc=ESP_ERR_NO_MEM;goto done; }
    rc=voice_opus_open(&codec,16000);if(rc!=ESP_OK) goto done;
    esp_websocket_client_config_t cfg={.uri=uri,.headers=headers,.crt_bundle_attach=esp_crt_bundle_attach,
        .disable_auto_reconnect=true,.network_timeout_ms=5000,.task_prio=4,.task_stack=6144,
        .task_core_id_set=true,.task_core_id=0,.buffer_size=1024};
    ws=esp_websocket_client_init(&cfg);if(!ws) { rc=ESP_ERR_NO_MEM;goto done; }
    esp_websocket_register_events(ws,WEBSOCKET_EVENT_ANY,event,s);
    s_stats.active=true;s_stats.sessions++;
    printf("LX_CHAT_MEMORY internal=%u largest=%u psram=%u\n",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    rc=esp_websocket_client_start(ws);if(rc!=ESP_OK) goto done;
    int64_t deadline=esp_timer_get_time()+10000000;
    while(!s->connected && !s->failed && !cancelled() && esp_timer_get_time()<deadline) vTaskDelay(pdMS_TO_TICKS(10));
    if(!s->connected || s->failed || cancelled()) { rc=ESP_ERR_TIMEOUT;goto done; }
    char hello_text[240];
    snprintf(hello_text,sizeof(hello_text),"{\"type\":\"hello\",\"version\":%u,\"transport\":\"websocket\",\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,\"channels\":1,\"frame_duration\":60}}",version);
    printf("LX_CHAT protocol=%u\n",version);
    rc=send_text(ws,hello_text);
    if(rc!=ESP_OK) goto done;
    bool hello=false;
    deadline=esp_timer_get_time()+10000000;
    while(!s->failed && !cancelled() && esp_timer_get_time()<deadline) {
        if(xQueueReceive(s->queue,packet,pdMS_TO_TICKS(20))==pdTRUE) {
            if(packet->kind==CHAT_STT) continue;
            if(packet->kind!=CHAT_HELLO) { rc=ESP_ERR_INVALID_RESPONSE;goto done; }
            chat_message_t m;memcpy(&m,packet->bytes,sizeof(m));strcpy(session,m.session);hello=true;break;
        }
    }
    if(!hello) { rc=ESP_ERR_TIMEOUT;goto done; }
    if(probe) { puts("LX_CHAT_PROBE hello=ok microphone=off audio_upload=0");goto done; }
    if(dictation) { rc=dictate(ws,s,session,&codec,pcm,packet,dictation,cancelled);goto done; }
    /* Espressif demo: hello -> listen/detect(text) -> listen/start.
     * The service supplies the agent's spoken greeting through normal TTS. */
    rc=control(ws,session,"listen","detect");
    printf("LX_CHAT wake_notify=%s greeting=server\n",esp_err_to_name(rc));
    if(rc!=ESP_OK || cancelled()) goto done;
    rc=control(ws,session,"listen","start");if(rc!=ESP_OK) goto done;
    uplink=chat_uplink_start(ws,test_stall_ms);if(!uplink) { rc=ESP_ERR_NO_MEM;goto done; }
    duplex=voice_duplex_start();if(!duplex) { rc=ESP_FAIL;goto done; }
    bool speaking=false,received_reply=false,discard_audio=false;unsigned filled=0,warmup=4;int64_t started=esp_timer_get_time();deadline=started+20000000;
    /* Bound decode-ahead work by captured audio time. A server burst must not
     * decode 60 ms of TTS on every 20 ms microphone iteration. */
    int decode_credit=960;
    puts("LX_CHAT listening");
    reason="deadline";
    while(!s->failed && !cancelled() && esp_timer_get_time()<deadline && esp_timer_get_time()-started<180000000) {
        rc=chat_uplink_result(uplink);if(rc!=ESP_OK) { reason="upload";break; }
        bool queued=xQueuePeek(s->queue,packet,0)==pdTRUE;
        if(queued && (packet->kind!=128 || discard_audio || !speaking ||
                      (decode_credit>0 && voice_duplex_has_space(duplex))) &&
           xQueueReceive(s->queue,packet,0)==pdTRUE) {
            if(packet->kind==CHAT_GOODBYE) { reason="goodbye";rc=voice_duplex_drain(duplex,cancelled);break; }
            if(packet->kind==CHAT_TTS_START) {
                voice_duplex_reply(duplex,false);
                discard_audio=false;
                speaking=true;deadline=esp_timer_get_time()+30000000;
                printf("LX_CHAT speaking at_ms=%lld\n",(long long)(esp_timer_get_time()/1000));
            } else if(packet->kind==CHAT_TTS_STOP) {
                voice_duplex_reply(duplex,true);
                speaking=false;deadline=esp_timer_get_time()+20000000;
                /* Realtime listen stays open through TTS; no duplicate start. */
                printf("LX_CHAT listening at_ms=%lld\n",(long long)(esp_timer_get_time()/1000));
            } else if(packet->kind==CHAT_STT) {
                puts("LX_CHAT recognized=1");
                if(speaking || voice_duplex_pending(duplex)) {
                    voice_duplex_clear(duplex);discard_audio=true;interruptions++;
                    puts("LX_CHAT interrupted=1 source=server_recognition");
                }
            } else if(packet->kind==128 && speaking && !discard_audio) {
                size_t samples=0;rc=voice_opus_decode(&codec,packet->bytes,packet->n,decoded,1920,&samples);
                if(rc!=ESP_OK) { reason="decode";break; }
                rc=voice_duplex_write(duplex,decoded,samples);if(rc!=ESP_OK) { reason="playback";break; }
                decode_credit-=(int)samples;
                received_reply=true;s_stats.rx++;deadline=esp_timer_get_time()+30000000;
            }
        }
        {
            rc=voice_duplex_read(duplex,pcm+filled);if(rc!=ESP_OK) { reason="capture";break; }
            decode_credit+=320;if(decode_credit>1920) decode_credit=1920;
            if(warmup) { warmup--;continue; }
            filled+=320;
            if(filled==960) {
                size_t retained=0;
                if(diagnostic) {
                    retained=diagnostic->capacity-diagnostic->used;
                    if(retained>960) retained=960;
                    if(retained) memcpy(diagnostic->pcm+diagnostic->used,pcm,retained*sizeof(*pcm));
                }
                /* Measure AEC output before passing the mutable encoder input. */
                for(unsigned i=0;i<960;i++) { int v=pcm[i];unsigned a=v<0?-v:v;if(a>peak) peak=a;energy+=(uint64_t)(v*v); }
                samples_sent+=960;
                size_t h=chat_audio_header(version),n=0;
                rc=voice_opus_encode(&codec,pcm,960,packet->bytes+h,PACKET_MAX-h,&n);if(rc!=ESP_OK) { reason="encode";break; }
                if(!chat_audio_wrap(version,packet->bytes,n,(uint32_t)(esp_timer_get_time()/1000))) { rc=ESP_ERR_INVALID_SIZE;break; }
                n+=h;
                rc=chat_uplink_push(uplink,packet->bytes,n);if(rc!=ESP_OK) { reason="upload_queue";break; }
                if(diagnostic) diagnostic->used+=retained;
                filled=0;s_stats.tx++;
                if(speaking || voice_duplex_pending(duplex)) tx_playing++;
            }
        }
        /* Buffered microphone reads may not block; leave idle/service time
         * during decode catch-up instead of monopolizing CPU 0. */
        vTaskDelay(1);
    }
    if(s->failed && rc==ESP_OK) rc=ESP_FAIL;
    if(!cancelled() && rc==ESP_OK && !received_reply && esp_timer_get_time()>=deadline) {
        puts("LX_CHAT listening_timeout reply_audio=0");rc=ESP_ERR_TIMEOUT;
    }
done:
    if(s->failed) reason="socket";
    if(cancelled()) reason="cancelled";
    printf("LX_CHAT_END reason=%s result=%s\n",reason,esp_err_to_name(rc));
    if(samples_sent) printf("LX_CHAT_INPUT samples=%llu peak=%u mean_square=%llu\n",(unsigned long long)samples_sent,peak,(unsigned long long)(energy/samples_sent));
    if(duplex) {
        voice_duplex_stop(duplex);
        printf("LX_CHAT_DUPLEX tx_during_playback=%u interruptions=%u mode=realtime\n",tx_playing,interruptions);
    }
    chat_uplink_stop(uplink);
    if(ws) {
        if(esp_websocket_client_is_connected(ws)) control(ws,session,"abort",NULL);
        esp_websocket_client_stop(ws);esp_websocket_client_destroy(ws);
    }
    printf("LX_CHAT_CODEC encode_avg_us=%llu frames=%u decode_max_us=%u\n",
        codec.encode_frames?codec.encode_total_us/codec.encode_frames:0,codec.encode_frames,codec.decode_max_us);
    s_stats.encode_us=codec.encode_max_us;s_stats.decode_us=codec.decode_max_us;
    voice_opus_close(&codec);if(s->queue) vQueueDelete(s->queue);free(s);free(pcm);free(packet);
    memset(headers,0,sizeof(headers));
    s_stats.active=false;s_stats.last=rc;if(rc!=ESP_OK) s_stats.errors++;
    return rc;
}
esp_err_t chat_run(bool (*cancelled)(void),chat_capture_t *capture)
{
    if(capture) capture->used=0;
    char uri[CHAT_URI_CAP],token[CHAT_TOKEN_CAP];uint8_t version;
    if(!profile(uri,token,&version)) return ESP_ERR_NOT_FOUND;
    if(!cloud_enabled()) { memset(token,0,sizeof(token));puts("LX_CHAT cloud_enabled=0");return ESP_ERR_INVALID_STATE; }
    if(!network_ready() || (!strncmp(uri,"wss://",6) && time(NULL)<1700000000)) return ESP_ERR_INVALID_STATE;
    esp_err_t rc=run(uri,token,version,cancelled,false,capture,NULL,0);memset(token,0,sizeof(token));return rc;
}
esp_err_t chat_probe(bool (*cancelled)(void))
{
    char uri[CHAT_URI_CAP],token[CHAT_TOKEN_CAP];uint8_t version;
    if(!profile(uri,token,&version)) return ESP_ERR_NOT_FOUND;
    if(!network_ready() || (!strncmp(uri,"wss://",6) && time(NULL)<1700000000)) return ESP_ERR_INVALID_STATE;
    esp_err_t rc=run(uri,token,version,cancelled,true,NULL,NULL,0);memset(token,0,sizeof(token));return rc;
}
esp_err_t chat_local_test(bool (*cancelled)(void),chat_capture_t *capture)
{
    if(capture) capture->used=0;
    chat_fixture_t *fixture=chat_fixture_start();if(!fixture) return ESP_FAIL;
    unsigned tx=s_stats.tx,rx=s_stats.rx;
    esp_err_t rc=run("ws://127.0.0.1:39572/chat-test","",1,cancelled,false,capture,NULL,900);
    unsigned echoed=chat_fixture_stop(fixture);
    printf("LX_CHAT_TEST result=%s sent=%u received=%u echoed=%u cloud=0\n",esp_err_to_name(rc),s_stats.tx-tx,s_stats.rx-rx,echoed);
    return rc==ESP_OK && echoed==10 && s_stats.tx-tx>=10 && s_stats.rx-rx==10?ESP_OK:ESP_FAIL;
}
esp_err_t chat_recognize(const int16_t *pcm,size_t samples,char *text,size_t capacity,bool (*cancelled)(void))
{
    if(!text || !capacity) return ESP_ERR_INVALID_ARG;
    text[0]=0;
    if(!pcm || samples<3200 || samples>320000 || !cancelled) return ESP_ERR_INVALID_ARG;
    if(cancelled()) return ESP_ERR_INVALID_STATE;
    char uri[CHAT_URI_CAP],token[CHAT_TOKEN_CAP];uint8_t version;
    if(!profile(uri,token,&version)) { memset(token,0,sizeof(token));return ESP_ERR_NOT_FOUND; }
    esp_err_t rc=ESP_ERR_INVALID_STATE;
    if(cloud_enabled() && network_ready() && (strncmp(uri,"wss://",6) || time(NULL)>=1700000000)) {
        dictation_t d={.pcm=pcm,.samples=samples,.text=text,.capacity=capacity};
        rc=run(uri,token,version,cancelled,false,NULL,&d,0);
    }
    if(rc!=ESP_OK || cancelled()) { text[0]=0;if(rc==ESP_OK) rc=ESP_ERR_INVALID_STATE; }
    memset(token,0,sizeof(token));return rc;
}
esp_err_t chat_recognize_stream(chat_pcm_reader_t read,void *context,
    bool (*submit)(void *,const char *),bool (*cancelled)(void))
{
    if(!read || !submit || !cancelled || cancelled()) return ESP_ERR_INVALID_STATE;
    char uri[CHAT_URI_CAP],token[CHAT_TOKEN_CAP],text[1025];uint8_t version;
    if(!profile(uri,token,&version)) { memset(token,0,sizeof(token));return ESP_ERR_NOT_FOUND; }
    esp_err_t rc=ESP_ERR_INVALID_STATE;
    if(cloud_enabled() && network_ready() && (strncmp(uri,"wss://",6) || time(NULL)>=1700000000)) {
        dictation_t d={.text=text,.capacity=sizeof(text),.read=read,.context=context,.submit=submit};
        rc=run(uri,token,version,cancelled,false,NULL,&d,0);
    }
    memset(text,0,sizeof(text));memset(token,0,sizeof(token));return rc;
}
typedef struct { size_t sent;unsigned tx_start;bool live,submitted,abort; } test_stream_t;
static esp_err_t test_read(void *context,int16_t *pcm,size_t *samples)
{
    test_stream_t *t=context;
    if(t->abort && t->sent>=1920) return ESP_ERR_INVALID_STATE;
    size_t n=10000-t->sent;if(n>960)n=960;
    if(!n) t->live=s_stats.tx-t->tx_start==11;
    else vTaskDelay(pdMS_TO_TICKS(60));
    memset(pcm,0,n*sizeof(*pcm));t->sent+=n;*samples=n;return ESP_OK;
}
static bool test_submit(void *context,const char *text)
{
    test_stream_t *t=context;t->submitted=!strcmp(text,"你好，测试123。");return t->submitted;
}
esp_err_t chat_recognize_test(bool (*cancelled)(void))
{
    int16_t *pcm=heap_caps_calloc(10000,sizeof(*pcm),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!pcm) return ESP_ERR_NO_MEM;
    chat_fixture_t *fixture=chat_fixture_start();if(!fixture) { free(pcm);return ESP_FAIL; }
    char text[1025];dictation_t d={.pcm=pcm,.samples=10000,.text=text,.capacity=sizeof(text)};text[0]=0;
    esp_err_t rc=run("ws://127.0.0.1:39572/chat-test","",1,cancelled,false,NULL,&d,0);
    unsigned checked=chat_fixture_stop(fixture);
    bool passed=rc==ESP_OK && checked==12 && !strcmp(text,"你好，测试123。");
    memset(text,0,sizeof(text));free(pcm);
    printf("LX_ASR_TEST result=%s frames=%u cloud=0 playback=0\n",passed?"ESP_OK":"ESP_FAIL",checked);
    for(unsigned cancel=0;cancel<2 && passed;cancel++) {
        fixture=chat_fixture_start();if(!fixture)return ESP_FAIL;
        test_stream_t t={.tx_start=s_stats.tx,.abort=cancel!=0};
        dictation_t live={.text=text,.capacity=sizeof(text),.read=test_read,.context=&t,.submit=test_submit};
        rc=run("ws://127.0.0.1:39572/chat-test","",1,cancelled,false,NULL,&live,0);
        checked=chat_fixture_stop(fixture);
        passed=cancel?(rc==ESP_ERR_INVALID_STATE && !t.submitted && !checked):
            (rc==ESP_OK && checked==12 && t.live && t.submitted);
        memset(text,0,sizeof(text));
        printf("LX_ASR_STREAM_TEST cancel=%u result=%s sent_before_end=%u submitted=%u cloud=0\n",
            cancel,passed?"ESP_OK":"ESP_FAIL",t.live,t.submitted);
    }
    return passed?ESP_OK:ESP_FAIL;
}
void chat_stats(void)
{
    printf("LX_CHAT_CLOUD enabled=%u\n",cloud_enabled());
    printf("LX_CHAT configured=%u active=%u sessions=%u tx=%u rx=%u errors=%u overflows=%u encode_max_us=%u decode_max_us=%u last=%s\n",
        chat_configured(),s_stats.active,s_stats.sessions,s_stats.tx,s_stats.rx,s_stats.errors,s_stats.overflows,s_stats.encode_us,s_stats.decode_us,esp_err_to_name(s_stats.last));
}
