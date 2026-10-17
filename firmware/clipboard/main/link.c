#include "link.h"

#include <string.h>

#include "esp_crc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_private/system_internal.h"
#include "hal/usb_serial_jtag_ll.h"
#include "protocol.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#include "tusb_cdc_acm.h"
#include "ui.h"

static const char *TAG = "link";

#define RING_SIZE (256 * 1024)

static uint8_t *s_ring;
static size_t s_head;
static size_t s_tail;
static size_t s_count;
static SemaphoreHandle_t s_mutex;
static SemaphoreHandle_t s_data;
static bool s_dtr;
static volatile bool s_hello_pending;
static bool s_host_seen;
static TickType_t s_next_hello;

static void s_write_all(const uint8_t *data, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        size_t queued = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, data + offset, length - offset);
        tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(100));
        if (queued == 0) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        offset += queued;
    }
}

static SemaphoreHandle_t s_tx_mutex;

/* Called from the link task (ACKs) and the LVGL task (taps, menus); the
 * mutex keeps their frames from interleaving on the wire. */
void link_send(uint8_t type, const uint8_t *payload, uint32_t length)
{
    if (s_tx_mutex) {
        xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    }
    uint8_t header[6] = {CLIP_MAGIC, type};
    memcpy(header + 2, &length, 4);
    uint32_t crc = esp_crc32_le(0, header + 1, 5);
    if (length > 0 && payload != NULL) {
        crc = esp_crc32_le(crc, payload, length);
    }
    s_write_all(header, sizeof(header));
    if (length > 0 && payload != NULL) {
        s_write_all(payload, length);
    }
    s_write_all((const uint8_t *)&crc, 4);
    if (s_tx_mutex) {
        xSemaphoreGive(s_tx_mutex);
    }
}

static void s_ack(uint8_t type)
{
    link_send(type, NULL, 0);
}

/* Inter-byte limit inside one frame. A frame cut short by the host is
 * dropped after this long instead of swallowing the start of the next one. */
#define FRAME_GAP_TICKS pdMS_TO_TICKS(500)

/*
 * Copy up to `length` bytes out of the ring, waiting at most `wait` ticks for
 * the first byte. s_data is a binary semaphore that link_rx gives once per USB
 * chunk, so it is often already "given" for bytes we consumed earlier. Taking
 * it therefore only means "maybe new data": always re-check s_count and keep
 * waiting until the deadline instead of treating an empty ring as failure.
 * (The old code returned false in that case, which aborted any frame that
 * arrived in more than one burst and left the host waiting for an ACK.)
 */
static size_t s_read_some(uint8_t *out, size_t length, TickType_t wait)
{
    TickType_t start = xTaskGetTickCount();
    while (true) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        size_t n = s_count < length ? s_count : length;
        if (n > 0) {
            size_t first = RING_SIZE - s_tail;
            if (first > n) {
                first = n;
            }
            memcpy(out, s_ring + s_tail, first);
            if (n > first) {
                memcpy(out + first, s_ring, n - first);
            }
            s_tail = (s_tail + n) % RING_SIZE;
            s_count -= n;
            xSemaphoreGive(s_mutex);
            return n;
        }
        xSemaphoreGive(s_mutex);
        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= wait) {
            return 0;
        }
        xSemaphoreTake(s_data, wait - elapsed);
    }
}

static bool s_read_byte(uint8_t *out, TickType_t wait)
{
    return s_read_some(out, 1, wait) == 1;
}

static bool s_read_exact(uint8_t *out, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        size_t n = s_read_some(out + offset, length - offset, FRAME_GAP_TICKS);
        if (n == 0) {
            ESP_LOGW(TAG, "frame timed out");
            return false;
        }
        offset += n;
    }
    return true;
}

static void s_on_tap(uint32_t id, uint8_t kind, uint8_t chip)
{
    uint8_t payload[6];
    memcpy(payload, &id, 4);
    payload[4] = kind;
    payload[5] = chip;
    link_send(CLIP_TAP, payload, sizeof(payload));
}

static void s_on_menu(uint32_t id, uint8_t action)
{
    uint8_t payload[5];
    memcpy(payload, &id, 4);
    payload[4] = action;
    link_send(CLIP_MENU, payload, sizeof(payload));
}

static void s_on_page(uint8_t action)
{
    if (action == CLIP_PAGE_TOP) {
        return;
    }
    link_send(CLIP_PAGE_ACTION, &action, 1);
}

static void s_enter_download(void)
{
    vTaskDelay(pdMS_TO_TICKS(50));
    usb_serial_jtag_ll_phy_enable_external(false);
    usb_serial_jtag_ll_phy_enable_pad(true);
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart_noos();
}

