#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "hub.h"
#include "hub_store.h"
#include "pair_crypto.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* No clipboard credentials or content are sent in plaintext. USB provisions
 * a different 256-bit secret per computer. Fresh nonces derive each BLE
 * session; direction-separated AES-GCM records reject replay and tampering. */
#define CONNECTIONS 4
#define MAX_REQUEST (48 * 1024 + 64)
#define MAX_RESPONSE (1024 * 1024)
static const ble_uuid128_t SERVICE =
    BLE_UUID128_INIT(0x70, 0x40, 0x62, 0x8a, 0xc1, 0xdf, 0x18, 0x94, 0x49, 0x4a, 0x46, 0x4e, 0x00, 0x10, 0xc1, 0x89);
static const ble_uuid128_t WRITE =
    BLE_UUID128_INIT(0x70, 0x40, 0x62, 0x8a, 0xc1, 0xdf, 0x18, 0x94, 0x49, 0x4a, 0x46, 0x4e, 0x01, 0x10, 0xc1, 0x89);
static const ble_uuid128_t NOTIFY =
    BLE_UUID128_INIT(0x70, 0x40, 0x62, 0x8a, 0xc1, 0xdf, 0x18, 0x94, 0x49, 0x4a, 0x46, 0x4e, 0x02, 0x10, 0xc1, 0x89);
typedef struct
{
    bool connected, authenticated;
    uint16_t handle;
    uint32_t epoch;
    char token[65];
    uint8_t key[32], header[4], header_size;
    uint8_t *incoming;
    size_t expected, received;
    uint64_t rx, tx;
    SemaphoreHandle_t credits;
    /* Phone pairing by code (P records, before any handshake). */
    bool pairing;
    int64_t pair_at;
    pair_result_t pair;
    char pair_id[37], pair_name[65], pair_platform[12];
} connection_t;
typedef struct
{
    uint8_t slot;
    uint32_t epoch;
    uint8_t *data;
    size_t length;
    char disconnected[65];
} message_t;
static connection_t s_connections[CONNECTIONS];
static SemaphoreHandle_t s_lock;
static _Atomic uint8_t s_ble_stage; /* diagnostics, see guard.h */
static QueueHandle_t s_messages;
static uint16_t s_notify;
static uint8_t s_address_type;
static int gap(struct ble_gap_event *event, void *arg);
#define PAIR_SESSION_US 30000000LL

static int find(uint16_t handle)
{
    for (int i = 0; i < CONNECTIONS; i++)
        if (s_connections[i].connected && s_connections[i].handle == handle)
            return i;
    return -1;
}
static bool current(const message_t *m)
{
    connection_t *c = &s_connections[m->slot];
    return c->connected && c->epoch == m->epoch;
}
static void advertise(void)
{
    if (ble_gap_adv_active())
        return;
    struct ble_hs_adv_fields fields = {0}, response = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&SERVICE;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    const char *name = ble_svc_gap_device_name();
    response.name = (uint8_t *)name;
    response.name_len = strlen(name);
    response.name_is_complete = 1;
    if (ble_gap_adv_set_fields(&fields) || ble_gap_adv_rsp_set_fields(&response))
        return;
    struct ble_gap_adv_params p = {.conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN};
    ble_gap_adv_start(s_address_type, NULL, BLE_HS_FOREVER, &p, gap, NULL);
}
static void reset_input(connection_t *c)
{
    free(c->incoming);
    c->incoming = NULL;
    c->header_size = 0;
    c->expected = c->received = 0;
}
static int access(uint16_t handle, uint16_t attr, struct ble_gatt_access_ctxt *ctx, void *arg)
{
    (void)attr;
    (void)arg;
    if (ctx->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    uint8_t bytes[512];
    uint16_t length;
    if (ble_hs_mbuf_to_flat(ctx->om, bytes, sizeof(bytes), &length))
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    int result = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int slot = find(handle);
    if (slot < 0)
    {
        result = BLE_ATT_ERR_UNLIKELY;
        goto done;
    }
    connection_t *c = &s_connections[slot];
    size_t offset = 0;
    while (offset < length)
    {
        if (c->header_size < 4)
        {
            c->header[c->header_size++] = bytes[offset++];
            if (c->header_size < 4)
                continue;
            uint32_t size;
            memcpy(&size, c->header, 4);
            if (!size || size > MAX_REQUEST)
            {
                reset_input(c);
                result = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
                break;
            }
            c->expected = size;
            c->incoming = malloc(size);
            if (!c->incoming)
            {
                reset_input(c);
                result = BLE_ATT_ERR_INSUFFICIENT_RES;
                break;
            }
        }
        size_t n = c->expected - c->received;
        if (n > length - offset)
            n = length - offset;
        memcpy(c->incoming + c->received, bytes + offset, n);
        c->received += n;
        offset += n;
        if (c->received == c->expected)
        {
            message_t m = {.slot = slot, .epoch = c->epoch, .data = c->incoming, .length = c->expected};
            c->incoming = NULL;
            reset_input(c);
            if (xQueueSend(s_messages, &m, 0) != pdTRUE)
            {
                free(m.data);
                result = BLE_ATT_ERR_INSUFFICIENT_RES;
                break;
            }
        }
    }
done:
    xSemaphoreGive(s_lock);
    return result;
}
static const struct ble_gatt_svc_def SERVICES[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = &SERVICE.u,
     .characteristics =
         (struct ble_gatt_chr_def[]){
             {.uuid = &WRITE.u, .access_cb = access, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP},
             {.uuid = &NOTIFY.u, .access_cb = access, .val_handle = &s_notify, .flags = BLE_GATT_CHR_F_NOTIFY},
             {0}}},
    {0}};

