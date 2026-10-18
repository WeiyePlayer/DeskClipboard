#pragma once
/* UI stall guard + freeze diagnostics.
 *
 * Restart: if the LVGL task stops running for GUARD_STALL_MS the board
 * restarts and the stage is kept in NVS (last_stall_stage).
 *
 * Diagnostics (survive software/watchdog resets in RTC no-init memory, read
 * after the reboot with the USB "diag" op):
 *  - a per-second timeline: uptime, LVGL heartbeat age, task on each core
 *  - task snapshots after 3 s and 8 s of UI silence: every task's state,
 *    priority, core, stack high-water mark and backtrace PCs; holders of the
 *    hub and BLE locks; whether the LVGL lock is free; worker stages; the last
 *    UI event and screen wake
 *  - the Task-WDT interrupt's backtrace of the interrupted context
 * Decode PCs with xtensa-esp32s3-elf-addr2line -pfiaC -e clipboard.elf. */
#include <stdint.h>
#include "cJSON.h"
#include "lvgl.h"

enum { GUARD_IDLE = 0, GUARD_TOUCH, GUARD_RENDER, GUARD_FLUSH_WAIT, GUARD_TIMERS };
#define GUARD_STALL_MS 15000

void guard_stage(uint8_t stage);
/* Call after NVS is initialised and the display exists. */
void guard_start(lv_display_t *disp);
/* Stage + 1 of the last stall restart (0 = none), and its uptime in seconds. */
uint32_t guard_last_stage(void);
uint32_t guard_last_uptime_s(void);
/* Previous boot's freeze record (or {"present":false}). */
cJSON *guard_diag_json(void);
