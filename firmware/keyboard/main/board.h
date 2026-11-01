#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "hid_mouse.h"

#define BOARD_SCAN_BYTES 11
enum {
    PIN_POWER=45, PIN_KNOB_POWER=39, PIN_USB_MODE=4, PIN_USB_SD_EN=6, PIN_FN=9,
    PIN_SCAN_PL=3, PIN_SCAN_CE=7, PIN_SCAN_CLK=12, PIN_SCAN_MISO=13,
    PIN_KNOB_TX=14, PIN_KNOB_RX=21,
    PIN_RGB_POWER=5, PIN_RGB_DATA=38, PIN_SIDE_LED=42, PIN_AUDIO_POWER_OFF=1,
    PIN_AUDIO_SDA=8, PIN_AUDIO_SCL=18, PIN_AUDIO_MCLK=2,
    PIN_AUDIO_BCLK=17, PIN_AUDIO_WS=47, PIN_AUDIO_DOUT=15, PIN_AUDIO_DIN=16,
    PIN_POWER_SDA=41, PIN_POWER_SCL=40,
};
esp_err_t board_init(bool usb_host);
esp_err_t board_scan(uint8_t data[BOARD_SCAN_BYTES]);
void board_print_i2c(void);
void board_print_uart(void);
esp_err_t board_knob_power(bool on);
bool board_knob_powered(void);
int board_knob_write(const void *data,size_t size);
int board_knob_power_level(void);
void board_led_test(void);
void board_side_led_tick(void);
void board_side_led_stats(void);
esp_err_t board_led_rgb(uint8_t r,uint8_t g,uint8_t b,uint8_t battery_v);
esp_err_t board_lights_off(void);
void board_power_off(void);
esp_err_t board_power_read(uint16_t *mv,int16_t *ma);
unsigned board_audio_level(void);
void board_audio_stats(void);
esp_err_t board_audio_setup(void);
esp_err_t board_audio_capture_begin(void);
esp_err_t board_audio_capture_fresh_begin(void);
esp_err_t board_audio_capture_read(int16_t mono[320]);
void board_audio_capture_end(void);
esp_err_t board_audio_tone(void);
esp_err_t board_audio_shutdown_cue(void);
esp_err_t board_audio_startup_cue(void);
esp_err_t board_audio_tone_volume(unsigned volume);
esp_err_t board_audio_play_begin(void);
esp_err_t board_audio_play_write(const int16_t *mono,size_t samples);
void board_audio_play_end(void);
/* Duplex owner holds the audio lock for the entire session. 240 samples = DMA block. */
esp_err_t board_audio_duplex_begin(void);
esp_err_t board_audio_duplex_read(int16_t mic[240],int16_t ref[240]);
void board_audio_duplex_end(void);
unsigned board_audio_rx_overflows(void);
i2c_master_bus_handle_t board_audio_bus(void);
esp_err_t board_audio_init(void);
void board_audio_test(void);
void board_audio_channel_test(void);
esp_err_t board_audio_stop(void);
esp_err_t usb_host_base_start(void (*mouse_input)(const lx_mouse_sample_t *));
void board_print_mouse(void);
esp_err_t board_mouse_recover(bool fault);