static bool send_record(const message_t *m, const uint8_t *data, size_t size)
{
    if (!size || size > MAX_RESPONSE)
        return false;
    uint32_t len = size;
    size_t offset = 0;
    int64_t deadline = esp_timer_get_time() + 30000000;
    while (offset < size + 4 && esp_timer_get_time() < deadline)
    {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool valid = current(m);
        uint16_t handle = s_connections[m->slot].handle;
        xSemaphoreGive(s_lock);
        if (!valid)
            return false;
        if (xSemaphoreTake(s_connections[m->slot].credits, pdMS_TO_TICKS(100)) != pdTRUE)
            continue;
        uint16_t mtu = ble_att_mtu(handle);
        size_t n = mtu > 3 ? mtu - 3 : 20;
        if (n > 244)
            n = 244;
        if (n > size + 4 - offset)
            n = size + 4 - offset;
        uint8_t fragment[244];
        for (size_t i = 0; i < n; i++)
            fragment[i] = offset + i < 4 ? ((uint8_t *)&len)[offset + i] : data[offset + i - 4];
        struct os_mbuf *b = ble_hs_mbuf_from_flat(fragment, n);
        int rc = b ? ble_gatts_notify_custom(handle, s_notify, b) : BLE_HS_ENOMEM;
        if (rc)
        {
            xSemaphoreGive(s_connections[m->slot].credits);
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        offset += n;
    }
    return offset == size + 4;
}
static bool mac(const uint8_t key[32], const char *label, const uint8_t *context, size_t n, uint8_t out[32])
{
    uint8_t data[256];
    size_t prefix = strlen(label) + 1;
    if (prefix + n > sizeof(data))
        return false;
    memcpy(data, label, prefix);
    memcpy(data + prefix, context, n);
    return mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, 32, data, prefix + n, out) == 0;
}
static bool handshake(const message_t *m)
{
    if (m->length != 53 || m->data[0] != 'H')
        return false;
    char id[37], token[65];
    memcpy(id, m->data + 1, 36);
    id[36] = 0;
    if (!hub_peer_credential(id, token))
        return false;
    uint8_t key[32];
    for (int i = 0; i < 32; i++)
    {
        unsigned value;
        if (sscanf(token + 2 * i, "%2x", &value) != 1)
            return false;
        key[i] = value;
    }
    uint8_t response[49] = {'H'}, context[160], session[32];
    esp_fill_random(response + 1, 16);
    size_t n = strlen(hub_board_id());
    memcpy(context, hub_board_id(), n);
    memcpy(context + n, id, 36);
    n += 36;
    memcpy(context + n, m->data + 37, 16);
    n += 16;
    memcpy(context + n, response + 1, 16);
    n += 16;
    if (!mac(key, "deskclip-v4/server", context, n, response + 17) ||
        !mac(key, "deskclip-v4/session", context, n, session))
        return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool valid = current(m) && !s_connections[m->slot].authenticated;
    if (valid)
    {
        connection_t *c = &s_connections[m->slot];
        memcpy(c->key, session, 32);
        memcpy(c->token, token, 65);
        c->rx = c->tx = 0;
        c->authenticated = true;
    }
    xSemaphoreGive(s_lock);
    memset(key, 0, sizeof(key));
    memset(session, 0, sizeof(session));
    return valid && send_record(m, response, sizeof(response));
}
static void pair_reset(connection_t *c)
{
    c->pairing = false;
    memset(&c->pair, 0, sizeof(c->pair));
    c->pair_id[0] = c->pair_name[0] = c->pair_platform[0] = 0;
}
static int pair_rng(void *ctx, unsigned char *out, size_t len)
{
    (void)ctx;
    esp_fill_random(out, len);
    return 0;
}
static bool pair_error(const message_t *m, const char *error)
{
    uint8_t out[40] = {'P', 0xff};
    size_t n = strlen(error);
    if (n > sizeof(out) - 2)
        n = sizeof(out) - 2;
    memcpy(out + 2, error, n);
    return send_record(m, out, n + 2);
}
/* SPAKE2 pairing (see pair_crypto.h / host/clipboard_agent/pake.py):
 *   P01 device_id pA name platform -> P02 board_id pB
 *   P03 cA                         -> P04 cB (peer stored) | PFF error
 * Only one connection pairs at a time; a wrong cA counts against the code. */
