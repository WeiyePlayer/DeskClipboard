#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

void hub_start(void);
cJSON *hub_request(const cJSON *request, bool usb, const char *token);
cJSON *hub_chunk(const char *token, const char *event, bool card, size_t offset,
                 const uint8_t *data, size_t length);
const char *hub_download(const char *token, const char *job, size_t offset,
                         size_t wanted, uint8_t *out, size_t *length);
void hub_ble_start(void);
bool hub_peer_credential(const char *id, char token[65]);
void hub_peer_disconnected(const char *token);
const char *hub_board_id(void);
bool hub_authorized(const char *token);
/* Phone pairing by code (hub_ble.c drives the PAKE; hub.c owns the window). */
const char *hub_pair_code(char code[7]);
void hub_pair_failed(void);
const char *hub_pair_add(const char *id, const char *name, const char *platform, const char *token);

/* Diagnostics for the freeze guard (guard.c). */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
TaskHandle_t hub_lock_holder(void);
TaskHandle_t hub_ble_lock_holder(void);
uint8_t hub_worker_stage(void);  /* 0 idle 1 lock wait 2 locked 10+action event 3 jobs 4 render 5 peers 6 notice 7 wake */
uint8_t hub_ble_stage(void);     /* 0 idle 1 got message 2 handshake 3 pairing 4 request 5 done */
uint32_t hub_last_event(uint32_t *at_ms);
