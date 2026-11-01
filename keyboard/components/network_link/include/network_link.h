#pragma once
void wifi_keys_cancel_pending(void);
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "input_core.h"
#include "hid_mouse.h"
esp_err_t network_start(bool join_router);
esp_err_t network_power_save(bool enabled);
bool network_power_save_enabled(void);
bool network_profile_present(void);
esp_err_t network_setting(const char *key,const char *value);
esp_err_t network_retry(void);
void network_auto_retry(bool enable);
void network_radio_activity(void);
esp_err_t network_disconnect_test(void);
esp_err_t network_apply_config(const uint8_t *payload,size_t length);
esp_err_t network_apply_mode(const uint8_t *payload,size_t length);
void network_set_mode_handler(esp_err_t (*handler)(uint8_t mode));
void network_set_extra_config_handler(esp_err_t (*handler)(uint8_t,const uint8_t *,size_t));
esp_err_t network_control_start(void);
bool network_ready(void);
bool network_target_pc(void);
unsigned network_discovery_selftest(void);
esp_err_t network_discovery_start(void);
uint32_t network_find_receiver(void);
void network_set_discovered_receiver(uint32_t address);
uint32_t network_peer_address(void);
void network_print(void);
esp_err_t wifi_keys_start(bool receiver);
void wifi_keys_submit(const lx_key_report_t *report,uint32_t sequence,int64_t event_us);
bool wifi_keys_text_room(bool *pending);
/* Validated Unicode BMP text, for a peer that identified itself as the PC. */
void wifi_consumer_submit(uint16_t usage);
void wifi_mouse_submit(const lx_mouse_sample_t *sample);
void wifi_host_stats(const uint8_t values[3]);
void wifi_set_stats_handler(void (*handler)(const uint8_t values[3]));
bool wifi_input_active(void);
void wifi_keys_stats(void);
void wifi_keys_trace_clear(void);
void wifi_keys_trace_print(void);