static bool pairing(const message_t *m)
{
    const uint8_t *d = m->data;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    connection_t *c = &s_connections[m->slot];
    bool authenticated = c->authenticated;
    bool busy = false;
    int64_t t = esp_timer_get_time();
    for (int i = 0; i < CONNECTIONS; i++)
        if (i != m->slot && s_connections[i].connected && s_connections[i].pairing &&
            t - s_connections[i].pair_at < PAIR_SESSION_US)
            busy = true;
    xSemaphoreGive(s_lock);
    if (authenticated || m->length < 2)
        return false;
    if (d[1] == 1)
    {
        if (busy)
            return pair_error(m, "busy");
        char code[7];
        const char *err = hub_pair_code(code);
        if (err)
            return pair_error(m, err);
        if (m->length < 2 + 36 + 65 + 2)
            return pair_error(m, "invalid_pair_request");
        size_t name_len = d[103];
        if (104 + name_len + 1 > m->length || name_len > 64)
            return pair_error(m, "invalid_pair_request");
        size_t plat_len = d[104 + name_len];
        if (105 + name_len + plat_len != m->length || plat_len >= 12)
            return pair_error(m, "invalid_pair_request");
        char id[37], name[65], platform[12];
        memcpy(id, d + 2, 36);
        id[36] = 0;
        memcpy(name, d + 104, name_len);
        name[name_len] = 0;
        memcpy(platform, d + 105 + name_len, plat_len);
        platform[plat_len] = 0;
        if (!hub_valid_id(id) || strlen(name) != name_len || strcmp(platform, "android"))
            return pair_error(m, "invalid_device");
        uint8_t w[32];
        pair_result_t result;
        const char *board = hub_board_id();
        int rc = pair_code_scalar(code, w);
        memset(code, 0, sizeof(code));
        if (!rc)
            rc = pair_spake2_b(w, (const uint8_t *)id, 36, (const uint8_t *)board, strlen(board), d + 38, NULL,
                               pair_rng, NULL, &result);
        memset(w, 0, sizeof(w));
        if (rc)
            return pair_error(m, "invalid_pair_request");
        uint8_t reply[3 + 64 + 65] = {'P', 2, (uint8_t)strlen(board)};
        memcpy(reply + 3, board, reply[2]);
        memcpy(reply + 3 + reply[2], result.pb, 65);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool valid = current(m);
        if (valid)
        {
            c->pairing = true;
            c->pair_at = esp_timer_get_time();
            c->pair = result;
            memcpy(c->pair_id, id, sizeof(id));
            memcpy(c->pair_name, name, sizeof(name));
            memcpy(c->pair_platform, platform, sizeof(platform));
        }
        xSemaphoreGive(s_lock);
        memset(&result, 0, sizeof(result));
        return valid && send_record(m, reply, 3 + reply[2] + 65);
    }
    if (d[1] == 3)
    {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool active = c->pairing && esp_timer_get_time() - c->pair_at < PAIR_SESSION_US && current(m);
        pair_result_t result = c->pair;
        char id[37], name[65], platform[12];
        memcpy(id, c->pair_id, sizeof(id));
        memcpy(name, c->pair_name, sizeof(name));
        memcpy(platform, c->pair_platform, sizeof(platform));
        pair_reset(c);
        xSemaphoreGive(s_lock);
        if (!active)
            return pair_error(m, "pairing_closed");
        bool ok = m->length == 34 && pair_equal32(d + 2, result.ca);
        const char *err = NULL;
        if (!ok)
        {
            hub_pair_failed();
            err = "bad_code";
        }
        else
            err = hub_pair_add(id, name, platform, result.token);
        uint8_t reply[34] = {'P', 4};
        memcpy(reply + 2, result.cb, 32);
        memset(&result, 0, sizeof(result));
        return err ? pair_error(m, err) : send_record(m, reply, sizeof(reply));
    }
    return pair_error(m, "invalid_pair_request");
}
static uint8_t *json_response(cJSON *json, size_t *size)
{
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!text)
        return NULL;
    *size = strlen(text) + 1;
    uint8_t *out = malloc(*size);
    if (out)
    {
        out[0] = 'J';
        memcpy(out + 1, text, *size - 1);
    }
    free(text);
    return out;
}
static uint8_t *dispatch(const char *token, uint8_t *data, size_t n, size_t *size)
{
    cJSON *answer = NULL;
    if (n > 1 && data[0] == 'J')
    {
        cJSON *request = cJSON_ParseWithLength((char *)data + 1, n - 1);
        answer = hub_request(request, false, token);
        cJSON_Delete(request);
    }
    else if (n >= 42 && data[0] == 'U' && data[37] <= 1)
    {
        char event[37];
        memcpy(event, data + 1, 36);
        event[36] = 0;
        uint32_t offset;
        memcpy(&offset, data + 38, 4);
        answer = hub_chunk(token, event, data[37], offset, data + 42, n - 42);
    }
    else if (n == 41 && data[0] == 'D')
    {
        char job[33];
        memcpy(job, data + 1, 32);
        job[32] = 0;
        uint32_t offset, wanted;
        memcpy(&offset, data + 33, 4);
        memcpy(&wanted, data + 37, 4);
        if (wanted <= 16384)
        {
            uint8_t *out = malloc(wanted + 1);
            size_t length = 0;
            const char *error = out ? hub_download(token, job, offset, wanted, out + 1, &length) : "out_of_memory";
            if (!error)
            {
                out[0] = 'B';
                *size = length + 1;
                return out;
            }
            free(out);
            answer = cJSON_CreateObject();
            cJSON_AddBoolToObject(answer, "ok", false);
            cJSON_AddStringToObject(answer, "error", error);
        }
    }
    if (!answer)
    {
        answer = cJSON_CreateObject();
        cJSON_AddBoolToObject(answer, "ok", false);
        cJSON_AddStringToObject(answer, "error", "invalid_ble_request");
    }
    return json_response(answer, size);
}
static bool encrypted(const message_t *m)
{
    if (m->length < 26 || m->data[0] != 'E')
        return false;
    uint8_t key[32];
    char token[65];
    uint64_t sequence;
    memcpy(&sequence, m->data + 1, 8);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    connection_t *c = &s_connections[m->slot];
    bool valid = current(m) && c->authenticated && sequence == c->rx + 1;
    memcpy(key, c->key, 32);
    memcpy(token, c->token, 65);
    xSemaphoreGive(s_lock);
    if (!valid || !hub_authorized(token))
        return false;
    size_t size = m->length - 25;
    uint8_t *plain = malloc(size + 1), nonce[12] = {'C', '2', 'B', 0};
    memcpy(nonce + 4, m->data + 1, 8);
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int rc = plain ? mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) : -1;
    if (!rc)
        rc = mbedtls_gcm_auth_decrypt(&gcm, size, nonce, 12, m->data, 9, m->data + 9 + size, 16, m->data + 9, plain);
    if (rc)
    {
        free(plain);
        mbedtls_gcm_free(&gcm);
        return false;
    }
    plain[size] = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    c->rx = sequence;
    xSemaphoreGive(s_lock);
    size_t response_size = 0;
    uint8_t *response = dispatch(token, plain, size, &response_size);
    free(plain);
    if (!response || response_size + 25 > MAX_RESPONSE)
    {
        free(response);
        mbedtls_gcm_free(&gcm);
        return false;
    }
    uint8_t *out = malloc(response_size + 25);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    sequence = ++c->tx;
    valid = current(m);
    xSemaphoreGive(s_lock);
    if (!out || !valid)
    {
        free(out);
        free(response);
        mbedtls_gcm_free(&gcm);
        return false;
    }
    out[0] = 'E';
    memcpy(out + 1, &sequence, 8);
    memcpy(nonce, "B2C", 4);
    memcpy(nonce + 4, out + 1, 8);
    rc = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, response_size, nonce, 12, out, 9, response, out + 9, 16,
                                   out + 9 + response_size);
    mbedtls_gcm_free(&gcm);
    memset(key, 0, sizeof(key));
    free(response);
    bool sent = !rc && send_record(m, out, response_size + 25);
    free(out);
    return sent;
}
static void worker(void *arg)
{
    (void)arg;
    message_t m;
    for (;;)
    {
        s_ble_stage = 0;
        if (xQueueReceive(s_messages, &m, portMAX_DELAY) != pdTRUE)
            continue;
        s_ble_stage = 1;
        if (m.disconnected[0])
        {
            hub_peer_disconnected(m.disconnected);
            continue;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool valid = current(&m);
        uint16_t handle = s_connections[m.slot].handle;
        xSemaphoreGive(s_lock);
        if (valid)
        {
            s_ble_stage = m.data[0] == 'H' ? 2 : m.data[0] == 'P' ? 3 : 4;
            bool ok = m.data[0] == 'H' ? handshake(&m) : m.data[0] == 'P' ? pairing(&m) : encrypted(&m);
            s_ble_stage = 5;
            if (!ok)
                ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        free(m.data);
    }
}
TaskHandle_t hub_ble_lock_holder(void) { return s_lock ? xSemaphoreGetMutexHolder(s_lock) : NULL; }
uint8_t hub_ble_stage(void) { return s_ble_stage; }
static int gap(struct ble_gap_event *e, void *arg)
{
    (void)arg;
    if (e->type == BLE_GAP_EVENT_CONNECT)
    {
        if (!e->connect.status)
        {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            for (int i = 0; i < CONNECTIONS; i++)
                if (!s_connections[i].connected)
                {
                    connection_t *c = &s_connections[i];
                    c->connected = true;
                    c->authenticated = false;
                    c->handle = e->connect.conn_handle;
                    c->epoch++;
                    c->token[0] = 0;
                    memset(c->key, 0, 32);
                    pair_reset(c);
                    reset_input(c);
                    while (xSemaphoreTake(c->credits, 0) == pdTRUE)
                    {
                    }
                    for (int n = 0; n < 4; n++)
                        xSemaphoreGive(c->credits);
                    break;
                }
            xSemaphoreGive(s_lock);
            /* Negotiate long link packets as in IDF's throughput peripheral. */
            ble_gap_set_data_len(e->connect.conn_handle, 251, 2120);
            struct ble_gap_upd_params p = {.itvl_min = 12, .itvl_max = 24, .latency = 0, .supervision_timeout = 400};
            ble_gap_update_params(e->connect.conn_handle, &p);
        }
        advertise();
    }
    else if (e->type == BLE_GAP_EVENT_DISCONNECT)
    {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        int i = find(e->disconnect.conn.conn_handle);
        if (i >= 0)
        {
            connection_t *c = &s_connections[i];
            message_t m = {.slot = i, .epoch = c->epoch};
            memcpy(m.disconnected, c->token, 65);
            c->connected = false;
            c->authenticated = false;
            pair_reset(c);
            reset_input(c);
            memset(c->key, 0, 32);
            xQueueSend(s_messages, &m, 0);
            for (int n = 0; n < 4; n++)
                xSemaphoreGive(c->credits);
        }
        xSemaphoreGive(s_lock);
        advertise();
    }
    else if (e->type == BLE_GAP_EVENT_ADV_COMPLETE)
        advertise();
    else if (e->type == BLE_GAP_EVENT_NOTIFY_TX && !e->notify_tx.indication)
    {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        int i = find(e->notify_tx.conn_handle);
        if (i >= 0)
            xSemaphoreGive(s_connections[i].credits);
        xSemaphoreGive(s_lock);
    }
    return 0;
}
static void sync(void)
{
    ble_gap_set_prefered_default_le_phy(BLE_GAP_LE_PHY_1M_MASK | BLE_GAP_LE_PHY_2M_MASK,
                                      BLE_GAP_LE_PHY_1M_MASK | BLE_GAP_LE_PHY_2M_MASK);
    if (!ble_hs_id_infer_auto(0, &s_address_type))
        advertise();
}
static void host(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}
void hub_ble_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_messages = xQueueCreate(16, sizeof(message_t));
    for (int i = 0; i < CONNECTIONS; i++)
        s_connections[i].credits = xSemaphoreCreateCounting(4, 4);
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    char name[24];
    const char *board = hub_board_id();
    snprintf(name, sizeof(name), "DeskClip-%s", board + strlen(board) - 6);
    ESP_ERROR_CHECK(ble_svc_gap_device_name_set(name));
    ESP_ERROR_CHECK(ble_gatts_count_cfg(SERVICES));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(SERVICES));
    ble_att_set_preferred_mtu(247);
    xTaskCreate(worker, "hub_ble", 12288, NULL, 4, NULL);
    nimble_port_freertos_init(host);
}
