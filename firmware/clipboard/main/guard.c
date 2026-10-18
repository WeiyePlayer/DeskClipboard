#include "guard.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_cpu_utils.h"
#include "esp_debug_helpers.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_memory_utils.h"
#include "esp_private/freertos_debug.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hub.h"
#include "nvs.h"
#include "screen.h"
#include "xtensa_context.h"

static const char *TAG = "guard";
static _Atomic int64_t s_alive_ms;
static _Atomic uint8_t s_stage;
static uint32_t s_last_stage, s_last_uptime;

/* ---------------------------------------------------------------- RTC dump */
#define DIAG_MAGIC 0x44494147u
#define DIAG_TASKS 24
#define DIAG_DEPTH 8
#define DIAG_RING 20
typedef struct {
    char name[12];
    uint8_t state, prio, core, running;
    uint16_t hwm, npc;
    uint32_t pc[DIAG_DEPTH];
} diag_task_t;
typedef struct {
    uint32_t magic, uptime_ms, lvgl_age_ms, ntasks;
    char cur[2][12], hub_holder[12], ble_holder[12];
    uint8_t lvgl_lock_free, stage, worker_stage, ble_stage;
    uint32_t last_event, last_event_ms, last_wake_ms;
    diag_task_t t[DIAG_TASKS];
} diag_snap_t;
typedef struct {
    uint32_t uptime_s;
    uint16_t lvgl_age_ds; /* 0.1 s */
    uint8_t stage, worker_stage;
    char cur[2][8];
} diag_tick_t;
typedef struct {
    uint32_t magic, boot;
    diag_snap_t snap[2];
    uint32_t twdt_magic, twdt_uptime_ms, twdt_npc, twdt_count;
    char twdt_cur[2][12];
    uint32_t twdt_pc[16];
    uint32_t ring_pos;
    diag_tick_t ring[DIAG_RING];
} diag_rtc_t;
static RTC_NOINIT_ATTR diag_rtc_t s_rtc;
static diag_rtc_t *s_prev; /* previous boot's record, copied at start */
static uint32_t s_prev_reset;

static void copy_name(char *out, size_t n, TaskHandle_t t)
{
    const char *name = t ? pcTaskGetName(t) : "-";
    snprintf(out, n, "%s", name ? name : "?");
}

static int walk_frame(uint32_t pc, uint32_t sp, uint32_t next, uint32_t *out, int depth)
{
    esp_backtrace_frame_t f = { .pc = esp_cpu_process_stack_pc(pc), .sp = sp, .next_pc = next, .exc_frame = NULL };
    int n = 0;
    out[n++] = f.pc;
    while (n < depth && f.next_pc && esp_stack_ptr_is_sane(f.sp)) {
        if (!esp_backtrace_get_next_frame(&f)) break;
        uint32_t p = esp_cpu_process_stack_pc(f.pc);
        if (!esp_ptr_executable((void *)p)) break;
        out[n++] = p;
    }
    return n;
}

/* Backtrace of a task that is not running, from its saved context. */
static int walk_task(const TaskSnapshot_t *s, uint32_t *out, int depth)
{
    uint32_t top = (uint32_t)s->pxTopOfStack;
    if (!esp_stack_ptr_is_sane(top)) return 0;
    const XtExcFrame *exc = (const XtExcFrame *)top;
    if (exc->exit != 0) return walk_frame(exc->pc, exc->a1, exc->a0, out, depth);
    const XtSolFrame *sol = (const XtSolFrame *)top;
    return walk_frame(sol->pc, sol->a1, sol->a0, out, depth);
}

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void snapshot(diag_snap_t *d, int64_t idle)
{
    static TaskSnapshot_t snaps[DIAG_TASKS];
    UBaseType_t tcb = 0, n;
    memset(d, 0, sizeof(*d));
    TaskHandle_t cur0 = xTaskGetCurrentTaskHandleForCore(0), cur1 = xTaskGetCurrentTaskHandleForCore(1);
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    vTaskSuspendAll();
    n = uxTaskGetSnapshotAll(snaps, DIAG_TASKS, &tcb);
    for (UBaseType_t i = 0; i < n; i++) {
        TaskHandle_t h = (TaskHandle_t)snaps[i].pxTCB;
        diag_task_t *t = &d->t[i];
        t->running = (h == cur0 || h == cur1 || h == self);
        if (!t->running) t->npc = walk_task(&snaps[i], t->pc, DIAG_DEPTH);
    }
    xTaskResumeAll();
    for (UBaseType_t i = 0; i < n; i++) {
        TaskHandle_t h = (TaskHandle_t)snaps[i].pxTCB;
        diag_task_t *t = &d->t[i];
        copy_name(t->name, sizeof(t->name), h);
        t->state = (uint8_t)eTaskGetState(h);
        t->prio = (uint8_t)uxTaskPriorityGet(h);
        BaseType_t core = xTaskGetCoreID(h);
        t->core = core == tskNO_AFFINITY ? 9 : (uint8_t)core;
        t->hwm = (uint16_t)uxTaskGetStackHighWaterMark(h);
    }
    d->ntasks = n;
    d->uptime_ms = (uint32_t)now_ms();
    d->lvgl_age_ms = (uint32_t)idle;
    copy_name(d->cur[0], sizeof(d->cur[0]), cur0);
    copy_name(d->cur[1], sizeof(d->cur[1]), cur1);
    copy_name(d->hub_holder, sizeof(d->hub_holder), hub_lock_holder());
    copy_name(d->ble_holder, sizeof(d->ble_holder), hub_ble_lock_holder());
    d->lvgl_lock_free = lvgl_port_lock(1);
    if (d->lvgl_lock_free) lvgl_port_unlock();
    d->stage = s_stage;
    d->worker_stage = hub_worker_stage();
    d->ble_stage = hub_ble_stage();
    d->last_event = hub_last_event(&d->last_event_ms);
    d->last_wake_ms = screen_last_wake_ms();
    d->magic = DIAG_MAGIC;
}

