#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "voice_duplex.h"
#include "board.h"
#include "esp_aec.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"

/* The worker owns the codec lock. Network/Opus delays cannot stop ADC capture.
 * Hardware MIC3 reference and MIC1 arrive in the same ES7210 TDM frame. */
#define PCM_BYTES 19200
/* Three 60 ms Opus frames absorb packet jitter before the DAC starts. */
#define PLAY_PREFILL_SAMPLES 2880
static size_t playout_take(bool *buffering,size_t available,bool finished)
{
    if(!available) { *buffering=true;return 0; }
    if(*buffering && available<PLAY_PREFILL_SAMPLES && !finished)return 0;
    size_t n=available<240?available:240;
    *buffering=n<240;return n;
}
struct voice_duplex {
    aec_handle_t *aec;unsigned chunk;
    int16_t *aligned;
    StreamBufferHandle_t in,out;StaticStreamBuffer_t in_state,out_state;
    uint8_t in_bytes[PCM_BYTES+1],out_bytes[PCM_BYTES+1];
    SemaphoreHandle_t ready,done;
    volatile bool stop,clear;
    atomic_bool playback_finished;
    volatile esp_err_t error;
    unsigned frames,max_us,in_overflow,played;uint64_t total_us;
    unsigned play_peak,play_clipped,gap_samples,gap_events,gap_max,gap_pending;
    unsigned rx_overflows;int64_t elapsed_us;
    uint64_t raw_energy,clean_energy,ref_energy;unsigned measured,ref_peak,ref_clip;
    uint64_t raw_bins[16],clean_bins[16],ref_bins[16];unsigned bin_samples[16];
};
static aec_handle_t *create_aec(void)
{
    aec_config_t config={.mic_num=1,.ref_num=1,.out_num=1,.filter_length=4,.sample_rate=16000,
        .caps=MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT,.mode=AEC_MODE_FD_LOW_COST,.nlp_level=AEC_NLP_LEVEL_AGGR};
    return aec_create_from_config(&config);
}
static void worker(void *arg)
{
    voice_duplex_t *d=arg;
    d->error=board_audio_duplex_begin();xSemaphoreGive(d->ready);
    if(d->error!=ESP_OK) goto done;
    unsigned used=0,rx_start=board_audio_rx_overflows();
    int64_t started=esp_timer_get_time();
    int16_t mic[240],ref[240],play[240];
    bool buffering=true;
    int16_t *a=d->aligned,*b=a+d->chunk,*c=b+d->chunk;
    while(!d->stop) {
        if(d->clear) {
            while(xStreamBufferReceive(d->out,play,sizeof(play),0)) {}
            buffering=true;
            d->clear=false;
        }
        size_t take=playout_take(&buffering,xStreamBufferBytesAvailable(d->out)/2,
                                atomic_load(&d->playback_finished));
        size_t n=take?xStreamBufferReceive(d->out,play,take*2,0):0;
        if(n) {
            if(d->gap_pending) {
                d->gap_events++;d->gap_samples+=d->gap_pending;
                if(d->gap_pending>d->gap_max)d->gap_max=d->gap_pending;
                if(d->gap_events<=16)printf("LX_PLAY_GAP at_ms=%lld samples=%u\n",
                    (long long)(esp_timer_get_time()/1000),d->gap_pending);
                d->gap_pending=0;
            }
            for(unsigned i=0;i<n/2;i++) {
                unsigned v=abs((int)play[i]);if(v>d->play_peak)d->play_peak=v;
                if(v>=32760)d->play_clipped++;
            }
        }
        if(d->played && n<sizeof(play))d->gap_pending+=(sizeof(play)-n)/2;
        if(n<sizeof(play)) memset((uint8_t*)play+n,0,sizeof(play)-n);
        if(n) d->played+=n/2;
        d->error=board_audio_play_write(play,240);if(d->error!=ESP_OK) break;
        d->error=board_audio_duplex_read(mic,ref);if(d->error!=ESP_OK) break;
        for(unsigned i=0;i<240;i++) {
            a[used]=mic[i];b[used]=ref[i];used++;
            if(used!=d->chunk) continue;
            uint64_t re=0,raw=0;
            unsigned peak=0,clip=0;
            for(unsigned k=0;k<d->chunk;k++) {
                re+=(int64_t)b[k]*b[k];raw+=(int64_t)a[k]*a[k];
                unsigned v=abs((int)b[k]);if(v>peak)peak=v;if(v>=32760)clip++;
            }
            int64_t begin=esp_timer_get_time();aec_process(d->aec,a,b,c);
            unsigned us=esp_timer_get_time()-begin;if(us>d->max_us) d->max_us=us;
            d->total_us+=us;
            uint64_t clean=0;
            for(unsigned k=0;k<d->chunk;k++) clean+=(int64_t)c[k]*c[k];
            unsigned bin=d->frames/32;
            if(bin<16) {
                d->raw_bins[bin]+=raw;d->ref_bins[bin]+=re;d->clean_bins[bin]+=clean;
                d->bin_samples[bin]+=d->chunk;
            }
            d->frames++;used=0;
            /* Report far-end-active energy after initial filter convergence.
             * These are diagnostics, not an acoustic pass/fail verdict. */
            if(d->frames*d->chunk>32000 && re/d->chunk>100) {
                d->clean_energy+=clean;
                d->raw_energy+=raw;if(peak>d->ref_peak)d->ref_peak=peak;d->ref_clip+=clip;
                d->ref_energy+=re;d->measured+=d->chunk;
            }
            if(xStreamBufferSend(d->in,c,d->chunk*2,0)!=d->chunk*2) {
                d->in_overflow++;d->error=ESP_ERR_TIMEOUT;goto release;
            }
        }
    }
release:
    d->elapsed_us=esp_timer_get_time()-started;
    d->rx_overflows=board_audio_rx_overflows()-rx_start;
    board_audio_duplex_end();
done:
    xSemaphoreGive(d->done);vTaskDeleteWithCaps(NULL);
}
voice_duplex_t *voice_duplex_start(void)
{
    const char *stage="context";
    voice_duplex_t *d=heap_caps_calloc(1,sizeof(*d),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!d) { puts("LX_AEC init_failed=context");return NULL; }
    atomic_init(&d->playback_finished,false);
    stage="engine";
    d->aec=create_aec();if(!d->aec) goto fail;
    d->chunk=aec_get_chunksize(d->aec);
    if(!d->chunk || d->chunk>1024 || d->chunk%8) goto fail;
    stage="buffers";
    /* Keep hot AEC scratch internal. Prefer an internal worker stack, but TLS
     * and Bluetooth can leave less than 8 KiB contiguous during a real call. */
    d->aligned=heap_caps_aligned_calloc(16,d->chunk*3,sizeof(int16_t),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    d->in=xStreamBufferCreateStatic(PCM_BYTES+1,1,d->in_bytes,&d->in_state);
    d->out=xStreamBufferCreateStatic(PCM_BYTES+1,1,d->out_bytes,&d->out_state);
    d->ready=xSemaphoreCreateBinary();d->done=xSemaphoreCreateBinary();
    if(!d->aligned || !d->in || !d->out || !d->ready || !d->done) goto fail;
    stage="task";
    /* AEC uses core 1's spare cycles, below key scan (12) and key send (11).
     * Opus/TLS remain on core 0 so full-duplex DSP does not serialize there. */
    bool external_stack=false;
    if(xTaskCreatePinnedToCoreWithCaps(worker,"audio_duplex",8192,d,5,NULL,1,
                                    MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)!=pdPASS) {
        /* This worker does no flash/NVS writes. Its DSP engine already uses
         * PSRAM; retain the full stack size instead of shrinking it unsafely. */
        external_stack=true;
        if(xTaskCreatePinnedToCoreWithCaps(worker,"audio_duplex",8192,d,5,NULL,1,
                                        MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS) goto fail;
    }
    xSemaphoreTake(d->ready,portMAX_DELAY);
    if(d->error!=ESP_OK) { voice_duplex_stop(d);return NULL; }
    printf("LX_AEC started=1 chunk=%u reference=MIC3 mic=MIC1 core=1 priority=5 stack=%s\n",
        d->chunk,external_stack?"psram":"internal");
    return d;
fail:
    printf("LX_AEC init_failed=%s internal=%u largest=%u psram=%u\n",stage,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if(d->aec) aec_destroy(d->aec);
    if(d->in)vStreamBufferDelete(d->in);
    if(d->out)vStreamBufferDelete(d->out);
    if(d->ready)vSemaphoreDelete(d->ready);
    if(d->done)vSemaphoreDelete(d->done);
    free(d->aligned);free(d);return NULL;
}
esp_err_t voice_duplex_read(voice_duplex_t *d,int16_t pcm[320])
{
    size_t used=0;
    while(used<640 && d->error==ESP_OK) {
        size_t n=xStreamBufferReceive(d->in,(uint8_t*)pcm+used,640-used,pdMS_TO_TICKS(150));
        if(!n) return ESP_ERR_TIMEOUT;
        used+=n;
    }
    return d->error;
}
esp_err_t voice_duplex_write(voice_duplex_t *d,const int16_t *pcm,size_t n)
{
    if(d->error!=ESP_OK) return d->error;
    if(xStreamBufferSpacesAvailable(d->out)<n*2) return ESP_ERR_TIMEOUT;
    return xStreamBufferSend(d->out,pcm,n*2,0)==n*2?ESP_OK:ESP_FAIL;
}
void voice_duplex_clear(voice_duplex_t *d)
{
    atomic_store(&d->playback_finished,false);
    d->clear=true;
    while(d->clear && d->error==ESP_OK && !d->stop) vTaskDelay(1);
}
void voice_duplex_reply(voice_duplex_t *d,bool finished)
{
    atomic_store(&d->playback_finished,finished);
}
size_t voice_duplex_pending(voice_duplex_t *d) { return xStreamBufferBytesAvailable(d->out)/2; }
bool voice_duplex_has_space(voice_duplex_t *d) { return xStreamBufferSpacesAvailable(d->out)>=1920*2; }
esp_err_t voice_duplex_drain(voice_duplex_t *d,bool (*cancelled)(void))
{
    voice_duplex_reply(d,true);
    int16_t discard[320];int64_t until=esp_timer_get_time()+2000000;
    while(xStreamBufferBytesAvailable(d->out) && !cancelled() && esp_timer_get_time()<until) {
        esp_err_t rc=voice_duplex_read(d,discard);if(rc!=ESP_OK)return rc;
    }
    return xStreamBufferBytesAvailable(d->out)?ESP_ERR_TIMEOUT:ESP_OK;
}
/* No hardware or playback: verify short tails, restart after starvation, and
 * a 100-packet stream with up to 90 ms arrival jitter at the 15 ms DAC cadence. */
unsigned voice_playout_selftest(void)
{
    unsigned errors=0;bool buffering=true;
    #define CHECK(condition) do { if(!(condition))errors++; } while(0)
    CHECK(playout_take(&buffering,0,false)==0);
    CHECK(playout_take(&buffering,960,false)==0);
    CHECK(playout_take(&buffering,1920,false)==0);
    CHECK(playout_take(&buffering,2880,false)==240);
    CHECK(playout_take(&buffering,240,false)==240);
    CHECK(playout_take(&buffering,0,false)==0 && buffering);
    CHECK(playout_take(&buffering,960,false)==0);
    CHECK(playout_take(&buffering,960,true)==240);
    CHECK(playout_take(&buffering,100,true)==100 && buffering);
    buffering=true;CHECK(playout_take(&buffering,960,false)==0);
    size_t queued=0,played=0;unsigned packet=0,gaps=0;
    const unsigned jitter[]={0,15,30,45,60,75,90,75,60,45,30,15};
    for(unsigned ms=0;ms<7000 && played<96000;ms+=15) {
        while(packet<100 && ms>=packet*60+jitter[packet%12]) { queued+=960;packet++; }
        size_t n=playout_take(&buffering,queued,packet==100);
        if(played && played<96000 && !n)gaps++;
        queued-=n;played+=n;
    }
    CHECK(packet==100 && played==96000 && queued==0 && gaps==0);
    #undef CHECK
    return errors;
}
void voice_duplex_stop(voice_duplex_t *d)
{
    if(!d) return;
    d->stop=true;xSemaphoreTake(d->done,portMAX_DELAY);
    printf("LX_AEC_TIMING elapsed_ms=%lld processed_ms=%u adc_overflow=%u\n",
        (long long)(d->elapsed_us/1000),d->frames*d->chunk/16,d->rx_overflows);
    printf("LX_PLAY_STATS samples=%u peak=%u clipped=%u gaps=%u gap_samples=%u max_gap_samples=%u\n",
        d->played,d->play_peak,d->play_clipped,d->gap_events,d->gap_samples,d->gap_max);
    printf("LX_AEC frames=%u max_us=%u avg_us=%llu overflow=%u played=%u measured=%u raw_ms=%llu clean_ms=%llu ref_ms=%llu ref_peak=%u ref_clip=%u result=%s\n",
        d->frames,d->max_us,d->frames?d->total_us/d->frames:0,d->in_overflow,d->played,d->measured,
        d->measured?d->raw_energy/d->measured:0,d->measured?d->clean_energy/d->measured:0,
        d->measured?d->ref_energy/d->measured:0,d->ref_peak,d->ref_clip,esp_err_to_name(d->error));
    for(unsigned i=0;i<16 && d->bin_samples[i];i++)
        printf("LX_AEC_LEVEL block=%u samples=%u raw_ms=%llu clean_ms=%llu ref_ms=%llu\n",
            i,d->bin_samples[i],d->raw_bins[i]/d->bin_samples[i],
            d->clean_bins[i]/d->bin_samples[i],d->ref_bins[i]/d->bin_samples[i]);
    aec_destroy(d->aec);free(d->aligned);vStreamBufferDelete(d->in);vStreamBufferDelete(d->out);
    vSemaphoreDelete(d->ready);vSemaphoreDelete(d->done);free(d);
}
/* Explicit local acoustic check, no cloud, no retained microphone recording. */
esp_err_t voice_duplex_test(bool (*cancelled)(void))
{
    voice_duplex_t *d=voice_duplex_start();if(!d)return ESP_FAIL;
    extern const unsigned char clips[] asm("_binary_clips_bin_start");
    extern const unsigned char clips_end[] asm("_binary_clips_bin_end");
    size_t at=0,total=(clips_end-clips)/2;
    int16_t in[320],out[320];esp_err_t rc=ESP_OK;
    for(unsigned frame=0;frame<500 && !cancelled();frame++) {
        /* First 3 seconds leave the DAC silent: an external acoustic source
         * should appear only in the microphone, never in the MIC3 reference. */
        for(unsigned i=0;i<320;i++) {
            out[i]=frame<150?0:(int16_t)(clips[2*at]|clips[2*at+1]<<8);
            if(frame>=150) at=(at+1)%total;
        }
        rc=voice_duplex_write(d,out,320);if(rc!=ESP_OK)break;
        rc=voice_duplex_read(d,in);if(rc!=ESP_OK)break;
    }
    voice_duplex_stop(d);return rc;
}
