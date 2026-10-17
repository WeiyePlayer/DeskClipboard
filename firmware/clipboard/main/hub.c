#include "hub.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hub_assets.h"
#include "hub_store.h"
#include "screen.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "protocol.h"
#include "ui.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ONLINE_MS 15000
#define JOB_MS 120000
#define ROOT "/hub"
static hub_store_t s_store;
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_events;
static nvs_handle_t s_nvs;
static wl_handle_t s_wl = WL_INVALID_HANDLE;
static bool s_initialized;
static char s_board_id[37];
static int64_t s_upload_at;
static int64_t s_pair_until;
static bool s_dirty;
static _Atomic uint8_t s_notice;
static _Atomic bool s_wake; /* screen_wake() outside s_lock (it takes the LVGL lock) */
static int64_t s_restart_at;
/* Phone pairing code (⋯ → 配对管理): 6 digits, valid with the pairing window,
 * single use, closed after PAIR_MAX_FAILURES wrong confirmations. */
#define PAIR_MAX_FAILURES 3
static char s_pair_code[7];
static int64_t s_code_until;
static uint8_t s_code_failures;
static bool s_show_peers;

typedef struct
{
    char id[37], token[65], name[65], platform[12], session[37];
    int64_t seen;
    char job[33], job_session[37], sha[65];
    uint32_t record;
    uint8_t kind, chip;
    int64_t deadline;
    bool claimed, ready;
} peer_t;
static peer_t s_peers[UI_MAX_PEERS];
typedef struct
{
    uint8_t action, kind, chip;
    uint32_t id;
    char peer[37];
} event_t;
enum
{
    E_TAP = 1,
    E_MENU,
    E_PAGE,
    E_SEND,
    E_REVOKE
};
static const char* str(const cJSON* o, const char* k)
{
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}
static size_t num(const cJSON* o, const char* k)
{
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) && isfinite(v->valuedouble) && v->valuedouble >= 0 && v->valuedouble <= UINT32_MAX && floor(v->valuedouble) == v->valuedouble ? (size_t)v->valuedouble : 0;
}
const char* hub_board_id(void) { return s_board_id; }
static int64_t now(void) { return esp_timer_get_time() / 1000; }
static cJSON* answer(const char* error)
{
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", !error);
    if (error)
        cJSON_AddStringToObject(o, "error", error);
    return o;
}
static void random_hex(char* out, size_t bytes)
{
    uint8_t b[32];
    esp_fill_random(b, bytes);
    for (size_t i = 0; i < bytes; i++)
        sprintf(out + i * 2, "%02x", b[i]);
}
static bool constant_equal(const char* a, const char* b)
{
    if (strlen(a) != 64 || strlen(b) != 64)
        return false;
    uint8_t x = 0;
    for (int i = 0; i < 64; i++)
        x |= a[i] ^ b[i];
    return x == 0;
}
static peer_t* authenticate(const char* token)
{
    for (int i = 0; i < UI_MAX_PEERS; i++)
        if (s_peers[i].id[0] && constant_equal(s_peers[i].token, token ? token : ""))
            return &s_peers[i];
    return NULL;
}
bool hub_authorized(const char* token)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool valid = authenticate(token) != NULL;
    xSemaphoreGive(s_lock);
    return valid;
}
bool hub_peer_credential(const char *id, char token[65])
{
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < UI_MAX_PEERS; i++)
        if (s_peers[i].id[0] && !strcmp(s_peers[i].id, id)) {
            memcpy(token, s_peers[i].token, 65);
            found = true;
        }
    xSemaphoreGive(s_lock);
    return found;
}
static bool online(peer_t* p) { return p->seen > 0 && now() - p->seen < ONLINE_MS; }
static void clear_job(peer_t* p, bool fail)
{
    if (p->job[0] && fail)
        s_notice = CLIP_ASSET_SEND_FAILED;
    p->job[0] = 0;
    p->claimed = p->ready = false;
}
void hub_peer_disconnected(const char *token)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    peer_t *p = authenticate(token);
    if (p) {
        p->seen = 0;
        clear_job(p, true);
    }
    xSemaphoreGive(s_lock);
}
static bool save_peers(void)
{
    cJSON* a = cJSON_CreateArray();
    for (int i = 0; i < UI_MAX_PEERS; i++)
        if (s_peers[i].id[0])
        {
            peer_t* p = &s_peers[i];
            cJSON* o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "id", p->id);
            cJSON_AddStringToObject(o, "token", p->token);
            cJSON_AddStringToObject(o, "name", p->name);
            cJSON_AddStringToObject(o, "platform", p->platform);
            cJSON_AddItemToArray(a, o);
        }
    char* b = cJSON_PrintUnformatted(a);
    bool ok = b && nvs_set_str(s_nvs, "peers", b) == ESP_OK && nvs_commit(s_nvs) == ESP_OK;
    free(b);
    cJSON_Delete(a);
    return ok;
}
static void load_peers(void)
{
    size_t n = 0;
    if (nvs_get_str(s_nvs, "peers", NULL, &n) != ESP_OK || n > 8192)
        return;
    char* b = malloc(n);
    if (!b)
        return;
    if (nvs_get_str(s_nvs, "peers", b, &n) == ESP_OK)
    {
        cJSON* a = cJSON_Parse(b);
        int i = 0;
        cJSON* o;
        cJSON_ArrayForEach(o, a)
        {
            if (i >= UI_MAX_PEERS)
                break;
            if (!hub_valid_id(str(o, "id")) || strlen(str(o, "token")) != 64)
                continue;
            peer_t* p = &s_peers[i++];
            snprintf(p->id, sizeof(p->id), "%s", str(o, "id"));
            snprintf(p->token, sizeof(p->token), "%s", str(o, "token"));
            snprintf(p->name, sizeof(p->name), "%s", str(o, "name"));
            snprintf(p->platform, sizeof(p->platform), "%s", str(o, "platform"));
        }
        cJSON_Delete(a);
    }
    free(b);
}
static bool mount_store(bool initialize)
{
    esp_vfs_fat_mount_config_t cfg = { .format_if_mount_failed = initialize, .max_files = 8, .allocation_unit_size = 4096 };
    if (s_wl == WL_INVALID_HANDLE && esp_vfs_fat_spiflash_mount_rw_wl(ROOT, "hubdata", &cfg, &s_wl) != ESP_OK)
        return false;
    if (!initialize && access(ROOT "/INDEX0.BIN", F_OK) != 0 && access(ROOT "/INDEX1.BIN", F_OK) != 0)
        return false;
    if (!hub_store_open(&s_store, ROOT))
        return false;
    if (initialize)
    {
        if (nvs_set_u8(s_nvs, "initialized", 1) != ESP_OK || nvs_commit(s_nvs) != ESP_OK)
            return false;
        s_initialized = true;
    }
    return true;
}
static size_t ui_peers(ui_peer_t out[UI_MAX_PEERS])
{
    size_t n = 0;
    for (int i = 0; i < UI_MAX_PEERS; i++)
        if (s_peers[i].id[0])
        {
            peer_t* p = &s_peers[i];
            snprintf(out[n].id, 37, "%s", p->id);
            snprintf(out[n].label, 48, "%s - %.6s",
                     !strcmp(p->platform, "windows") ? "Windows" : !strcmp(p->platform, "android") ? "Android" : "Mac",
                     p->id);
            out[n].online = online(p);
            n++;
        }
    return n;
}
static int compare_records(const void* a, const void* b)
{
    const cJSON *x = *(const cJSON* const*)a, *y = *(const cJSON* const*)b;
    double d = (double)num(y, "order") - (double)num(x, "order");
    return d > 0 ? 1 : d < 0 ? -1
                             : 0;
}
static void render_store(void)
{
    /* Called by the worker only, under the store lock. UI callbacks only queue. */
    uint32_t ids[HUB_MAX_RECORDS], fav[HUB_MAX_RECORDS];
    cJSON* sorted[HUB_MAX_RECORDS];
    size_t n = 0, nf = 0;
    cJSON* r;
    cJSON_ArrayForEach(r, hub_store_records(&s_store)) if (n < HUB_MAX_RECORDS) sorted[n++] = r;
    qsort(sorted, n, sizeof(*sorted), compare_records);
    /* Clearing both orders releases every LVGL widget before reusing pixels. */
    ui_set_order(CLIP_LIST_CLIPBOARD, NULL, 0);
    ui_set_order(CLIP_LIST_FAVORITES, NULL, 0);
    ui_reset_cards();
    for (size_t i = 0; i < n; i++)
    {
        r = sorted[i];
        ids[i] = num(r, "id");
        if (cJSON_IsTrue(cJSON_GetObjectItem(r, "favorite")))
            fav[nf++] = ids[i];
        char path[192];
        hub_store_path(&s_store, ids[i], true, path, sizeof(path));
        FILE* f = fopen(path, "rb");
        size_t len = num(r, "card_bytes");
        uint8_t* b = malloc(len);
        if (f && b && fread(b, 1, len, f) == len && len >= 10)
        {
            uint16_t w, h;
            memcpy(&w, b + 5, 2);
            memcpy(&h, b + 7, 2);
            uint32_t dropped;
            ui_put_card(ids[i], b + 10 + b[9] * 10, w, h, b + 10, b[9], &dropped);
        }
        if (f)
            fclose(f);
        free(b);
    }
    ui_set_order(CLIP_LIST_CLIPBOARD, ids, n);
    ui_set_order(CLIP_LIST_FAVORITES, fav, nf);
    ui_set_state(cJSON_IsTrue(cJSON_GetObjectItem(s_store.index, "paused")), n == 0, !s_store.ready);
}
static void post(uint8_t action, uint32_t id, uint8_t kind, uint8_t chip, const char* peer)
{
    event_t e = { .action = action, .id = id, .kind = kind, .chip = chip };
    if (peer)
        snprintf(e.peer, sizeof(e.peer), "%s", peer);
    if (xQueueSend(s_events, &e, 0) != pdTRUE)
        s_notice = CLIP_ASSET_BUSY;
}
static void on_tap(uint32_t id, uint8_t kind, uint8_t chip) { post(E_TAP, id, kind, chip, NULL); }
static void on_menu(uint32_t id, uint8_t action) { post(E_MENU, id, action, 0, NULL); }
static void on_page(uint8_t action) { post(E_PAGE, 0, action, 0, NULL); }
static void on_send(uint32_t id, uint8_t kind, uint8_t chip, const char* peer) { post(E_SEND, id, kind, chip, peer); }
static void on_revoke(const char* peer) { post(E_REVOKE, 0, 0, 0, peer); }
static const char* job_content(peer_t* p, char path[192], const char** chip, size_t* length, const char** kind)
{
    cJSON* r = hub_store_find(&s_store, p->record);
    if (!r)
        return "not_found";
    if (p->kind == CLIP_REGION_CHIP)
    {
        cJSON* v = cJSON_GetArrayItem(cJSON_GetObjectItem(r, "chips"), p->chip);
        if (!cJSON_IsString(v))
            return "not_found";
        *chip = v->valuestring;
        *length = strlen(*chip);
        *kind = "text";
    }
    else
    {
        *chip = NULL;
        *length = num(r, "bytes");
        *kind = str(r, "kind");
        hub_store_path(&s_store, p->record, false, path, 192);
    }
    return NULL;
}
static void start_job(event_t* e)
{
    peer_t* p = NULL;
    for (int i = 0; i < UI_MAX_PEERS; i++)
        if (!strcmp(s_peers[i].id, e->peer))
            p = &s_peers[i];
    if (!p || !online(p) || p->job[0])
    {
        s_notice = p && p->job[0] ? CLIP_ASSET_BUSY : CLIP_ASSET_SEND_FAILED;
        return;
    }
    p->record = e->id;
    p->kind = e->kind;
    p->chip = e->chip;
    char path[192];
    const char *chip, *kind;
    size_t size;
    if (job_content(p, path, &chip, &size, &kind))
    {
        s_notice = CLIP_ASSET_SEND_FAILED;
        return;
    }
    cJSON* r = hub_store_find(&s_store, p->record);
    if (chip)
        hub_sha256((const uint8_t*)chip, size, p->sha);
    else
        snprintf(p->sha, 65, "%s", str(r, "sha256"));
    random_hex(p->job, 16);
    snprintf(p->job_session, sizeof(p->job_session), "%s", p->session);
    p->deadline = now() + JOB_MS;
    p->claimed = p->ready = false;
    s_notice = CLIP_ASSET_BUSY;
}
static bool code_open(void) { return s_pair_code[0] && now() < s_code_until; }
static void close_code(void)
{
    memset(s_pair_code, 0, sizeof(s_pair_code));
    s_code_until = 0;
}
/* Open the 2-minute pairing window (USB and phone code). Caller holds s_lock. */
static void open_pairing(void)
{
    s_pair_until = now() + 120000;
    uint32_t r;
    do
        r = esp_random();
    while (r >= 4294000000u); /* 4294000000 = 4294 * 10^6: uniform 6 digits */
    snprintf(s_pair_code, sizeof(s_pair_code), "%06lu", (unsigned long)(r % 1000000));
    s_code_until = s_pair_until;
    s_code_failures = 0;
}
static void show_peers(void)
{
    ui_peer_t peers[UI_MAX_PEERS];
    size_t n = ui_peers(peers);
    ui_show_peers(peers, n, code_open() ? s_pair_code : NULL);
}
const char* hub_pair_code(char code[7])
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool open = code_open();
    if (open)
        memcpy(code, s_pair_code, 7);
    xSemaphoreGive(s_lock);
    return open ? NULL : "pairing_closed";
}
void hub_pair_failed(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (code_open() && ++s_code_failures >= PAIR_MAX_FAILURES)
    {
        close_code();
        s_show_peers = true;
    }
    s_notice = CLIP_ASSET_PAIR_BAD_CODE;
    xSemaphoreGive(s_lock);
}
const char* hub_pair_add(const char* id, const char* name, const char* platform, const char* token)
{
    const char* err = NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    peer_t* p = NULL;
    if (!code_open())
        err = "pairing_closed";
    else if (!hub_valid_id(id) || strlen(name) > 64 || strlen(token) != 64 || strcmp(platform, "android"))
        err = "invalid_device";
    if (!err)
    {
        for (int i = 0; i < UI_MAX_PEERS; i++)
            if (!strcmp(s_peers[i].id, id))
                p = &s_peers[i];
        for (int i = 0; !p && i < UI_MAX_PEERS; i++)
            if (!s_peers[i].id[0])
                p = &s_peers[i];
        if (!p)
            err = "peer_limit";
    }
    if (!err)
    {
        peer_t old = *p;
        clear_job(p, false);
        memset(p, 0, sizeof(*p));
        snprintf(p->id, 37, "%s", id);
        snprintf(p->name, 65, "%s", name);
        snprintf(p->platform, 12, "%s", platform);
        snprintf(p->token, 65, "%s", token);
        if (!save_peers())
        {
            *p = old;
            err = "storage_error";
        }
        else
        {
            close_code(); /* single use */
            s_notice = CLIP_ASSET_PHONE_PAIRED;
            s_show_peers = true;
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}
static void handle_event(event_t* e)
{
    const char* err = NULL;
    ui_peer_t peers[UI_MAX_PEERS];
    if (e->action == E_TAP)
    {
        size_t n = ui_peers(peers);
        ui_show_targets(e->id, e->kind, e->chip, peers, n);
    }
    else if (e->action == E_SEND)
        start_job(e);
    else if (e->action == E_REVOKE)
    {
        for (int i = 0; i < UI_MAX_PEERS; i++)
            if (!strcmp(s_peers[i].id, e->peer))
            {
                peer_t old = s_peers[i];
                clear_job(&s_peers[i], true);
                memset(&s_peers[i], 0, sizeof(peer_t));
                if (!save_peers())
                {
                    s_peers[i] = old;
                    err = "storage_error";
                }
            }
    }
    else if (e->action == E_MENU)
    {
        if (e->kind == CLIP_MENU_DELETE)
            err = hub_store_delete(&s_store, e->id);
        else
            err = hub_store_favorite(&s_store, e->id, e->kind == CLIP_MENU_FAVORITE);
        s_dirty = true;
    }
    else if (e->action == E_PAGE)
    {
        if (e->kind == CLIP_PAGE_CLEAR)
        {
            err = hub_store_clear(&s_store);
            s_dirty = true;
        }
        else if (e->kind == CLIP_PAGE_PAUSE || e->kind == CLIP_PAGE_RESUME)
        {
            err = hub_store_pause(&s_store, e->kind == CLIP_PAGE_PAUSE);
            s_dirty = true;
        }
        else if (e->kind == CLIP_PAGE_STORAGE)
        {
            char info[128];
            snprintf(info, sizeof(info), "%u / %u records\n%.2f / 8 MiB\nText: 64 KiB max", cJSON_GetArraySize(hub_store_records(&s_store)), HUB_MAX_RECORDS, hub_store_used(&s_store) / 1048576.0);
            ui_show_info(CLIP_ASSET_STORAGE_TITLE, info);
        }
        else if (e->kind == CLIP_PAGE_PAIRING)
        {
            open_pairing();
            show_peers();
        }
        else if (e->kind == CLIP_PAGE_DEVICES)
        {
            char info[220];
            snprintf(info, sizeof(info), "Bluetooth LE\n\n");
            size_t n = ui_peers(peers);
            for (size_t i = 0; i < n; i++)
            {
                size_t used = strlen(info);
                snprintf(info + used, sizeof(info) - used, "%s: %s\n", peers[i].label, peers[i].online ? "online" : "offline");
            }
            ui_show_info(CLIP_ASSET_DEVICES_TITLE, info);
        }
    }
    if (err)
        s_notice = CLIP_ASSET_ERROR;
}
static void worker(void* arg)
{
    (void)arg;
    event_t e;
    for (;;)
    {
        bool event = xQueueReceive(s_events, &e, pdMS_TO_TICKS(100)) == pdTRUE;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (event)
            handle_event(&e);
        for (int i = 0; i < UI_MAX_PEERS; i++)
        {
            peer_t* p = &s_peers[i];
            if (p->job[0] && (!online(p) || now() > p->deadline || !hub_store_find(&s_store, p->record)))
                clear_job(p, true);
        }
        if (s_store.upload && now() - s_upload_at > 30000)
            hub_store_abort(&s_store);
        if (s_dirty)
        {
            render_store();
            s_dirty = false;
        }
        if (s_show_peers)
        {
            show_peers();
            s_show_peers = false;
        }
        uint8_t notice = atomic_exchange(&s_notice, 0);
        bool restart = s_restart_at && now() >= s_restart_at;
        xSemaphoreGive(s_lock);
        if (notice)
            hub_notice(notice);
        if (atomic_exchange(&s_wake, false))
            screen_wake();
        if (restart)
            esp_restart();
    }
}
static cJSON* status(bool usb)
{
    cJSON* o = answer(NULL);
    cJSON_AddNumberToObject(o, "version", 4);
    cJSON_AddStringToObject(o, "board_id", s_board_id);
    cJSON_AddBoolToObject(o, "storage_ready", s_store.ready);
    cJSON_AddBoolToObject(o, "initialized", s_initialized);
    cJSON_AddBoolToObject(o, "paused", cJSON_IsTrue(cJSON_GetObjectItem(s_store.index, "paused")));
    cJSON_AddNumberToObject(o, "used_bytes", s_store.ready ? hub_store_used(&s_store) : 0);
    cJSON_AddNumberToObject(o, "records", cJSON_GetArraySize(hub_store_records(&s_store)));
    cJSON_AddNumberToObject(o, "uptime_ms", now());
    cJSON_AddNumberToObject(o, "reset_reason", esp_reset_reason());
    cJSON_AddNumberToObject(o, "free_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddStringToObject(o, "transport", "ble");
    cJSON_AddStringToObject(o, "content", "text_only");
    (void)usb;
    return o;
}
static const char* download_locked(peer_t* p, const char* job, size_t offset, size_t wanted, uint8_t* out, size_t* length);
static cJSON* request_locked(const cJSON* r, bool usb, const char* token)
{
    const char* op = str(r, "op");
    peer_t* peer = authenticate(token);
    if (usb && !strcmp(op, "status"))
        return status(true);
    if (usb && !strcmp(op, "initialize"))
    {
        if (s_initialized)
            return answer(s_store.ready ? NULL : "storage_corrupt");
        if (!mount_store(true))
            return answer("storage_error");
        s_dirty = true;
        return answer(NULL);
    }
    if (usb && !strcmp(op, "pair"))
    {
        const char *id = str(r, "device_id"), *name = str(r, "name"), *platform = str(r, "platform");
        if (!hub_valid_id(id) || strlen(name) > 64 || (strcmp(platform, "macos") && strcmp(platform, "windows")))
            return answer("invalid_device");
        bool any = false;
        for (int i = 0; i < UI_MAX_PEERS; i++)
            any |= s_peers[i].id[0] != 0;
        peer_t* p = NULL;
        for (int i = 0; i < UI_MAX_PEERS; i++)
            if (!strcmp(s_peers[i].id, id))
                p = &s_peers[i];
        if (any && now() > s_pair_until && (!p || !constant_equal(p->token, str(r, "token"))))
            return answer("pairing_closed");
        if (!p)
            for (int i = 0; i < UI_MAX_PEERS; i++)
                if (!s_peers[i].id[0])
                {
                    p = &s_peers[i];
                    break;
                }
        if (!p)
            return answer("peer_limit");
        peer_t old = *p;
        memset(p, 0, sizeof(*p));
        snprintf(p->id, 37, "%s", id);
        snprintf(p->name, 65, "%s", name);
        snprintf(p->platform, 12, "%s", platform);
        random_hex(p->token, 32);
        if (!save_peers())
        {
            *p = old;
            return answer("storage_error");
        }
        cJSON* o = status(true);
        cJSON_AddStringToObject(o, "token", p->token);
        return o;
    }
    /* USB is trusted for local setup/test and carries the same per-device
     * credential for content operations. It cannot impersonate an online peer. */
    if (!peer)
        return answer("unauthorized");
    if (usb && !strcmp(op, "pair_window"))
    {
        s_pair_until = now() + 120000;
        return answer(NULL);
    }
    if (usb && !strcmp(op, "pair_code"))
    {
        /* Same as tapping ⋯ → 配对管理 (window + code on screen); the code is
         * also returned so a USB-trusted tool can test phone pairing. */
        open_pairing();
        s_show_peers = true;
        cJSON* o = answer(NULL);
        cJSON_AddStringToObject(o, "code", s_pair_code);
        return o;
    }
    if (usb && !strcmp(op, "control"))
    {
        const char *action = str(r, "action"), *err = "unknown_action";
        if (!strcmp(action, "favorite"))
            err = hub_store_favorite(&s_store, num(r, "record_id"), cJSON_IsTrue(cJSON_GetObjectItem(r, "favorite")));
        else if (!strcmp(action, "delete"))
            err = hub_store_delete(&s_store, num(r, "record_id"));
        else if (!strcmp(action, "clear"))
            err = hub_store_clear(&s_store);
        else if (!strcmp(action, "pause"))
            err = hub_store_pause(&s_store, cJSON_IsTrue(cJSON_GetObjectItem(r, "paused")));
        if (!err)
            s_dirty = true;
        return answer(err);
    }
    if (usb && !strcmp(op, "restart"))
    {
        s_restart_at = now() + 500;
        return answer(NULL);
    }
    if (usb && !strcmp(op, "revoke"))
    {
        for (int i = 0; i < UI_MAX_PEERS; i++)
            if (!strcmp(s_peers[i].id, str(r, "device_id")))
            {
                peer_t old = s_peers[i];
                clear_job(&s_peers[i], true);
                memset(&s_peers[i], 0, sizeof(peer_t));
                if (!save_peers())
                {
                    s_peers[i] = old;
                    return answer("storage_error");
                }
                return answer(NULL);
            }
        return answer("not_found");
    }
    if (!strcmp(op, "status"))
        return status(false);
    if (!strcmp(op, "heartbeat"))
    {
        const char* session = str(r, "session");
        if (!hub_valid_id(session))
            return answer("invalid_session");
        if (strcmp(peer->session, session) || !online(peer))
            clear_job(peer, true);
        snprintf(peer->session, 37, "%s", session);
        peer->seen = now();
        return status(false);
    }
    if (!strcmp(op, "list"))
    {
        cJSON* o = answer(NULL);
        cJSON_AddItemToObject(o, "records", cJSON_Duplicate(hub_store_records(&s_store), true));
        return o;
    }
    if (!strcmp(op, "begin"))
    {
        cJSON* meta = cJSON_Duplicate(cJSON_GetObjectItem(r, "metadata"), true);
        if (!cJSON_IsObject(meta))
        {
            cJSON_Delete(meta);
            return answer("invalid_metadata");
        }
        cJSON_DeleteItemFromObject(meta, "source_device");
        cJSON_DeleteItemFromObject(meta, "source_name");
        cJSON_AddStringToObject(meta, "source_device", peer->id);
        cJSON_AddStringToObject(meta, "source_name", peer->name);
        uint32_t existing;
        const char* err = hub_store_begin(&s_store, meta, &existing);
        cJSON_Delete(meta);
        cJSON* o = answer(err);
        if (!err)
        {
            cJSON_AddNumberToObject(o, "record_id", existing);
            cJSON_AddNumberToObject(o, "content_offset", s_store.content_offset);
            cJSON_AddNumberToObject(o, "card_offset", s_store.card_offset);
            s_upload_at = now();
            s_dirty = true;
        }
        return o;
    }
    if (!strcmp(op, "chunk"))
    {
        if (!s_store.upload || strcmp(str(s_store.upload, "source_device"), peer->id))
            return answer("no_upload");
        const char* encoded = str(r, "data");
        size_t n = 0;
        if (strlen(encoded) > 32768)
            return answer("too_large");
        uint8_t* b = malloc(24576);
        if (!b)
            return answer("out_of_memory");
        int rc = mbedtls_base64_decode(b, 24576, &n, (const uint8_t*)encoded, strlen(encoded));
        const char* err = rc ? "invalid_base64" : hub_store_chunk(&s_store, str(r, "event_id"), !strcmp(str(r, "part"), "card"), num(r, "offset"), b, n);
        free(b);
        s_upload_at = now();
        return answer(err);
    }
    if (!strcmp(op, "commit") || !strcmp(op, "abort"))
    {
        if (!s_store.upload || strcmp(str(s_store.upload, "source_device"), peer->id) || strcmp(str(s_store.upload, "event_id"), str(r, "event_id")))
            return answer("no_upload");
        if (!strcmp(op, "abort"))
        {
            hub_store_abort(&s_store);
            return answer(NULL);
        }
        uint32_t id;
        const char* err = hub_store_commit(&s_store, str(r, "event_id"), &id);
        cJSON* o = answer(err);
        if (!err)
        {
            cJSON_AddNumberToObject(o, "record_id", id);
            s_dirty = true;
            s_wake = true; /* a computer added an item: worker lights the screen up */
        }
        return o;
    }
    if (!strcmp(op, "poll"))
    {
        cJSON* o = answer(NULL);
        if (peer->job[0] && online(peer) && !peer->claimed && now() < peer->deadline && !strcmp(peer->session, peer->job_session))
        {
            const char *chip, *kind;
            size_t size;
            char path[192];
            const char* err = job_content(peer, path, &chip, &size, &kind);
            if (err)
            {
                clear_job(peer, true);
                cJSON_Delete(o);
                return answer(err);
            }
            peer->claimed = true;
            cJSON* job = cJSON_AddObjectToObject(o, "job");
            cJSON_AddStringToObject(job, "id", peer->job);
            cJSON_AddStringToObject(job, "kind", kind);
            cJSON_AddNumberToObject(job, "bytes", size);
            cJSON_AddStringToObject(job, "sha256", peer->sha);
        }
        return o;
    }
    if (!strcmp(op, "ready") || !strcmp(op, "ack"))
    {
        if (!peer->job[0] || strcmp(peer->job, str(r, "job_id")) || !online(peer) || now() > peer->deadline || strcmp(peer->session, peer->job_session) || !peer->claimed)
        {
            clear_job(peer, true);
            return answer("expired_delivery");
        }
        if (!strcmp(op, "ready"))
        {
            peer->ready = true;
            peer->deadline = now() + 2000;
            cJSON* o = answer(NULL);
            cJSON_AddNumberToObject(o, "lease_ms", 2000);
            return o;
        }
        bool success = peer->ready && cJSON_IsTrue(cJSON_GetObjectItem(r, "success"));
        clear_job(peer, false);
        /* "paste" is the receiver's auto-paste outcome (absent from older
         * clients, "off" when disabled): pick the matching toast. */
        const char* paste = str(r, "paste");
        uint8_t done = CLIP_ASSET_SENT;
        if (!strcmp(paste, "pasted"))
            done = CLIP_ASSET_PASTED;
        else if (!strcmp(paste, "accessibility"))
            done = CLIP_ASSET_PASTE_ACCESS;
        else if (!strcmp(paste, "secure_input") || !strcmp(paste, "blocked"))
            done = CLIP_ASSET_PASTE_MANUAL;
        s_notice = success ? done : CLIP_ASSET_SEND_FAILED;
        return answer(NULL);
    }
    if (usb && !strcmp(op, "test_select"))
    {
        /* Explicit diagnostic operation used by the hardware acceptance tool.
         * It queues the same send command as a physical target selection. */
        event_t e = { .action = E_SEND, .id = num(r, "record_id"), .kind = num(r, "kind"), .chip = num(r, "chip") };
        snprintf(e.peer, 37, "%s", str(r, "target"));
        start_job(&e);
        return answer(NULL);
    }
    if (usb && !strcmp(op, "fetch"))
    {
        size_t wanted = num(r, "size"), n = 0;
        if (wanted > 16384)
            return answer("too_large");
        uint8_t* b = malloc(wanted ? wanted : 1);
        if (!b)
            return answer("out_of_memory");
        const char* e = download_locked(peer, str(r, "job_id"), num(r, "offset"), wanted, b, &n);
        cJSON* o = answer(e);
        if (!e)
        {
            size_t out_size = 4 * ((n + 2) / 3) + 1, wrote = 0;
            char* out = calloc(out_size, 1);
            if (out && mbedtls_base64_encode((uint8_t*)out, out_size, &wrote, b, n) == 0)
                cJSON_AddStringToObject(o, "data", out);
            else
            {
                cJSON_Delete(o);
                o = answer("out_of_memory");
            }
            free(out);
        }
        free(b);
        return o;
    }
    return answer("unknown_operation");
}
cJSON* hub_request(const cJSON* request, bool usb, const char* token)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON* o = request_locked(request, usb, token ? token : str(request, "token"));
    xSemaphoreGive(s_lock);
    return o;
}
cJSON* hub_chunk(const char* token, const char* event, bool card, size_t offset, const uint8_t* data, size_t length)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    peer_t* p = authenticate(token);
    const char* err = "unauthorized";
    if (p && s_store.upload && !strcmp(p->id, str(s_store.upload, "source_device")))
    {
        err = hub_store_chunk(&s_store, event, card, offset, data, length);
        s_upload_at = now();
    }
    cJSON* o = answer(err);
    xSemaphoreGive(s_lock);
    return o;
}
static const char* download_locked(peer_t* p, const char* job, size_t offset, size_t wanted, uint8_t* out, size_t* length)
{
    *length = 0;
    if (!p || !p->job[0] || strcmp(p->job, job) || !online(p) || !p->claimed || now() > p->deadline || strcmp(p->session, p->job_session))
        return "expired_delivery";
    const char *chip, *kind;
    size_t n;
    char path[192];
    const char* err = job_content(p, path, &chip, &n, &kind);
    if (err)
        return err;
    if (offset > n || wanted > 32768)
        return "invalid_offset";
    size_t take = n - offset < wanted ? n - offset : wanted;
    if (chip)
        memcpy(out, chip + offset, take);
    else
    {
        FILE* f = fopen(path, "rb");
        if (!f)
            return "storage_error";
        bool ok = fseek(f, offset, SEEK_SET) == 0 && fread(out, 1, take, f) == take;
        fclose(f);
        if (!ok)
            return "storage_error";
    }
    *length = take;
    /* Large lossless images can exceed two minutes on this flash/BLE path.
     * A successful block renews the download's idle timeout. Never renew the
     * separate two-second clipboard-write lease after ready has been granted. */
    if (take && !p->ready)
        p->deadline = now() + JOB_MS;
    return NULL;
}
const char* hub_download(const char* token, const char* job, size_t offset, size_t wanted, uint8_t* out, size_t* length)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const char* e = download_locked(authenticate(token), job, offset, wanted, out, length);
    xSemaphoreGive(s_lock);
    return e;
}
void hub_start(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(nvs_open("deskclip", NVS_READWRITE, &s_nvs));
    s_lock = xSemaphoreCreateMutex();
    s_events = xQueueCreate(16, sizeof(event_t));
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_board_id, 37, "deskclip-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    uint8_t initialized = 0;
    nvs_get_u8(s_nvs, "initialized", &initialized);
    s_initialized = initialized == 1;
    if (s_initialized && !mount_store(false))
        ESP_LOGE("hub", "storage unavailable; preserving partition");
    load_peers();
    hub_assets_init();
    ui_set_handlers(on_tap, on_menu, on_page);
    ui_set_hub_handlers(on_send, on_revoke);
    s_dirty = true;
    xTaskCreate(worker, "hub", 12288, NULL, 4, NULL);
    /* Remove credentials from the superseded Wi-Fi prototype. No Wi-Fi
     * initialization or IP networking is linked into this BLE product. */
    bool removed = false;
    const char *obsolete[] = {"ssid", "psk", "cert", "key"};
    for (size_t i=0;i<4;i++)
        removed |= nvs_erase_key(s_nvs, obsolete[i]) == ESP_OK;
    if (removed) ESP_ERROR_CHECK(nvs_commit(s_nvs));
    hub_ble_start();
}
