#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include "status_led.h"
#include "bringup_usb.h"
#include "radio_keys.h"
#include "network_link.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 3V3 -> 1k -> LED -> GPIO13: invert the PWM output so duty means light. */
#define LED_PIN GPIO_NUM_13
#define LED_PEAK 256U
#define LED_FLOOR 4U
/* Transport leases last up to 500ms; add 1.5s for about 2s total loss grace. */
#define LINK_GRACE_US 1500000LL
typedef enum { LED_BOOT, LED_WAIT, LED_LINKED, LED_ERROR } led_state_t;
static atomic_bool s_ready, s_fault;
static atomic_uint s_state, s_duty, s_errors;
static const char *const s_names[]={"boot","waiting","breathing","error"};

static unsigned brightness(led_state_t state, int64_t elapsed_us)
{
    unsigned ms=(unsigned)((elapsed_us/1000)%6000);
    if(state==LED_BOOT)return LED_PEAK;
    if(state==LED_WAIT)return ms%2000<100?LED_PEAK:0;
    if(state==LED_ERROR)return ms%500<250?LED_PEAK:0;
    /* Smooth both ends without a fade interrupt or floating-point work. */
    unsigned phase=ms%3000;
    unsigned t=(phase<=1500?phase:3000-phase)*1000/1500;
    unsigned ease=(t*t*(3000-2*t))/1000000;
    return LED_FLOOR+(LED_PEAK-LED_FLOOR)*ease*ease/1000000;
}

static void led_task(void *arg)
{
    (void)arg;
    int64_t start=esp_timer_get_time(),entered=start,last_active=0;
    led_state_t previous=LED_BOOT;
    unsigned applied=LED_PEAK;
    TickType_t tick=xTaskGetTickCount();
    while(true) {
        int64_t now=esp_timer_get_time();
        bool usb=bringup_usb_mounted();
        /* These leases are refreshed by valid input/keepalives, not driver init.
         * Their 500ms expiry plus this grace tolerates brief radio loss. */
        if(s_ready && usb && (wifi_input_active() || radio_input_active()))last_active=now;
        led_state_t state;
        if(now-start<300000)state=LED_BOOT;
        else if(!s_ready || s_fault || !usb)state=LED_ERROR;
        else if(last_active && now-last_active<LINK_GRACE_US)state=LED_LINKED;
        else state=LED_WAIT;
        if(!usb)last_active=0;
        if(state!=previous) { previous=state;entered=now;s_state=state; }
        unsigned duty=brightness(state,now-entered);
        if(duty!=applied) {
            esp_err_t rc=ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,duty);
            if(rc==ESP_OK)rc=ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0);
            if(rc==ESP_OK) { applied=duty;s_duty=duty; } else s_errors++;
        }
        vTaskDelayUntil(&tick,pdMS_TO_TICKS(20));
    }
}

esp_err_t status_led_start(void)
{
    gpio_set_level(LED_PIN,1);
    gpio_set_direction(LED_PIN,GPIO_MODE_OUTPUT);
    ledc_timer_config_t timer={.speed_mode=LEDC_LOW_SPEED_MODE,
        .duty_resolution=LEDC_TIMER_10_BIT,.timer_num=LEDC_TIMER_0,.freq_hz=2000,.clk_cfg=LEDC_AUTO_CLK};
    esp_err_t rc=ledc_timer_config(&timer);
    if(rc!=ESP_OK)return rc;
    ledc_channel_config_t channel={.gpio_num=LED_PIN,.speed_mode=LEDC_LOW_SPEED_MODE,
        .channel=LEDC_CHANNEL_0,.timer_sel=LEDC_TIMER_0,.duty=LED_PEAK,.hpoint=0,
        .flags.output_invert=1};
    rc=ledc_channel_config(&channel);
    if(rc!=ESP_OK)return rc;
    s_state=LED_BOOT;s_duty=LED_PEAK;
    if(xTaskCreate(led_task,"status_led",2048,NULL,1,NULL)!=pdPASS) {
        ledc_stop(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,0);
        gpio_set_level(LED_PIN,1);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
void status_led_ready(void) { s_ready=true; }
void status_led_fault(void) { s_fault=true; }
void status_led_print(void)
{
    printf("LX_STATUS_LED gpio=13 active_low=1 state=%s duty=%u peak=%u errors=%u physical=not_measured\n",
        s_names[s_state],(unsigned)s_duty,LED_PEAK,(unsigned)s_errors);
}
