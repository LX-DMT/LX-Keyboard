#pragma once
#include <stddef.h>
#include "esp_err.h"
#include "esp_websocket_client.h"

typedef struct chat_uplink chat_uplink_t;
/* Only local regression supplies stall_ms; cloud sessions always pass zero. */
chat_uplink_t *chat_uplink_start(esp_websocket_client_handle_t ws,unsigned stall_ms);
esp_err_t chat_uplink_push(chat_uplink_t *u,const void *bytes,size_t n);
esp_err_t chat_uplink_result(chat_uplink_t *u);
/* Cooperatively joins the sender before the caller destroys the websocket. */
void chat_uplink_stop(chat_uplink_t *u);
