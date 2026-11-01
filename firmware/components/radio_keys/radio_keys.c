#include <stdio.h>
#include <string.h>
#include "radio_keys.h"
#include "lx_protocol.h"
#include "usb_keys.h"
#include "mouse_queue.h"
#include "consumer_queue.h"
#include "network_link.h"
#include "link_control.h"
#include "device_pair.h"
#include "device_setup.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

typedef struct { lx_key_report_t report; uint32_t sequence,session; int64_t event_us; uint8_t type,buttons;uint16_t consumer;lx_mouse_report_t mouse; } item_t;
typedef struct { uint32_t sequence; int64_t event_us,sent_us,done_us; } trace_t;
static trace_t *s_trace;
static unsigned s_trace_count;
static bool s_receiver;
static int64_t s_last_input;
static uint8_t s_peer_storage[6];
static const uint8_t *s_peer;
static unsigned s_rate=1;
esp_err_t radio_keys_rate(unsigned mbps)
{
    if(!s_peer || (mbps!=1 && mbps!=6)) return ESP_ERR_INVALID_ARG;
    esp_now_rate_config_t rate={.phymode=mbps==1?WIFI_PHY_MODE_11B:WIFI_PHY_MODE_11G,
        .rate=mbps==1?WIFI_PHY_RATE_1M_L:WIFI_PHY_RATE_6M};
    esp_err_t rc=esp_now_set_peer_rate_config(s_peer,&rate);if(rc==ESP_OK) s_rate=mbps;return rc;
}
static TaskHandle_t s_task;
static QueueHandle_t s_rx;
static portMUX_TYPE s_lock=portMUX_INITIALIZER_UNLOCKED;
bool radio_input_active(void)
{
    portENTER_CRITICAL(&s_lock);int64_t last=s_last_input;portEXIT_CRITICAL(&s_lock);
    return last && esp_timer_get_time()-last<500000;
}
static item_t s_queue[32],s_latest;
static int64_t s_key_delivered_us;
static unsigned s_head,s_count;
static uint32_t s_cancel_epoch;
static bool s_resync,s_acked;
static uint32_t s_session,s_wait_seq,s_wire_seq;
static uint32_t s_sent,s_received,s_retries,s_dropped,s_timeouts,s_full;
static uint8_t s_leds;
static int s_rssi;
static bool s_tx_pending,s_tx_ok;
static int64_t s_tx_done_us;
static uint32_t s_tx_failed,s_api_failed;
static int64_t s_tx_start_us,s_tx_max_us;
static lx_mouse_queue_t s_mouse;
static lx_consumer_queue_t s_consumer;
void radio_consumer_submit(uint16_t usage)
{
    portENTER_CRITICAL(&s_lock);lx_consumer_push(&s_consumer,usage);portEXIT_CRITICAL(&s_lock);
    if(s_task) xTaskNotifyGive(s_task);
}
static uint32_t s_mouse_sent,s_mouse_received;
static uint8_t s_host_stats[3];
static bool s_have_stats;
static int64_t s_pause_until;
void radio_keys_pause(unsigned milliseconds)
{
    portENTER_CRITICAL(&s_lock);s_pause_until=esp_timer_get_time()+milliseconds*1000LL;s_resync=true;portEXIT_CRITICAL(&s_lock);
}
static void (*s_stats_handler)(const uint8_t values[3]);
void radio_set_stats_handler(void (*handler)(const uint8_t values[3])) { s_stats_handler=handler; }
void radio_host_stats(const uint8_t values[3])
{
    portENTER_CRITICAL(&s_lock);memcpy(s_host_stats,values,3);s_have_stats=true;portEXIT_CRITICAL(&s_lock);
}

static void sent_callback(const esp_now_send_info_t *info,esp_now_send_status_t status)
{
    (void)info;
    portENTER_CRITICAL(&s_lock);
    s_tx_ok=status==ESP_NOW_SEND_SUCCESS;s_tx_pending=false;s_tx_done_us=esp_timer_get_time();
    if(s_tx_done_us-s_tx_start_us>s_tx_max_us) s_tx_max_us=s_tx_done_us-s_tx_start_us;
    if(!s_tx_ok) s_tx_failed++;
    portEXIT_CRITICAL(&s_lock);
    if(s_task) xTaskNotifyGive(s_task);
}
static esp_err_t wire_send(const uint8_t *bytes,size_t length)
{
    portENTER_CRITICAL(&s_lock);s_tx_pending=true;s_tx_ok=false;s_tx_start_us=esp_timer_get_time();portEXIT_CRITICAL(&s_lock);
    esp_err_t result=esp_now_send(s_peer,bytes,length);
    if(result!=ESP_OK) {
        portENTER_CRITICAL(&s_lock);s_tx_pending=false;s_api_failed++;portEXIT_CRITICAL(&s_lock);
    }
    return result;
}

