#include "link.h"

#include <string.h>

#include "esp_crc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
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

void link_send(uint8_t type, const uint8_t *payload, uint32_t length)
{
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
}

static void s_ack(uint8_t type)
{
    link_send(type, NULL, 0);
}

static bool s_read_byte(uint8_t *out, TickType_t wait)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_count == 0) {
        xSemaphoreGive(s_mutex);
        if (xSemaphoreTake(s_data, wait) != pdTRUE) {
            return false;
        }
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
    if (s_count == 0) {
        xSemaphoreGive(s_mutex);
        return false;
    }
    *out = s_ring[s_tail];
    s_tail = (s_tail + 1) % RING_SIZE;
    s_count--;
    xSemaphoreGive(s_mutex);
    return true;
}

static bool s_read_exact(uint8_t *out, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        if (!s_read_byte(&out[i], portMAX_DELAY)) {
            return false;
        }
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
            uint8_t version = CLIP_PROTOCOL_VERSION;
            link_send(CLIP_HELLO, &version, 1);
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
        s_hello_pending = true;
        xSemaphoreGive(s_data);
    }
}

void link_start(void)
{
    s_ring = heap_caps_malloc(RING_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_mutex = xSemaphoreCreateMutex();
    s_data = xSemaphoreCreateBinary();
    ui_set_handlers(s_on_tap, s_on_menu, s_on_page);
    xTaskCreate(s_task, "link", 8192, NULL, 5, NULL);
}
