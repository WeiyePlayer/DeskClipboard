#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*ui_tap_fn)(uint32_t id, uint8_t kind, uint8_t chip);
typedef void (*ui_menu_fn)(uint32_t id, uint8_t action);
typedef void (*ui_page_fn)(uint8_t action);

void ui_init(void);
void ui_set_handlers(ui_tap_fn tap, ui_menu_fn menu, ui_page_fn page);
void ui_set_asset(uint8_t asset_id, const uint8_t *pixels, uint16_t width, uint16_t height);
bool ui_put_card(uint32_t id, const uint8_t *pixels, uint16_t width, uint16_t height,
                 const uint8_t *region_bytes, uint8_t region_count, uint32_t *dropped_id);
void ui_delete_card(uint32_t id);
void ui_set_order(uint8_t list_id, const uint32_t *ids, uint16_t count);
void ui_toast(const uint8_t *pixels, uint16_t width, uint16_t height, uint16_t duration_ms);
void ui_scroll_top(void);

#define UI_MAX_PEERS 4
typedef struct { char id[37]; char label[48]; bool online; } ui_peer_t;
typedef void (*ui_send_fn)(uint32_t id, uint8_t kind, uint8_t chip, const char *peer);
typedef void (*ui_revoke_fn)(const char *peer);
void ui_set_hub_handlers(ui_send_fn send, ui_revoke_fn revoke);
void ui_show_targets(uint32_t id, uint8_t kind, uint8_t chip, const ui_peer_t *peers, size_t count);
void ui_show_info(uint8_t title_asset, const char *text);
/* code: 6-digit phone pairing code to display, or NULL. */
void ui_show_peers(const ui_peer_t *peers, size_t count, const char *code);
void ui_set_state(bool paused, bool empty, bool setup);
void ui_reset_cards(void);
/* Screen-off timeout entry in the settings menu: current index / advance + persist. */
typedef uint8_t (*ui_screen_fn)(void);
void ui_set_screen_handlers(ui_screen_fn current, ui_screen_fn cycle);
