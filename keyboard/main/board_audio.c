#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <math.h>
#include "board.h"
#include "voice_capture.h"
#include "power_policy.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

static esp_codec_dev_handle_t s_codec,s_adc;
static i2s_chan_handle_t s_rx,s_tx;
static const audio_codec_data_if_t *s_data_if;
static const audio_codec_ctrl_if_t *s_dac_ctrl,*s_adc_ctrl;
static const audio_codec_gpio_if_t *s_gpio_if;
static const audio_codec_if_t *s_dac_if,*s_adc_if;
static bool s_muted=true;
static unsigned s_level;
static int64_t s_level_at;
static portMUX_TYPE s_level_lock=portMUX_INITIALIZER_UNLOCKED;
/* A capture lease can end immediately after each 20 ms wake-word frame.
 * Keep a short envelope across leases so the LED task can observe it. */
static unsigned level_at(int64_t now)
{
    int64_t age=now-s_level_at;
    return age<0 || age>=180000?0:(unsigned)((uint64_t)s_level*(180000-age)/180000);
}
static void level_publish(unsigned peak)
{
    unsigned value=peak>8191?255:peak/32;int64_t now=esp_timer_get_time();
    portENTER_CRITICAL(&s_level_lock);
    if(value>=level_at(now)) { s_level=value;s_level_at=now; }
    portEXIT_CRITICAL(&s_level_lock);
}
static void level_clear(void)
{
    portENTER_CRITICAL(&s_level_lock);s_level=0;s_level_at=0;portEXIT_CRITICAL(&s_level_lock);
}
static SemaphoreHandle_t s_audio_lock;
static bool s_open;
static volatile unsigned s_rx_overflows;
static bool rx_overflow(i2s_chan_handle_t channel,i2s_event_data_t *event,void *arg)
{
    (void)channel;(void)event;(void)arg;s_rx_overflows++;return false;
}
unsigned board_audio_rx_overflows(void) { return s_rx_overflows; }
/* The codec's percentage curve is -50..0 dB, not linear amplitude.
 * 80 (-10 dB) was audibly accepted on this board; 25/60 were inaudible. */
#define PLAYBACK_VOLUME 80
esp_err_t board_audio_setup(void) { s_audio_lock=xSemaphoreCreateMutex();return s_audio_lock?ESP_OK:ESP_ERR_NO_MEM; }
/* Q8 switches the board supply, not just the speaker. Only the audio owner
 * changes it, after the last lease has ended. External I2C pullups remain a
 * hardware backfeed consideration; off is not a measured zero-voltage claim. */
