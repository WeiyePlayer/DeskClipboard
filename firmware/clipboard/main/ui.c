#include "ui.h"

#include <string.h>
#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "protocol.h"

#define MAX_CARDS 80
#define MAX_REGIONS 8
#define SCREEN_W 240
#define CARD_W 224
#define HEADER_H 48
#define SEGMENT_W 128
#define SEGMENT_H 32
#define SEGMENT_BG 0xD8D8DE

typedef struct {
    uint8_t kind;
    uint8_t chip;
    uint16_t x, y, w, h;
} region_t;

typedef struct {
    bool used;
    uint32_t id;
    uint8_t *pixels;
    uint16_t width;
    uint16_t height;
    uint8_t region_count;
    region_t regions[MAX_REGIONS];
    lv_image_dsc_t dsc;
    lv_obj_t *clip_obj;
    lv_obj_t *fav_obj;
} card_t;

typedef struct {
    bool used;
    uint8_t *pixels;
    lv_image_dsc_t dsc;
} asset_t;

static card_t s_cards[MAX_CARDS];
static asset_t s_assets[CLIP_ASSET_COUNT];
static uint32_t s_clip_order[MAX_CARDS];
static uint32_t s_fav_order[MAX_CARDS];
static uint16_t s_clip_count;
static uint16_t s_fav_count;
static uint8_t s_tab;
static ui_tap_fn s_tap;
static ui_menu_fn s_menu;
static ui_page_fn s_page;
static lv_obj_t *s_clip_list;
static lv_obj_t *s_fav_list;
static lv_obj_t *s_tab_clip;
static lv_obj_t *s_tab_fav;
static lv_obj_t *s_pill;
static lv_obj_t *s_page_menu;
static lv_obj_t *s_item_menu;
static lv_obj_t *s_confirm;
static lv_obj_t *s_toast;
static lv_timer_t *s_toast_timer;
static uint32_t s_menu_id;
static bool s_recording_paused;
static lv_obj_t *s_hub_layer;
static lv_obj_t *s_empty;
static ui_send_fn s_send;
static ui_revoke_fn s_revoke;
static ui_peer_t s_peers[UI_MAX_PEERS];
static uint32_t s_target_id;
static uint8_t s_target_kind, s_target_chip;

