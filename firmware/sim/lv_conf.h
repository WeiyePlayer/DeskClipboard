/* Host-side LVGL config for simulating firmware/clipboard/main/ui.c. */
#ifndef LV_CONF_H
#define LV_CONF_H
#define LV_COLOR_DEPTH 16
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_USE_OS LV_OS_NONE
#define LV_USE_FLEX 1
#define LV_USE_GRID 1
#define LV_USE_IMAGE 1
#define LV_USE_CANVAS 1
#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_ASSERT_OBJ 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_USE_DRAW_SW 1
#endif
