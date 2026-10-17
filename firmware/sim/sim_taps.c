/* Drive main/ui.c on the host with a fake display and touch panel, fed with
 * real host frames (build/frames.bin from gen_frames.py), and check which
 * handler each tap reaches. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "protocol.h"
#include "ui.h"

int sim_replay(const char *path);

static uint32_t s_now;
static int32_t s_x, s_y;
static bool s_down;
static int s_taps, s_menus;
static uint8_t s_framebuffer[240 * 320 * 2];
static const char *s_preview_dir;
static uint8_t s_page_action;
static uint32_t s_last_id, s_menu_id;
static uint8_t s_last_kind, s_menu_action;
static int s_sends;
static char s_target[37];
static uint8_t s_sent_kind, s_sent_chip;
static void on_send(uint32_t id, uint8_t kind, uint8_t chip, const char *peer)
{
    (void)id; s_sends++; s_sent_kind=kind; s_sent_chip=chip;
    snprintf(s_target,sizeof(s_target),"%s",peer);
}

static uint32_t tick(void) { return s_now; }
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *p)
{
    size_t row_bytes = (a->x2 - a->x1 + 1) * 2;
    for (int y = a->y1; y <= a->y2; y++) {
        memcpy(s_framebuffer + (y * 240 + a->x1) * 2, p, row_bytes);
        p += row_bytes;
    }
    lv_display_flush_ready(d);
}
static void read_cb(lv_indev_t *i, lv_indev_data_t *data) { (void)i; data->point.x = s_x; data->point.y = s_y; data->state = s_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED; }
static void on_tap(uint32_t id, uint8_t kind, uint8_t chip) { (void)chip; s_taps++; s_last_id = id; s_last_kind = kind; }
static void on_menu(uint32_t id, uint8_t action) { s_menus++; s_menu_id = id; s_menu_action = action; }
static void on_page(uint8_t a) { s_page_action = a; }
static void run(uint32_t ms) { for (uint32_t t = 0; t < ms; t += 5) { s_now += 5; lv_timer_handler(); } }
static void tap(int32_t x, int32_t y) { s_x = x; s_y = y; s_down = true; run(80); s_down = false; run(80); }

static lv_obj_t *screen, *list, *item_menu;
static int failures;

static bool menu_open(void) { return !lv_obj_has_flag(item_menu, LV_OBJ_FLAG_HIDDEN); }

static void snapshot(const char *name)
{
    if (!s_preview_dir) return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.ppm", s_preview_dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); failures++; return; }
    fprintf(f, "P6\n240 320\n255\n");
    for (size_t i = 0; i < sizeof(s_framebuffer); i += 2) {
        uint16_t p = s_framebuffer[i] | ((uint16_t)s_framebuffer[i + 1] << 8);
        fputc(((p >> 11) & 31) * 255 / 31, f);
        fputc(((p >> 5) & 63) * 255 / 63, f);
        fputc((p & 31) * 255 / 31, f);
    }
    fclose(f);
}

static void check_header(void)
{
    lv_obj_t *favorites = lv_obj_get_child(screen, 2);
    lv_obj_t *page_menu = lv_obj_get_child(screen, 3);
    /* Both padding and text inside each half should switch pages. */
    const int32_t points[][2] = {{180, 24}, {60, 24}, {152, 12}, {88, 36}};
    for (unsigned i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
        tap(points[i][0], points[i][1]);
        bool favorite = (i % 2) == 0;
        if (lv_obj_has_flag(list, LV_OBJ_FLAG_HIDDEN) != favorite ||
            lv_obj_has_flag(favorites, LV_OBJ_FLAG_HIDDEN) == favorite) {
            printf("header switch failed at (%d,%d)\n", (int)points[i][0], (int)points[i][1]);
            failures++;
        }
        if (i == 0) snapshot("header-favorites");
    }
    /* Tap the drawn dots and use the page menu to pause and resume. */
    for (int i = 0; i < 2; i++) {
        tap(216, 24);
        if (lv_obj_has_flag(page_menu, LV_OBJ_FLAG_HIDDEN)) {
            printf("settings menu did not open\n"); failures++; continue;
        }
        if (i == 0) snapshot("header-settings");
        lv_obj_t *box = lv_obj_get_child(page_menu, 0);
        lv_area_t a;
        lv_obj_get_coords(lv_obj_get_child(box, 1), &a);
        tap((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
        if (s_page_action != (i == 0 ? CLIP_PAGE_PAUSE : CLIP_PAGE_RESUME)) {
            printf("settings action failed\n"); failures++;
        }
    }
    printf("header switching and settings checked\n");
}

static void check_targets(void)
{
    ui_peer_t peers[2] = {
        {.id="11111111-1111-4111-8111-111111111111",.label="Mac - 111111",.online=true},
        {.id="22222222-2222-4222-8222-222222222222",.label="Windows - 222222",.online=false}
    };
    ui_set_hub_handlers(on_send,NULL);
    ui_show_targets(7,CLIP_REGION_CHIP,1,peers,2); run(100);
    snapshot("targets");
    lv_obj_t *layer=lv_obj_get_child(screen,-1), *box=lv_obj_get_child(layer,0);
    lv_area_t a;
    lv_obj_get_coords(lv_obj_get_child(box,2),&a);
    tap((a.x1+a.x2)/2,(a.y1+a.y2)/2);
    if(s_sends) { printf("offline target was selected\n"); failures++; }
    lv_obj_get_coords(lv_obj_get_child(box,1),&a);
    tap((a.x1+a.x2)/2,(a.y1+a.y2)/2);
    if(s_sends!=1||strcmp(s_target,peers[0].id)||s_sent_kind!=CLIP_REGION_CHIP||s_sent_chip!=1) {
        printf("target selection lost the selected content\n"); failures++;
    }
    printf("online/offline target selection checked\n");
}

/* Tap every card's ⋮ at the dots and at points a sloppy finger might land
 * (left of the dots, low in the strip): each must open the item menu and
 * must not paste. */
static void check_all(const char *phase)
{
    for (uint32_t i = 0; i < lv_obj_get_child_count(list); i++) {
        lv_obj_t *holder = lv_obj_get_child(list, i);
        lv_obj_scroll_to_view(holder, LV_ANIM_OFF);
        run(50);
        lv_area_t a; lv_obj_get_coords(holder, &a);
        int32_t h = a.y2 - a.y1 + 1;
        const int32_t pts[][2] = {{206, 18}, {184, 18}, {220, 4}, {200, h / 2}, {214, h - 6}};
        for (unsigned k = 0; k < sizeof(pts) / sizeof(pts[0]); k++) {
            int32_t x = a.x1 + pts[k][0], y = a.y1 + pts[k][1];
            int before = s_taps;
            tap(x, y);
            bool open = menu_open();
            bool ok = open && s_taps == before;
            if (!ok || k == 0)
                printf("[%s] card %u y=%d..%d tap(%d,%d): menu_open=%d body_taps+%d%s\n", phase, i, (int)a.y1,
                       (int)a.y2, (int)x, (int)y, open, s_taps - before, ok ? "" : "  <-- FAIL");
            if (!ok) failures++;
            if (open) tap(10, 310);
            if (menu_open()) { printf("  menu did not close\n"); failures++; }
        }
    }
}

int main(int argc, char **argv)
{
    const char *frames = argc > 1 ? argv[1] : "build/frames.bin";
    s_preview_dir = argc > 3 ? argv[3] : NULL;
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *d = lv_display_create(240, 320);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    static uint8_t buf[240 * 40 * 2];
    lv_display_set_buffers(d, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flush);
    lv_indev_t *in = lv_indev_create();
    lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in, read_cb);
    ui_init();
    ui_set_handlers(on_tap, on_menu, on_page);
    printf("replayed %d frames\n", sim_replay(frames));
    run(200);
    screen = lv_screen_active();
    list = lv_obj_get_child(screen, 1);
    item_menu = lv_obj_get_child(screen, 4);

    snapshot("header-clipboard");
    check_header();
    check_targets();
    check_all("fresh");
    /* Body tap -> toast, like a paste. */
    lv_obj_scroll_to_view(lv_obj_get_child(list, 1), LV_ANIM_OFF); run(50);
    lv_area_t a; lv_obj_get_coords(lv_obj_get_child(list, 1), &a);
    tap(a.x1 + 60, a.y1 + 20);
    printf("body tap card 1 -> taps=%d id=%u\n", s_taps, s_last_id);
    if (argc > 2) sim_replay(argv[2]); /* host toast + resync */
    run(100);
    check_all("after-paste");
    /* Open ⋮ on card 0 and press its first action button. */
    lv_obj_scroll_to_view(lv_obj_get_child(list, 0), LV_ANIM_OFF); run(50);
    lv_obj_get_coords(lv_obj_get_child(list, 0), &a);
    tap(a.x1 + 208, a.y1 + 18);
    lv_obj_t *box = lv_obj_get_child(item_menu, 0);
    lv_area_t b; lv_obj_get_coords(lv_obj_get_child(box, 0), &b);
    tap((b.x1 + b.x2) / 2, (b.y1 + b.y2) / 2);
    printf("menu action -> menus=%d id=%u action=%u open=%d\n", s_menus, s_menu_id, s_menu_action, menu_open());
    if (argc > 2) sim_replay(argv[2]);
    run(100);
    check_all("after-menu-action");
    printf("failures: %d\n", failures);
    return failures ? 1 : 0;
}
