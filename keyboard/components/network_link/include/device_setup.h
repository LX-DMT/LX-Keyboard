#pragma once
#include "network_link.h"
#include "lx_protocol.h"

/* USB-only versioned provisioning. No secret is returned by INFO/STATUS. */
typedef struct {
    uint8_t version, flags; /* bit0 Wi-Fi, bit1 receiver pair, bit2 PC binding */
    char ssid[33], password[64];
    uint8_t peer[6], pair_key[16], pc_id[16], pc_key[32];
} lx_setup_config_t;
void device_setup_init(bool receiver);
void device_setup_receive(const lx_packet_t *packet);
void device_setup_poll(void);
bool device_setup_busy(void);
bool device_setup_profile(char ssid[33], char password[64]);
bool device_setup_peer(uint8_t mac[6], uint8_t key[16], bool *encrypted);
bool device_setup_pc(uint8_t id[16], uint8_t key[32]);
bool device_setup_bound(void);
esp_err_t network_trial(const char *ssid, const char *password);
void network_trial_end(void);
void network_status(uint8_t out[8]);
