#include <string.h>
#include "lx_protocol.h"
static uint16_t crc16(const uint8_t *p,size_t n)
{
    uint16_t crc=0xffff;
    while(n--) { crc^=(uint16_t)*p++<<8; for(unsigned i=0;i<8;i++) crc=(crc&0x8000)?(crc<<1)^0x1021:crc<<1; }
    return crc;
}
static uint32_t get32(const uint8_t *p) { return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void put32(uint8_t *p,uint32_t v) { for(unsigned i=0;i<4;i++) p[i]=v>>(8*i); }
size_t lx_packet_encode(uint8_t *out,size_t capacity,uint8_t type,uint32_t sequence,
                        uint32_t session,const void *payload,uint16_t length)
{
    size_t total=16+length;
    if(length>LX_PAYLOAD_MAX || capacity<total || (length && !payload)) return 0;
    out[0]='L';out[1]='X';out[2]=2;out[3]=type;out[4]=length;out[5]=length>>8;
    put32(out+6,sequence);put32(out+10,session);
    if(length) memcpy(out+14,payload,length);
    uint16_t crc=crc16(out,total-2);out[total-2]=crc;out[total-1]=crc>>8;
    return total;
}
bool lx_packet_decode(const uint8_t *data,size_t n,lx_packet_t *out)
{
    if(n<16 || n>LX_PACKET_MAX || data[0]!='L' || data[1]!='X' || data[2]!=2) return false;
    unsigned size=data[4]|(unsigned)data[5]<<8;
    if(n!=16+size || crc16(data,n-2)!=(data[n-2]|(uint16_t)data[n-1]<<8)) return false;
    *out=(lx_packet_t){data[3],size,get32(data+6),get32(data+10),data+14};
    return true;
}
void lx_stream_feed(lx_stream_t *s,const uint8_t *data,size_t length,lx_packet_callback_t callback,void *arg)
{
    for(size_t i=0;i<length;i++) {
        s->bytes[s->length++]=data[i];
        while(s->length>=16) {
            size_t payload=s->bytes[4]|(size_t)s->bytes[5]<<8,total=16+payload;
            bool header=s->bytes[0]=='L' && s->bytes[1]=='X' && s->bytes[2]==2 && payload<=LX_PAYLOAD_MAX;
            if(header && s->length<total) break;
            lx_packet_t packet;size_t used=1;
            if(header && lx_packet_decode(s->bytes,total,&packet)) { callback(&packet,arg);used=total; }
            memmove(s->bytes,s->bytes+used,s->length-used);s->length-=used;
        }
    }
}
static void count_packet(const lx_packet_t *packet,void *arg) { (void)packet;(*(unsigned*)arg)++; }
unsigned lx_protocol_selftest(void)
{
    uint8_t data[LX_PACKET_MAX],payload[]={1,2,3,4,5,6,7,8};lx_packet_t p;
    size_t n=lx_packet_encode(data,sizeof(data),LX_MSG_KEYBOARD,0x12345678,0x10203040,payload,8);
    unsigned errors=0;
    const uint8_t golden[]={0x4c,0x58,2,1,8,0,0x78,0x56,0x34,0x12,0x40,0x30,0x20,0x10,1,2,3,4,5,6,7,8,0x3b,0xe3};
    if(n!=sizeof(golden) || memcmp(data,golden,sizeof(golden))) errors++;
    if(n!=24 || !lx_packet_decode(data,n,&p) || p.sequence!=0x12345678 || p.session!=0x10203040 || memcmp(p.payload,payload,8)) errors++;
    lx_stream_t stream={0};unsigned count=0;
    for(unsigned i=0;i<n;i++) lx_stream_feed(&stream,data+i,1,count_packet,&count);
    if(count!=1 || stream.length) errors++;
    for(unsigned i=0;i<n;i++) if(lx_packet_decode(data,i,&p)) errors++;
    data[14]^=1;if(lx_packet_decode(data,n,&p)) errors++;
    lx_stream_feed(&stream,data,n,count_packet,&count);
    lx_stream_feed(&stream,golden,sizeof(golden),count_packet,&count);
    if(count!=2) errors++;
    if(lx_packet_encode(data,16,1,0,0,payload,8)) errors++;
    return errors;
}