/* Task-WDT interrupt (idle task starved for CONFIG_ESP_TASK_WDT_TIMEOUT_S). */
void esp_task_wdt_isr_user_handler(void)
{
    s_rtc.twdt_count++;
    if (s_rtc.twdt_magic == DIAG_MAGIC) return; /* keep the first one */
    s_rtc.twdt_uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
    copy_name(s_rtc.twdt_cur[0], 12, xTaskGetCurrentTaskHandleForCore(0));
    copy_name(s_rtc.twdt_cur[1], 12, xTaskGetCurrentTaskHandleForCore(1));
    uint32_t pc, sp, next;
    esp_backtrace_get_start(&pc, &sp, &next);
    s_rtc.twdt_npc = walk_frame(pc, sp, next, s_rtc.twdt_pc, 16);
    s_rtc.twdt_magic = DIAG_MAGIC;
}

/* ---------------------------------------------------------------- guard */
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
    bool snap1 = false, snap2 = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t idle = now_ms() - s_alive_ms;
        diag_tick_t *k = &s_rtc.ring[s_rtc.ring_pos++ % DIAG_RING];
        k->uptime_s = (uint32_t)(now_ms() / 1000);
        k->lvgl_age_ds = (uint16_t)(idle / 100 > 65535 ? 65535 : idle / 100);
        k->stage = s_stage;
        k->worker_stage = hub_worker_stage();
        copy_name(k->cur[0], 8, xTaskGetCurrentTaskHandleForCore(0));
        copy_name(k->cur[1], 8, xTaskGetCurrentTaskHandleForCore(1));
        if (idle < 3000) {
            snap1 = snap2 = false;
            continue;
        }
        if (!snap1) { snapshot(&s_rtc.snap[0], idle); snap1 = true; }
        else if (!snap2 && idle >= 8000) { snapshot(&s_rtc.snap[1], idle); snap2 = true; }
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
    /* Keep the previous boot's record for the USB "diag" op, then start fresh. */
    esp_reset_reason_t reason = esp_reset_reason();
    if (s_rtc.magic == DIAG_MAGIC && reason != ESP_RST_POWERON) {
        s_prev = heap_caps_malloc(sizeof(s_rtc), MALLOC_CAP_SPIRAM);
        if (s_prev) memcpy(s_prev, &s_rtc, sizeof(s_rtc));
        s_prev_reset = reason;
    }
    uint32_t boot = s_rtc.magic == DIAG_MAGIC ? s_rtc.boot + 1 : 1;
    memset(&s_rtc, 0, sizeof(s_rtc));
    s_rtc.boot = boot;
    s_rtc.magic = DIAG_MAGIC;

    s_alive_ms = now_ms();
    lvgl_port_lock(0);
    lv_timer_create(heartbeat, 500, NULL);
    if (disp) lv_display_add_event_cb(disp, display_event, LV_EVENT_ALL, NULL);
    lvgl_port_unlock();
    /* Above the hub/BLE/LVGL tasks so a stall elsewhere cannot starve it. */
    xTaskCreate(monitor, "guard", 4096, NULL, configMAX_PRIORITIES - 2, NULL);
}

