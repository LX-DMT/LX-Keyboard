#pragma once
#include <stdbool.h>
#include <stdint.h>

#define LX_SCAN_BYTES 11
#define LX_KEY_COUNT 88
#define LX_DEBOUNCE_US 5000U
#define LX_CUSTOM_KEY_COUNT 2
#define LX_CUSTOM_POWER_PREVIEW 0

typedef struct {
    uint8_t modifiers;
    uint8_t reserved;
    uint8_t keys[6];
} lx_key_report_t;

typedef struct {
    uint8_t candidate[LX_SCAN_BYTES], stable[LX_SCAN_BYTES];
    uint32_t changed_at[LX_KEY_COUNT];
    lx_key_report_t report;
    bool extra_keys, fn, ptt, power_preview;
    uint8_t custom_keys[LX_CUSTOM_KEY_COUNT],held_custom_keys[LX_CUSTOM_KEY_COUNT];
} lx_input_t;

void lx_input_init(lx_input_t *state, bool extra_keys);
bool lx_custom_key_valid(uint8_t action);
bool lx_input_set_custom(lx_input_t *state,const uint8_t actions[LX_CUSTOM_KEY_COUNT]);
bool lx_input_scan(lx_input_t *state, const uint8_t raw[LX_SCAN_BYTES], uint32_t now_us);
uint8_t lx_key_usage(unsigned raw_bit);
unsigned lx_input_selftest(void);
