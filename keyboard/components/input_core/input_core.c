#include <string.h>
#include "input_core.h"

/* Electrical scan order: U46..U55, each D7..D0, followed by the optional header.
 * Only the physical switch-to-keycap assignment is retained from the board map.
 * Values are USB HID Usage Page 7. Power/charging/status lines never become keys.
 * SW69 (bit 67) is PTT; bit 69 is the Fn/power switch; these emit no HID usage.
 */
static const uint8_t usage[LX_KEY_COUNT] = {
    0x29,0x3a,0x3b,0x3c,0x3d,0x3e,0x3f,0x40,
    0x41,0x42,0x43,0x44,0x45,0x2a,0x2e,0x2d,
    0x27,0x26,0x25,0x24,0x23,0x22,0x21,0x20,
    0x1f,0x1e,0x35,0x2b,0x14,0x1a,0x08,0x15,
    0x17,0x1c,0x18,0x0c,0x12,0x13,0x2f,0x30,
    0x31,0x28,0x34,0x33,0x0f,0x0e,0x0d,0x0b,
    0x0a,0x09,0x07,0x16,0x04,0x39,0xe1,0x1d,
    0x1b,0x06,0x19,0x05,0x11,0x10,0x36,0x37,
    0x38,0xe5,0xe4,0,0xe6,0,0x2c,0xe2,
    0xe3,0xe0,0,0,0,0,0,0,
    0,0,0x4c,0x52,0,0x50,0x51,0x4f,
};
uint8_t lx_key_usage(unsigned bit) { return bit<LX_KEY_COUNT?usage[bit]:0; }
bool lx_custom_key_valid(uint8_t action) { return action==LX_CUSTOM_POWER_PREVIEW || (action>=0x46 && action<=0x4e); }
bool lx_input_set_custom(lx_input_t *s,const uint8_t actions[LX_CUSTOM_KEY_COUNT])
{
    if(!actions || !lx_custom_key_valid(actions[0]) || !lx_custom_key_valid(actions[1])) return false;
    memcpy(s->custom_keys,actions,LX_CUSTOM_KEY_COUNT);return true;
}
static bool down(const uint8_t *data,unsigned bit) { return !(data[bit/8] & (0x80u>>(bit%8))); }
static void set(uint8_t *data,unsigned bit,bool pressed)
{
    uint8_t mask=0x80u>>(bit%8);
    if(pressed) data[bit/8]&=~mask; else data[bit/8]|=mask;
}
void lx_input_init(lx_input_t *s,bool extra_keys)
{
    memset(s,0,sizeof(*s));
    memset(s->candidate,0xff,sizeof(s->candidate));
    memset(s->stable,0xff,sizeof(s->stable));
    s->extra_keys=extra_keys;
    s->custom_keys[0]=0x4c;s->custom_keys[1]=LX_CUSTOM_POWER_PREVIEW;
}
bool lx_input_scan(lx_input_t *s,const uint8_t raw[LX_SCAN_BYTES],uint32_t now)
{
    bool was_down[]={down(s->stable,82),down(s->stable,84)};
    for(unsigned bit=0;bit<LX_KEY_COUNT;bit++) {
        bool value=down(raw,bit);
        if(value!=down(s->candidate,bit)) {
            set(s->candidate,bit,value);
            s->changed_at[bit]=now;
        }
        if(value!=down(s->stable,bit) && (uint32_t)(now-s->changed_at[bit])>=LX_DEBOUNCE_US)
            set(s->stable,bit,value);
    }
    s->ptt=down(s->stable,67);
    s->fn=down(s->stable,69);
    s->power_preview=false;
    for(unsigned i=0;i<LX_CUSTOM_KEY_COUNT;i++) {
        bool pressed=s->extra_keys && down(s->stable,82+i*2);
        /* Saving while held only affects the next press: always release the
         * original HID usage or power-preview action, never switch mid-hold. */
        if(pressed && !was_down[i]) s->held_custom_keys[i]=s->custom_keys[i];
        if(pressed && s->held_custom_keys[i]==LX_CUSTOM_POWER_PREVIEW) s->power_preview=true;
    }
    lx_key_report_t next={0};
    unsigned count=0;
    for(unsigned bit=0;bit<(s->extra_keys?LX_KEY_COUNT:74);bit++) {
        uint8_t key=usage[bit];
        if(bit==82 || bit==84) key=s->held_custom_keys[(bit-82)/2];
        if(!key || !down(s->stable,bit)) continue;
        if(key>=0xe0 && key<=0xe7) next.modifiers|=1u<<(key-0xe0);
        else {
            bool duplicate=false;
            for(unsigned i=0;i<count && i<6;i++) if(next.keys[i]==key) duplicate=true;
            if(duplicate) continue;
            if(count<6) next.keys[count]=key;
            count++;
        }
    }
    /* HID ErrorRollOver preserves modifiers until enough ordinary keys release. */
    if(count>6) memset(next.keys,1,sizeof(next.keys));
    if(!memcmp(&next,&s->report,sizeof(next))) return false;
    s->report=next;
    return true;
}

