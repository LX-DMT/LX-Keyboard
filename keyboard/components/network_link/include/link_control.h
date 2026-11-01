#pragma once
#include "esp_err.h"
#include "lx_protocol.h"
enum { LX_CONTROL_USB=1,LX_CONTROL_RADIO=2,LX_CONTROL_WIFI=3 };
esp_err_t link_control_start(bool receiver,uint8_t mode);
/* Empty reads identity; 32 changes mode; 33 submits CPU/GPU/RAM + target MAC. */
void link_control_set_stats_handler(void (*handler)(const uint8_t values[3]));
esp_err_t link_control_apply(const uint8_t *data,size_t length,uint8_t reply[10]);
void link_control_receive(unsigned route,const lx_packet_t *packet);
void link_control_forward(unsigned route,const lx_packet_t *packet);
size_t link_control_take(unsigned route,uint8_t bytes[LX_PACKET_MAX]);
void link_control_drop(unsigned route);
