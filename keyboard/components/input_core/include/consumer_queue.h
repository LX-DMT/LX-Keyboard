#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Caller supplies its transport lock. Single 16-bit Consumer usage per report. */
typedef struct { uint16_t items[8],latest; unsigned head,count; } lx_consumer_queue_t;
static inline void lx_consumer_resync(lx_consumer_queue_t *q)
{
    q->head=0;q->count=1;q->items[0]=0;
    if(q->latest) q->items[q->count++]=q->latest;
}
static inline void lx_consumer_push(lx_consumer_queue_t *q,uint16_t usage)
{
    if(usage==q->latest) return;
    q->latest=usage;
    if(q->count==8) lx_consumer_resync(q);
    else { q->items[(q->head+q->count)%8]=usage;q->count++; }
}
static inline bool lx_consumer_take(lx_consumer_queue_t *q,uint16_t *usage)
{
    if(!q->count) return false;
    *usage=q->items[q->head];q->head=(q->head+1)%8;q->count--;return true;
}
static inline unsigned lx_consumer_selftest(void)
{
    lx_consumer_queue_t q={0};uint16_t v=0;unsigned fail=0;
    lx_consumer_push(&q,0xe9);lx_consumer_push(&q,0xe9);lx_consumer_push(&q,0);
    fail+=!lx_consumer_take(&q,&v)||v!=0xe9;
    fail+=!lx_consumer_take(&q,&v)||v!=0;
    fail+=lx_consumer_take(&q,&v);
    for(unsigned i=1;i<=9;i++) lx_consumer_push(&q,i);
    fail+=!lx_consumer_take(&q,&v)||v!=0;
    fail+=!lx_consumer_take(&q,&v)||v!=9;
    lx_consumer_push(&q,0);lx_consumer_resync(&q);
    fail+=!lx_consumer_take(&q,&v)||v!=0;
    fail+=lx_consumer_take(&q,&v);
    return fail;
}
