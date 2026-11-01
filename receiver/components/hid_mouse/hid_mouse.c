#include <string.h>
#include "hid_mouse.h"
typedef struct { uint32_t page,size,count;int32_t minimum;uint8_t id; } global_t;
enum { FIELD_X=9,FIELD_Y,FIELD_WHEEL,FIELD_PAN };
static uint32_t usage_value(uint32_t value,unsigned size,uint32_t page) { return size==4?value:(page<<16)|value; }
static int report_index(lx_mouse_map_t *m,uint8_t id)
{
    for(unsigned i=0;i<m->reports;i++) if(m->ids[i]==id) return i;
    if(m->reports==8) return -1;
    m->ids[m->reports]=id;return m->reports++;
}
bool lx_mouse_parse(lx_mouse_map_t *m,const uint8_t *desc,size_t length)
{
    memset(m,0,sizeof(*m));
    global_t g={0},stack[4];unsigned stack_count=0,depth=0,usage_count=0;
    uint32_t usages[16],usage_min=0,usage_max=0;bool range=false,mouse[8]={0},axis=false;
    for(size_t at=0;at<length;) {
        uint8_t prefix=desc[at++];
        if(prefix==0xfe) return false; /* Long/vendor items are outside initial scope. */
        unsigned n=prefix&3;if(n==3) n=4;
        unsigned type=(prefix>>2)&3,tag=prefix>>4;
        if(n>length-at) return false;
        uint32_t value=0;for(unsigned i=0;i<n;i++) value|=(uint32_t)desc[at++]<<(8*i);
        int64_t signed_value=value;
        if(n && (value&(1u<<(n*8-1)))) signed_value-=(int64_t)1<<(n*8);
        if(type==1) {
            switch(tag) {
            case 0:if(value>0xffff) return false;g.page=value;break;
            case 1:g.minimum=(int32_t)signed_value;break;
            case 7:g.size=value;break;
            case 8:if(!value || value>255) return false;g.id=value;m->report_ids=true;break;
            case 9:g.count=value;break;
            case 10:if(stack_count==4) return false;stack[stack_count++]=g;break;
            case 11:if(!stack_count) return false;g=stack[--stack_count];break;
            default:break;
            }
        } else if(type==2) {
            if(tag==0) { if(usage_count==16) return false;usages[usage_count++]=usage_value(value,n,g.page); }
            else if(tag==1) { usage_min=usage_value(value,n,g.page);range=true; }
            else if(tag==2) usage_max=usage_value(value,n,g.page);
            else if(tag==10) return false; /* Delimiter alternatives unsupported. */
        } else if(type==0) {
            if(tag==10) {
                if(depth==8) return false;
                uint32_t collection=usage_count?usages[0]:usage_min;
                mouse[depth]=(depth && mouse[depth-1]) || collection==0x00010002;
                depth++;
            } else if(tag==12) {
                if(!depth) return false;
                depth--;
            } else if(tag==8) {
                if(!g.size || g.size>32 || !g.count || g.count>512/g.size) return false;
                int r=report_index(m,g.id);if(r<0 || m->bits[r]+g.size*g.count>512) return false;
                if(depth && mouse[depth-1] && !(value&1) && (value&2)) {
                    for(unsigned i=0;i<g.count;i++) {
                        uint32_t usage=i<usage_count?usages[i]:range&&usage_min<=usage_max?
                            usage_min+(i<usage_max-usage_min?i:usage_max-usage_min):usage_count?usages[usage_count-1]:0;
                        uint8_t kind=0;
                        if((usage>>16)==9 && (usage&0xffff)>=1 && (usage&0xffff)<=8) kind=usage&0xff;
                        if(value&4) {
                            if(usage==0x00010030) kind=FIELD_X;
                            else if(usage==0x00010031) kind=FIELD_Y;
                            else if(usage==0x00010038) kind=FIELD_WHEEL;
                            else if(usage==0x000c0238) kind=FIELD_PAN;
                        }
                        if(kind) {
                            if(m->count==32) return false;
                            m->fields[m->count++]=(lx_mouse_field_t){m->bits[r]+i*g.size,g.id,g.size,kind,g.minimum<0};
                            axis|=kind==FIELD_X || kind==FIELD_Y;
                        }
                    }
                }
                m->bits[r]+=g.size*g.count;
            }
            usage_count=0;range=false;usage_min=usage_max=0;
        }
    }
    if(depth || stack_count || !axis) return false;
    if(m->report_ids) for(unsigned i=0;i<m->reports;i++) if(!m->ids[i]) return false;
    return true;
}
bool lx_mouse_decode(const lx_mouse_map_t *m,const uint8_t *data,size_t n,lx_mouse_sample_t *out)
{
    *out=(lx_mouse_sample_t){0};
    if(!n) return false;
    uint8_t id=m->report_ids?*data++:0;if(m->report_ids) n--;
    int report=-1;for(unsigned i=0;i<m->reports;i++) if(m->ids[i]==id) report=i;
    if(report<0 || n*8<m->bits[report]) return false;
    bool recognized=false;
    for(unsigned i=0;i<m->count;i++) {
        const lx_mouse_field_t *f=&m->fields[i];if(f->report_id!=id) continue;
        uint32_t raw=0;
        for(unsigned bit=0;bit<f->width;bit++) raw|=(uint32_t)((data[(f->bit+bit)/8]>>((f->bit+bit)%8))&1)<<bit;
        int64_t value=raw;
        if(f->signed_value && (raw&(1u<<(f->width-1)))) value-=(int64_t)1<<f->width;
        recognized=true;
        if(f->kind<=8) { uint8_t mask=1u<<(f->kind-1);out->button_mask|=mask;if(value) out->buttons|=mask; }
        else if(f->kind==FIELD_X) out->x=(int32_t)value;
        else if(f->kind==FIELD_Y) out->y=(int32_t)value;
        else if(f->kind==FIELD_WHEEL) out->wheel=(int32_t)value;
        else if(f->kind==FIELD_PAN) out->pan=(int32_t)value;
    }
    return recognized;
}
unsigned lx_mouse_selftest(void)
{
    /* Independently specified USB HID examples, not captures claimed as hardware tests. */
    static const uint8_t boot[]={5,1,9,2,0xa1,1,9,1,0xa1,0,5,9,0x19,1,0x29,3,
        0x15,0,0x25,1,0x95,3,0x75,1,0x81,2,0x95,1,0x75,5,0x81,3,
        5,1,9,0x30,9,0x31,9,0x38,0x15,0x81,0x25,0x7f,0x75,8,0x95,3,0x81,6,0xc0,0xc0};
    static const uint8_t wide[]={5,1,9,2,0xa1,1,0x85,7,5,9,0x19,1,0x29,8,
        0x15,0,0x25,1,0x75,1,0x95,8,0x81,2,5,1,9,0x30,9,0x31,
        0x16,1,0x80,0x26,0xff,0x7f,0x75,16,0x95,2,0x81,6,
        9,0x38,0x15,0x81,0x25,0x7f,0x75,8,0x95,1,0x81,6,
        5,0x0c,0x0a,0x38,2,0x81,6,0xc0};
    static const uint8_t packed[]={5,1,9,2,0xa1,1,5,9,0x19,1,0x29,8,0x15,0,0x25,1,
        0x75,1,0x95,8,0x81,2,5,1,9,0x30,9,0x31,0x16,1,0xf8,0x26,0xff,7,
        0x75,12,0x95,2,0x81,6,0xc0};
    /* Buttons and motion on separate IDs, plus a non-mouse Consumer report.
     * Motion-only reports must not clear held buttons from the other ID. */
    static const uint8_t split[]={5,1,9,2,0xa1,1,0x85,1,5,9,0x19,1,0x29,8,
        0x15,0,0x25,1,0x75,1,0x95,8,0x81,2,0xa4,0x85,2,5,1,9,0x30,9,0x31,
        0x15,0x81,0x25,0x7f,0x75,8,0x95,2,0x81,6,0xb4,0xc0,
        5,0x0c,9,1,0xa1,1,0x85,3,9,0xe9,0x75,8,0x95,1,0x81,2,0xc0};
    lx_mouse_map_t map;lx_mouse_sample_t sample;unsigned failures=0;
#define CHECK(x) do { if(!(x)) failures++; } while(0)
    const uint8_t b[]={5,0xfb,7,0xff},w[]={7,0x91,0xd4,0xfe,0xc2,1,0xff,2},p[]={0x80,0x85,0x5f,0x34};
    CHECK(lx_mouse_parse(&map,boot,sizeof(boot)));
    CHECK(lx_mouse_decode(&map,b,sizeof(b),&sample) && sample.buttons==5 && sample.button_mask==7 && sample.x==-5 && sample.y==7 && sample.wheel==-1);
    CHECK(!lx_mouse_decode(&map,b,2,&sample));
    CHECK(lx_mouse_parse(&map,wide,sizeof(wide)));
    CHECK(lx_mouse_decode(&map,w,sizeof(w),&sample) && sample.buttons==0x91 && sample.x==-300 && sample.y==450 && sample.wheel==-1 && sample.pan==2);
    CHECK(!lx_mouse_decode(&map,b,sizeof(b),&sample));
    CHECK(lx_mouse_parse(&map,packed,sizeof(packed)));
    CHECK(lx_mouse_decode(&map,p,sizeof(p),&sample) && sample.buttons==0x80 && sample.x==-123 && sample.y==837);
    CHECK(lx_mouse_parse(&map,split,sizeof(split)));
    const uint8_t buttons[]={1,0x19},motion[]={2,0xf9,11},consumer[]={3,1};
    CHECK(lx_mouse_decode(&map,buttons,sizeof(buttons),&sample) && sample.buttons==0x19 && sample.button_mask==0xff);
    uint8_t held=sample.buttons;
    CHECK(lx_mouse_decode(&map,motion,sizeof(motion),&sample) && !sample.button_mask && sample.x==-7 && sample.y==11);
    CHECK(((held & ~sample.button_mask) | sample.buttons)==0x19);
    CHECK(!lx_mouse_decode(&map,consumer,sizeof(consumer),&sample));
    CHECK(!lx_mouse_decode(&map,motion,2,&sample));
    for(unsigned n=0;n<sizeof(wide);n++) CHECK(!lx_mouse_parse(&map,wide,n));
#undef CHECK
    return failures;
}
