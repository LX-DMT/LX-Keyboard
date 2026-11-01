#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "voice_opus.h"
#include "esp_opus_enc.h"
#include "esp_opus_dec.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static esp_err_t open_mode(voice_opus_t *c,unsigned rate,bool realtime)
{
    *c=(voice_opus_t){0};
    esp_opus_enc_config_t enc={.sample_rate=rate,.channel=1,.bits_per_sample=16,
        .bitrate=rate==24000?40000:24000,.frame_duration=ESP_OPUS_ENC_FRAME_DURATION_60_MS,
        .application_mode=realtime?ESP_OPUS_ENC_APPLICATION_LOWDELAY:ESP_OPUS_ENC_APPLICATION_VOIP,
        .complexity=0,.enable_vbr=true};
    /* Opus carries its bandwidth in-band. Decode both 16/24 kHz sources directly
     * at the DAC's 16 kHz rate, using the codec's own resampling/filtering. */
    esp_opus_dec_cfg_t dec={.sample_rate=16000,.channel=1,.frame_duration=ESP_OPUS_DEC_FRAME_DURATION_60_MS};
    if(esp_opus_enc_open(&enc,sizeof(enc),&c->encoder)!=ESP_AUDIO_ERR_OK) return ESP_FAIL;
    if(esp_opus_dec_open(&dec,sizeof(dec),&c->decoder)!=ESP_AUDIO_ERR_OK) { voice_opus_close(c);return ESP_FAIL; }
    return ESP_OK;
}
esp_err_t voice_opus_open(voice_opus_t *c,unsigned rate) { return open_mode(c,rate,false); }
void voice_opus_close(voice_opus_t *c)
{
    if(c->encoder) esp_opus_enc_close(c->encoder);
    if(c->decoder) esp_opus_dec_close(c->decoder);
    c->encoder=c->decoder=NULL;
}
esp_err_t voice_opus_encode(voice_opus_t *c,const int16_t *pcm,size_t samples,uint8_t *out,size_t cap,size_t *n)
{
    *n=0;esp_audio_enc_in_frame_t in={.buffer=(uint8_t*)pcm,.len=samples*2};
    esp_audio_enc_out_frame_t frame={.buffer=out,.len=cap};
    int64_t start=esp_timer_get_time();
    int rc=esp_opus_enc_process(c->encoder,&in,&frame);
    unsigned elapsed=esp_timer_get_time()-start;if(elapsed>c->encode_max_us) c->encode_max_us=elapsed;
    c->encode_total_us+=elapsed;c->encode_frames++;
    if(rc!=ESP_AUDIO_ERR_OK || !frame.encoded_bytes || frame.encoded_bytes>cap) return ESP_FAIL;
    *n=frame.encoded_bytes;return ESP_OK;
}
esp_err_t voice_opus_decode(voice_opus_t *c,const uint8_t *packet,size_t n,int16_t *pcm,size_t cap,size_t *samples)
{
    *samples=0;esp_audio_dec_in_raw_t raw={.buffer=(uint8_t*)packet,.len=n};
    esp_audio_dec_out_frame_t frame={.buffer=(uint8_t*)pcm,.len=cap*2};esp_audio_dec_info_t info={0};
    int64_t start=esp_timer_get_time();int rc=esp_opus_dec_decode(c->decoder,&raw,&frame,&info);
    unsigned elapsed=esp_timer_get_time()-start;if(elapsed>c->decode_max_us) c->decode_max_us=elapsed;
    if(rc!=ESP_AUDIO_ERR_OK || raw.consumed!=n || info.sample_rate!=16000 || info.channel!=1 || info.bits_per_sample!=16 ||
       !frame.decoded_size || frame.decoded_size>cap*2 || frame.decoded_size%2) return ESP_FAIL;
    *samples=frame.decoded_size/2;return ESP_OK;
}
unsigned voice_opus_selftest(void)
{
    int16_t *pcm=malloc(1440*2),*out=malloc(1920*2);uint8_t *packet=malloc(1536);
    if(!pcm || !out || !packet) { free(pcm);free(out);free(packet);return 1; }
    unsigned errors=0;
    const unsigned amplitudes[]={128,2048,6000};
    for(unsigned mode=0;mode<2;mode++) for(unsigned level=0;level<3;level++) for(unsigned rate=16000;rate<=24000;rate+=8000) {
        voice_opus_t codec;esp_err_t rc=open_mode(&codec,rate,mode);unsigned frames=0,bytes=0,changed=0;double energy=0,input_energy=0;
        if(rc==ESP_OK) {
            for(unsigned frame=0;frame<12;frame++) {
                unsigned count=rate*60/1000;
                for(unsigned i=0;i<count;i++) { pcm[i]=amplitudes[level]*sinf(6.2831853f*440*(frame*count+i)/rate);input_energy+=(double)pcm[i]*pcm[i]; }
                memcpy(out,pcm,count*2);
                size_t n=0,samples=0;rc=voice_opus_encode(&codec,pcm,count,packet,1536,&n);
                if(memcmp(out,pcm,count*2)) changed++;
                if(rc==ESP_OK) rc=voice_opus_decode(&codec,packet,n,out,1920,&samples);
                if(rc!=ESP_OK || samples!=960) { errors++;break; }
                for(unsigned i=0;i<samples;i++) energy+=(double)out[i]*out[i];
                bytes+=n;frames++;vTaskDelay(1);
            }
            double expected=(double)amplitudes[level]*amplitudes[level]/2;
            if(frames!=12 || energy/(frames*960)<expected*0.25 || energy/(frames*960)>expected*4) errors++;
        } else errors++;
        printf("LX_OPUS rate=%u amplitude=%u decoded_rate=16000 frames=%u bytes=%u input_rms=%.1f rms=%.1f input_changed=%u encode_max_us=%u decode_max_us=%u result=%s\n",
            rate,amplitudes[level],frames,bytes,frames?sqrt(input_energy/(frames*rate*60/1000)):0,frames?sqrt(energy/(frames*960)):0,changed,codec.encode_max_us,codec.decode_max_us,esp_err_to_name(rc));
        voice_opus_close(&codec);
        printf("LX_OPUS_MODE realtime=%u encode_avg_us=%llu\n",mode,codec.encode_frames?codec.encode_total_us/codec.encode_frames:0);
    }
    free(pcm);free(out);free(packet);return errors;
}
