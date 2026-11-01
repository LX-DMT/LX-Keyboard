#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "link_auth.h"
#include "device_setup.h"
#include "mbedtls/md.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

bool link_hmac(const uint8_t *key,size_t key_size,const uint8_t *data,size_t size,uint8_t out[32])
{
    return mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),key,key_size,data,size,out)==0;
}
bool link_equal(const uint8_t *a,const uint8_t *b,size_t n)
{
    uint8_t difference=0;while(n--) difference|=*a++ ^ *b++;return difference==0;
}
static bool transfer(int fd,uint8_t *bytes,size_t size,bool sending)
{
    size_t at=0;int64_t end=esp_timer_get_time()+1500000;
    while(at<size && esp_timer_get_time()<end) {
        int n=sending?send(fd,bytes+at,size-at,MSG_DONTWAIT):recv(fd,bytes+at,size-at,MSG_DONTWAIT);
        if(n>0) at+=n;
        else if(n==0 || (errno!=EAGAIN && errno!=EWOULDBLOCK)) return false;
        else vTaskDelay(1);
    }
    return at==size;
}
/* Mutual challenge/response binds two fresh nonces, sender MAC and destination
 * identity. This authenticates the TCP peer, not encryption of application data. */
bool link_auth_socket(int fd,bool receiver)
{
    uint8_t key[32]={0},peer[6],target[16]={0},own[6];bool encrypted=false;size_t key_n=16;
    esp_read_mac(own,ESP_MAC_WIFI_STA);
    {
        if(!device_setup_peer(peer,key,&encrypted)) return false;
        if(!encrypted) return true;
        memcpy(target,receiver?own:peer,6);
    }
    uint8_t hello[42]={0},response[48],proof[32],transcript[59],expected[32];bool good=false;
    if(receiver) {
        if(!transfer(fd,hello,sizeof(hello),false) || memcmp(hello,"LXA2",4) || memcmp(hello+4,peer,6) || memcmp(hello+10,target,16)) goto done;
        esp_fill_random(response,16);
    } else {
        memcpy(hello,"LXA2",4);memcpy(hello+4,own,6);memcpy(hello+10,target,16);esp_fill_random(hello+26,16);
        if(!transfer(fd,hello,sizeof(hello),true) || !transfer(fd,response,sizeof(response),false)) goto done;
    }
    memcpy(transcript,hello,42);memcpy(transcript+42,response,16);transcript[58]='S';
    if(!link_hmac(key,key_n,transcript,59,expected)) goto done;
    if(receiver) { memcpy(response+16,expected,32);if(!transfer(fd,response,48,true)) goto done; }
    else if(!link_equal(response+16,expected,32)) goto done;
    transcript[58]='C';if(!link_hmac(key,key_n,transcript,59,expected)) goto done;
    if(receiver) { if(!transfer(fd,proof,32,false) || !link_equal(proof,expected,32)) goto done; }
    else { memcpy(proof,expected,32);if(!transfer(fd,proof,32,true)) goto done; }
    transcript[58]='A';if(!link_hmac(key,key_n,transcript,59,expected)) goto done;
    if(receiver) good=transfer(fd,expected,32,true);
    else good=transfer(fd,proof,32,false) && link_equal(proof,expected,32);
done:
    memset(key,0,sizeof(key));return good;
}
bool link_discovery_reply(uint8_t *bytes,size_t n)
{
    uint8_t own[16]={0},peer[6],key[16],tag[32];bool encrypted=false;
    if(n!=78 || memcmp(bytes,"LXDISC2?",8) || !device_setup_peer(peer,key,&encrypted) || !encrypted) return false;
    esp_read_mac(own,ESP_MAC_WIFI_STA);
    bool good=!memcmp(bytes+8,peer,6) && !memcmp(bytes+14,own,16) &&
        link_hmac(key,16,bytes,46,tag) && link_equal(tag,bytes+46,32);
    if(good) { bytes[7]='!';good=link_hmac(key,16,bytes,46,bytes+46); }
    memset(key,0,16);return good;
}
uint32_t link_find_bound_peer(void)
{
    uint8_t key[32]={0},target[16]={0},mac[6];bool encrypted=false;size_t key_n=16;
    if(!device_setup_peer(mac,key,&encrypted) || !encrypted) return 0;
    memcpy(target,mac,6);
    uint8_t query[78]={0},reply[79],tag[32];memcpy(query,"LXDISC2?",8);esp_read_mac(query+8,ESP_MAC_WIFI_STA);
    memcpy(query+14,target,16);esp_fill_random(query+30,16);
    if(!link_hmac(key,key_n,query,46,query+46)) { memset(key,0,32);return 0; }
    int fd=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(fd<0) { memset(key,0,32);return 0; }
    int one=1;setsockopt(fd,SOL_SOCKET,SO_BROADCAST,&one,sizeof(one));
    struct sockaddr_in broadcast={.sin_family=AF_INET,.sin_port=htons(39572),.sin_addr={.s_addr=htonl(INADDR_BROADCAST)}};
    sendto(fd,query,sizeof(query),0,(struct sockaddr*)&broadcast,sizeof(broadcast));
    int64_t end=esp_timer_get_time()+350000;uint32_t address=0;
    while(esp_timer_get_time()<end) {
        struct sockaddr_in from;socklen_t size=sizeof(from);int n=recvfrom(fd,reply,sizeof(reply),MSG_DONTWAIT,(struct sockaddr*)&from,&size);
        if(n==78 && from.sin_port==htons(39572) && !memcmp(reply,"LXDISC2!",8) && !memcmp(reply+8,query+8,38) &&
            link_hmac(key,key_n,reply,46,tag) && link_equal(tag,reply+46,32)) { address=from.sin_addr.s_addr;break; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    close(fd);memset(key,0,sizeof(key));return address;
}