static void power_off(void)
{
    const int pins[]={PIN_AUDIO_MCLK,PIN_AUDIO_BCLK,PIN_AUDIO_WS,PIN_AUDIO_DOUT};
    for(unsigned i=0;i<sizeof(pins)/sizeof(pins[0]);i++) {
        gpio_reset_pin(pins[i]);gpio_set_direction(pins[i],GPIO_MODE_OUTPUT);
        gpio_set_level(pins[i],0);
    }
    gpio_set_level(PIN_AUDIO_POWER_OFF,1);s_muted=true;
}
unsigned board_audio_level(void)
{
    int64_t now=esp_timer_get_time();portENTER_CRITICAL(&s_level_lock);
    unsigned value=level_at(now);portEXIT_CRITICAL(&s_level_lock);return value;
}
void board_audio_stats(void)
{
    printf("LX_AUDIO_HW open=%u gpio1=%d rx=%u tx=%u overflows=%u level=%u voltage=not_measured\n",
        s_open,gpio_get_level(PIN_AUDIO_POWER_OFF),s_rx!=NULL,s_tx!=NULL,s_rx_overflows,board_audio_level());
}
static const char *TAG="board_audio";
static esp_err_t audio_init_locked(void)
{
    if(s_open) return ESP_OK;
    gpio_set_level(PIN_AUDIO_POWER_OFF,0);
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_err_t ret=ESP_OK;
    i2s_chan_handle_t tx=NULL,rx=NULL;
    bool tx_enabled=false,rx_enabled=false;
    const audio_codec_data_if_t *data=NULL;
    const audio_codec_ctrl_if_t *ctrl=NULL,*adc_ctrl=NULL;
    const audio_codec_gpio_if_t *gpio=NULL;
    const audio_codec_if_t *codec=NULL,*adc=NULL;
    esp_codec_dev_handle_t handle=NULL,capture=NULL;
    i2s_chan_config_t channel=I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1,I2S_ROLE_MASTER);
    channel.dma_desc_num=6;channel.dma_frame_num=240; /* 90 ms at 16 kHz. */
    channel.auto_clear=true;
    ESP_GOTO_ON_ERROR(i2s_new_channel(&channel,&tx,&rx),fail,TAG,"channels");
    i2s_std_config_t config={
        .clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg=I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_MONO),
        .gpio_cfg={.mclk=PIN_AUDIO_MCLK,.bclk=PIN_AUDIO_BCLK,.ws=PIN_AUDIO_WS,
                   .dout=PIN_AUDIO_DOUT,.din=PIN_AUDIO_DIN},
    };
    ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(tx,&config),fail,TAG,"tx");
    ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(rx,&config),fail,TAG,"rx");
    const i2s_event_callbacks_t callbacks={.on_recv_q_ovf=rx_overflow};
    ESP_GOTO_ON_ERROR(i2s_channel_register_event_callback(rx,&callbacks,NULL),fail,TAG,"RX overflow counter");
    ESP_GOTO_ON_ERROR(i2s_channel_enable(tx),fail,TAG,"enable tx");tx_enabled=true;
    ESP_GOTO_ON_ERROR(i2s_channel_enable(rx),fail,TAG,"enable rx");rx_enabled=true;
    audio_codec_i2s_cfg_t data_config={.port=1,.rx_handle=rx,.tx_handle=tx};
    data=audio_codec_new_i2s_data(&data_config);
    audio_codec_i2c_cfg_t ctrl_config={.port=1,.addr=ES8311_CODEC_DEFAULT_ADDR,
                                      .bus_handle=board_audio_bus()};
    ctrl=audio_codec_new_i2c_ctrl(&ctrl_config);
    gpio=audio_codec_new_gpio();
    ESP_GOTO_ON_FALSE(data && ctrl && gpio,ESP_ERR_NO_MEM,fail,TAG,"codec interfaces");
    es8311_codec_cfg_t chip={.ctrl_if=ctrl,.gpio_if=gpio,
        .codec_mode=ESP_CODEC_DEV_WORK_MODE_DAC,.pa_pin=-1,.use_mclk=true,
        .hw_gain={.pa_voltage=5.0,.codec_dac_voltage=3.3}};
    codec=es8311_codec_new(&chip);
    ESP_GOTO_ON_FALSE(codec,ESP_FAIL,fail,TAG,"ES8311");
    esp_codec_dev_cfg_t device={.dev_type=ESP_CODEC_DEV_TYPE_OUT,.codec_if=codec,.data_if=data};
    handle=esp_codec_dev_new(&device);
    ESP_GOTO_ON_FALSE(handle,ESP_FAIL,fail,TAG,"codec device");
    esp_codec_dev_sample_info_t sample={.sample_rate=16000,.channel=2,.bits_per_sample=16};
    ESP_GOTO_ON_ERROR(esp_codec_dev_open(handle,&sample),fail,TAG,"open");
    esp_codec_dev_set_out_mute(handle,true);
    audio_codec_i2c_cfg_t adc_ctrl_config={.port=1,.addr=ES7210_CODEC_DEFAULT_ADDR,
                                          .bus_handle=board_audio_bus()};
    adc_ctrl=audio_codec_new_i2c_ctrl(&adc_ctrl_config);
    ESP_GOTO_ON_FALSE(adc_ctrl,ESP_ERR_NO_MEM,fail,TAG,"ADC control");
    uint8_t id_high=0,id_low=0;
    ESP_GOTO_ON_ERROR(adc_ctrl->read_reg(adc_ctrl,0x3d,1,&id_high,1),fail,TAG,"ADC ID high");
    ESP_GOTO_ON_ERROR(adc_ctrl->read_reg(adc_ctrl,0x3e,1,&id_low,1),fail,TAG,"ADC ID low");
    printf("LX_M0 adc_id=%02x%02x address=0x40 bus=1\n",id_high,id_low);
    ESP_GOTO_ON_FALSE(id_high==0x72 && id_low==0x10,ESP_ERR_NOT_SUPPORTED,fail,TAG,"ADC identity");
    es7210_codec_cfg_t adc_chip={.ctrl_if=adc_ctrl,
        .mic_selected=ES7210_SEL_MIC1|ES7210_SEL_MIC2|ES7210_SEL_MIC3};
    adc=es7210_codec_new(&adc_chip);
    ESP_GOTO_ON_FALSE(adc,ESP_FAIL,fail,TAG,"ES7210");
    esp_codec_dev_cfg_t input={.dev_type=ESP_CODEC_DEV_TYPE_IN,.codec_if=adc,.data_if=data};
    capture=esp_codec_dev_new(&input);
    ESP_GOTO_ON_FALSE(capture,ESP_ERR_NO_MEM,fail,TAG,"ADC device");
    sample.channel=4;
    ESP_GOTO_ON_ERROR(esp_codec_dev_open(capture,&sample),fail,TAG,"ADC open");
    ESP_GOTO_ON_ERROR(esp_codec_dev_set_in_channel_gain(capture,3,24.0f),fail,TAG,"ADC gain");
    ESP_GOTO_ON_ERROR(esp_codec_dev_set_in_channel_gain(capture,4,0.0f),fail,TAG,"reference gain");
    s_adc=capture;s_codec=handle;s_rx=rx;s_tx=tx;
    s_data_if=data;s_dac_ctrl=ctrl;s_adc_ctrl=adc_ctrl;s_gpio_if=gpio;
    s_dac_if=codec;s_adc_if=adc;s_muted=false;
    s_open=true;
    return ESP_OK;
