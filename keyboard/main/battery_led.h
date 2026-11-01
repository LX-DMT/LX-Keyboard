#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Physical LEDs 81..84, matching the old config's zero-based 80..83. */
#define BATTERY_LED_FIRST 80
#define BATTERY_LED_COUNT 4
#define BOARD_LED_COUNT 96
void battery_led_sample(bool valid,uint16_t mv,int16_t raw_ma);
typedef struct {
    uint32_t generation;
    bool valid,blocked,idle;
    uint16_t mv,load;
    unsigned other_lit;
    uint8_t brightness,pixels[4][3];
} battery_led_frame_t;
void battery_led_tick(bool awake,uint32_t setting,uint16_t peak_load);
bool battery_led_refresh_needed(void);
bool battery_led_blocked(void);
bool battery_led_protection_enabled(void);
void battery_led_protection_set(bool enabled);
void battery_led_overlay(uint8_t pixels[BOARD_LED_COUNT][3],uint8_t key_v,battery_led_frame_t *frame);
void battery_led_result(esp_err_t result,const battery_led_frame_t *frame);
void battery_led_stats(void);
unsigned battery_led_selftest(void);
