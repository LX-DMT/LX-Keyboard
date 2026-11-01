#include <stdio.h>
#include <string.h>
#include "wake_word.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

static const esp_wn_iface_t *s_wn;
static model_iface_data_t *s_model;
static int16_t *s_buffer;
static unsigned s_chunk,s_used,s_frames,s_detected,s_max_us;
static unsigned s_reset_max_us,s_reset_errors;
static char s_name[64];
bool wake_word_ready(void) { return s_buffer && s_model; }
esp_err_t wake_word_start(void)
{
    static srmodel_list_t *models;
    if(wake_word_ready()) return ESP_OK;
    if(!models) models=esp_srmodel_init("model");
    if(!models) return ESP_ERR_NOT_FOUND;
    if(!s_wn) {
        char *name=esp_srmodel_filter(models,ESP_WN_PREFIX,"nihaoxiaozhi");
        if(!name) return ESP_ERR_NOT_FOUND;
        s_wn=esp_wn_handle_from_name(name);if(!s_wn) return ESP_ERR_NOT_SUPPORTED;
        snprintf(s_name,sizeof(s_name),"%s",name);
    }
    s_model=s_wn->create(s_name,DET_MODE_95);if(!s_model) return ESP_ERR_NO_MEM;
    s_chunk=s_wn->get_samp_chunksize(s_model);
    if(s_wn->get_samp_rate(s_model)!=16000 || s_wn->get_channel_num(s_model)!=1 || !s_chunk || s_chunk>2048) {
        wake_word_suspend();return ESP_ERR_NOT_SUPPORTED;
    }
    if(!s_buffer) s_buffer=heap_caps_malloc(s_chunk*sizeof(int16_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!s_buffer) { wake_word_suspend();return ESP_ERR_NO_MEM; }
    printf("LX_WAKE model=%s samples=%u rate=16000\n",s_name,s_chunk);return ESP_OK;
}
void wake_word_suspend(void)
{
    s_used=0;
    if(s_model) s_wn->destroy(s_model);
    s_model=NULL;
}
void wake_word_reset(void)
{
    s_used=0;
    if(!s_wn || !s_name[0]) return;
    /* ESP-SR 2.5.3 WN9s clean() dereferences a missing convolution queue in
     * this model (captured board backtrace). Recreate through its public API
     * instead of changing vendor binaries or retaining pre-PTT audio history. */
    int64_t begin=esp_timer_get_time();
    wake_word_suspend();s_model=s_wn->create(s_name,DET_MODE_95);
    if(!s_model) s_reset_errors++;
    unsigned duration=esp_timer_get_time()-begin;if(duration>s_reset_max_us) s_reset_max_us=duration;
}
bool wake_word_feed(const int16_t *pcm,size_t n)
{
    if(!s_buffer || !s_model) return false;
    bool found=false;
    while(n) {
        size_t count=s_chunk-s_used;if(count>n) count=n;
        memcpy(s_buffer+s_used,pcm,count*sizeof(*pcm));s_used+=count;pcm+=count;n-=count;
        if(s_used==s_chunk) {
            int64_t begin=esp_timer_get_time();int result=s_wn->detect(s_model,s_buffer);
            unsigned duration=esp_timer_get_time()-begin;if(duration>s_max_us) s_max_us=duration;
            s_frames++;s_used=0;
            if(result>0) { s_detected++;found=true; }
        }
    }
    return found;
}
void wake_word_stats(void)
{
    printf("LX_WAKE ready=%u model=%s frames=%u detected=%u inference_max_us=%u free_internal=%u free_psram=%u reset_max_us=%u reset_errors=%u\n",
        s_buffer!=NULL && s_model!=NULL,s_name,s_frames,s_detected,s_max_us,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),(unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),s_reset_max_us,s_reset_errors);
}
