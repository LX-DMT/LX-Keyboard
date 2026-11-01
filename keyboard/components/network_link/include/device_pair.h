#pragma once
#include <stdint.h>

/* Synthetic locally administered addresses for discovery self-tests only.
 * Runtime pairing is read from provisioned NVS by device_setup_peer(). */
static const uint8_t keyboard_mac[]={0x02,0x00,0x00,0x00,0x00,0x01};
static const uint8_t receiver_mac[]={0x02,0x00,0x00,0x00,0x00,0x02};