static uint8_t *s_alloc_rgb565(const uint8_t *src, uint16_t width, uint16_t height,
                             uint32_t *stride_out, uint32_t *size_out)
{
    uint32_t stride = (uint32_t)width * 2u;
    uint32_t size = stride * height;
    uint8_t *dest = heap_caps_aligned_alloc(4, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (dest == NULL) {
        dest = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    if (dest == NULL) {
        return NULL;
    }
    memcpy(dest, src, size);
    *stride_out = stride;
    *size_out = size;
    return dest;
}

static bool s_favorite(uint32_t id)
{
    for (uint16_t i = 0; i < s_fav_count; i++) {
        if (s_fav_order[i] == id) {
            return true;
        }
    }
    return false;
}

static card_t *s_find(uint32_t id)
{
    for (int i = 0; i < MAX_CARDS; i++) {
        if (s_cards[i].used && s_cards[i].id == id) {
            return &s_cards[i];
        }
    }
    return NULL;
}

/*
 * Point an lv_image straight at our PSRAM pixel copy. The previous version
 * copied every card into a fresh lv_draw_buf on each (re)build. Those buffers
 * came out of LVGL's 64 KB builtin pool and were never freed (the canvas
 * destructor does not own them), so after one or two cards the pool was
 * exhausted, the following lv_malloc in LVGL returned NULL and the board
 * crashed and re-enumerated. The descriptor lives in card_t / asset_t, so it
 * stays valid as long as the widget does. Setting NULL first forces LVGL to
 * re-read the header when the same descriptor got new pixels.
 */
static void s_style_image(lv_obj_t *image, const lv_image_dsc_t *dsc)
{
    if (image == NULL || dsc == NULL || dsc->data == NULL || dsc->header.w == 0) {
        return;
    }
    lv_image_set_src(image, NULL);
    lv_image_set_src(image, dsc);
}

static void s_apply_asset(lv_obj_t *image, uint8_t asset_id)
{
    if (image == NULL || asset_id >= CLIP_ASSET_COUNT || !s_assets[asset_id].used) {
        return;
    }
    s_style_image(image, &s_assets[asset_id].dsc);
}

static void s_show_tab(uint8_t tab)
{
    s_tab = tab;
    if (tab == CLIP_LIST_FAVORITES) {
        lv_obj_add_flag(s_clip_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_fav_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(s_pill, LV_ALIGN_RIGHT_MID, -2, 0);
        s_apply_asset(s_tab_clip, CLIP_ASSET_TAB_CLIP_OFF);
        s_apply_asset(s_tab_fav, CLIP_ASSET_TAB_FAV_ON);
    } else {
        lv_obj_add_flag(s_fav_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_clip_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(s_pill, LV_ALIGN_LEFT_MID, 2, 0);
        s_apply_asset(s_tab_clip, CLIP_ASSET_TAB_CLIP_ON);
        s_apply_asset(s_tab_fav, CLIP_ASSET_TAB_FAV_OFF);
    }
}

static void s_tab_event(lv_event_t *event)
{
    s_show_tab((uint8_t)(uintptr_t)lv_event_get_user_data(event));
}

static void s_close_layers(void)
{
    lv_obj_add_flag(s_page_menu, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_item_menu, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_confirm, LV_OBJ_FLAG_HIDDEN);
    if (s_hub_layer) lv_obj_add_flag(s_hub_layer, LV_OBJ_FLAG_HIDDEN);
}

static void s_layer_clicked(lv_event_t *event)
{
    (void)event;
    s_close_layers();
}

static void s_page_menu_event(lv_event_t *event)
{
    (void)event;
    s_close_layers();
    lv_obj_t *box = lv_obj_get_child(s_page_menu, 0);
    s_apply_asset(lv_obj_get_child(box, 0), CLIP_ASSET_CLEAR);
    s_apply_asset(lv_obj_get_child(box, 1),
                  s_recording_paused ? CLIP_ASSET_RESUME : CLIP_ASSET_PAUSE);
    s_apply_asset(lv_obj_get_child(box, 2), CLIP_ASSET_DEVICES);
    s_apply_asset(lv_obj_get_child(box, 3), CLIP_ASSET_PAIRING);
    s_apply_asset(lv_obj_get_child(box, 4), CLIP_ASSET_STORAGE);
    lv_obj_remove_flag(s_page_menu, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_page_menu);
}

static void s_page_action_event(lv_event_t *event)
{
    uint8_t action = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    if (action == CLIP_PAGE_CLEAR) {
        lv_obj_add_flag(s_page_menu, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *box = lv_obj_get_child(s_confirm, 0);
        s_apply_asset(lv_obj_get_child(box, 0), CLIP_ASSET_CONFIRM_TITLE);
        lv_obj_t *row = lv_obj_get_child(box, 1);
        s_apply_asset(lv_obj_get_child(row, 0), CLIP_ASSET_CONFIRM);
        s_apply_asset(lv_obj_get_child(row, 1), CLIP_ASSET_CANCEL);
        lv_obj_remove_flag(s_confirm, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_confirm);
        return;
    }
    if (action == CLIP_PAGE_PAUSE) {
        s_recording_paused = !s_recording_paused;
        action = s_recording_paused ? CLIP_PAGE_PAUSE : CLIP_PAGE_RESUME;
        s_apply_asset(lv_event_get_target(event),
                      s_recording_paused ? CLIP_ASSET_RESUME : CLIP_ASSET_PAUSE);
    }
    s_close_layers();
    if (s_page) {
        s_page(action);
    }
}

static void s_confirm_event(lv_event_t *event)
{
    bool accepted = lv_event_get_user_data(event) != NULL;
    s_close_layers();
    if (accepted && s_page) {
        s_page(CLIP_PAGE_CLEAR);
    }
}

static void s_item_action_event(lv_event_t *event)
{
    uint8_t action = (uint8_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_target(event));
    uint32_t id = s_menu_id;
    s_close_layers();
    if (s_menu) {
        s_menu(id, action);
    }
}

static void s_hit_event(lv_event_t *event)
{
    region_t *region = lv_event_get_user_data(event);
    card_t *card = (card_t *)lv_obj_get_user_data(lv_obj_get_parent(lv_event_get_target(event)));
    if (region->kind == CLIP_REGION_MENU) {
        s_menu_id = card->id;
        s_close_layers();
        lv_obj_t *box = lv_obj_get_child(s_item_menu, 0);
        lv_obj_t *fav = lv_obj_get_child(box, 0);
        lv_obj_t *drop = lv_obj_get_child(box, 1);
        bool favorite = s_favorite(card->id);
        s_apply_asset(fav, favorite ? CLIP_ASSET_UNFAVORITE : CLIP_ASSET_FAVORITE);
        s_apply_asset(drop, CLIP_ASSET_DELETE);
        lv_obj_set_user_data(fav, (void *)(uintptr_t)(favorite ? CLIP_MENU_UNFAVORITE : CLIP_MENU_FAVORITE));
        lv_obj_set_user_data(drop, (void *)(uintptr_t)CLIP_MENU_DELETE);
        lv_obj_remove_flag(s_item_menu, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_item_menu);
        return;
    }
    if (s_tap) {
        s_tap(card->id, region->kind, region->chip);
    }
    lv_obj_t *target = lv_event_get_target(event);
    lv_obj_set_style_bg_opa(lv_obj_get_parent(target), LV_OPA_20, 0);
}

static void s_toast_hide(lv_timer_t *timer)
{
    (void)timer;
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
}

/* The single header action is the home for page actions and future settings. */
static lv_obj_t *s_settings_button(lv_obj_t *parent)
{
    lv_obj_t *button = lv_obj_create(parent);
    lv_obj_set_size(button, 32, 32);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(button, lv_color_white(), 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(button, s_page_menu_event, LV_EVENT_CLICKED, NULL);
    for (int offset = -6; offset <= 6; offset += 6) {
        lv_obj_t *dot = lv_obj_create(button);
        lv_obj_set_size(dot, 3, 3);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(0x333333), 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(dot, LV_ALIGN_CENTER, offset, 0);
    }
    return button;
}

static lv_obj_t *s_segment_tab(lv_obj_t *segment, uint8_t tab)
{
    lv_obj_t *hit = lv_obj_create(segment);
    lv_obj_set_size(hit, SEGMENT_W / 2, SEGMENT_H);
    lv_obj_set_pos(hit, tab * (SEGMENT_W / 2), 0);
    lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hit, 0, 0);
    lv_obj_set_style_pad_all(hit, 0, 0);
    lv_obj_remove_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(hit, s_tab_event, LV_EVENT_CLICKED, (void *)(uintptr_t)tab);
    lv_obj_t *label = lv_image_create(hit);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(label);
    return label;
}

static lv_obj_t *s_image_button(lv_obj_t *parent, lv_event_cb_t callback, void *user)
{
    lv_obj_t *button = lv_image_create(parent);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user);
    return button;
}

static lv_obj_t *s_make_list(lv_obj_t *parent)
{
    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_set_size(list, SCREEN_W, 320 - HEADER_H - 4);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 8, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    return list;
}

static lv_obj_t *s_layer(lv_obj_t *parent)
{
    lv_obj_t *layer = lv_obj_create(parent);
    lv_obj_set_size(layer, SCREEN_W, 320);
    lv_obj_set_style_bg_color(layer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(layer, LV_OPA_40, 0);
    lv_obj_set_style_border_width(layer, 0, 0);
    lv_obj_add_flag(layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(layer, s_layer_clicked, LV_EVENT_CLICKED, NULL);
    return layer;
}

void ui_init(void)
{
    lvgl_port_lock(0);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xE4E4E9), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    lv_obj_t *bar = lv_obj_create(screen);
    lv_obj_set_size(bar, SCREEN_W, HEADER_H);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *segment = lv_obj_create(bar);
    lv_obj_set_size(segment, SEGMENT_W, SEGMENT_H);
    lv_obj_center(segment);
    lv_obj_set_style_radius(segment, 8, 0);
    lv_obj_set_style_bg_color(segment, lv_color_hex(SEGMENT_BG), 0);
    lv_obj_set_style_border_width(segment, 0, 0);
    lv_obj_set_style_pad_all(segment, 0, 0);
    lv_obj_remove_flag(segment, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    s_pill = lv_obj_create(segment);
    lv_obj_set_size(s_pill, SEGMENT_W / 2 - 4, SEGMENT_H - 4);
    lv_obj_set_style_radius(s_pill, 6, 0);
    lv_obj_set_style_bg_color(s_pill, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_pill, 0, 0);
    lv_obj_set_style_pad_all(s_pill, 0, 0);
    lv_obj_remove_flag(s_pill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_pill, LV_ALIGN_LEFT_MID, 2, 0);
    s_tab_clip = s_segment_tab(segment, CLIP_LIST_CLIPBOARD);
    s_tab_fav = s_segment_tab(segment, CLIP_LIST_FAVORITES);
    lv_obj_t *settings = s_settings_button(bar);
    lv_obj_align(settings, LV_ALIGN_RIGHT_MID, -8, 0);

    s_clip_list = s_make_list(screen);
    s_fav_list = s_make_list(screen);
    lv_obj_add_flag(s_fav_list, LV_OBJ_FLAG_HIDDEN);

    s_page_menu = s_layer(screen);
    lv_obj_t *page_box = lv_obj_create(s_page_menu);
    lv_obj_set_size(page_box, 204, 190);
    lv_obj_align(page_box, LV_ALIGN_TOP_RIGHT, -8, 52);
    lv_obj_set_flex_flow(page_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page_box, 8, 0);
    s_image_button(page_box, s_page_action_event, (void *)(uintptr_t)CLIP_PAGE_CLEAR);
    s_image_button(page_box, s_page_action_event, (void *)(uintptr_t)CLIP_PAGE_PAUSE);
    s_image_button(page_box, s_page_action_event, (void *)(uintptr_t)CLIP_PAGE_DEVICES);
    s_image_button(page_box, s_page_action_event, (void *)(uintptr_t)CLIP_PAGE_PAIRING);
    s_image_button(page_box, s_page_action_event, (void *)(uintptr_t)CLIP_PAGE_STORAGE);

    s_item_menu = s_layer(screen);
    lv_obj_t *item_box = lv_obj_create(s_item_menu);
    lv_obj_set_size(item_box, 160, 90);
    lv_obj_center(item_box);
    lv_obj_set_flex_flow(item_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(item_box, 8, 0);
    s_image_button(item_box, s_item_action_event, (void *)(uintptr_t)CLIP_MENU_FAVORITE);
    s_image_button(item_box, s_item_action_event, (void *)(uintptr_t)CLIP_MENU_DELETE);

    s_confirm = s_layer(screen);
    lv_obj_t *confirm_box = lv_obj_create(s_confirm);
    lv_obj_set_size(confirm_box, 200, 110);
    lv_obj_center(confirm_box);
    lv_obj_set_flex_flow(confirm_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(confirm_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(confirm_box, 8, 0);
    lv_image_create(confirm_box);
    lv_obj_t *row = lv_obj_create(confirm_box);
    lv_obj_set_size(row, 180, 36);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_image_button(row, s_confirm_event, (void *)1);
    s_image_button(row, s_confirm_event, NULL);

    s_toast = lv_image_create(screen);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast);
    s_hub_layer = s_layer(screen);
    s_empty = lv_image_create(screen);
    lv_obj_align(s_empty, LV_ALIGN_CENTER, 0, 20);
    lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

void ui_set_handlers(ui_tap_fn tap, ui_menu_fn menu, ui_page_fn page)
{
    s_tap = tap;
    s_menu = menu;
    s_page = page;
}

void ui_set_asset(uint8_t asset_id, const uint8_t *pixels, uint16_t width, uint16_t height)
{
    if (asset_id == 0 || asset_id >= CLIP_ASSET_COUNT || width == 0 || height == 0) {
        return;
    }
    lvgl_port_lock(0);
    asset_t *asset = &s_assets[asset_id];
    uint32_t stride = 0;
    uint32_t bytes = 0;
    uint8_t *copy = s_alloc_rgb565(pixels, width, height, &stride, &bytes);
    if (copy == NULL) {
        lvgl_port_unlock();
        return;
    }
    free(asset->pixels);
    asset->pixels = copy;
    asset->used = true;
    memset(&asset->dsc, 0, sizeof(asset->dsc));
    asset->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    asset->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    asset->dsc.header.w = width;
    asset->dsc.header.h = height;
    asset->dsc.header.stride = stride;
    asset->dsc.data_size = bytes;
    asset->dsc.data = copy;
    if (asset_id == CLIP_ASSET_TAB_CLIP_ON || asset_id == CLIP_ASSET_TAB_CLIP_OFF) {
        s_apply_asset(s_tab_clip, s_tab == CLIP_LIST_FAVORITES ? CLIP_ASSET_TAB_CLIP_OFF : CLIP_ASSET_TAB_CLIP_ON);
    } else if (asset_id == CLIP_ASSET_TAB_FAV_ON || asset_id == CLIP_ASSET_TAB_FAV_OFF) {
        s_apply_asset(s_tab_fav, s_tab == CLIP_LIST_FAVORITES ? CLIP_ASSET_TAB_FAV_ON : CLIP_ASSET_TAB_FAV_OFF);
    }
    lvgl_port_unlock();
}

static void s_free_widget(lv_obj_t **obj)
{
    if (*obj) {
        lv_obj_delete(*obj);
        *obj = NULL;
    }
}

static void s_delete_unlocked(uint32_t id)
{
    card_t *card = s_find(id);
    if (card == NULL) {
        return;
    }
    s_free_widget(&card->clip_obj);
    s_free_widget(&card->fav_obj);
    free(card->pixels);
    memset(card, 0, sizeof(*card));
}

void ui_reset_cards(void)
{
    lvgl_port_lock(0);
    for (size_t i = 0; i < MAX_CARDS; i++) {
        if (s_cards[i].used) s_delete_unlocked(s_cards[i].id);
    }
    lvgl_port_unlock();
}

static lv_obj_t *s_build_card(card_t *card, lv_obj_t *parent)
{
    lv_obj_t *holder = lv_obj_create(parent);
    lv_obj_set_size(holder, card->width, card->height);
    lv_obj_set_style_bg_opa(holder, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(holder, 0, 0);
    lv_obj_set_style_pad_all(holder, 0, 0);
    lv_obj_set_user_data(holder, card);
    lv_obj_t *image = lv_image_create(holder);
    s_style_image(image, &card->dsc);
    for (uint8_t i = 0; i < card->region_count; i++) {
        lv_obj_t *hit = lv_obj_create(holder);
        region_t *region = &card->regions[i];
        lv_obj_set_pos(hit, region->x, region->y);
        lv_obj_set_size(hit, region->w, region->h);
        lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(hit, 0, 0);
        lv_obj_add_flag(hit, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
        lv_obj_remove_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(hit, s_hit_event, LV_EVENT_CLICKED, region);
    }
    return holder;
}

static bool s_drop_oldest(uint32_t *dropped_id)
{
    for (int index = (int)s_clip_count - 1; index >= 0; index--) {
        if (s_favorite(s_clip_order[index])) {
            continue;
        }
        *dropped_id = s_clip_order[index];
        s_delete_unlocked(*dropped_id);
        return true;
    }
    return false;
}

bool ui_put_card(uint32_t id, const uint8_t *pixels, uint16_t width, uint16_t height,
                 const uint8_t *region_bytes, uint8_t region_count, uint32_t *dropped_id)
{
    if (dropped_id != NULL) {
        *dropped_id = 0;
    }
    if (width == 0 || height == 0 || width > CARD_W || region_count > MAX_REGIONS) {
        return false;
    }
    lvgl_port_lock(0);
    card_t *card = s_find(id);
    if (card == NULL) {
        for (int i = 0; i < MAX_CARDS; i++) {
            if (!s_cards[i].used) {
                card = &s_cards[i];
                break;
            }
        }
    }
    if (card == NULL) {
        s_drop_oldest(dropped_id);
        card = s_find(id);
        if (card == NULL) {
            for (int i = 0; i < MAX_CARDS; i++) {
                if (!s_cards[i].used) {
                    card = &s_cards[i];
                    break;
                }
            }
        }
    }
    if (card == NULL) {
        lvgl_port_unlock();
        return false;
    }
    uint32_t stride = 0;
    uint32_t bytes = 0;
    uint8_t *copy = s_alloc_rgb565(pixels, width, height, &stride, &bytes);
    if (copy == NULL) {
        lvgl_port_unlock();
        return false;
    }
    /* Widgets of a replaced card point at the old pixels and regions; drop
     * them. The LIST_ORDER that follows every CARD_PUT rebuilds them. */
    s_free_widget(&card->clip_obj);
    s_free_widget(&card->fav_obj);
    free(card->pixels);
    card->used = true;
    card->id = id;
    card->pixels = copy;
    card->width = width;
    card->height = height;
    memset(&card->dsc, 0, sizeof(card->dsc));
    card->region_count = region_count;
    for (uint8_t i = 0; i < region_count; i++) {
        const uint8_t *raw = region_bytes + i * 10;
        card->regions[i].kind = raw[0];
        card->regions[i].chip = raw[1];
        memcpy(&card->regions[i].x, raw + 2, 2);
        memcpy(&card->regions[i].y, raw + 4, 2);
        memcpy(&card->regions[i].w, raw + 6, 2);
        memcpy(&card->regions[i].h, raw + 8, 2);
    }
    card->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    card->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    card->dsc.header.w = width;
    card->dsc.header.h = height;
    card->dsc.header.stride = stride;
    card->dsc.data_size = bytes;
    card->dsc.data = copy;
    lvgl_port_unlock();
    return true;
}

void ui_delete_card(uint32_t id)
{
    lvgl_port_lock(0);
    s_delete_unlocked(id);
    lvgl_port_unlock();
}

void ui_set_order(uint8_t list_id, const uint32_t *ids, uint16_t count)
{
    if (count > MAX_CARDS) {
        count = MAX_CARDS;
    }
    lvgl_port_lock(0);
    if (list_id == CLIP_LIST_FAVORITES) {
        if (count) memcpy(s_fav_order, ids, count * sizeof(uint32_t));
        s_fav_count = count;
        for (int i = 0; i < MAX_CARDS; i++) {
            s_cards[i].fav_obj = NULL;
        }
        lv_obj_clean(s_fav_list);
        for (uint16_t i = 0; i < count; i++) {
            card_t *card = s_find(ids[i]);
            if (card) {
                card->fav_obj = s_build_card(card, s_fav_list);
            }
        }
    } else {
        if (count) memcpy(s_clip_order, ids, count * sizeof(uint32_t));
        s_clip_count = count;
        for (int i = 0; i < MAX_CARDS; i++) {
            s_cards[i].clip_obj = NULL;
        }
        lv_obj_clean(s_clip_list);
        for (uint16_t i = 0; i < count; i++) {
            card_t *card = s_find(ids[i]);
            if (card) {
                card->clip_obj = s_build_card(card, s_clip_list);
            }
        }
    }
    lvgl_port_unlock();
}

void ui_toast(const uint8_t *pixels, uint16_t width, uint16_t height, uint16_t duration_ms)
{
    lvgl_port_lock(0);
    static uint8_t *toast_pixels;
    static lv_image_dsc_t toast_dsc;
    uint32_t stride = 0;
    uint32_t bytes = 0;
    uint8_t *copy = s_alloc_rgb565(pixels, width, height, &stride, &bytes);
    if (copy == NULL) {
        lvgl_port_unlock();
        return;
    }
    free(toast_pixels);
    toast_pixels = copy;
    memset(&toast_dsc, 0, sizeof(toast_dsc));
    toast_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    toast_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    toast_dsc.header.w = width;
    toast_dsc.header.h = height;
    toast_dsc.header.stride = stride;
    toast_dsc.data_size = bytes;
    toast_dsc.data = copy;
    s_style_image(s_toast, &toast_dsc);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast);
    /* One persistent one-shot timer. LVGL auto-deletes a timer whose repeat
     * count runs out, so the old code's lv_timer_delete() on the second toast
     * freed an already-freed timer and the board panicked (two taps -> crash). */
    if (s_toast_timer == NULL) {
        s_toast_timer = lv_timer_create(s_toast_hide, duration_ms, NULL);
        lv_timer_set_auto_delete(s_toast_timer, false);
    }
    lv_timer_set_period(s_toast_timer, duration_ms);
    lv_timer_set_repeat_count(s_toast_timer, 1);
    lv_timer_reset(s_toast_timer);
    lv_timer_resume(s_toast_timer);
    lvgl_port_unlock();
}

void ui_scroll_top(void)
{
    lvgl_port_lock(0);
    lv_obj_scroll_to_y(s_tab == CLIP_LIST_FAVORITES ? s_fav_list : s_clip_list, 0, LV_ANIM_OFF);
    lvgl_port_unlock();
}

void ui_set_hub_handlers(ui_send_fn send, ui_revoke_fn revoke)
{
    s_send = send;
    s_revoke = revoke;
}

static lv_obj_t *s_hub_box(uint8_t title)
{
    s_close_layers();
    lv_obj_clean(s_hub_layer);
    lv_obj_t *box = lv_obj_create(s_hub_layer);
    lv_obj_set_size(box, 224, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(box, 280, 0);
    lv_obj_center(box);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(box, 12, 0);
    lv_obj_set_style_pad_row(box, 8, 0);
    s_apply_asset(lv_image_create(box), title);
    lv_obj_remove_flag(s_hub_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_hub_layer);
    return box;
}

static void s_target_event(lv_event_t *event)
{
    ui_peer_t *peer = lv_event_get_user_data(event);
    if (!peer->online) return;
    s_close_layers();
    if (s_send) s_send(s_target_id, s_target_kind, s_target_chip, peer->id);
}

static void s_revoke_event(lv_event_t *event)
{
    ui_peer_t *peer = lv_event_get_user_data(event);
    s_close_layers();
    if (s_revoke) s_revoke(peer->id);
}

static void s_peer_buttons(lv_obj_t *box, const ui_peer_t *peers, size_t count, bool revoke)
{
    if (count > UI_MAX_PEERS) count = UI_MAX_PEERS;
    memcpy(s_peers, peers, count * sizeof(*peers));
    for (size_t i = 0; i < count; i++) {
        lv_obj_t *button = lv_button_create(box);
        lv_obj_set_size(button, 196, 34);
        lv_obj_add_event_cb(button, revoke ? s_revoke_event : s_target_event,
                            LV_EVENT_CLICKED, &s_peers[i]);
        lv_obj_t *label = lv_label_create(button);
        char text[80];
        snprintf(text, sizeof(text), "%s%s", s_peers[i].label,
                 revoke ? "  [remove]" : (s_peers[i].online ? "" : "  [offline]"));
        lv_label_set_text(label, text);
        lv_obj_center(label);
        if (!revoke && !s_peers[i].online) lv_obj_add_state(button, LV_STATE_DISABLED);
    }
    if (!count) lv_label_set_text(lv_label_create(box), "No paired computers");
    s_apply_asset(s_image_button(box, s_layer_clicked, NULL), CLIP_ASSET_CANCEL);
}

void ui_show_targets(uint32_t id, uint8_t kind, uint8_t chip, const ui_peer_t *peers, size_t count)
{
    lvgl_port_lock(0);
    s_target_id = id;
    s_target_kind = kind;
    s_target_chip = chip;
    s_peer_buttons(s_hub_box(CLIP_ASSET_TARGET_TITLE), peers, count, false);
    lvgl_port_unlock();
}

void ui_show_peers(const ui_peer_t *peers, size_t count)
{
    lvgl_port_lock(0);
    s_peer_buttons(s_hub_box(CLIP_ASSET_PAIR_TITLE), peers, count, true);
    lvgl_port_unlock();
}

void ui_show_info(uint8_t title_asset, const char *text)
{
    lvgl_port_lock(0);
    lv_obj_t *box = s_hub_box(title_asset);
    lv_obj_t *label = lv_label_create(box);
    lv_obj_set_width(label, 196);
    lv_label_set_text(label, text);
    s_apply_asset(s_image_button(box, s_layer_clicked, NULL), CLIP_ASSET_CANCEL);
    lvgl_port_unlock();
}

void ui_set_state(bool paused, bool empty, bool setup)
{
    lvgl_port_lock(0);
    s_recording_paused = paused;
    s_apply_asset(s_empty, setup ? CLIP_ASSET_SETUP : CLIP_ASSET_EMPTY);
    if (empty || setup) lv_obj_remove_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}