fail:
    /* Publish handles only after complete setup. A failed DMA allocation must
     * not permanently occupy I2S1 and make every later attempt fail. */
    if(capture) esp_codec_dev_delete(capture);
    if(handle) esp_codec_dev_delete(handle);
    if(adc) audio_codec_delete_codec_if(adc);
    if(codec) audio_codec_delete_codec_if(codec);
    if(adc_ctrl) audio_codec_delete_ctrl_if(adc_ctrl);
    if(ctrl) audio_codec_delete_ctrl_if(ctrl);
    if(gpio) audio_codec_delete_gpio_if(gpio);
    if(data) audio_codec_delete_data_if(data);
    if(rx_enabled) i2s_channel_disable(rx);
    if(tx_enabled) i2s_channel_disable(tx);
    if(rx) i2s_del_channel(rx);
    if(tx) i2s_del_channel(tx);
    power_off();return ret;
}
esp_err_t board_audio_init(void)
{
    if(xSemaphoreTake(s_audio_lock,pdMS_TO_TICKS(2000))!=pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t rc=audio_init_locked();xSemaphoreGive(s_audio_lock);return rc;
}
esp_err_t board_audio_stop(void)
{
    if(xSemaphoreTake(s_audio_lock,pdMS_TO_TICKS(2000))!=pdTRUE) return ESP_ERR_TIMEOUT;
    if(s_codec) esp_codec_dev_set_out_mute(s_codec,true);
    /* Close the ADC before the DAC/data interface removes its MCLK. Destroy
     * codec instances so the next power-on programs ALL reset registers. */
    if(s_adc) esp_codec_dev_delete(s_adc);
    if(s_codec) esp_codec_dev_delete(s_codec);
    if(s_adc_if) audio_codec_delete_codec_if(s_adc_if);
    if(s_dac_if) audio_codec_delete_codec_if(s_dac_if);
    if(s_adc_ctrl) audio_codec_delete_ctrl_if(s_adc_ctrl);
    if(s_dac_ctrl) audio_codec_delete_ctrl_if(s_dac_ctrl);
    if(s_gpio_if) audio_codec_delete_gpio_if(s_gpio_if);
    if(s_data_if) audio_codec_delete_data_if(s_data_if);
    /* Codec close normally disables both directions. Invalid-state here
     * simply means that direction is already stopped. */
    if(s_rx) { i2s_channel_disable(s_rx);i2s_del_channel(s_rx); }
    if(s_tx) { i2s_channel_disable(s_tx);i2s_del_channel(s_tx); }
    s_adc=NULL;s_codec=NULL;s_rx=NULL;s_tx=NULL;
    s_adc_if=NULL;s_dac_if=NULL;s_adc_ctrl=NULL;s_dac_ctrl=NULL;s_gpio_if=NULL;s_data_if=NULL;
    s_open=false;level_clear();power_off();xSemaphoreGive(s_audio_lock);return ESP_OK;
}
extern const uint8_t power_start[] asm("_binary_startup_pcm_start");
extern const uint8_t power_start_end[] asm("_binary_startup_pcm_end");
extern const uint8_t power_stop[] asm("_binary_shutdown_pcm_start");
extern const uint8_t power_stop_end[] asm("_binary_shutdown_pcm_end");
static esp_err_t power_cue(bool shutdown)
{
    /* Called by the audio owner. Shutdown deliberately plays after normal
     * producers are cancelled. Neither path repowers a disabled board. */
    if(shutdown?!power_policy_shutting_down():!voice_audio_ready()) return ESP_ERR_INVALID_STATE;
    if(xSemaphoreTake(s_audio_lock,pdMS_TO_TICKS(100))!=pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t rc=ESP_ERR_INVALID_STATE;
    if(s_open && !s_muted) {
        rc=(esp_codec_dev_set_out_vol(s_codec,PLAYBACK_VOLUME) || esp_codec_dev_set_out_mute(s_codec,false))?ESP_FAIL:ESP_OK;
        const uint8_t *data=shutdown?power_stop:power_start;
        size_t count=((shutdown?power_stop_end:power_start_end)-data)/2;
        int16_t pcm[640];
        /* Flash-backed original samples, followed by >90ms DMA-drain silence. */
        for(size_t start=0;start<count+1920 && rc==ESP_OK;start+=320) {
            if(!shutdown && !voice_audio_ready()) { rc=ESP_ERR_INVALID_STATE;break; }
            for(unsigned i=0;i<320;i++) {
                size_t n=start+i;
                int16_t v=n<count?(int16_t)((unsigned)data[2*n]|((unsigned)data[2*n+1]<<8)):0;
                pcm[2*i]=pcm[2*i+1]=v;
            }
            if(esp_codec_dev_write(s_codec,pcm,sizeof(pcm))) rc=ESP_FAIL;
        }
        esp_codec_dev_set_out_mute(s_codec,true);
        printf("LX_POWER_CUE source=%s samples=%u rate=16000 result=%s\n",
               shutdown?"reference_poweron.wav":"reference_poweroff.wav",(unsigned)count,esp_err_to_name(rc));
    }
    xSemaphoreGive(s_audio_lock);return rc;
}
esp_err_t board_audio_startup_cue(void) { return power_cue(false); }
esp_err_t board_audio_shutdown_cue(void) { return power_cue(true); }
esp_err_t board_audio_capture_begin(void)
{
    if(!voice_audio_ready()) return ESP_ERR_INVALID_STATE;
    if(xSemaphoreTake(s_audio_lock,pdMS_TO_TICKS(20))!=pdTRUE) return ESP_ERR_TIMEOUT;
    if(!voice_audio_ready() || !s_open) { xSemaphoreGive(s_audio_lock);return ESP_ERR_INVALID_STATE; }
    return ESP_OK;
}
esp_err_t board_audio_capture_fresh_begin(void)
{
    esp_err_t rc=board_audio_capture_begin();if(rc!=ESP_OK)return rc;
    /* A new PTT must not inherit the idle/WakeNet DMA backlog. */
    rc=i2s_channel_disable(s_rx);
    if(rc==ESP_OK)rc=i2s_channel_enable(s_rx);
    if(rc!=ESP_OK)board_audio_capture_end();
    return rc;
}
esp_err_t board_audio_capture_read(int16_t mono[320])
{
    if(!voice_audio_ready()) return ESP_ERR_INVALID_STATE;
    int16_t stereo[1280];
    int result=esp_codec_dev_read(s_adc,stereo,sizeof(stereo));
    if(result) return ESP_FAIL;
    unsigned peak=0;
    for(unsigned i=0;i<320;i++) {
        mono[i]=stereo[i*4+1];unsigned v=mono[i]<0?-(int)mono[i]:mono[i];if(v>peak) peak=v;
    }
    level_publish(peak);return ESP_OK;
}
void board_audio_capture_end(void) { xSemaphoreGive(s_audio_lock); }
esp_err_t board_audio_play_begin(void)
{
    esp_err_t rc=board_audio_capture_begin();if(rc!=ESP_OK) return rc;
    if(esp_codec_dev_set_out_vol(s_codec,PLAYBACK_VOLUME) || esp_codec_dev_set_out_mute(s_codec,s_muted)) {
        esp_codec_dev_set_out_mute(s_codec,true);board_audio_capture_end();return ESP_FAIL;
    }
    return ESP_OK;
}
esp_err_t board_audio_play_write(const int16_t *mono,size_t samples)
{
    int16_t stereo[640];
    while(samples) {
        if(!voice_audio_ready()) return ESP_ERR_INVALID_STATE;
        size_t n=samples>320?320:samples;
        for(size_t i=0;i<n;i++) stereo[2*i]=stereo[2*i+1]=mono[i];
        if(esp_codec_dev_set_out_mute(s_codec,s_muted)) return ESP_FAIL;
        if(esp_codec_dev_write(s_codec,stereo,n*4)) return ESP_FAIL;
        mono+=n;samples-=n;
    }
    return ESP_OK;
}
void board_audio_play_end(void)
{
    /* Queue more silence than the 6*240-frame/90 ms DMA capacity before mute. */
    int16_t silence[320]={0};
    for(unsigned i=0;i<6;i++) board_audio_play_write(silence,320);
    esp_codec_dev_set_out_mute(s_codec,true);board_audio_capture_end();
}
esp_err_t board_audio_duplex_begin(void)
{
    esp_err_t rc=board_audio_play_begin();if(rc!=ESP_OK)return rc;
    /* ADC runs during TLS setup while no consumer owns its DMA queue. Start
     * the conversation with fresh, frame-aligned RX data, not that backlog.
     * TX/MCLK keep running. IDF resets the RX queue on channel enable. */
    rc=i2s_channel_disable(s_rx);
    if(rc==ESP_OK)rc=i2s_channel_enable(s_rx);
    if(rc!=ESP_OK)board_audio_play_end();
    return rc;
}
esp_err_t board_audio_duplex_read(int16_t mic[240],int16_t ref[240])
{
    /* ES7210 TDM16 physical order verified by independent MIC1/MIC3 gains:
     * little-endian halves are R3,M1,M4,M2. MIC3 is the DAC analog reference
     * through R9/R11 on AU_AEC+PA. MIC2 must never be used as AEC reference. */
    if(!voice_audio_ready()) return ESP_ERR_INVALID_STATE;
    int16_t raw[240*4];
    if(esp_codec_dev_read(s_adc,raw,sizeof(raw))) return ESP_FAIL;
    for(unsigned i=0;i<240;i++) { mic[i]=raw[4*i+1];ref[i]=raw[4*i]; }
    return ESP_OK;
}
void board_audio_duplex_end(void) { board_audio_play_end(); }
/* Identify physical ADC channels without requiring a person to speak. Only
 * MIC1 or MIC3 gain changes, while the DAC sends the same reference tone. */
void board_audio_channel_test(void)
{
    esp_err_t rc=board_audio_play_begin();
    if(rc!=ESP_OK) { printf("LX_AUDIO_CHANNELS result=%s\n",esp_err_to_name(rc));return; }
    int reg11=0,reg12=0;
    esp_codec_dev_read_reg(s_adc,0x11,&reg11);esp_codec_dev_read_reg(s_adc,0x12,&reg12);
    printf("LX_AUDIO_CHANNELS adc11=%02x adc12=%02x\n",reg11,reg12);
    int16_t play[240],raw[960];
    for(unsigned i=0;i<240;i++) play[i]=(int16_t)(1000*sinf(6.2831853f*1000*i/16000));
    for(unsigned phase=0;phase<3;phase++) {
        esp_codec_dev_set_in_channel_gain(s_adc,1,phase==1?0:24);
        esp_codec_dev_set_in_channel_gain(s_adc,4,phase==2?12:0);
        uint64_t energy[4]={0};unsigned samples=0;
        for(unsigned frame=0;frame<70;frame++) {
            rc=board_audio_play_write(play,240);if(rc!=ESP_OK)break;
            if(esp_codec_dev_read(s_adc,raw,sizeof(raw))) {rc=ESP_FAIL;break;}
            if(frame<20)continue;
            for(unsigned i=0;i<960;i++) energy[i%4]+=(int64_t)raw[i]*raw[i];
            samples+=240;
        }
        printf("LX_AUDIO_CHANNELS phase=%u mic1_db=%u mic3_db=%u samples=%u slot_ms=%llu,%llu,%llu,%llu result=%s\n",
            phase,phase==1?0:24,phase==2?12:0,samples,
            samples?energy[0]/samples:0,samples?energy[1]/samples:0,
            samples?energy[2]/samples:0,samples?energy[3]/samples:0,esp_err_to_name(rc));
        if(rc!=ESP_OK)break;
    }
    esp_codec_dev_set_in_channel_gain(s_adc,1,24);
    esp_codec_dev_set_in_channel_gain(s_adc,4,0);
    board_audio_play_end();
}
esp_err_t board_audio_tone(void)
{ return board_audio_tone_volume(PLAYBACK_VOLUME); }
esp_err_t board_audio_tone_volume(unsigned volume)
{
    if(volume>80) return ESP_ERR_INVALID_ARG;
    if(!voice_audio_ready()) return ESP_ERR_INVALID_STATE;
    esp_err_t result=board_audio_play_begin();if(result!=ESP_OK) return result;
    if(esp_codec_dev_set_out_vol(s_codec,volume)) { board_audio_play_end();return ESP_FAIL; }
    int mute_reg=0,volume_reg=0;
    int mute_rc=esp_codec_dev_read_reg(s_codec,0x31,&mute_reg);
    int volume_rc=esp_codec_dev_read_reg(s_codec,0x32,&volume_reg);
    printf("LX_AUDIO_OUTPUT volume=%u gpio1=%d mute_reg=%02x(%d) volume_reg=%02x(%d)\n",
           volume,gpio_get_level(PIN_AUDIO_POWER_OFF),mute_reg,mute_rc,volume_reg,volume_rc);
    if(mute_rc || volume_rc) { board_audio_play_end();return ESP_FAIL; }
    /* Read back pads without changing their I2S output routing. Counts only
     * detect activity; they do not measure clock rate or validate PCM framing. */
    const gpio_num_t pins[]={PIN_AUDIO_MCLK,PIN_AUDIO_BCLK,PIN_AUDIO_WS,PIN_AUDIO_DOUT};
    unsigned highs[4]={0},probes=0;
    for(unsigned p=0;p<4;p++) gpio_input_enable(pins[p]);
    int64_t started=esp_timer_get_time();
    int16_t pcm[320];
    for(unsigned frame=0;frame<30;frame++) {
        for(unsigned i=0;i<320;i++) {
            unsigned sample=frame*320+i;float envelope=sinf(3.14159265f*sample/9600);
            pcm[i]=(int16_t)(2400*envelope*sinf(6.2831853f*(frame<15?660:880)*sample/16000));
        }
        result=board_audio_play_write(pcm,320);if(result!=ESP_OK) break;
        for(unsigned probe=0;probe<256;probe++)
            for(unsigned p=0;p<4;p++) highs[p]+=gpio_get_level(pins[p])!=0;
        probes+=256;
    }
    board_audio_play_end();
    printf("LX_AUDIO_PADS high_counts=%u,%u,%u,%u probes_per_pin=%u elapsed_us=%lld framing=not_measured\n",
           highs[0],highs[1],highs[2],highs[3],probes,(long long)(esp_timer_get_time()-started));
    return result;
}
void board_audio_test(void)
{
    esp_err_t result=board_audio_capture_begin();
    if(result!=ESP_OK) { printf("LX_M0 audio_init=%s\n",esp_err_to_name(result)); return; }
    const uint8_t registers[]={0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0d,0x0e,0x12,0x13,0x14,0x31,0x32,0x37,0x44,0xfd,0xfe,0xff};
    char register_line[320];
    unsigned used=snprintf(register_line,sizeof(register_line),"LX_M0 codec_regs");
    for(unsigned i=0;i<sizeof(registers);i++) {
        int value=0;
        int read=esp_codec_dev_read_reg(s_codec,registers[i],&value);
        used+=snprintf(register_line+used,sizeof(register_line)-used," %02x=%02x(%d)",registers[i],value,read);
    }
    printf("%s\n",register_line);
    /* Read-only capture diagnostics: I2C acknowledgement alone cannot prove
     * that ADC clocks, power/mute settings and the serial data path work. */
    const uint8_t adc_registers[]={0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
        0x11,0x12,0x13,0x14,0x15,0x20,0x21,0x22,0x23,
        0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x4b,0x4c};
    printf("LX_AUDIO_ADC_REGS");
    for(unsigned i=0;i<sizeof(adc_registers);i++) {
        int value=0,rc=esp_codec_dev_read_reg(s_adc,adc_registers[i],&value);
        printf(" %02x=%02x(%d)",adc_registers[i],value,rc);
    }
    puts("");
    static int16_t pcm[1280];
    int minimum=INT16_MAX,maximum=INT16_MIN; unsigned nonzero=0,samples=0;
    unsigned channel_nonzero[4]={0};
    unsigned din_high=0,din_edges=0,din_probes=0;
    int code=0;
    /* Drain old DMA frames and collect ~1 s; one frame can precede ADC settling. */
    for(unsigned frame=0;frame<54;frame++) {
        if(!voice_audio_ready()) { code=ESP_ERR_INVALID_STATE;break; }
        code=esp_codec_dev_read(s_adc,pcm,sizeof(pcm));
        if(code!=0) break;
        if(frame<4) continue;
        if(frame==4) {
            printf("LX_AUDIO_RAW_PREFIX");
            for(unsigned i=0;i<16;i++) printf(" %04x",(unsigned)(uint16_t)pcm[i]);
            puts("");
        }
        int previous=-1;
        for(unsigned i=0;i<256;i++) {
            int level=gpio_get_level(PIN_AUDIO_DIN);
            din_high+=level!=0;
            if(previous>=0 && level!=previous) din_edges++;
            previous=level;din_probes++;
        }
        samples+=1280;
        for(unsigned i=0;i<1280;i++) {
            if(pcm[i]<minimum) minimum=pcm[i];
            if(pcm[i]>maximum) maximum=pcm[i];
            if(pcm[i]) { nonzero++;channel_nonzero[i%4]++; }
        }
        unsigned peak=(unsigned)(maximum>-minimum?maximum:-minimum);
        level_publish(peak);
    }
    printf("LX_M0 audio_read=%d samples=%u min=%d max=%d nonzero=%u mic1_nonzero=%u mic2_nonzero=%u mic3_ref_nonzero=%u mic4_nonzero=%u acoustic_verification=not_measured\n",
           code,samples,minimum,maximum,nonzero,channel_nonzero[1],channel_nonzero[3],channel_nonzero[0],channel_nonzero[2]);
    printf("LX_AUDIO_DIN gpio=%u high=%u transitions=%u probes=%u framing=not_measured\n",
           PIN_AUDIO_DIN,din_high,din_edges,din_probes);
    board_audio_capture_end();
}
