#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include "network_link.h"
#include "link_control.h"
#include "link_auth.h"
#include "lx_protocol.h"
#include "mouse_queue.h"
#include "consumer_queue.h"
#include "text_keys.h"
#include "usb_keys.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define INPUT_PORT 39570
typedef struct { lx_key_report_t report;uint32_t sequence;int64_t event_us; } item_t;
typedef struct { uint32_t sequence;int64_t event_us,sent_us,done_us; } trace_t;
static portMUX_TYPE s_lock=portMUX_INITIALIZER_UNLOCKED;
static item_t s_queue[32],s_latest;
static int64_t s_key_delivered_us;
static unsigned s_head,s_count;
static bool s_resync,s_receiver,s_connected,s_peer_seen,s_reply_hello,s_reply_ack,s_owns_input;
static uint32_t s_sent,s_received,s_full,s_reconnects,s_wire,s_session,s_ack_sequence;
static volatile unsigned s_connect_phase,s_connect_attempts;
static volatile int s_connect_error;
static volatile int s_listen_error;
static volatile bool s_listening;
static unsigned s_auth_failures;
static lx_mouse_queue_t s_mouse;
static lx_consumer_queue_t s_consumer;
void wifi_consumer_submit(uint16_t usage)
{
    portENTER_CRITICAL(&s_lock);lx_consumer_push(&s_consumer,usage);portEXIT_CRITICAL(&s_lock);
}
static int64_t s_last_input,s_last_peer;
static lx_key_report_t s_receiver_keys;
static uint16_t s_receiver_consumer;
static uint8_t s_receiver_buttons,s_stats[3],s_leds;
static bool s_have_stats;
static void (*s_stats_handler)(const uint8_t values[3]);
static trace_t *s_trace;
static unsigned s_trace_count;
void wifi_set_stats_handler(void (*handler)(const uint8_t values[3])) { s_stats_handler=handler; }
void wifi_host_stats(const uint8_t values[3])
{
    portENTER_CRITICAL(&s_lock);memcpy(s_stats,values,3);s_have_stats=true;portEXIT_CRITICAL(&s_lock);
}
bool wifi_input_active(void)
{
    portENTER_CRITICAL(&s_lock);bool owns=s_owns_input;portEXIT_CRITICAL(&s_lock);
    return owns;
}
void wifi_keys_submit(const lx_key_report_t *report,uint32_t sequence,int64_t event_us)
{
    item_t item={*report,sequence,event_us};
    portENTER_CRITICAL(&s_lock);s_latest=item;
    if(s_count<32) s_queue[(s_head+s_count++)%32]=item;
    else { s_full++;s_resync=true; }
    portEXIT_CRITICAL(&s_lock);
}
void wifi_mouse_submit(const lx_mouse_sample_t *sample)
{
    portENTER_CRITICAL(&s_lock);lx_mouse_queue_push(&s_mouse,sample);portEXIT_CRITICAL(&s_lock);
}
bool wifi_keys_text_room(bool *pending)
{
    portENTER_CRITICAL(&s_lock);bool room=s_count<2;
    *pending=s_count || s_latest.event_us>s_key_delivered_us;
    portEXIT_CRITICAL(&s_lock);return room;
}
void wifi_keys_cancel_pending(void)
{
    item_t release={.event_us=esp_timer_get_time()};
    portENTER_CRITICAL(&s_lock);
    s_head=0;s_count=1;s_queue[0]=s_latest=release;s_resync=false;
    portEXIT_CRITICAL(&s_lock);
}
static bool send_bytes(int fd,const uint8_t *bytes,size_t n)
{
    size_t at=0;
    /* A short TCP send-buffer stall is not a disconnected peer. 20 ms caused
     * EAGAIN reconnects and truncated VUC under measured sustained typing.
     * This wait yields only the output task; key scanning stays independent. */
    int64_t deadline=esp_timer_get_time()+200000;
    while(at<n) {
        int sent=send(fd,bytes+at,n-at,MSG_DONTWAIT);
        if(sent>0) at+=sent;
        else if(sent<0 && (errno==EAGAIN || errno==EWOULDBLOCK) && esp_timer_get_time()<deadline) vTaskDelay(1);
        else { printf("LX_WIFI_IO write=%d error=%d partial=%u/%u\n",sent,errno,(unsigned)at,(unsigned)n);return false; }
    }
    return n!=0;
}
static bool send_packet(int fd,uint8_t type,uint32_t sequence,const void *payload,uint16_t length)
{
    uint8_t bytes[LX_PACKET_MAX];size_t n=lx_packet_encode(bytes,sizeof(bytes),type,sequence,s_session,payload,length);
    return send_bytes(fd,bytes,n);
}
static void release_receiver(void)
{
    if(wifi_input_active()) {
        lx_key_report_t release={0};lx_mouse_sample_t mouse={0};
        usb_keys_submit(&release,0,esp_timer_get_time());usb_mouse_submit(&mouse);usb_consumer_submit(0);
    }
    s_receiver_keys=(lx_key_report_t){0};s_receiver_buttons=0;s_receiver_consumer=0;
    portENTER_CRITICAL(&s_lock);s_last_input=0;s_owns_input=false;portEXIT_CRITICAL(&s_lock);
}
static void received(const lx_packet_t *p,void *arg)
{
    (void)arg;
    if(p->type==LX_MSG_HELLO) {
        if(s_receiver) s_peer_seen=p->length==8 && !memcmp(p->payload,"keyboard",8);
        else s_peer_seen=p->length==8 && !memcmp(p->payload,"receiver",8);
        if(s_receiver && s_peer_seen) s_reply_hello=true;
        if(s_peer_seen) s_last_peer=esp_timer_get_time();
        return;
    }
    if(!s_peer_seen) return;
    if((!s_receiver && p->type==LX_MSG_CONTROL) || (s_receiver && p->type==LX_MSG_CONTROL_REPLY)) {
        link_control_receive(LX_CONTROL_WIFI,p);return;
    }
    if(!s_receiver) {
        if(p->type==LX_MSG_ACK && (p->length==1 || p->length==4)) {
            s_last_peer=esp_timer_get_time();
            s_leds=p->payload[0];
            if(p->length==4 && s_stats_handler) s_stats_handler(p->payload+1);
        } else if(p->type==LX_MSG_HOST_STATS && p->length==3 && s_stats_handler) s_stats_handler(p->payload);
        return;
    }
    bool valid=(p->type==LX_MSG_KEYBOARD && p->length==8) || (p->type==LX_MSG_KEEPALIVE && p->length==11) ||
               (p->type==LX_MSG_MOUSE && p->length==5) || (p->type==LX_MSG_CONSUMER && p->length==2) || (p->type==LX_MSG_RELEASE && p->length==0);
    if(!valid) return;
    int64_t now=esp_timer_get_time();
    if(!wifi_input_active()) {
        lx_key_report_t release={0};lx_mouse_sample_t mouse={0};
        usb_keys_submit(&release,0,now);usb_mouse_submit(&mouse);usb_consumer_submit(0);
        s_receiver_keys=release;s_receiver_buttons=0;s_receiver_consumer=0;
    }
    portENTER_CRITICAL(&s_lock);s_last_input=now;s_owns_input=true;portEXIT_CRITICAL(&s_lock);
    if(p->type==LX_MSG_RELEASE) release_receiver();
    else if(p->type==LX_MSG_CONSUMER) {
        s_receiver_consumer=p->payload[0]|(p->payload[1]<<8);usb_consumer_submit(s_receiver_consumer);
    } else if(p->type==LX_MSG_MOUSE) {
        lx_mouse_report_t r;memcpy(&r,p->payload,5);
        lx_mouse_sample_t sample={.buttons=r.buttons,.x=r.x,.y=r.y,.wheel=r.wheel,.pan=r.pan};
        usb_mouse_submit(&sample);s_receiver_buttons=r.buttons;
    } else {
        lx_key_report_t report;memcpy(&report,p->payload,8);report.reserved=0;
        if(memcmp(&report,&s_receiver_keys,8)) { usb_keys_submit(&report,p->sequence,now);s_receiver_keys=report; }
        if(p->type==LX_MSG_KEEPALIVE) {
            uint16_t consumer=p->payload[9]|(p->payload[10]<<8);
            if(consumer!=s_receiver_consumer) { usb_consumer_submit(consumer);s_receiver_consumer=consumer; }
            if(p->payload[8]!=s_receiver_buttons) {
                lx_mouse_sample_t sample={.buttons=p->payload[8]};usb_mouse_submit(&sample);s_receiver_buttons=sample.buttons;
            }
            s_ack_sequence=p->sequence;s_reply_ack=true;
        }
    }
    s_received++;
}
static bool read_packets(int fd,lx_stream_t *stream)
{
    uint8_t bytes[250];int n=recv(fd,bytes,sizeof(bytes),MSG_DONTWAIT);
    if(n>0) lx_stream_feed(stream,bytes,n,received,NULL);
    if(n==0 || (n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK))
        printf("LX_WIFI_IO read=%d error=%d\n",n,errno);
    return n!=0 && (n>0 || errno==EAGAIN || errno==EWOULDBLOCK);
}
static void socket_options(int fd)
{
    int one=1;setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
    fcntl(fd,F_SETFL,fcntl(fd,F_GETFL,0)|O_NONBLOCK);
}
static int connect_peer(uint32_t address)
{
    int fd=socket(AF_INET,SOCK_STREAM,IPPROTO_IP);if(fd<0) return -1;
    socket_options(fd);
    struct sockaddr_in peer={.sin_family=AF_INET,.sin_port=htons(INPUT_PORT),.sin_addr={.s_addr=address}};
    int result=connect(fd,(struct sockaddr*)&peer,sizeof(peer));
    if(result<0 && errno==EINPROGRESS) {
        fd_set writable;FD_ZERO(&writable);FD_SET(fd,&writable);struct timeval timeout={.tv_sec=4};
        result=select(fd+1,NULL,&writable,NULL,&timeout);
        if(result>0) { int error=0;socklen_t size=sizeof(error);result=getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&size);s_connect_error=result<0?errno:error;result=result==0 && !error?0:-1; }
        else { s_connect_error=result==0?ETIMEDOUT:errno;result=-1; }
    }
    else s_connect_error=result<0?errno:0;
    if(result<0) { close(fd);return -1; }
    return fd;
}
static void sender_task(void *arg)
{
    (void)arg;
    while(1) {
        s_connect_phase=0;
        if(!network_ready()) { vTaskDelay(pdMS_TO_TICKS(100));continue; }
        s_connect_phase=1;
        network_set_discovered_receiver(network_find_receiver());
        s_connect_phase=2;
        uint32_t address=network_peer_address();
        if(!network_ready() || !address) { vTaskDelay(pdMS_TO_TICKS(1000));continue; }
        s_connect_phase=3;s_connect_attempts++;
        int fd=connect_peer(address);if(fd<0) { vTaskDelay(pdMS_TO_TICKS(1000));continue; }
        if(!link_auth_socket(fd,false)) { s_auth_failures++;close(fd);vTaskDelay(pdMS_TO_TICKS(1000));continue; }
        s_connect_phase=4;
        lx_stream_t stream={0};s_peer_seen=false;s_reconnects++;
        bool good=send_packet(fd,LX_MSG_HELLO,++s_wire,"keyboard",8);
        int64_t handshake=esp_timer_get_time()+1000000;
        while(good && !s_peer_seen && esp_timer_get_time()<handshake) { good=read_packets(fd,&stream);vTaskDelay(1); }
        good=good && s_peer_seen;s_connected=good;
        s_connect_phase=5;
        portENTER_CRITICAL(&s_lock);s_head=0;s_count=0;s_resync=false;lx_mouse_queue_resync(&s_mouse);lx_consumer_resync(&s_consumer);portEXIT_CRITICAL(&s_lock);
        int64_t last=0,mouse_next=0;
        lx_key_report_t delivered={0};
        while(good && network_ready() && address==network_peer_address()) {
            good=read_packets(fd,&stream);if(!good) break;
            /* Drain queued ACKs before judging liveness. TCP retransmission or
             * bounded send-buffer waits can exceed the old 500 ms ACK grace.
             * Receiver-side 500 ms key release remains unchanged. */
            if(esp_timer_get_time()-s_last_peer>=2000000) break;
            item_t item;bool have=false,resync=false;uint16_t consumer=0;bool consumer_have=false;
            portENTER_CRITICAL(&s_lock);
            if(s_resync) { delivered=s_latest.report;s_head=0;s_count=0;s_resync=false;resync=true;lx_mouse_queue_resync(&s_mouse);lx_consumer_resync(&s_consumer); }
            if(s_count) { item=s_queue[s_head];s_head=(s_head+1)%32;s_count--;have=true; }
            else consumer_have=lx_consumer_take(&s_consumer,&consumer);
            portEXIT_CRITICAL(&s_lock);
            if(have) {
                int64_t begin=esp_timer_get_time();good=send_packet(fd,LX_MSG_KEYBOARD,++s_wire,&item.report,8);
                if(good) {
                    delivered=item.report;
                    s_sent++;portENTER_CRITICAL(&s_lock);
                    s_key_delivered_us=item.event_us;
                    if(s_trace && s_trace_count<1200) s_trace[s_trace_count++]=(trace_t){item.sequence,item.event_us,begin,esp_timer_get_time()};
                    portEXIT_CRITICAL(&s_lock);
                }
            } else if(consumer_have) {
                good=send_packet(fd,LX_MSG_CONSUMER,++s_wire,&consumer,2);
            } else if(esp_timer_get_time()>=mouse_next) {
                lx_mouse_report_t mouse;portENTER_CRITICAL(&s_lock);bool have_mouse=lx_mouse_queue_take(&s_mouse,&mouse);portEXIT_CRITICAL(&s_lock);
                if(have_mouse) { good=send_packet(fd,LX_MSG_MOUSE,++s_wire,&mouse,5);mouse_next=esp_timer_get_time()+4000; }
            }
            /* The receiver ACKs keepalives, not every key report. A continuous
             * text queue must not starve them and trigger the 500 ms timeout.
             * Repeat only the last sent key state, never jump ahead of queued
             * down/up edges with s_latest. Physical reports still go first. */
            if(good && (resync || esp_timer_get_time()-last>=100000)) {
                uint8_t state[11];memcpy(state,&delivered,8);
                portENTER_CRITICAL(&s_lock);state[8]=s_mouse.latest_buttons;state[9]=s_consumer.latest;state[10]=s_consumer.latest>>8;portEXIT_CRITICAL(&s_lock);
                good=send_packet(fd,LX_MSG_KEEPALIVE,++s_wire,state,sizeof(state));last=esp_timer_get_time();
            }
            /* Control never overtakes physical reports or keepalives. */
            if(good && !have && !consumer_have) {
                uint8_t control[LX_PACKET_MAX];size_t control_n=link_control_take(LX_CONTROL_WIFI,control);
                if(control_n) good=send_bytes(fd,control,control_n);
            }
            vTaskDelay(1);
        }
        printf("LX_WIFI_RECONNECT good=%u ready=%u peer_age_ms=%lld\n",good,network_ready(),(esp_timer_get_time()-s_last_peer)/1000);
        link_control_drop(LX_CONTROL_WIFI);
        shutdown(fd,SHUT_RDWR);close(fd);s_connected=false;
        portENTER_CRITICAL(&s_lock);s_head=0;s_count=0;lx_mouse_queue_resync(&s_mouse);lx_consumer_resync(&s_consumer);portEXIT_CRITICAL(&s_lock);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
static void receiver_task(void *arg)
{
    (void)arg;
    int server=socket(AF_INET,SOCK_STREAM,IPPROTO_IP);if(server<0) { s_listen_error=errno;vTaskDelete(NULL);return; }
    int one=1;setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    struct sockaddr_in local={.sin_family=AF_INET,.sin_port=htons(INPUT_PORT),.sin_addr={.s_addr=htonl(INADDR_ANY)}};
    if(bind(server,(struct sockaddr*)&local,sizeof(local))<0 || listen(server,1)<0) { s_listen_error=errno;close(server);vTaskDelete(NULL);return; }
    s_listening=true;
    socket_options(server);
    while(1) {
        int fd=accept(server,NULL,NULL);if(fd<0) { vTaskDelay(pdMS_TO_TICKS(10));continue; }
        socket_options(fd);lx_stream_t stream={0};s_peer_seen=false;s_reply_hello=false;s_reply_ack=false;
        if(!link_auth_socket(fd,true)) { s_auth_failures++;close(fd);continue; }
        s_connected=true;s_reconnects++;int64_t accepted=esp_timer_get_time();
        while(network_ready() && read_packets(fd,&stream)) {
            if(s_reply_hello) { s_reply_hello=false;if(!send_packet(fd,LX_MSG_HELLO,0,"receiver",8)) break; }
            if(s_reply_ack) {
                s_reply_ack=false;uint8_t payload[4]={usb_keys_leds()};
                portENTER_CRITICAL(&s_lock);bool stats=s_have_stats;memcpy(payload+1,s_stats,3);portEXIT_CRITICAL(&s_lock);
                if(!send_packet(fd,LX_MSG_ACK,s_ack_sequence,payload,stats?4:1)) break;
            }
            uint8_t control[LX_PACKET_MAX];size_t control_n=link_control_take(LX_CONTROL_WIFI,control);
            if(control_n && !send_bytes(fd,control,control_n)) break;
            portENTER_CRITICAL(&s_lock);int64_t last=s_last_input;portEXIT_CRITICAL(&s_lock);
            if(esp_timer_get_time()-(last?last:accepted)>500000) break;
            vTaskDelay(1);
        }
        link_control_drop(LX_CONTROL_WIFI);release_receiver();shutdown(fd,SHUT_RDWR);close(fd);s_connected=false;
    }
}
esp_err_t wifi_keys_start(bool receiver)
{
    s_receiver=receiver;s_session=esp_random();network_auto_retry(true);ESP_ERROR_CHECK(network_start(true));
    if(receiver) ESP_ERROR_CHECK(network_discovery_start());
    return xTaskCreatePinnedToCore(receiver?receiver_task:sender_task,"wifi_keys",6144,NULL,9,NULL,1)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;
}
void wifi_keys_stats(void)
{
    printf("LX_WIFI receiver=%u ready=%u connected=%u sent=%lu received=%lu queue_full=%lu connections=%lu leds=%u\n",
        s_receiver,network_ready(),s_connected,(unsigned long)s_sent,(unsigned long)s_received,(unsigned long)s_full,(unsigned long)s_reconnects,s_leds);
    if(!s_receiver) printf("LX_WIFI_CONNECT phase=%u attempts=%u error=%d\n",s_connect_phase,s_connect_attempts,s_connect_error);
    printf("LX_WIFI_AUTH failures=%u listening=%u listen_error=%d\n",s_auth_failures,s_listening,s_listen_error);
}
void wifi_keys_trace_clear(void)
{
    if(!s_trace) { trace_t *trace=heap_caps_calloc(1200,sizeof(trace_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        portENTER_CRITICAL(&s_lock);s_trace=trace;portEXIT_CRITICAL(&s_lock); }
    portENTER_CRITICAL(&s_lock);s_trace_count=0;portEXIT_CRITICAL(&s_lock);
}
void wifi_keys_trace_print(void)
{
    portENTER_CRITICAL(&s_lock);unsigned count=s_trace_count;portEXIT_CRITICAL(&s_lock);
    for(unsigned i=0;i<count;i++) {
        portENTER_CRITICAL(&s_lock);trace_t t=s_trace[i];portEXIT_CRITICAL(&s_lock);
        printf("LX_TRACE %lu %lld %lld %lld\n",(unsigned long)t.sequence,(long long)t.event_us,(long long)t.sent_us,(long long)t.done_us);
    }
    printf("LX_TRACE_END %u\n",count);
}
