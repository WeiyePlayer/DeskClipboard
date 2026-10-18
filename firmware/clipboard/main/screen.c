/* Automatic dimming / screen-off with wake-on-touch.
 *
 * Inactivity comes from LVGL (lv_display_get_inactive_time: last touch or
 * lv_display_trigger_activity). At the timeout the backlight drops to ~20 %;
 * DIM_MS later it goes off and the panel is blanked (DISPOFF).
 * While dimmed or off a transparent shield on the top layer takes the first
 * touch, so waking never presses a card, ⋮ or button underneath; it is
 * removed only after that finger lifts. LVGL keeps running (BLE and the hub
 * are untouched); nothing redraws while idle anyway.
 */
#include "screen.h"

#include "driver/ledc.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "nvs.h"

#define BL_GPIO 45
#define BL_MODE LEDC_LOW_SPEED_MODE
#define BL_CHANNEL LEDC_CHANNEL_0
#define BL_MAX 1023
#define BL_DIM (BL_MAX / 5)
#define DIM_MS 5000u
#define TICK_MS 200u

static const uint32_t s_timeouts_ms[SCREEN_TIMEOUT_COUNT] = {30000, 60000, 120000, 300000, 0};
static uint8_t s_index = SCREEN_TIMEOUT_DEFAULT;
static uint8_t s_level; /* 0 awake, 1 dim, 2 off */
static bool s_ready, s_shield_pressed;
static lv_obj_t *s_shield;
static uint32_t s_wake_ms; /* last wake touch, diagnostics */
static esp_lcd_panel_handle_t s_panel;

static void backlight(uint32_t duty)
{
    if (!s_ready) return;
    ledc_set_duty(BL_MODE, BL_CHANNEL, duty);
    ledc_update_duty(BL_MODE, BL_CHANNEL);
}

void screen_backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = BL_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_channel_config_t channel = {
        .gpio_num = BL_GPIO, .speed_mode = BL_MODE, .channel = BL_CHANNEL,
        .timer_sel = LEDC_TIMER_0, .duty = BL_MAX, .hpoint = 0,
    };
    s_ready = ledc_timer_config(&timer) == ESP_OK && ledc_channel_config(&channel) == ESP_OK;
}

void screen_set_panel(void *panel) { s_panel = panel; }

static void set_level(uint8_t level)
{
    if (level == s_level) return;
    if (s_level == 2 && s_panel) esp_lcd_panel_disp_on_off(s_panel, true);
    s_level = level;
    backlight(level == 0 ? BL_MAX : level == 1 ? BL_DIM : 0);
    if (level == 2 && s_panel) esp_lcd_panel_disp_on_off(s_panel, false);
    if (level) {
        lv_obj_remove_flag(s_shield, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_shield);
    }
}

static void shield_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_shield_pressed = true;
        s_wake_ms = lv_tick_get();
        set_level(0); /* light up immediately; this touch does nothing else */
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_shield_pressed = false;
        if (s_level == 0) lv_obj_add_flag(s_shield, LV_OBJ_FLAG_HIDDEN);
    }
}

static void tick(lv_timer_t *t)
{
    (void)t;
    uint32_t timeout = s_timeouts_ms[s_index];
    uint32_t idle = lv_display_get_inactive_time(NULL);
    uint8_t level = 0;
    if (timeout && idle >= timeout + DIM_MS) level = 2;
    else if (timeout && idle >= timeout) level = 1;
    set_level(level);
    if (level == 0 && !s_shield_pressed && !lv_obj_has_flag(s_shield, LV_OBJ_FLAG_HIDDEN))
        lv_obj_add_flag(s_shield, LV_OBJ_FLAG_HIDDEN); /* woken by a new item, no finger down */
}

void screen_start(void)
{
    nvs_handle_t nvs;
    if (nvs_open("screen", NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(nvs, "timeout", &v) == ESP_OK && v < SCREEN_TIMEOUT_COUNT) s_index = v;
        nvs_close(nvs);
    }
    lvgl_port_lock(0);
    s_shield = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_shield);
    lv_obj_set_size(s_shield, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(s_shield, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_shield, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(s_shield, shield_event, LV_EVENT_ALL, NULL);
    lv_display_trigger_activity(NULL);
    lv_timer_create(tick, TICK_MS, NULL);
    lvgl_port_unlock();
    ESP_LOGI("screen", "timeout index %u", s_index);
}

void screen_wake(void)
{
    if (!s_shield) return;
    lvgl_port_lock(0);
    lv_display_trigger_activity(NULL);
    tick(NULL);
    lvgl_port_unlock();
}

uint8_t screen_timeout_index(void) { return s_index; }
uint8_t screen_level(void) { return s_level; }
uint32_t screen_last_wake_ms(void) { return s_wake_ms; }

uint8_t screen_cycle_timeout(void)
{
    s_index = (uint8_t)((s_index + 1) % SCREEN_TIMEOUT_COUNT);
    nvs_handle_t nvs;
    if (nvs_open("screen", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "timeout", s_index);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    return s_index;
}