static void s_handle(uint8_t type, uint8_t *payload, uint32_t length)
{
    s_host_seen = true;
    if (type == CLIP_CARD_PUT && length >= 10) {
        uint32_t id;
        uint16_t width;
        uint16_t height;
        memcpy(&id, payload, 4);
        memcpy(&width, payload + 5, 2);
        memcpy(&height, payload + 7, 2);
        uint8_t regions = payload[9];
        uint32_t region_bytes = (uint32_t)regions * 10;
        if (10u + region_bytes > length) {
            s_ack(CLIP_NACK);
            return;
        }
        const uint8_t *pixels = payload + 10 + region_bytes;
        uint32_t pixel_bytes = length - 10 - region_bytes;
        if (pixel_bytes != (uint32_t)width * height * 2) {
            s_ack(CLIP_NACK);
            return;
        }
        uint32_t dropped = 0;
        if (!ui_put_card(id, pixels, width, height, payload + 10, regions, &dropped)) {
            s_ack(CLIP_NACK);
            return;
        }
        if (dropped != 0) {
            link_send(CLIP_CARD_DROP, (const uint8_t *)&dropped, 4);
        }
        s_ack(CLIP_ACK);
        return;
    }
    if (type == CLIP_CARD_DEL && length == 4) {
        uint32_t id;
        memcpy(&id, payload, 4);
        ui_delete_card(id);
        s_ack(CLIP_ACK);
        return;
    }
    if (type == CLIP_LIST_ORDER && length >= 3) {
        uint16_t count;
        memcpy(&count, payload + 1, 2);
        if (3u + count * 4u != length || count > 80) {
            s_ack(CLIP_NACK);
            return;
        }
        uint32_t ids[80];
        for (uint16_t i = 0; i < count; i++) {
            memcpy(&ids[i], payload + 3 + i * 4, 4);
        }
        ui_set_order(payload[0], ids, count);
        s_ack(CLIP_ACK);
        return;
    }
    if (type == CLIP_ASSET_PUT && length >= 5) {
        uint16_t width;
        uint16_t height;
        memcpy(&width, payload + 1, 2);
        memcpy(&height, payload + 3, 2);
        if (5u + (uint32_t)width * height * 2u != length) {
            s_ack(CLIP_NACK);
            return;
        }
        ui_set_asset(payload[0], payload + 5, width, height);
        s_ack(CLIP_ACK);
        return;
    }
    if (type == CLIP_TOAST && length >= 6) {
        uint16_t duration;
        uint16_t width;
        uint16_t height;
        memcpy(&duration, payload, 2);
        memcpy(&width, payload + 2, 2);
        memcpy(&height, payload + 4, 2);
        if (6u + (uint32_t)width * height * 2u != length) {
            s_ack(CLIP_NACK);
            return;
        }
        ui_toast(payload + 6, width, height, duration);
        s_ack(CLIP_ACK);
        return;
    }
    if (type == CLIP_VERIFY_TAP && length == 6) {
        uint32_t id;
        memcpy(&id, payload, 4);
        s_ack(CLIP_ACK);
        s_on_tap(id, payload[4], payload[5]);
        return;
    }
    if (type == CLIP_ENTER_BOOT && length == 0) {
        s_ack(CLIP_ACK);
        s_enter_download();
        return;
    }
    s_ack(CLIP_NACK);
}

static void s_task(void *arg)
{
    (void)arg;
    uint8_t *payload = heap_caps_malloc(CLIP_MAX_PAYLOAD, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (payload == NULL) {
        ESP_LOGE(TAG, "payload buffer missing");
        vTaskDelete(NULL);
        return;
    }
    while (true) {
        if (s_dtr && !s_host_seen && xTaskGetTickCount() >= s_next_hello) {
            /* version, then esp_reset_reason() so the host can log crashes. */
            uint8_t hello[2] = {CLIP_PROTOCOL_VERSION, (uint8_t)esp_reset_reason()};
            link_send(CLIP_HELLO, hello, sizeof(hello));
            s_next_hello = xTaskGetTickCount() + pdMS_TO_TICKS(500);
            s_hello_pending = false;
        }
        uint8_t magic = 0;
        if (!s_read_byte(&magic, pdMS_TO_TICKS(100))) {
            continue;
        }
        if (magic != CLIP_MAGIC) {
            continue;
        }
        uint8_t header[5];
        if (!s_read_exact(header, sizeof(header))) {
            continue;
        }
        uint32_t length = 0;
        memcpy(&length, header + 1, 4);
        if (length > CLIP_MAX_PAYLOAD) {
            s_ack(CLIP_NACK);
            continue;
        }
        if (length > 0 && !s_read_exact(payload, length)) {
            continue;
        }
        uint32_t received_crc = 0;
        if (!s_read_exact((uint8_t *)&received_crc, 4)) {
            continue;
        }
        uint32_t crc = esp_crc32_le(0, header, 5);
        if (length > 0) {
            crc = esp_crc32_le(crc, payload, length);
        }
        if (crc != received_crc) {
            ESP_LOGW(TAG, "crc mismatch");
            s_ack(CLIP_NACK);
            continue;
        }
        s_handle(header[0], payload, length);
    }
}

void link_rx(const uint8_t *data, size_t length)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t i = 0; i < length; i++) {
        if (s_count == RING_SIZE) {
            s_tail = (s_tail + 1) % RING_SIZE;
            s_count--;
        }
        s_ring[s_head] = data[i];
        s_head = (s_head + 1) % RING_SIZE;
        s_count++;
    }
    xSemaphoreGive(s_mutex);
    xSemaphoreGive(s_data);
}

void link_set_dtr(bool asserted)
{
    bool rising = asserted && !s_dtr;
    s_dtr = asserted;
    if (rising) {
        /* A new host session: announce ourselves again. */
        s_host_seen = false;
        s_next_hello = 0;
        s_hello_pending = true;
        xSemaphoreGive(s_data);
    }
}

void link_start(void)
{
    s_ring = heap_caps_malloc(RING_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_mutex = xSemaphoreCreateMutex();
    s_tx_mutex = xSemaphoreCreateMutex();
    s_data = xSemaphoreCreateBinary();
    ui_set_handlers(s_on_tap, s_on_menu, s_on_page);
    xTaskCreate(s_task, "link", 8192, NULL, 5, NULL);
}
