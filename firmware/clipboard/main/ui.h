#pragma once

#include <stdbool.h>
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
