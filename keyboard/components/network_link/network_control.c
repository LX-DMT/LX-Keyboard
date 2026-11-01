#include <string.h>
#include <unistd.h>
#include "network_link.h"
#include "device_setup.h"
#include "lx_protocol.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "nvs.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

#if CONFIG_SPIRAM_FETCH_INSTRUCTIONS && CONFIG_SPIRAM_RODATA
#define CONTROL_STACK_CAPS (MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)
#else
#define CONTROL_STACK_CAPS (MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)
#endif

static bool s_started;
static esp_err_t (*s_mode_handler)(uint8_t mode);
static esp_err_t (*s_extra_handler)(uint8_t,const uint8_t *,size_t);
void network_set_mode_handler(esp_err_t (*handler)(uint8_t mode)) { s_mode_handler=handler; }
void network_set_extra_config_handler(esp_err_t (*handler)(uint8_t,const uint8_t *,size_t)) { s_extra_handler=handler; }
esp_err_t network_apply_mode(const uint8_t *p,size_t n)
{
    if(device_setup_busy()) return ESP_ERR_INVALID_STATE;
    if(n!=7 || p[0]!=32 || p[1]>3 || !s_extra_handler || !s_mode_handler) return ESP_ERR_INVALID_ARG;
    if(p[1]!=2) {
        if(p[2]!=255 || p[3] || p[4] || p[5] || p[6]) return ESP_ERR_INVALID_ARG;
    } else {
        if(p[2]>1 || (p[2] && (!p[3] || p[3]==127 || p[3]>=224))) return ESP_ERR_INVALID_ARG;
        if(!p[2] && (p[3] || p[4] || p[5] || p[6])) return ESP_ERR_INVALID_ARG;
        nvs_handle_t h;esp_err_t result=nvs_open("lx_net",NVS_READWRITE,&h);
        if(result!=ESP_OK) return result;
        if(p[2]) {
            char address[16];struct in_addr ip;memcpy(&ip.s_addr,p+3,4);
            inet_ntoa_r(ip,address,sizeof(address));result=nvs_set_str(h,"pc",address);
        }
        if(result==ESP_OK) result=nvs_set_str(h,"target",p[2]?"pc":"receiver");
        if(result==ESP_OK) result=nvs_commit(h);
        nvs_close(h);if(result!=ESP_OK) return result;
        /* Keep the current TCP destination until the mode task reboots.
         * Otherwise changing target would tear down the link before its ACK. */
    }
    return s_mode_handler(p[1]);
}
esp_err_t network_apply_config(const uint8_t *p,size_t n)
{
    if(device_setup_busy()) return ESP_ERR_INVALID_STATE;
    if(!n) return ESP_ERR_INVALID_ARG;
    if(p[0]==12 && n==2 && p[1]<=2) return s_extra_handler?s_extra_handler(p[0],p+1,1):ESP_ERR_NOT_SUPPORTED;
    if((p[0]==8 || p[0]==9) && n<=65) return s_extra_handler?s_extra_handler(p[0],p+1,n-1):ESP_ERR_NOT_SUPPORTED;
    if((p[0]==10 || p[0]==11) && n<=234) return s_extra_handler?s_extra_handler(p[0],p+1,n-1):ESP_ERR_NOT_SUPPORTED;
    if(p[0]==1 && n==2 && p[1]<=1) return network_setting("target",p[1]?"pc":"receiver");
    if(p[0]==6 && n==2 && p[1]<=3) return s_mode_handler?s_mode_handler(p[1]):ESP_ERR_NOT_SUPPORTED;
    if(p[0]==7 && n==1) return s_mode_handler?s_mode_handler(255):ESP_ERR_NOT_SUPPORTED;
    if(p[0]<2 || p[0]>5 || n>64 || memchr(p+1,0,n-1)) return ESP_ERR_INVALID_ARG;
    const char *keys[]={NULL,NULL,"ssid","pass","receiver","pc"};
    char value[64];memcpy(value,p+1,n-1);value[n-1]=0;
    esp_err_t result=network_setting(keys[p[0]],value);memset(value,0,sizeof(value));return result;
}
static void control_packet(const lx_packet_t *packet,void *arg)
{
    int fd=*(int*)arg;
    if(packet->type!=LX_MSG_CONFIG) return;
    /* A newly bound device never accepts unauthenticated LAN configuration. */
    esp_err_t result=device_setup_bound()?ESP_ERR_NOT_SUPPORTED:network_apply_config(packet->payload,packet->length);
    uint8_t status[2]={(uint8_t)result,(uint8_t)(result>>8)},bytes[18];
    size_t n=lx_packet_encode(bytes,sizeof(bytes),LX_MSG_ACK,packet->sequence,packet->session,status,2);
    send(fd,bytes,n,0);
}
static void control_task(void *arg)
{
    (void)arg;int server=socket(AF_INET,SOCK_STREAM,IPPROTO_IP);
    if(server<0) { s_started=false;vTaskDeleteWithCaps(NULL);return; }
    int one=1;setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(39571),.sin_addr={.s_addr=htonl(INADDR_ANY)}};
    if(bind(server,(struct sockaddr*)&address,sizeof(address))<0 || listen(server,1)<0) {
        close(server);s_started=false;vTaskDeleteWithCaps(NULL);return;
    }
    while(1) {
        int fd=accept(server,NULL,NULL);if(fd<0) { vTaskDelay(pdMS_TO_TICKS(100));continue; }
        struct timeval timeout={.tv_usec=500000};
        setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
        lx_stream_t stream={0};uint8_t bytes[250];int n;
        while((n=recv(fd,bytes,sizeof(bytes),0))>0) lx_stream_feed(&stream,bytes,n,control_packet,&fd);
        shutdown(fd,SHUT_RDWR);close(fd);
    }
}
esp_err_t network_control_start(void)
{
    if(s_started) return ESP_OK;
    s_started=true;
    if(xTaskCreatePinnedToCoreWithCaps(control_task,"net_control",4096,NULL,3,NULL,0,
        CONTROL_STACK_CAPS)!=pdPASS) { s_started=false;return ESP_ERR_NO_MEM; }
    return ESP_OK;
}
