#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Screen-off timeout choices, index stored in NVS namespace "screen". */
#define SCREEN_TIMEOUT_COUNT 5
#define SCREEN_TIMEOUT_DEFAULT 1 /* 1 minute */

void screen_backlight_init(void);            /* LEDC PWM on the backlight, full on */
void screen_set_panel(void *panel);          /* esp_lcd_panel_handle_t for DISPOFF */
void screen_start(void);                     /* after nvs_flash_init: load setting, start timer */
void screen_wake(void);                      /* any task: new item etc. */
uint8_t screen_timeout_index(void);
uint8_t screen_cycle_timeout(void);          /* LVGL context: next option, persisted */
uint8_t screen_level(void);                  /* 0 awake, 1 dim, 2 off (tests/diagnostics) */
/* lv_tick of the last wake touch (diagnostics). */
uint32_t screen_last_wake_ms(void);
