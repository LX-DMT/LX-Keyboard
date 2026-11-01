#include <string.h>
#include "activation_audio.h"
#include "activation_ui.h"
#include "board.h"
#include "activation_audio/clips.h"
extern const unsigned char clips_start[] asm("_binary_clips_bin_start");
extern const unsigned char clips_end[] asm("_binary_clips_bin_end");
static esp_err_t play_clip(unsigned index,bool (*cancelled)(void))
{
    if(index>=sizeof(activation_clips)/sizeof(activation_clips[0])) return ESP_ERR_INVALID_ARG;
    unsigned at=activation_clips[index].offset,n=activation_clips[index].samples;
    if(at+2*n>(unsigned)(clips_end-clips_start)) return ESP_ERR_INVALID_SIZE;
    esp_err_t rc=ESP_OK;int16_t pcm[320];
    for(unsigned offset=0;offset<n && !cancelled();offset+=320) {
        unsigned count=n-offset;if(count>320) count=320;
        for(unsigned i=0;i<count;i++) { const unsigned char *p=clips_start+at+2*(offset+i);pcm[i]=(int16_t)(p[0]|p[1]<<8); }
        rc=board_audio_play_write(pcm,count);if(rc!=ESP_OK) return rc;
    }
    activation_ui_digit(-1);
    memset(pcm,0,sizeof(pcm));
    for(unsigned i=0;i<6 && rc==ESP_OK && !cancelled();i++) rc=board_audio_play_write(pcm,320);
    return cancelled()?ESP_ERR_INVALID_STATE:rc;
}
esp_err_t activation_audio_play(const char *code,bool (*cancelled)(void))
{
    if(!code || strlen(code)!=6 || strspn(code,"0123456789")!=6) return ESP_ERR_INVALID_ARG;
    esp_err_t rc=board_audio_play_begin();if(rc!=ESP_OK) return rc;
    for(unsigned digit=0;digit<7 && rc==ESP_OK && !cancelled();digit++) {
        unsigned index=digit?code[digit-1]-'0':10;
        activation_ui_digit(digit?(int)index:-1);
        rc=play_clip(index,cancelled);
    }
    activation_ui_digit(-1);board_audio_play_end();return rc;
}
