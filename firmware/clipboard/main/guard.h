#pragma once
/* UI stall guard: if the LVGL task stops running (a blocked touch/I2C read,
 * a lost display flush, a lock never released) everything that waits for the
 * LVGL lock blocks too, including the hub worker that holds the store lock,
 * so BLE and USB requests stop as well. The guard restarts the board after
 * GUARD_STALL_MS without an LVGL heartbeat and records where LVGL was. */
#include <stdint.h>
#include "lvgl.h"

enum { GUARD_IDLE = 0, GUARD_TOUCH, GUARD_RENDER, GUARD_FLUSH_WAIT, GUARD_TIMERS };
#define GUARD_STALL_MS 15000

void guard_stage(uint8_t stage);
/* Call after NVS is initialised and the display exists. */
void guard_start(lv_display_t *disp);
/* Stage + 1 of the last stall restart (0 = none), and its uptime in seconds. */
uint32_t guard_last_stage(void);
uint32_t guard_last_uptime_s(void);