static void receive(const esp_now_recv_info_t *info,const uint8_t *data,int length)
{
    if(memcmp(info->src_addr,s_peer,6)) return;
    if(s_receiver && wifi_input_active()) return;
    lx_packet_t p;
    if(!lx_packet_decode(data,length,&p)) return;
    if((!s_receiver && p.type==LX_MSG_CONTROL) || (s_receiver && p.type==LX_MSG_CONTROL_REPLY)) {
        link_control_receive(LX_CONTROL_RADIO,&p);return;
    }
    s_rssi=info->rx_ctrl->rssi;
    if(!s_receiver && p.type==LX_MSG_ACK && (p.length==1 || p.length==4)) {
        portENTER_CRITICAL(&s_lock);
        if(p.session==s_session && p.sequence==s_wait_seq) { s_acked=true;s_leds=p.payload[0]; }
        portEXIT_CRITICAL(&s_lock);
        if(p.length==4 && s_stats_handler) s_stats_handler(p.payload+1);
        if(s_task) xTaskNotifyGive(s_task);
    } else if(s_receiver && ((p.type==LX_MSG_KEYBOARD && p.length==8) ||
                (p.type==LX_MSG_KEEPALIVE && p.length==11) || (p.type==LX_MSG_MOUSE && p.length==5) || (p.type==LX_MSG_CONSUMER && p.length==2))) {
        network_radio_activity();
        portENTER_CRITICAL(&s_lock);s_last_input=esp_timer_get_time();portEXIT_CRITICAL(&s_lock);
        item_t item={.sequence=p.sequence,.session=p.session,.event_us=esp_timer_get_time(),.type=p.type};
        if(p.type==LX_MSG_CONSUMER) item.consumer=p.payload[0]|(p.payload[1]<<8);
        else if(p.type==LX_MSG_MOUSE) memcpy(&item.mouse,p.payload,5);
        else memcpy(&item.report,p.payload,8);
        if(p.type==LX_MSG_KEEPALIVE) { item.buttons=p.payload[8];item.consumer=p.payload[9]|(p.payload[10]<<8); }
        /* USB boot-style report byte 1 is reserved, never a seventh key. */
        item.report.reserved=0;
        if(xQueueSend(s_rx,&item,0)!=pdTRUE) {
            portENTER_CRITICAL(&s_lock);s_full++;portEXIT_CRITICAL(&s_lock);
        }
    }
}
void radio_mouse_submit(const lx_mouse_sample_t *sample)
{
    portENTER_CRITICAL(&s_lock);lx_mouse_queue_push(&s_mouse,sample);portEXIT_CRITICAL(&s_lock);
    if(s_task) xTaskNotifyGive(s_task);
}
void radio_keys_submit(const lx_key_report_t *report,uint32_t sequence,int64_t event_us)
{
    item_t item={.report=*report,.sequence=sequence,.event_us=event_us};
    portENTER_CRITICAL(&s_lock);
    s_latest=item;
    if(s_count<32) { s_queue[(s_head+s_count)%32]=item;s_count++; }
    else { s_full++;s_resync=true; }
    portEXIT_CRITICAL(&s_lock);
    if(s_task) xTaskNotifyGive(s_task);
}
bool radio_keys_text_room(bool *pending)
{
    portENTER_CRITICAL(&s_lock);bool room=s_count<2;
    *pending=s_count || s_latest.event_us>s_key_delivered_us;
    portEXIT_CRITICAL(&s_lock);return room;
}
void radio_keys_cancel_pending(void)
{
    item_t release={.event_us=esp_timer_get_time()};
    portENTER_CRITICAL(&s_lock);
    s_head=0;s_count=1;s_queue[0]=s_latest=release;s_resync=false;s_cancel_epoch++;
    portEXIT_CRITICAL(&s_lock);
    if(s_task) xTaskNotifyGive(s_task);
}
static bool transmit(const item_t *item,bool heartbeat,uint32_t epoch)
{
    uint8_t bytes[27],payload[11];memcpy(payload,&item->report,8);
    portENTER_CRITICAL(&s_lock);payload[8]=s_mouse.latest_buttons;payload[9]=s_consumer.latest;payload[10]=s_consumer.latest>>8;portEXIT_CRITICAL(&s_lock);
    uint32_t wire=++s_wire_seq;
    size_t length=lx_packet_encode(bytes,sizeof(bytes),heartbeat?LX_MSG_KEEPALIVE:LX_MSG_KEYBOARD,
                                  wire,s_session,payload,heartbeat?11:8);
    portENTER_CRITICAL(&s_lock);s_wait_seq=wire;s_acked=false;portEXIT_CRITICAL(&s_lock);
    int64_t sent=esp_timer_get_time();
    for(unsigned attempt=0;attempt<3;attempt++) {
        portENTER_CRITICAL(&s_lock);bool busy=s_tx_pending,cancelled=epoch!=s_cancel_epoch;portEXIT_CRITICAL(&s_lock);
        if(cancelled) return true;
        if(busy) break;
        portENTER_CRITICAL(&s_lock);s_sent++;if(attempt) s_retries++;portEXIT_CRITICAL(&s_lock);
        esp_err_t result=wire_send(bytes,length);
        /* Measured Windows arrival occasionally exceeds 40 ms while MAC still
         * succeeds. A still-running send is not a failed send; allow its callback
         * to finish. Host latency remains measured independently. */
        int64_t deadline=esp_timer_get_time()+80000;
        do {
            portENTER_CRITICAL(&s_lock);
            bool ack=s_acked,pending=s_tx_pending,ok=s_tx_ok;
            bool cancelled=epoch!=s_cancel_epoch;
            int64_t done=s_tx_done_us;
            portEXIT_CRITICAL(&s_lock);
            if(cancelled) return true; /* No more retries of superseded text. */
            if(ack && !pending) {
                portENTER_CRITICAL(&s_lock);
                if(!heartbeat && s_trace && s_trace_count<1200) s_trace[s_trace_count++]=(trace_t){
                    item->sequence,item->event_us,sent,esp_timer_get_time()};
                portEXIT_CRITICAL(&s_lock);
                return true;
            }
            if(!pending && (!ok || esp_timer_get_time()-done>=8000)) break;
            ulTaskNotifyTake(pdTRUE,1);
        } while(result==ESP_OK && esp_timer_get_time()<deadline);
    }
    portENTER_CRITICAL(&s_lock);
    s_dropped++;
    if(!heartbeat && s_trace && s_trace_count<1200) s_trace[s_trace_count++]=(trace_t){item->sequence,item->event_us,sent,0};
    portEXIT_CRITICAL(&s_lock);
    return false;
}
static void sender_task(void *arg)
{
    (void)arg;
    int64_t last=0,mouse_next=0;
    while(1) {
        portENTER_CRITICAL(&s_lock);bool paused=esp_timer_get_time()<s_pause_until;portEXIT_CRITICAL(&s_lock);
        if(paused) { ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(10));continue; }
        portENTER_CRITICAL(&s_lock);bool busy=s_tx_pending;portEXIT_CRITICAL(&s_lock);
        if(busy) { ulTaskNotifyTake(pdTRUE,1);continue; }
        item_t item;bool have=false,heartbeat=false;uint16_t consumer=0;bool consumer_have=false;
        portENTER_CRITICAL(&s_lock);
        uint32_t epoch=s_cancel_epoch;
        if(s_resync) { s_count=0;s_head=0;s_resync=false;item=s_latest;have=true;lx_mouse_queue_resync(&s_mouse);lx_consumer_resync(&s_consumer); }
        else if(s_count) { item=s_queue[s_head];s_head=(s_head+1)%32;s_count--;have=true; }
        else if(lx_consumer_take(&s_consumer,&consumer)) consumer_have=true;
        else if(esp_timer_get_time()-last>=100000) { item=s_latest;have=true;heartbeat=true; }
        portEXIT_CRITICAL(&s_lock);
        if(have) {
            if(!transmit(&item,heartbeat,epoch)) {
                /* A missing app ACK alone is not proof of a lost link: MAC
                 * success may already have delivered input (measured on PC).
                 * Preserve later edges in that case. Only discard stale taps
                 * after a radio-level failure, then reconcile held state. */
                portENTER_CRITICAL(&s_lock);
                if(!s_tx_pending && !s_tx_ok) { s_count=0;s_head=0;lx_mouse_queue_resync(&s_mouse);lx_consumer_resync(&s_consumer); }
                portEXIT_CRITICAL(&s_lock);
            } else {
                portENTER_CRITICAL(&s_lock);s_key_delivered_us=item.event_us;portEXIT_CRITICAL(&s_lock);
            }
            last=esp_timer_get_time();
        } else if(consumer_have) {
            uint8_t bytes[18];size_t n=lx_packet_encode(bytes,sizeof(bytes),LX_MSG_CONSUMER,++s_wire_seq,s_session,&consumer,2);
            /* MAC retry only, so media cannot start an application retry loop ahead of typing. */
            wire_send(bytes,n);
        } else {
            uint8_t control[LX_PACKET_MAX];size_t control_n=link_control_take(LX_CONTROL_RADIO,control);
            if(control_n) { wire_send(control,control_n);continue; }
            lx_mouse_report_t mouse;bool mouse_have=false;
            portENTER_CRITICAL(&s_lock);
            if(esp_timer_get_time()>=mouse_next) mouse_have=lx_mouse_queue_take(&s_mouse,&mouse);
            portEXIT_CRITICAL(&s_lock);
            if(mouse_have) {
                uint8_t bytes[21];size_t n=lx_packet_encode(bytes,sizeof(bytes),LX_MSG_MOUSE,++s_wire_seq,s_session,&mouse,5);
                /* MAC retries are supplied by ESP-NOW. Never hold typing for a
                 * second mouse ACK/retry loop; heartbeat repairs held buttons. */
                wire_send(bytes,n);s_mouse_sent++;mouse_next=esp_timer_get_time()+4000;
            } else ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(1));
        }
    }
}
static void receiver_task(void *arg)
{
    (void)arg;
    uint32_t session=0,sequence=0;bool connected=false;
    int64_t last=0;lx_key_report_t state={0};uint8_t buttons=0;uint16_t consumer=0;
    item_t ack_item={0};bool ack_waiting=false;
    while(1) {
        if(wifi_input_active()) {
            link_control_drop(LX_CONTROL_RADIO);
            connected=false;ack_waiting=false;state=(lx_key_report_t){0};buttons=0;consumer=0;
            item_t discarded;while(xQueueReceive(s_rx,&discarded,0)==pdTRUE) {}
            vTaskDelay(pdMS_TO_TICKS(1));continue;
        }
        item_t item;
        if(xQueueReceive(s_rx,&item,pdMS_TO_TICKS(ack_waiting?1:20))==pdTRUE) {
            bool fresh=!connected || item.session!=session || (int32_t)(item.sequence-sequence)>0;
            last=esp_timer_get_time();
            if(fresh) {
                if(!connected || item.session!=session) {
                    state=(lx_key_report_t){0};buttons=0;consumer=0;
                    usb_keys_submit(&state,0,item.event_us);
                    lx_mouse_sample_t release={0};usb_mouse_submit(&release);usb_consumer_submit(0);
                }
                if(item.type==LX_MSG_CONSUMER) { usb_consumer_submit(item.consumer);consumer=item.consumer; }
                else if(item.type==LX_MSG_MOUSE) {
                    lx_mouse_sample_t mouse={.buttons=item.mouse.buttons,.x=item.mouse.x,.y=item.mouse.y,
                        .wheel=item.mouse.wheel,.pan=item.mouse.pan};
                    usb_mouse_submit(&mouse);buttons=mouse.buttons;s_mouse_received++;
                } else {
                    if(memcmp(&state,&item.report,8)) { usb_keys_submit(&item.report,item.sequence,item.event_us);state=item.report; }
                    if(item.type==LX_MSG_KEEPALIVE && consumer!=item.consumer) { usb_consumer_submit(item.consumer);consumer=item.consumer; }
                    if(item.type==LX_MSG_KEEPALIVE && buttons!=item.buttons) {
                        lx_mouse_sample_t mouse={.buttons=item.buttons};usb_mouse_submit(&mouse);buttons=item.buttons;
                    }
                }
                session=item.session;sequence=item.sequence;connected=true;
                portENTER_CRITICAL(&s_lock);s_received++;portEXIT_CRITICAL(&s_lock);
            }
            if(item.type!=LX_MSG_MOUSE && item.type!=LX_MSG_CONSUMER) { ack_item=item;ack_waiting=true; }
        }
        portENTER_CRITICAL(&s_lock);bool busy=s_tx_pending;portEXIT_CRITICAL(&s_lock);
        if(ack_waiting && !busy) {
            uint8_t ack[20],payload[4]={usb_keys_leds()};
            portENTER_CRITICAL(&s_lock);bool stats=s_have_stats;memcpy(payload+1,s_host_stats,3);portEXIT_CRITICAL(&s_lock);
            size_t n=lx_packet_encode(ack,sizeof(ack),LX_MSG_ACK,ack_item.sequence,ack_item.session,payload,stats?4:1);
            wire_send(ack,n);ack_waiting=false;
        } else if(!busy && radio_input_active()) {
            uint8_t control[LX_PACKET_MAX];size_t n=link_control_take(LX_CONTROL_RADIO,control);
            if(n) wire_send(control,n);
        }
        if(connected && esp_timer_get_time()-last>500000) {
            link_control_drop(LX_CONTROL_RADIO);
            state=(lx_key_report_t){0};usb_keys_submit(&state,0,esp_timer_get_time());connected=false;
            lx_mouse_sample_t release={0};usb_mouse_submit(&release);usb_consumer_submit(0);buttons=0;consumer=0;
            portENTER_CRITICAL(&s_lock);s_timeouts++;portEXIT_CRITICAL(&s_lock);
        }
    }
}
esp_err_t radio_keys_start(bool receiver)
{
    s_receiver=receiver;s_session=esp_random();
    uint8_t key[16];bool encrypted=false;
    if(!device_setup_peer(s_peer_storage,key,&encrypted)) return ESP_OK;
    s_peer=s_peer_storage;
    s_rx=xQueueCreate(32,sizeof(item_t));if(!s_rx) return ESP_ERR_NO_MEM;
    ESP_ERROR_CHECK(network_start(true));
    ESP_ERROR_CHECK(esp_now_init());
    esp_now_peer_info_t peer={.channel=0,.ifidx=WIFI_IF_STA};memcpy(peer.peer_addr,s_peer,6);
    peer.encrypt=encrypted;if(encrypted) { memcpy(peer.lmk,key,16);ESP_ERROR_CHECK(esp_now_set_pmk(key)); }
    memset(key,0,sizeof(key));
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    /* 6 Mbps A/B tests showed occasional application ACK loss under load.
     * Keep the stable 1 Mbps baseline for this hardware/environment. */
    ESP_ERROR_CHECK(radio_keys_rate(1));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(receive));
    ESP_ERROR_CHECK(esp_now_register_send_cb(sent_callback));
    if(xTaskCreatePinnedToCore(receiver?receiver_task:sender_task,"radio_keys",4096,NULL,11,&s_task,1)!=pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
void radio_keys_stats(void)
{
    uint8_t channel=0;wifi_second_chan_t secondary;esp_wifi_get_channel(&channel,&secondary);
    portENTER_CRITICAL(&s_lock);
    uint32_t sent=s_sent,received=s_received,retries=s_retries,dropped=s_dropped,timeouts=s_timeouts,full=s_full;
    portEXIT_CRITICAL(&s_lock);
    printf("LX_RADIO role=%s channel=%u rate_mbps=%u sent=%lu received=%lu retries=%lu unconfirmed=%lu timeouts=%lu queue_full=%lu leds=%u rssi=%d tx_failed=%lu api_failed=%lu tx_max_us=%lld\n",
        s_receiver?"receiver":"keyboard",channel,s_rate,(unsigned long)sent,(unsigned long)received,(unsigned long)retries,
        (unsigned long)dropped,(unsigned long)timeouts,(unsigned long)full,s_leds,s_rssi,(unsigned long)s_tx_failed,(unsigned long)s_api_failed,(long long)s_tx_max_us);
}
void radio_keys_trace_clear(void)
{
    if(!s_trace) { trace_t *trace=heap_caps_calloc(1200,sizeof(trace_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        portENTER_CRITICAL(&s_lock);s_trace=trace;portEXIT_CRITICAL(&s_lock); }
    portENTER_CRITICAL(&s_lock);s_trace_count=0;portEXIT_CRITICAL(&s_lock);
}
void radio_keys_trace_print(void)
{
    portENTER_CRITICAL(&s_lock);unsigned count=s_trace_count;portEXIT_CRITICAL(&s_lock);
    for(unsigned i=0;i<count;i++) {
        portENTER_CRITICAL(&s_lock);trace_t t=s_trace[i];portEXIT_CRITICAL(&s_lock);
        printf("LX_TRACE %lu %lld %lld %lld\n",(unsigned long)t.sequence,(long long)t.event_us,(long long)t.sent_us,(long long)t.done_us);
    }
    printf("LX_TRACE_END %u\n",count);
}
