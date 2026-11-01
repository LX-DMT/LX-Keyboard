#include <string.h>
#include <stdlib.h>
#include "chat_protocol.h"
#include "cJSON.h"
size_t chat_audio_header(unsigned version) { return version==2?16:version==3?4:0; }
static void put32(uint8_t *p,uint32_t n) { p[0]=n>>24;p[1]=n>>16;p[2]=n>>8;p[3]=n; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
bool chat_audio_wrap(unsigned version,uint8_t *p,size_t n,uint32_t timestamp)
{
    if(version<1 || version>3 || !n || n>65535) return false;
    size_t h=chat_audio_header(version);memset(p,0,h);
    if(version==2) { p[1]=2;put32(p+8,timestamp);put32(p+12,n); }
    if(version==3) { p[2]=n>>8;p[3]=n; }
    return true;
}
bool chat_audio_unwrap(unsigned version,const uint8_t *p,size_t n,const uint8_t **audio,size_t *size)
{
    size_t h=chat_audio_header(version);
    if(version<1 || version>3 || n<=h) return false;
    if(version==2 && (p[0]!=0 || p[1]!=2 || p[2]!=0 || p[3]!=0 || get32(p+12)!=n-h)) return false;
    if(version==3 && (p[0]!=0 || (((size_t)p[2]<<8)|p[3])!=n-h)) return false;
    *audio=p+h;*size=n-h;return true;
}
int chat_rx_feed(chat_rx_t *r,uint8_t op,bool fin,size_t payload,size_t offset,const void *data,size_t n)
{
    if(op>=8) return 0; /* Interleaved ping/pong/close never modify the data message. */
    if(op>2 || offset>payload || n>payload-offset || (!data && n)) goto bad;
    if(offset==0) {
        if(op) {
            if(r->active) goto bad;
            r->used=0;r->opcode=op;r->active=true;
        } else if(!r->active) goto bad;
        r->payload=payload;r->offset=0;
    }
    if(!r->active || offset!=r->offset || payload!=r->payload || n>CHAT_MESSAGE_MAX-r->used) goto bad;
    if(n) memcpy(r->bytes+r->used,data,n);
    r->used+=n;r->offset+=n;
    if(r->offset==payload && fin) { r->active=false;r->bytes[r->used]=0;return 1; }
    return 0;
bad:
    r->active=false;r->used=0;return -1;
}
static bool equal(const cJSON *j,const char *key,const char *value)
{
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(j,key);
    return cJSON_IsString(v) && !strcmp(v->valuestring,value);
}
chat_message_t chat_json(const char *text,size_t n)
{
    chat_message_t out={.kind=CHAT_INVALID,.rate=16000};
    if(!n || memchr(text,0,n)) return out;
    const char *end=NULL;cJSON *j=cJSON_ParseWithLengthOpts(text,n,&end,false);
    if(!j) return out;
    while(end<text+n && (*end==' ' || *end=='\r' || *end=='\n' || *end=='\t')) end++;
    if(end!=text+n || !cJSON_IsObject(j)) goto done;
    out.kind=CHAT_IGNORE;
    if(equal(j,"type","hello")) {
        out.kind=CHAT_INVALID;
        if(!equal(j,"transport","websocket")) goto done;
        const cJSON *params=cJSON_GetObjectItemCaseSensitive(j,"audio_params");
        if(params) {
            const cJSON *rate=cJSON_GetObjectItemCaseSensitive(params,"sample_rate");
            const cJSON *ch=cJSON_GetObjectItemCaseSensitive(params,"channels");
            const cJSON *duration=cJSON_GetObjectItemCaseSensitive(params,"frame_duration");
            if(!equal(params,"format","opus") || !cJSON_IsNumber(rate) || (rate->valueint!=16000 && rate->valueint!=24000) ||
               !cJSON_IsNumber(ch) || ch->valueint!=1 || !cJSON_IsNumber(duration) || duration->valueint!=60) goto done;
            out.rate=rate->valueint;
        }
        const cJSON *session=cJSON_GetObjectItemCaseSensitive(j,"session_id");
        if(session) {
            if(!cJSON_IsString(session) || strlen(session->valuestring)>=sizeof(out.session)) goto done;
            strcpy(out.session,session->valuestring);
        }
        out.kind=CHAT_HELLO;
    } else if(equal(j,"type","tts")) {
        if(equal(j,"state","start")) out.kind=CHAT_TTS_START;
        else if(equal(j,"state","stop")) out.kind=CHAT_TTS_STOP;
    } else if(equal(j,"type","goodbye")) out.kind=CHAT_GOODBYE;
    else if(equal(j,"type","stt")) {
        const cJSON *recognized=cJSON_GetObjectItemCaseSensitive(j,"text");
        if(cJSON_IsString(recognized) && recognized->valuestring[0]) out.kind=CHAT_STT;
    }
done:
    cJSON_Delete(j);return out;
}
int chat_stt_text(const char *json,size_t n,char *out,size_t cap)
{
    if(!out || !cap) return -1;
    out[0]=0;
    chat_message_t m=chat_json(json,n);
    if(m.kind==CHAT_INVALID) return -1;
    if(m.kind!=CHAT_STT) return 0;
    cJSON *j=cJSON_ParseWithLength(json,n);
    const cJSON *t=cJSON_GetObjectItemCaseSensitive(j,"text");
    size_t bytes=cJSON_IsString(t)?strlen(t->valuestring):0;
    int result=-1;
    if(bytes && bytes<cap) { memcpy(out,t->valuestring,bytes+1);result=(int)bytes; }
    cJSON_Delete(j);return result;
}
unsigned chat_protocol_selftest(void)
{
    unsigned errors=0;chat_rx_t *r=calloc(1,sizeof(*r));if(!r) return 1;
#define CHECK(x) do { if(!(x)) errors++; } while(0)
    char text[32];
    const char *stt="{\"type\":\"stt\",\"text\":\"你好123\"}";
    CHECK(chat_stt_text(stt,strlen(stt),text,sizeof(text))==9 && !strcmp(text,"你好123"));
    CHECK(chat_stt_text(stt,strlen(stt),text,9)==-1 && !text[0]);
    const char *empty="{\"type\":\"stt\",\"text\":\"\"}";
    CHECK(chat_stt_text(empty,strlen(empty),text,sizeof(text))==0 && !text[0]);
    const char *reply="{\"type\":\"tts\",\"state\":\"sentence_start\",\"text\":\"reply\"}";
    CHECK(chat_stt_text(reply,strlen(reply),text,sizeof(text))==0 && !text[0]);
    CHECK(chat_stt_text("{",1,text,sizeof(text))==-1 && !text[0]);
    const uint8_t v2[]={0,2,0,0,0,0,0,0,1,2,3,4,0,0,0,3,'a','b','c'};
    const uint8_t v3[]={0,0,0,3,'a','b','c'};
    for(unsigned v=1;v<=3;v++) {
        uint8_t b[20];size_t h=chat_audio_header(v),n=0;const uint8_t *audio=NULL;
        memcpy(b+h,"abc",3);CHECK(chat_audio_wrap(v,b,3,0x01020304));
        if(v==2) CHECK(!memcmp(b,v2,sizeof(v2)));
        if(v==3) CHECK(!memcmp(b,v3,sizeof(v3)));
        CHECK(chat_audio_unwrap(v,b,h+3,&audio,&n) && n==3 && !memcmp(audio,"abc",3));
        CHECK(!chat_audio_unwrap(v,b,h,&audio,&n));
        if(v>1) {
            CHECK(!chat_audio_unwrap(v,b,h+2,&audio,&n));
            b[0]=1;CHECK(!chat_audio_unwrap(v,b,h+3,&audio,&n));
        }
    }
    { uint8_t b[20];const uint8_t *p=NULL;size_t n=0;
      CHECK(!chat_audio_wrap(4,b,3,0));CHECK(!chat_audio_unwrap(4,b,3,&p,&n)); }
    CHECK(chat_rx_feed(r,1,false,3,0,"ab",2)==0);
    CHECK(chat_rx_feed(r,9,true,1,0,"x",1)==0);
    CHECK(chat_rx_feed(r,1,false,3,2,"c",1)==0);
    CHECK(chat_rx_feed(r,0,true,2,0,"de",2)==1 && r->used==5 && !memcmp(r->bytes,"abcde",5));
    CHECK(chat_rx_feed(r,0,true,1,0,"x",1)==-1);
    CHECK(chat_rx_feed(r,2,true,4,0,"ab",2)==0);
    CHECK(chat_rx_feed(r,2,true,4,3,"d",1)==-1);
    CHECK(chat_rx_feed(r,2,true,CHAT_MESSAGE_MAX+1,0,"",CHAT_MESSAGE_MAX+1)==-1);
    const char *hello="{\"type\":\"hello\",\"transport\":\"websocket\",\"session_id\":\"test\",\"audio_params\":{\"format\":\"opus\",\"sample_rate\":24000,\"channels\":1,\"frame_duration\":60}}";
    chat_message_t m=chat_json(hello,strlen(hello));CHECK(m.kind==CHAT_HELLO && m.rate==24000 && !strcmp(m.session,"test"));
    const char *samples[]={"{\"type\":\"tts\",\"state\":\"start\"}","{\"type\":\"tts\",\"state\":\"stop\"}","{\"type\":\"system\",\"command\":\"reboot\"}","{\"type\":\"hello\",\"transport\":\"mqtt\"}","{}x"};
    chat_kind_t kinds[]={CHAT_TTS_START,CHAT_TTS_STOP,CHAT_IGNORE,CHAT_INVALID,CHAT_INVALID};
    for(unsigned i=0;i<5;i++) CHECK(chat_json(samples[i],strlen(samples[i])).kind==kinds[i]);
    free(r);return errors;
#undef CHECK
}