unsigned lx_input_selftest(void)
{
    unsigned failures=0;
    lx_input_t s; uint8_t raw[LX_SCAN_BYTES];
#define CHECK(value) do { if(!(value)) failures++; } while(0)
    lx_input_init(&s,false); memset(raw,0xff,sizeof(raw));
    CHECK(!lx_input_scan(&s,raw,0));
    set(raw,52,true); CHECK(!lx_input_scan(&s,raw,1000));
    set(raw,52,false); CHECK(!lx_input_scan(&s,raw,2000));
    set(raw,52,true); CHECK(!lx_input_scan(&s,raw,3000));
    CHECK(!lx_input_scan(&s,raw,7999));
    CHECK(lx_input_scan(&s,raw,8000) && s.report.keys[0]==4);
    CHECK(!lx_input_scan(&s,raw,900000)); /* long hold does not emit repeats */
    set(raw,73,true); lx_input_scan(&s,raw,901000);
    CHECK(lx_input_scan(&s,raw,906000) && s.report.modifiers==1 && s.report.keys[0]==4);
    set(raw,52,false); lx_input_scan(&s,raw,907000);
    CHECK(lx_input_scan(&s,raw,912000) && s.report.modifiers==1 && s.report.keys[0]==0);
    set(raw,73,false); lx_input_scan(&s,raw,913000);
    CHECK(lx_input_scan(&s,raw,918000) && s.report.modifiers==0);
    memset(raw,0, sizeof(raw)); lx_input_scan(&s,raw,919000); lx_input_scan(&s,raw,924000);
    CHECK(s.report.keys[0]==1 && s.report.keys[5]==1 && s.report.modifiers==0x7f);
    memset(raw,0xff,sizeof(raw)); lx_input_scan(&s,raw,925000); lx_input_scan(&s,raw,930000);
    CHECK(s.report.keys[0]==0 && s.report.modifiers==0);
    /* Status bits and absent expansion must not create phantom modifiers/keys. */
    for(unsigned bit=74;bit<88;bit++) set(raw,bit,true);
    set(raw,69,true); set(raw,67,true);
    lx_input_scan(&s,raw,931000);
    CHECK(!lx_input_scan(&s,raw,936000) && s.fn && s.ptt);
    lx_input_init(&s,false); memset(raw,0xff,sizeof(raw)); set(raw,52,true);
    lx_input_scan(&s,raw,UINT32_MAX-2000);
    CHECK(lx_input_scan(&s,raw,3000) && s.report.keys[0]==4); /* timer wrap */
    /* FPC9/U1 D5..D0: Delete, Up, power preview, Left, Down, Right.
     * D7/D6 (raw 80/81) are unconnected and must never create keys. */
    const uint8_t extra_usages[]={0x4c,0x52,0,0x50,0x51,0x4f};
    for(unsigned i=0;i<6;i++) {
        lx_input_init(&s,true);memset(raw,0xff,sizeof(raw));
        set(raw,80,true);set(raw,81,true);set(raw,82+i,true);
        CHECK(!lx_input_scan(&s,raw,1000));
        CHECK(!lx_input_scan(&s,raw,5999) && !s.power_preview);
        lx_input_scan(&s,raw,6000);
        CHECK(s.report.keys[0]==extra_usages[i] && s.report.keys[1]==0);
        CHECK(s.power_preview==(i==2) && s.report.modifiers==0);
        set(raw,82+i,false);lx_input_scan(&s,raw,7000);
        lx_input_scan(&s,raw,12000);
        CHECK(s.report.keys[0]==0 && !s.power_preview);
    }
    lx_input_init(&s,true);memset(raw,0xff,sizeof(raw));
    set(raw,52,true);set(raw,73,true); /* Ctrl+A alongside all six switches. */
    for(unsigned bit=82;bit<88;bit++) set(raw,bit,true);
    lx_input_scan(&s,raw,1000);lx_input_scan(&s,raw,6000);
    const uint8_t combined[]={4,0x4c,0x52,0x50,0x51,0x4f};
    CHECK(s.report.modifiers==1 && !memcmp(s.report.keys,combined,6) && s.power_preview);
    /* Every offered action on either physical custom key, modifier chords,
     * release, same-function overlap and remapping while held. */
    const uint8_t actions[]={0,0x46,0x47,0x48,0x49,0x4a,0x4b,0x4c,0x4d,0x4e};
    for(unsigned slot=0;slot<2;slot++) for(unsigned i=0;i<sizeof(actions);i++) {
        lx_input_init(&s,true);memset(raw,0xff,sizeof(raw));
        uint8_t map[]={0x4c,0};map[slot]=actions[i];CHECK(lx_input_set_custom(&s,map));
        set(raw,82+2*slot,true);set(raw,73,true);
        lx_input_scan(&s,raw,1000);lx_input_scan(&s,raw,6000);
        CHECK(s.report.keys[0]==actions[i] && s.report.modifiers==1 && s.power_preview==(actions[i]==0));
        map[slot]=actions[i]==0x4a?0x4d:0x4a;CHECK(lx_input_set_custom(&s,map));
        CHECK(!lx_input_scan(&s,raw,7000) && s.report.keys[0]==actions[i] && s.power_preview==(actions[i]==0));
        set(raw,82+2*slot,false);lx_input_scan(&s,raw,8000);lx_input_scan(&s,raw,13000);
        CHECK(s.report.keys[0]==0 && !s.power_preview && s.report.modifiers==1);
        set(raw,82+2*slot,true);lx_input_scan(&s,raw,14000);lx_input_scan(&s,raw,19000);
        CHECK(s.report.keys[0]==map[slot] && !s.power_preview);
    }
    lx_input_init(&s,true);memset(raw,0xff,sizeof(raw));
    uint8_t same[]={0x4a,0x4a};CHECK(lx_input_set_custom(&s,same));
    set(raw,82,true);set(raw,84,true);lx_input_scan(&s,raw,1000);lx_input_scan(&s,raw,6000);
    CHECK(s.report.keys[0]==0x4a && s.report.keys[1]==0);
    set(raw,82,false);lx_input_scan(&s,raw,7000);lx_input_scan(&s,raw,12000);
    CHECK(s.report.keys[0]==0x4a && s.report.keys[1]==0);
    set(raw,84,false);lx_input_scan(&s,raw,13000);lx_input_scan(&s,raw,18000);
    CHECK(s.report.keys[0]==0);
    uint8_t invalid[]={0xe0,0x4c};CHECK(!lx_input_set_custom(&s,invalid) && s.custom_keys[0]==0x4a);
    CHECK(!lx_custom_key_valid(0x45) && !lx_custom_key_valid(0x4f));
#undef CHECK
    return failures;
}
