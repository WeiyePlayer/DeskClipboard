#pragma once
#include <stdbool.h>
typedef void *esp_lcd_panel_handle_t;
extern int sim_panel_on;
static inline int esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t p, bool on) { (void)p; sim_panel_on = on; return 0; }
