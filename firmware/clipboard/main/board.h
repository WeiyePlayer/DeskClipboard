#pragma once

#include "esp_err.h"
#include "lvgl.h"

esp_err_t board_display_start(void);
/* The LVGL display created by board_display_start(). */
lv_display_t *board_display(void);
