#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "sdkconfig.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"

#include "board.h"
#include "link.h"
#include "ui.h"
#include "hub.h"
#include "screen.h"
#include "guard.h"

static const char *TAG = "clipboard";

#define BOOT_GPIO GPIO_NUM_0
#define LED_GPIO 42

static led_strip_handle_t s_led;

static void led_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    if (s_led == NULL) {
        return;
    }
    led_strip_set_pixel(s_led, 0, red, green, blue);
    led_strip_refresh(s_led);
}

static void led_init(void)
{
    led_strip_config_t strip = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    if (led_strip_new_rmt_device(&strip, &rmt, &s_led) != ESP_OK) {
        s_led = NULL;
    }
}

static void button_task(void *arg)
{
    bool down = false;
    TickType_t pressed_at = 0;
    (void)arg;
    while (true) {
        bool held = gpio_get_level(BOOT_GPIO) == 0;
        TickType_t now = xTaskGetTickCount();
        if (held && !down) {
            down = true;
            pressed_at = now;
        } else if (!held && down) {
            if ((now - pressed_at) >= pdMS_TO_TICKS(40)) {
                ui_scroll_top();
            }
            down = false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void tinyusb_cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    uint8_t buffer[CONFIG_TINYUSB_CDC_RX_BUFSIZE];
    size_t received = 0;
    (void)event;
    if (tinyusb_cdcacm_read(itf, buffer, sizeof(buffer), &received) == ESP_OK && received > 0) {
        link_rx(buffer, received);
    }
}

static void tinyusb_cdc_line_state_changed_callback(int itf, cdcacm_event_t *event)
{
    (void)itf;
    bool dtr = event->line_state_changed_data.dtr;
    link_set_dtr(dtr);
    led_rgb(dtr ? 0 : 40, dtr ? 48 : 0, 0);
}

static void usb_init(void)
{
    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = NULL,
        .string_descriptor = NULL,
        .external_phy = false,
#if (TUD_OPT_HIGH_SPEED)
        .fs_configuration_descriptor = NULL,
        .hs_configuration_descriptor = NULL,
        .qualifier_descriptor = NULL,
#else
        .configuration_descriptor = NULL,
#endif
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    tinyusb_config_cdcacm_t acm_cfg = {
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
        .rx_unread_buf_sz = 512,
        .callback_rx = &tinyusb_cdc_rx_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL,
    };
    ESP_ERROR_CHECK(tusb_cdc_acm_init(&acm_cfg));
    ESP_ERROR_CHECK(tinyusb_cdcacm_register_callback(
        TINYUSB_CDC_ACM_0, CDC_EVENT_LINE_STATE_CHANGED, &tinyusb_cdc_line_state_changed_callback));
}

void app_main(void)
{
    led_init();
    led_rgb(20, 20, 20);
    ESP_ERROR_CHECK(board_display_start());
    ui_init();
    hub_start(); /* initialises NVS */
    screen_start();
    guard_start(board_display()); /* restarts the board if the UI task stalls */
    ui_set_screen_handlers(screen_timeout_index, screen_cycle_timeout);
    link_start();
    usb_init();
    gpio_config_t boot = {
        .pin_bit_mask = 1ULL << BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&boot));
    xTaskCreate(button_task, "button", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "clipboard ready");
}
