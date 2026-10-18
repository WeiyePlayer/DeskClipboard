#include "guard.h"

#include <stdatomic.h>
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "guard";
static _Atomic int64_t s_alive_ms;
static _Atomic uint8_t s_stage;
static uint32_t s_last_stage, s_last_uptime;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
void guard_stage(uint8_t stage) { s_stage = stage; }
uint32_t guard_last_stage(void) { return s_last_stage; }
uint32_t guard_last_uptime_s(void) { return s_last_uptime; }

static void heartbeat(lv_timer_t *t)
{
    (void)t;
    s_alive_ms = now_ms();
}

static void display_event(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_RENDER_START: s_stage = GUARD_RENDER; break;
    case LV_EVENT_FLUSH_WAIT_START: s_stage = GUARD_FLUSH_WAIT; break;
    case LV_EVENT_FLUSH_WAIT_FINISH:
    case LV_EVENT_RENDER_READY:
    case LV_EVENT_REFR_READY: s_stage = GUARD_IDLE; break;
    default: break;
    }
}

static void monitor(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t idle = now_ms() - s_alive_ms;
        if (idle < GUARD_STALL_MS) continue;
        uint8_t stage = s_stage;
        nvs_handle_t nvs;
        if (nvs_open("guard", NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_set_u32(nvs, "stage", stage + 1u);
            nvs_set_u32(nvs, "uptime", (uint32_t)(now_ms() / 1000));
            nvs_commit(nvs);
            nvs_close(nvs);
        }
        ESP_LOGE(TAG, "LVGL stalled %lld ms in stage %u, restarting", idle, stage);
        esp_restart();
    }
}

void guard_start(lv_display_t *disp)
{
    nvs_handle_t nvs;
    if (nvs_open("guard", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u32(nvs, "stage", &s_last_stage);
        nvs_get_u32(nvs, "uptime", &s_last_uptime);
        nvs_close(nvs);
    }
    s_alive_ms = now_ms();
    lvgl_port_lock(0);
    lv_timer_create(heartbeat, 500, NULL);
    if (disp) lv_display_add_event_cb(disp, display_event, LV_EVENT_ALL, NULL);
    lvgl_port_unlock();
    /* Above the hub/BLE/LVGL tasks so a stall elsewhere cannot starve it. */
    xTaskCreate(monitor, "guard", 3072, NULL, configMAX_PRIORITIES - 2, NULL);
}
