#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

#define HUB_MAX_RECORDS 40
#define HUB_MAX_BYTES (8u * 1024 * 1024)
#define HUB_MAX_TEXT (64u * 1024)
#define HUB_MAX_CARD (10u + 80u + 224u * 256u * 2u)

/* Caller serializes access. Content is immutable; the two checked catalogs
 * are the commit point. An interrupted upload is never a visible record. */
typedef struct {
    char root[128];
    cJSON *index;
    cJSON *upload;
    uint32_t generation;
    size_t content_offset, card_offset;
    bool ready;
} hub_store_t;

bool hub_store_open(hub_store_t *s, const char *root);
void hub_store_close(hub_store_t *s);
size_t hub_store_used(hub_store_t *s);
cJSON *hub_store_records(hub_store_t *s); /* borrowed */
cJSON *hub_store_find(hub_store_t *s, uint32_t id); /* borrowed */
const char *hub_store_begin(hub_store_t *s, const cJSON *meta, uint32_t *existing);
const char *hub_store_chunk(hub_store_t *s, const char *event, bool card,
                            size_t offset, const uint8_t *data, size_t length);
const char *hub_store_commit(hub_store_t *s, const char *event, uint32_t *id);
void hub_store_abort(hub_store_t *s);
const char *hub_store_favorite(hub_store_t *s, uint32_t id, bool favorite);
const char *hub_store_delete(hub_store_t *s, uint32_t id);
const char *hub_store_clear(hub_store_t *s);
const char *hub_store_pause(hub_store_t *s, bool paused);
bool hub_store_path(hub_store_t *s, uint32_t id, bool card, char *out, size_t size);
void hub_sha256(const uint8_t *data, size_t size, char out[65]);
bool hub_valid_id(const char *value);
