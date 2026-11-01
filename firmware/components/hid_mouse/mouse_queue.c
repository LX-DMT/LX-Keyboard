#include <limits.h>
#include "mouse_queue.h"

static int32_t add(int32_t a,int32_t b)
{
    int64_t result=(int64_t)a+b;
    return result>INT32_MAX?INT32_MAX:result<INT32_MIN?INT32_MIN:(int32_t)result;
}
static int8_t part(int32_t value) { return value>127?127:value<-127?-127:(int8_t)value; }
void lx_mouse_queue_resync(lx_mouse_queue_t *q)
{
    q->head=0;q->count=1;q->items[0]=(lx_mouse_sample_t){0};
    if(q->latest_buttons) q->items[q->count++]=(lx_mouse_sample_t){.buttons=q->latest_buttons};
}
void lx_mouse_queue_push(lx_mouse_queue_t *q,const lx_mouse_sample_t *s)
{
    q->latest_buttons=s->buttons;
    if(q->count) {
        lx_mouse_sample_t *last=&q->items[(q->head+q->count-1)%16];
        if(last->buttons==s->buttons) {
            last->x=add(last->x,s->x);last->y=add(last->y,s->y);
            last->wheel=add(last->wheel,s->wheel);last->pan=add(last->pan,s->pan);return;
        }
    }
    if(q->count==16) { q->overflow++;lx_mouse_queue_resync(q);return; }
    q->items[(q->head+q->count++)%16]=*s;
}
bool lx_mouse_queue_take(lx_mouse_queue_t *q,lx_mouse_report_t *r)
{
    if(!q->count) return false;
    lx_mouse_sample_t *s=&q->items[q->head];
    *r=(lx_mouse_report_t){s->buttons,part(s->x),part(s->y),part(s->wheel),part(s->pan)};
    s->x-=r->x;s->y-=r->y;s->wheel-=r->wheel;s->pan-=r->pan;
    if(!s->x && !s->y && !s->wheel && !s->pan) { q->head=(q->head+1)%16;q->count--; }
    return true;
}
unsigned lx_mouse_queue_selftest(void)
{
    lx_mouse_queue_t q={0};lx_mouse_report_t r;unsigned failures=0;
    lx_mouse_sample_t s={.buttons=0x81,.x=-300,.y=450,.wheel=-1,.pan=2};
    lx_mouse_queue_push(&q,&s);
    s=(lx_mouse_sample_t){.buttons=0};lx_mouse_queue_push(&q,&s);
    int x=0,y=0,wheel=0,pan=0;unsigned n=0;
    while(lx_mouse_queue_take(&q,&r)) {
        x+=r.x;y+=r.y;wheel+=r.wheel;pan+=r.pan;
        if(n<4 && r.buttons!=0x81) failures++;
        if(n==4 && r.buttons!=0) failures++;
        n++;
    }
    if(n!=5 || x!=-300 || y!=450 || wheel!=-1 || pan!=2) failures++;
    for(unsigned i=0;i<17;i++) { s.buttons=(i%2)+1;lx_mouse_queue_push(&q,&s); }
    if(q.overflow!=1 || !lx_mouse_queue_take(&q,&r) || r.buttons) failures++;
    if(!lx_mouse_queue_take(&q,&r) || r.buttons!=1) failures++;
    return failures;
}
