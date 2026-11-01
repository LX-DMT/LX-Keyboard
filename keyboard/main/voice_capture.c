#include <stdio.h>
#include <limits.h>
#include <stdatomic.h>
#include <string.h>
#include "voice_capture.h"
#include "power_policy.h"
#include "network_link.h"
#include "user_controls.h"
#include "voice_focus.h"
#include "board.h"
#include "wake_word.h"
#include "asr_baidu.h"
#include "chat_client.h"
#include "chat_official.h"
#include "activation_audio.h"
#include "activation_ui.h"
#include "voice_opus.h"
#include "voice_duplex.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#define MAX_SAMPLES (16000*20)
static int16_t *s_pcm;
static unsigned s_samples,s_captures,s_errors,s_rejected;
static volatile bool s_ptt,s_cancel;
static atomic_bool s_chat_preemptible,s_chat_preempt;
static volatile bool s_suspended,s_wake_enabled=true;
static volatile lx_voice_state_t s_state;
static atomic_bool s_audio_requested,s_audio_ready;
static atomic_uint s_audio_generation,s_audio_applied;
static unsigned s_audio_transitions,s_audio_failures;
static esp_err_t s_audio_result;
static void (*s_text_cancel)(void);
bool voice_audio_ready(void)
{
    return atomic_load(&s_audio_ready) && atomic_load(&s_audio_requested) && !s_suspended &&
        atomic_load(&s_audio_generation)==atomic_load(&s_audio_applied);
}
static QueueHandle_t s_tests;
typedef struct { unsigned duration,mode; } test_t;
static TaskHandle_t s_task;
/* Reserve the codec's internal stack before Wi-Fi/BLE fragment the heap.
 * BLE+Wi-Fi left 56 KB total but no contiguous block larger than 31 KB.
 * Keep this stack internal: voice setup can write NVS while flash cache is off. */
