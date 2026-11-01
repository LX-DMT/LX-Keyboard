#include <unistd.h>
#include "lwip/sockets.h"
#include "network_link.h"
#include "discovery_wire.h"
#include "device_pair.h"
#include "device_setup.h"
#include "link_auth.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

unsigned network_discovery_selftest(void)
{
    unsigned failures=0;
    uint8_t query[LX_DISCOVERY_SIZE+1]="LXDISC1?",reply[LX_DISCOVERY_SIZE+1]="LXDISC1!";
    memcpy(query+8,receiver_mac,6);memcpy(reply+8,receiver_mac,6);
    failures+=!lx_discovery_matches(query,14,false,receiver_mac);
    failures+=!lx_discovery_matches(reply,14,true,receiver_mac);
    failures+=lx_discovery_matches(query,14,true,receiver_mac);
    failures+=lx_discovery_matches(reply,14,false,receiver_mac);
    failures+=lx_discovery_matches(reply,14,true,keyboard_mac);
    for(size_t n=0;n<14;n++) failures+=lx_discovery_matches(reply,n,true,receiver_mac);
    failures+=lx_discovery_matches(reply,15,true,receiver_mac);
    reply[6]='2';failures+=lx_discovery_matches(reply,14,true,receiver_mac);
    uint8_t encoded[14];lx_discovery_encode(encoded,false,receiver_mac);
    failures+=memcmp(encoded,query,14)!=0;
    return failures;
}

static void discovery_task(void *arg)
{
    (void)arg;
    uint8_t mac[6];ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA,mac));
    int fd=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(fd<0) { vTaskDelete(NULL);return; }
    struct sockaddr_in local={.sin_family=AF_INET,.sin_port=htons(LX_DISCOVERY_PORT),.sin_addr={.s_addr=htonl(INADDR_ANY)}};
    if(bind(fd,(struct sockaddr*)&local,sizeof(local))<0) { close(fd);vTaskDelete(NULL);return; }
    while(1) {
        uint8_t bytes[96];struct sockaddr_in from;socklen_t size=sizeof(from);
        int n=recvfrom(fd,bytes,sizeof(bytes),0,(struct sockaddr*)&from,&size);
        if(n>0 && network_ready() && link_discovery_reply(bytes,n)) {
            sendto(fd,bytes,78,0,(struct sockaddr*)&from,size);
        } else if(n>0 && network_ready() && !device_setup_bound() && lx_discovery_matches(bytes,n,false,mac)) {
            lx_discovery_encode(bytes,true,mac);
            sendto(fd,bytes,LX_DISCOVERY_SIZE,0,(struct sockaddr*)&from,size);
        }
        if(n<0) vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t network_discovery_start(void)
{
    return xTaskCreatePinnedToCore(discovery_task,"net_discovery",3072,NULL,3,NULL,0)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;
}

uint32_t network_find_receiver(void)
{
    /* Only called before opening TCP. No discovery polling in the typing loop. */
    if(!network_ready()) return 0;
    uint8_t peer[6],key[32];bool encrypted=false;
    if(!device_setup_peer(peer,key,&encrypted)) return 0;
    memset(key,0,sizeof(key));if(encrypted) return link_find_bound_peer();
    int fd=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(fd<0) return 0;
    int one=1;setsockopt(fd,SOL_SOCKET,SO_BROADCAST,&one,sizeof(one));
    struct sockaddr_in broadcast={.sin_family=AF_INET,.sin_port=htons(LX_DISCOVERY_PORT),.sin_addr={.s_addr=htonl(INADDR_BROADCAST)}};
    uint8_t bytes[64];lx_discovery_encode(bytes,false,peer);
    int sent=sendto(fd,bytes,LX_DISCOVERY_SIZE,0,(struct sockaddr*)&broadcast,sizeof(broadcast));
    uint32_t address=0;int64_t until=esp_timer_get_time()+350000;
    while(sent==LX_DISCOVERY_SIZE && network_ready() && esp_timer_get_time()<until) {
        struct sockaddr_in from;socklen_t size=sizeof(from);
        int n=recvfrom(fd,bytes,sizeof(bytes),MSG_DONTWAIT,(struct sockaddr*)&from,&size);
        if(n>0 && from.sin_port==htons(LX_DISCOVERY_PORT) && lx_discovery_matches(bytes,n,true,peer)) {
            address=from.sin_addr.s_addr;break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    close(fd);return address;
}
