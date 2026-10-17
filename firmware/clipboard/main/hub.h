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