/* ---------------------------------------------------------------- report */
static void add_pcs(cJSON *o, const char *key, const uint32_t *pc, uint32_t n)
{
    char buf[16 * 11 + 1] = "";
    size_t used = 0;
    for (uint32_t i = 0; i < n && i < 16; i++)
        used += snprintf(buf + used, sizeof(buf) - used, "%s0x%08lx", i ? " " : "", (unsigned long)pc[i]);
    cJSON_AddStringToObject(o, key, buf);
}

static cJSON *snap_json(const diag_snap_t *d)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "uptime_ms", d->uptime_ms);
    cJSON_AddNumberToObject(o, "lvgl_age_ms", d->lvgl_age_ms);
    cJSON_AddStringToObject(o, "cpu0", d->cur[0]);
    cJSON_AddStringToObject(o, "cpu1", d->cur[1]);
    cJSON_AddStringToObject(o, "hub_lock", d->hub_holder);
    cJSON_AddStringToObject(o, "ble_lock", d->ble_holder);
    cJSON_AddBoolToObject(o, "lvgl_lock_free", d->lvgl_lock_free);
    cJSON_AddNumberToObject(o, "ui_stage", d->stage);
    cJSON_AddNumberToObject(o, "worker_stage", d->worker_stage);
    cJSON_AddNumberToObject(o, "ble_stage", d->ble_stage);
    cJSON_AddNumberToObject(o, "last_event", d->last_event);
    cJSON_AddNumberToObject(o, "last_event_ms", d->last_event_ms);
    cJSON_AddNumberToObject(o, "last_wake_ms", d->last_wake_ms);
    cJSON *tasks = cJSON_AddArrayToObject(o, "tasks");
    for (uint32_t i = 0; i < d->ntasks && i < DIAG_TASKS; i++) {
        const diag_task_t *t = &d->t[i];
        cJSON *x = cJSON_CreateObject();
        cJSON_AddStringToObject(x, "n", t->name);
        cJSON_AddNumberToObject(x, "s", t->state); /* 0 run 1 ready 2 blocked 3 suspended 4 deleted */
        cJSON_AddNumberToObject(x, "p", t->prio);
        cJSON_AddNumberToObject(x, "c", t->core);
        cJSON_AddNumberToObject(x, "hwm", t->hwm);
        if (t->running) cJSON_AddBoolToObject(x, "running", true);
        add_pcs(x, "bt", t->pc, t->npc);
        cJSON_AddItemToArray(tasks, x);
    }
    return o;
}

cJSON *guard_diag_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddBoolToObject(o, "present", s_prev != NULL);
    cJSON_AddNumberToObject(o, "reset_reason", esp_reset_reason());
    if (!s_prev) return o;
    const diag_rtc_t *p = s_prev;
    cJSON_AddNumberToObject(o, "prev_boot", p->boot);
    cJSON_AddNumberToObject(o, "prev_reset", s_prev_reset);
    cJSON *snaps = cJSON_AddArrayToObject(o, "snapshots");
    for (int i = 0; i < 2; i++)
        if (p->snap[i].magic == DIAG_MAGIC) cJSON_AddItemToArray(snaps, snap_json(&p->snap[i]));
    cJSON_AddNumberToObject(o, "twdt_count", p->twdt_count);
    if (p->twdt_magic == DIAG_MAGIC) {
        cJSON *w = cJSON_AddObjectToObject(o, "twdt");
        cJSON_AddNumberToObject(w, "uptime_ms", p->twdt_uptime_ms);
        cJSON_AddStringToObject(w, "cpu0", p->twdt_cur[0]);
        cJSON_AddStringToObject(w, "cpu1", p->twdt_cur[1]);
        add_pcs(w, "bt", p->twdt_pc, p->twdt_npc);
    }
    cJSON *ring = cJSON_AddArrayToObject(o, "timeline");
    uint32_t count = p->ring_pos < DIAG_RING ? p->ring_pos : DIAG_RING;
    for (uint32_t i = 0; i < count; i++) {
        const diag_tick_t *k = &p->ring[(p->ring_pos - count + i) % DIAG_RING];
        char line[64];
        snprintf(line, sizeof(line), "%lus lvgl_age=%u.%us ui=%u worker=%u cpu0=%.8s cpu1=%.8s",
                 (unsigned long)k->uptime_s, k->lvgl_age_ds / 10, k->lvgl_age_ds % 10, k->stage, k->worker_stage,
                 k->cur[0], k->cur[1]);
        cJSON_AddItemToArray(ring, cJSON_CreateString(line));
    }
    return o;
}