static StackType_t s_voice_stack[40960] __attribute__((aligned(16)));
static StaticTask_t s_voice_tcb;
static unsigned s_codec_frames,s_encode_max,s_decode_max;
typedef struct { uint8_t action,n;int16_t samples[80]; } replay_t;
static QueueHandle_t s_replay;
static volatile bool s_replaying;
static volatile bool s_chat_request;
bool voice_busy(void) { return s_ptt || s_chat_request || s_state!=LX_VOICE_IDLE; }
bool voice_audio_stopped(void)
{ return !atomic_load(&s_audio_requested) && !atomic_load(&s_audio_ready) && atomic_load(&s_audio_generation)==atomic_load(&s_audio_applied); }
esp_err_t voice_chat_request(void) { if(!voice_audio_ready() || s_state!=LX_VOICE_IDLE) return ESP_ERR_INVALID_STATE;power_policy_activity();s_chat_request=true;return ESP_OK; }
static bool cancelled(void) { return s_cancel || !voice_audio_ready(); }
static SemaphoreHandle_t s_data;
static const char *names[]={"idle","recording","recognizing","chat"};
static bool (*s_text_submit)(const char *);
static bool (*s_text_busy)(void);
void voice_set_text_handler(bool (*submit)(const char *),bool (*busy)(void),void (*cancel)(void))
{ s_text_submit=submit;s_text_busy=busy;s_text_cancel=cancel; }
void voice_audio_enable(bool enabled)
{
    if(enabled && power_policy_shutting_down()) return;
    if(atomic_exchange(&s_audio_requested,enabled)==enabled) return;
    /* Generation mismatch stays cancelled through a rapid OFF/ON until the
     * old synchronous operation and its workers have returned to the owner. */
    atomic_fetch_add(&s_audio_generation,1);
    atomic_store(&s_audio_ready,false);
    if(!enabled && s_text_cancel) s_text_cancel();
    if(s_task) xTaskNotifyGive(s_task);
}
void voice_ptt(bool down)
{
    bool pressed=down && !s_ptt;s_ptt=down;
    /* The chat owner is blocked in its network loop. Latch the edge here so
     * even a short press stops chat; resource cleanup stays on that owner. */
    if(pressed && voice_audio_ready() && atomic_load(&s_chat_preemptible))
        atomic_store(&s_chat_preempt,true);
}
void voice_cancel(void) { s_cancel=true; }
void voice_suspend(void) { s_suspended=true;s_cancel=true;atomic_fetch_add(&s_audio_generation,1);if(s_task)xTaskNotifyGive(s_task); }
void voice_wake_enable(bool enabled) { s_wake_enabled=enabled;s_suspended=false; }
esp_err_t voice_replay(uint8_t action,const int16_t *pcm,size_t n)
{
    if(action>2 || n>80 || (action==1 && !n)) return ESP_ERR_INVALID_ARG;
    replay_t item={.action=action,.n=n};if(n) memcpy(item.samples,pcm,n*sizeof(*pcm));
    if(!voice_audio_ready()) return ESP_ERR_INVALID_STATE;
    return xQueueSend(s_replay,&item,0)==pdTRUE?ESP_OK:ESP_ERR_TIMEOUT;
}
esp_err_t voice_test_capture(unsigned duration)
{
    if(duration<200 || duration>20000) return ESP_ERR_INVALID_ARG;
    if(!voice_audio_ready()) return ESP_ERR_INVALID_STATE;
    test_t test={.duration=duration};
    if(xQueueSend(s_tests,&test,0)!=pdTRUE) return ESP_ERR_TIMEOUT;
    /* Exercise the same cleanup handoff with bounded local capture, without
     * cloud dictation or generated HID input. */
    if(atomic_load(&s_chat_preemptible)) atomic_store(&s_chat_preempt,true);
    return ESP_OK;
}
esp_err_t voice_test_codec(unsigned duration)
{
    if(duration<200 || duration>20000) return ESP_ERR_INVALID_ARG;
    if(!voice_audio_ready()) return ESP_ERR_INVALID_STATE;
    test_t test={.duration=duration,.mode=1};return xQueueSend(s_tests,&test,0)==pdTRUE?ESP_OK:ESP_ERR_TIMEOUT;
}
esp_err_t voice_test_opus(void)
{
    if(!voice_audio_ready()) return ESP_ERR_INVALID_STATE;
    test_t test={.mode=2};return xQueueSend(s_tests,&test,0)==pdTRUE?ESP_OK:ESP_ERR_TIMEOUT;
}
esp_err_t voice_test_chat(void)
{
    if(!voice_audio_ready()) return ESP_ERR_INVALID_STATE;
    test_t test={.mode=3};return xQueueSend(s_tests,&test,0)==pdTRUE?ESP_OK:ESP_ERR_TIMEOUT;
}
esp_err_t voice_chat_setup(bool activate)
{
    if(!voice_audio_ready() || s_state!=LX_VOICE_IDLE) return ESP_ERR_INVALID_STATE;
    test_t test={.mode=activate?5:4};return xQueueSend(s_tests,&test,0)==pdTRUE?ESP_OK:ESP_ERR_TIMEOUT;
}
esp_err_t voice_chat_setup_ui(uint32_t owner)
{
    if(!voice_audio_ready() || voice_busy() || power_policy_shutting_down())return ESP_ERR_INVALID_STATE;
    test_t test={.mode=13,.duration=owner};return xQueueSend(s_tests,&test,0)==pdTRUE?ESP_OK:ESP_ERR_TIMEOUT;
}
esp_err_t voice_chat_probe(void)
{
    if(!voice_audio_ready() || s_state!=LX_VOICE_IDLE) return ESP_ERR_INVALID_STATE;
    test_t test={.mode=6};return xQueueSend(s_tests,&test,0)==pdTRUE?ESP_OK:ESP_ERR_TIMEOUT;
}
esp_err_t voice_chat_diagnostic(bool local)
{
    if(!voice_audio_ready() || s_state!=LX_VOICE_IDLE) return ESP_ERR_INVALID_STATE;
    test_t test={.mode=local?8:7};return xQueueSend(s_tests,&test,0)==pdTRUE?ESP_OK:ESP_ERR_TIMEOUT;
}
esp_err_t voice_test_activation(void)
{
    if(!voice_audio_ready() || s_state!=LX_VOICE_IDLE) return ESP_ERR_INVALID_STATE;
    test_t test={.mode=9};return xQueueSend(s_tests,&test,0)==pdTRUE?ESP_OK:ESP_ERR_TIMEOUT;
}
esp_err_t voice_test_aec(void)
{
    if(!voice_audio_ready() || s_state!=LX_VOICE_IDLE) return ESP_ERR_INVALID_STATE;
    test_t test={.mode=10};return xQueueSend(s_tests,&test,0)==pdTRUE?ESP_OK:ESP_ERR_TIMEOUT;
}
esp_err_t voice_test_asr(void)
{
    if(!voice_audio_ready() || s_state!=LX_VOICE_IDLE) return ESP_ERR_INVALID_STATE;
    test_t test={.mode=11};return xQueueSend(s_tests,&test,0)==pdTRUE?ESP_OK:ESP_ERR_TIMEOUT;
}
esp_err_t voice_test_dictation(unsigned duration)
{
    if(duration<200 || duration>20000) return ESP_ERR_INVALID_ARG;
    if(!voice_audio_ready() || s_state!=LX_VOICE_IDLE || s_suspended) return ESP_ERR_INVALID_STATE;
    test_t test={.duration=duration,.mode=12};
    return xQueueSend(s_tests,&test,0)==pdTRUE?ESP_OK:ESP_ERR_TIMEOUT;
}
typedef struct {
    atomic_uint available;
    atomic_bool finished,stop;
    SemaphoreHandle_t done;
    int64_t deadline,released_us;
    size_t consumed;
    esp_err_t error;
    bool accepted;
} live_ptt_t;
static void live_capture(void *arg)
{
    live_ptt_t *p=arg;unsigned used=0,warmup=4;
    p->error=board_audio_capture_fresh_begin();
    if(p->error!=ESP_OK) goto done;
    unsigned overflows=board_audio_rx_overflows();
    s_captures++;puts("LX_VOICE recording");
    while(!cancelled() && !atomic_load(&p->stop) && used<MAX_SAMPLES &&
          (p->deadline?esp_timer_get_time()<p->deadline:s_ptt)) {
        int16_t frame[320];
        p->error=board_audio_capture_read(frame);if(p->error!=ESP_OK) break;
        if(warmup) { warmup--;continue; }
        memcpy(s_pcm+used,frame,sizeof(frame));used+=320;s_samples=used;
        atomic_store_explicit(&p->available,used,memory_order_release);
    }
    p->released_us=esp_timer_get_time();
    overflows=board_audio_rx_overflows()-overflows;
    if(overflows && p->error==ESP_OK) p->error=ESP_FAIL;
    board_audio_capture_end();
    s_state=LX_VOICE_RECOGNIZING;
    printf("LX_ASR_RELEASE samples=%u adc_overflow=%u\n",used,overflows);
done:
    atomic_store_explicit(&p->finished,true,memory_order_release);
    xSemaphoreGive(p->done);vTaskDeleteWithCaps(NULL);
}
static esp_err_t live_read(void *context,int16_t *pcm,size_t *samples)
{
    live_ptt_t *p=context;
    while(!cancelled()) {
        bool finished=atomic_load_explicit(&p->finished,memory_order_acquire);
        size_t available=atomic_load_explicit(&p->available,memory_order_acquire);
        if(finished && p->error!=ESP_OK) return p->error;
        if(finished && available<3200) return ESP_ERR_INVALID_SIZE;
        size_t count=available-p->consumed;
        if(count>=960 || finished) {
            if(count>960) count=960;
            if(count) memcpy(pcm,s_pcm+p->consumed,count*sizeof(*pcm));
            p->consumed+=count;*samples=count;return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return ESP_ERR_INVALID_STATE;
}
static bool live_submit(void *context,const char *text)
{
    live_ptt_t *p=context;
    /* Explicit timed diagnostics only; normal physical PTT never logs speech. */
    if(p->deadline) {
        printf("LX_ASR_RESULT utf8_hex=");
        for(const unsigned char *c=(const unsigned char*)text;*c;c++) printf("%02x",*c);
        puts("");
    }
    if(!cancelled() && s_text_submit) p->accepted=s_text_submit(text);
    printf("LX_ASR_LATENCY release_to_queue_ms=%lld text_queued=%u\n",
        (esp_timer_get_time()-p->released_us)/1000,p->accepted);
    return p->accepted;
}
static bool recognize_live(int64_t deadline)
{
    network_power_save(false);power_policy_activity();
    if(xSemaphoreTake(s_data,0)!=pdTRUE) { s_rejected++;return false; }
    live_ptt_t p={.deadline=deadline};
    atomic_init(&p.available,0);atomic_init(&p.finished,false);atomic_init(&p.stop,false);
    p.done=xSemaphoreCreateBinary();s_samples=0;s_state=LX_VOICE_RECORDING;
    esp_err_t rc=ESP_ERR_NO_MEM;
    /* Only capture runs on core 1, below key scan/send. The existing voice task
     * retains Opus/TLS on core 0. PSRAM buffers preserve audio during handshake. */
    if(p.done && xTaskCreatePinnedToCoreWithCaps(live_capture,"ptt_capture",6144,&p,4,NULL,1,
                                               MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)==pdPASS) {
        /* WakeNet is not fed during PTT. Reuse its scarce internal scratch RAM
         * for TLS/Opus while BLE remains active; reset below restores it. */
        wake_word_suspend();
        rc=chat_recognize_stream(live_read,&p,live_submit,cancelled);
        atomic_store(&p.stop,true);xSemaphoreTake(p.done,portMAX_DELAY);
    }
    if(p.done) vSemaphoreDelete(p.done);
    memset(s_pcm,0,s_samples*sizeof(*s_pcm));xSemaphoreGive(s_data);
    printf("LX_VOICE samples=%u duration_ms=%u asr=%s text_queued=%u\n",s_samples,s_samples/16,esp_err_to_name(rc),p.accepted);
    puts("LX_VOICE asr_provider=xiaozhi streaming=1");
    if(voice_audio_ready()) wake_word_reset();
    return p.accepted && !cancelled();
}
static bool chat_cancelled(void)
{
    return cancelled() || atomic_load(&s_chat_preempt);
}
static esp_err_t interruptible_chat(bool local)
{
    network_power_save(false);power_policy_activity();
    atomic_store(&s_chat_preempt,false);
    atomic_store(&s_chat_preemptible,true);
    /* Catch a press between the owner's last scan and entering this call. */
    if(s_ptt) atomic_store(&s_chat_preempt,true);
    esp_err_t result=local?chat_local_test(chat_cancelled,NULL):chat_run(chat_cancelled,NULL);
    atomic_store(&s_chat_preemptible,false);
    bool takeover=atomic_exchange(&s_chat_preempt,false);
    printf("LX_CHAT_HANDOFF requested=%u held=%u audio_ready=%u\n",takeover,s_ptt,voice_audio_ready());
    /* chat_run has stopped playback/capture and closed the old connection.
     * A held PTT is processed as a fresh press on the next owner iteration.
     * Released taps stop chat but must not start a phantom dictation. */
    if(voice_audio_ready() && !s_ptt) wake_word_reset();
    return result;
}
static void voice_task(void *arg)
{
    (void)arg;lx_voice_state_t state=LX_VOICE_IDLE;bool last_ptt=false,recording=false;
    int64_t deadline=0,replay_until=0;unsigned warmup=0;
    vTaskDelay(pdMS_TO_TICKS(200));
    bool wake_started=false,wake_ok=false,shutdown_cue_done=false;
    bool boot_cue=esp_reset_reason()==ESP_RST_POWERON;
    unsigned handled=UINT_MAX;
    bool waiting_text=false,diagnostic=false,codec_test=false;voice_opus_t codec={0};
    int16_t *decoded=heap_caps_malloc(1920*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    uint8_t *packet=heap_caps_malloc(1536,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    while(1) {
        unsigned generation=atomic_load(&s_audio_generation);
        if(generation!=handled) {
            atomic_store(&s_audio_ready,false);
            if(recording) { board_audio_capture_end();recording=false;xSemaphoreGive(s_data); }
            if(codec_test) { voice_opus_close(&codec);codec_test=false; }
            state=LX_VOICE_IDLE;s_state=state;deadline=0;waiting_text=false;diagnostic=false;
            s_replaying=false;s_chat_request=false;s_cancel=false;
            xQueueReset(s_tests);xQueueReset(s_replay);
            if(xSemaphoreTake(s_data,0)==pdTRUE) {
                memset(s_pcm,0,MAX_SAMPLES*sizeof(*s_pcm));s_samples=0;xSemaphoreGive(s_data);
            }
            wake_word_suspend();
            if(power_policy_shutting_down() && !shutdown_cue_done) {
                shutdown_cue_done=true;
                if(user_controls_sound_on()) printf("LX_POWER shutdown_cue=%s\n",esp_err_to_name(board_audio_shutdown_cue()));
            }
            s_audio_result=board_audio_stop();
            if(s_audio_result!=ESP_OK) {
                /* A diagnostic may still hold the lease. Never cut its power. */
                s_audio_failures++;vTaskDelay(pdMS_TO_TICKS(50));continue;
            }
            handled=generation;s_audio_transitions++;
            if(atomic_load(&s_audio_requested) && !s_suspended &&
               generation==atomic_load(&s_audio_generation)) {
                /* Allocate mandatory DMA before recreating WakeNet scratch. */
                s_audio_result=board_audio_init();
                if(s_audio_result==ESP_OK) {
                    if(!wake_started) { s_audio_result=wake_word_start();wake_started=s_audio_result==ESP_OK; }
                    else wake_word_reset();
                    wake_ok=wake_word_ready();
                    if(!wake_ok && s_audio_result==ESP_OK) s_audio_result=ESP_ERR_NO_MEM;
                }
                if(s_audio_result==ESP_OK && generation==atomic_load(&s_audio_generation) &&
                   atomic_load(&s_audio_requested) && !s_suspended) {
                    atomic_store(&s_audio_applied,generation);atomic_store(&s_audio_ready,true);
                    if(boot_cue) printf("LX_POWER startup_cue=%s\n",esp_err_to_name(board_audio_startup_cue()));
                } else {
                    wake_word_suspend();board_audio_stop();
                    if(s_audio_result!=ESP_OK) s_audio_failures++;
                }
            }
            if(!atomic_load(&s_audio_requested) && generation==atomic_load(&s_audio_generation))
                atomic_store(&s_audio_applied,generation);
            boot_cue=false;last_ptt=s_ptt;
            printf("LX_AUDIO_MASTER requested=%u ready=%u generation=%u result=%s\n",
                atomic_load(&s_audio_requested),voice_audio_ready(),generation,
                esp_err_to_name(s_audio_result));
        }
        if(!voice_audio_ready()) {
            last_ptt=s_ptt;ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(200));continue;
        }

        replay_t replay;
        if(xQueueReceive(s_replay,&replay,0)==pdTRUE) {
            replay_until=esp_timer_get_time()+5000000;
            if(replay.action==0 && state==LX_VOICE_IDLE) { s_replaying=true;wake_word_reset(); }
            else if(replay.action==2) { s_replaying=false;wake_word_reset(); }
            else if(replay.action==1 && s_replaying && state==LX_VOICE_IDLE) {
                if(wake_word_feed(replay.samples,replay.n)) puts("LX_WAKE replay_detected=1 acoustic=not_tested");
            } else s_rejected++;
        }
        if(s_replaying && esp_timer_get_time()>replay_until) { s_replaying=false;wake_word_reset(); }
        test_t test;
        if(xQueueReceive(s_tests,&test,0)==pdTRUE) {
            if(test.mode==13) {
                bool allowed=state==LX_VOICE_IDLE && !s_suspended;
                if(allowed) { s_state=LX_VOICE_CHAT;wake_word_suspend(); }
                printf("LX_ACTIVATION_UI complete=%s\n",esp_err_to_name(activation_ui_run(test.duration,allowed,cancelled)));
                if(allowed) { last_ptt=s_ptt;if(voice_audio_ready())wake_word_reset();s_state=LX_VOICE_IDLE; }
            }
            else if(test.mode==2 && state==LX_VOICE_IDLE) { printf("LX_OPUS failures=%u\n",voice_opus_selftest()); }
            else if(test.mode==11 && state==LX_VOICE_IDLE && !s_suspended) {
                s_state=LX_VOICE_RECOGNIZING;wake_word_suspend();
                printf("LX_ASR_TEST complete=%s\n",esp_err_to_name(chat_recognize_test(cancelled)));
                last_ptt=s_ptt;if(voice_audio_ready()) wake_word_reset();s_state=LX_VOICE_IDLE;
            }
            else if(test.mode==10 && state==LX_VOICE_IDLE && !s_suspended) {
                s_state=LX_VOICE_CHAT;wake_word_suspend();
                printf("LX_AEC_TEST result=%s cloud=0\n",esp_err_to_name(voice_duplex_test(cancelled)));
                last_ptt=s_ptt;if(voice_audio_ready()) wake_word_reset();s_state=LX_VOICE_IDLE;
            }
            else if(test.mode==9 && state==LX_VOICE_IDLE && !s_suspended) {
                s_state=LX_VOICE_CHAT;
                esp_err_t rc=activation_audio_play("012345",cancelled);
                if(rc==ESP_OK && !cancelled()) rc=activation_audio_play("678901",cancelled);
                printf("LX_ACTIVATION_AUDIO_TEST complete=%s real_activation=0\n",esp_err_to_name(rc));
                last_ptt=s_ptt;if(voice_audio_ready()) wake_word_reset();s_state=LX_VOICE_IDLE;
            }
            else if(test.mode==3 && state==LX_VOICE_IDLE && !s_suspended) {
                s_state=LX_VOICE_CHAT;wake_word_suspend();
                printf("LX_CHAT_TEST complete=%s\n",esp_err_to_name(interruptible_chat(true)));
                last_ptt=false;s_state=LX_VOICE_IDLE;
            }
            else if((test.mode==7 || test.mode==8) && state==LX_VOICE_IDLE && !s_suspended) {
                if(xSemaphoreTake(s_data,0)==pdTRUE) {
                    s_state=LX_VOICE_CHAT;s_samples=0;wake_word_suspend();
                    /* Local fixture sends 9600 samples; a non-frame-aligned cap
                     * exercises a partial final copy and the full-buffer path. */
                    chat_capture_t capture={.pcm=s_pcm,.capacity=test.mode==8?4000:16000*8};
                    esp_err_t rc=test.mode==8?chat_local_test(cancelled,&capture):chat_run(cancelled,&capture);
                    if(test.mode==8 && rc==ESP_OK && capture.used!=4000) rc=ESP_FAIL;
                    s_samples=capture.used;
                    printf("LX_CHAT_CAPTURE complete=%s samples=%u rate=16000 prefix_ms=%u cloud=%u\n",esp_err_to_name(rc),s_samples,s_samples/16,test.mode==7);
                    xSemaphoreGive(s_data);last_ptt=s_ptt;if(voice_audio_ready()) wake_word_reset();s_state=LX_VOICE_IDLE;
                } else s_rejected++;
            }
            else if(test.mode==6 && state==LX_VOICE_IDLE && !s_suspended) {
                s_state=LX_VOICE_CHAT;
                printf("LX_CHAT_PROBE complete=%s\n",esp_err_to_name(chat_probe(cancelled)));
                last_ptt=s_ptt;if(voice_audio_ready()) wake_word_reset();s_state=LX_VOICE_IDLE;
            }
            else if((test.mode==4 || test.mode==5) && state==LX_VOICE_IDLE && !s_suspended) {
                s_state=LX_VOICE_CHAT;
                printf("LX_CHAT_OFFICIAL complete=%s\n",esp_err_to_name(chat_official_setup(test.mode==5,cancelled)));
                last_ptt=s_ptt;if(voice_audio_ready()) wake_word_reset();s_state=LX_VOICE_IDLE;
            }
            else if((test.mode<2 || test.mode==12) && lx_voice_event(&state,LX_VOICE_PRESS)) {
                diagnostic=test.mode!=12;codec_test=test.mode==1;
                if(codec_test && (!decoded || !packet || voice_opus_open(&codec,16000)!=ESP_OK)) { s_errors++;state=LX_VOICE_IDLE;codec_test=false; }
                deadline=esp_timer_get_time()+test.duration*1000LL;
            }
            else s_rejected++;
        }
        bool ptt=s_ptt;
        if(ptt!=last_ptt && !s_suspended) {
            if(ptt && state==LX_VOICE_IDLE) diagnostic=false;
            if(!lx_voice_event(&state,ptt?LX_VOICE_PRESS:LX_VOICE_RELEASE)) s_rejected++;
            last_ptt=ptt;
        }
        if(s_cancel) { lx_voice_event(&state,LX_VOICE_CANCEL);s_cancel=false;deadline=0;waiting_text=false; }
        if(s_suspended) {
            if(recording) { board_audio_capture_end();recording=false;xSemaphoreGive(s_data); }
            if(codec_test) { voice_opus_close(&codec);codec_test=false; }
            s_state=LX_VOICE_IDLE;last_ptt=ptt;vTaskDelay(pdMS_TO_TICKS(10));continue;
        }
        if(deadline && esp_timer_get_time()>=deadline) { lx_voice_event(&state,LX_VOICE_RELEASE);deadline=0; }
        if(s_chat_request) { s_chat_request=false;if(!lx_voice_event(&state,LX_VOICE_WAKE)) s_rejected++; }
        if(state==LX_VOICE_RECORDING && !diagnostic && !recording) {
            waiting_text=recognize_live(deadline);deadline=0;last_ptt=s_ptt;
            state=waiting_text?LX_VOICE_RECOGNIZING:LX_VOICE_IDLE;
            s_state=state;continue;
        }
        if(state==LX_VOICE_RECORDING && !recording) {
            if(xSemaphoreTake(s_data,0)!=pdTRUE) { s_rejected++;state=LX_VOICE_IDLE; }
            else {
                esp_err_t result=board_audio_capture_begin();
                if(result!=ESP_OK) { xSemaphoreGive(s_data);s_errors++;state=LX_VOICE_IDLE;printf("LX_VOICE capture=%s\n",esp_err_to_name(result)); }
                else { s_samples=0;warmup=4;recording=true;s_captures++;wake_word_reset();puts("LX_VOICE recording"); }
            }
        }
        if(recording && state!=LX_VOICE_RECORDING) { board_audio_capture_end();recording=false;xSemaphoreGive(s_data); }
        if(codec_test && state!=LX_VOICE_RECORDING) { voice_opus_close(&codec);codec_test=false; }
        if(state==LX_VOICE_RECORDING) {
            int16_t frame[320];esp_err_t result=board_audio_capture_read(frame);
            if(result!=ESP_OK) { if(voice_audio_ready()) s_errors++;state=LX_VOICE_IDLE; }
            else if(warmup) warmup--;
            else {
                memcpy(s_pcm+s_samples,frame,sizeof(frame));s_samples+=320;
                if(codec_test && s_samples%960==0) {
                    size_t bytes=0,samples=0;esp_err_t rc=voice_opus_encode(&codec,s_pcm+s_samples-960,960,packet,1536,&bytes);
                    if(rc==ESP_OK) rc=voice_opus_decode(&codec,packet,bytes,decoded,1920,&samples);
                    if(rc!=ESP_OK || samples!=960) { s_errors++;state=LX_VOICE_IDLE; }
                    else s_codec_frames++;
                    if(codec.encode_max_us>s_encode_max) s_encode_max=codec.encode_max_us;
                    if(codec.decode_max_us>s_decode_max) s_decode_max=codec.decode_max_us;
                }
                if(s_samples==MAX_SAMPLES) { lx_voice_event(&state,LX_VOICE_RELEASE);deadline=0; }
            }
        } else if(state==LX_VOICE_RECOGNIZING) {
            if(diagnostic) { puts("LX_VOICE diagnostic_complete cloud_upload=0");lx_voice_event(&state,LX_VOICE_DONE);diagnostic=false; }
            else if(waiting_text) {
                if(!s_text_busy || !s_text_busy()) { waiting_text=false;lx_voice_event(&state,LX_VOICE_DONE); }
            } else lx_voice_event(&state,LX_VOICE_DONE);
        } else if(state==LX_VOICE_CHAT) {
            s_state=state;wake_word_suspend();esp_err_t result=interruptible_chat(false);
            printf("LX_VOICE chat=%s\n",esp_err_to_name(result));
            lx_voice_event(&state,LX_VOICE_DONE);last_ptt=false;
        } else if(state==LX_VOICE_IDLE && s_wake_enabled && wake_ok && !s_replaying) {
            if(board_audio_capture_begin()==ESP_OK) {
                int16_t frame[320];esp_err_t result=board_audio_capture_read(frame);
                board_audio_capture_end();
                if(result==ESP_OK && wake_word_feed(frame,320)) {
                    power_policy_activity();
                    lx_voice_event(&state,LX_VOICE_WAKE);
                    puts("LX_VOICE wake=nihaoxiaozhi");
                }
            }
        }
        s_state=state;
        if(!recording) vTaskDelay(pdMS_TO_TICKS(state==LX_VOICE_IDLE && s_wake_enabled && wake_ok?1:10));
    }
}
esp_err_t voice_capture_start(void)
{
    printf("LX_VOICE_MEMORY internal_free=%u largest=%u\n",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    s_pcm=heap_caps_malloc(MAX_SAMPLES*sizeof(*s_pcm),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    s_tests=xQueueCreate(2,sizeof(test_t));s_data=xSemaphoreCreateMutex();s_replay=xQueueCreate(8,sizeof(replay_t));
    if(!s_pcm || !s_tests || !s_data || !s_replay) return ESP_ERR_NO_MEM;
    /* Vendor encoders require up to 40 KB including their internal scratch stack. */
    s_task=xTaskCreateStaticPinnedToCore(voice_task,"voice_capture",sizeof(s_voice_stack),NULL,4,
        s_voice_stack,&s_voice_tcb,0);
    return s_task?ESP_OK:ESP_ERR_NO_MEM;
}
void voice_capture_stats(void)
{
    printf("LX_AUDIO_MASTER requested=%u ready=%u generation=%u applied=%u transitions=%u failures=%u result=%s\n",
        atomic_load(&s_audio_requested),voice_audio_ready(),atomic_load(&s_audio_generation),
        atomic_load(&s_audio_applied),s_audio_transitions,s_audio_failures,esp_err_to_name(s_audio_result));
    printf("LX_VOICE state=%s samples=%u captures=%u errors=%u rejected=%u ptt=%u wake_enabled=%u suspended=%u asr_configured=%u chat_configured=%u\n",
        names[s_state],s_samples,s_captures,s_errors,s_rejected,s_ptt,s_wake_enabled,s_suspended,chat_configured(),chat_configured());
    board_audio_stats();
    puts("LX_VOICE asr_provider=xiaozhi");
    wake_word_stats();
    chat_stats();
    printf("LX_VOICE_CODEC frames=%u encode_max_us=%u decode_max_us=%u stack_free=%u\n",s_codec_frames,s_encode_max,s_decode_max,(unsigned)uxTaskGetStackHighWaterMark(s_task));
}
void voice_capture_dump(void)
{
    if(xSemaphoreTake(s_data,0)!=pdTRUE) { puts("LX_PCM busy");return; }
    /* Diagnostic-only binary-free UART dump. Each frame is independently
     * line delimited; logs can interleave without corrupting sample bytes. */
    printf("LX_PCM_BEGIN samples=%u rate=16000 channels=1 bits=16\n",s_samples);
    for(unsigned offset=0;offset<s_samples;offset+=160) {
        unsigned n=s_samples-offset;if(n>160) n=160;
        char line[700];unsigned used=snprintf(line,sizeof(line),"LX_PCM %u ",offset);
        for(unsigned i=0;i<n;i++) {
            uint16_t value=s_pcm[offset+i];
            used+=snprintf(line+used,sizeof(line)-used,"%02x%02x",value&255,value>>8);
        }
        puts(line);vTaskDelay(1);
    }
    puts("LX_PCM_END");xSemaphoreGive(s_data);
}
