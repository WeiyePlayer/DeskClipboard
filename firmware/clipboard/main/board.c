#include "board.h"
#include "screen.h"
#include "guard.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "board";

#define LCD_HOST SPI3_HOST
#define LCD_H 240
#define LCD_V 320

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_touch_dev;
static lv_display_t *s_disp;

lv_display_t *board_display(void) { return s_disp; }

/* Polled FT6336 read with a bounded I2C timeout. esp_lcd's I2C panel IO (and
 * so esp_lvgl_port's touch indev) waits forever (-1) for a transfer; a wedged
 * touch controller or bus then blocks the LVGL task while it holds the LVGL
 * lock, which froze the whole board (UI, hub worker, BLE and USB requests). */
#define TOUCH_ADDR 0x38
#define TOUCH_TIMEOUT_MS 50
static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    static lv_point_t last;
    static uint8_t failures;
    uint8_t reg = 0x02, buf[5]; /* TD_STATUS, P1_XH, P1_XL, P1_YH, P1_YL */
    guard_stage(GUARD_TOUCH);
    esp_err_t err = s_touch_dev ? i2c_master_transmit_receive(s_touch_dev, &reg, 1, buf, sizeof(buf), TOUCH_TIMEOUT_MS)
                                : ESP_ERR_INVALID_STATE;
    guard_stage(GUARD_IDLE);
    data->point = last;
    data->state = LV_INDEV_STATE_RELEASED;
    if (err != ESP_OK) {
        if (++failures >= 5) { /* ~5 reads: recover the bus, keep the UI running */
            failures = 0;
            ESP_LOGW(TAG, "touch read failed (%s), resetting I2C bus", esp_err_to_name(err));
            i2c_master_bus_reset(s_i2c);
        }
        return;
    }
    failures = 0;
    uint8_t points = buf[0] & 0x0f;
    uint8_t event = buf[1] >> 6; /* 0 down, 1 up, 2 contact */
    if (points == 0 || points > 2 || event == 1)
        return;
    int32_t x = ((buf[1] & 0x0f) << 8) | buf[2];
    int32_t y = ((buf[3] & 0x0f) << 8) | buf[4];
    if (x >= LCD_H) x = LCD_H - 1;
    if (y >= LCD_V) y = LCD_V - 1;
    last.x = x;
    last.y = y;
    data->point = last;
    data->state = LV_INDEV_STATE_PRESSED;
}

esp_err_t board_display_start(void)
{
    screen_backlight_init(); /* LEDC PWM on GPIO45 (active high), full brightness */

    const spi_bus_config_t bus = {
        .sclk_io_num = GPIO_NUM_12,
        .mosi_io_num = GPIO_NUM_11,
        .miso_io_num = GPIO_NUM_13,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_H * 50 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = GPIO_NUM_46,
        .cs_gpio_num = GPIO_NUM_10,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io));

    esp_lcd_panel_handle_t panel = NULL;
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    screen_set_panel(panel);

    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_stack = 24576;
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));
    const lvgl_port_display_cfg_t display = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = LCD_H * 50,
        .double_buffer = true,
        .hres = LCD_H,
        .vres = LCD_V,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = true,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram = true,
            .swap_bytes = true,
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&display);
    if (disp == NULL) {
        return ESP_FAIL;
    }
    s_disp = disp;

    const i2c_master_bus_config_t i2c_bus = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = GPIO_NUM_16,
        .scl_io_num = GPIO_NUM_15,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus, &s_i2c));

    esp_lcd_panel_io_handle_t touch_io = NULL;
    esp_lcd_panel_io_i2c_config_t touch_io_config = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    touch_io_config.scl_speed_hz = 400000;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(s_i2c, &touch_io_config, &touch_io));
    const esp_lcd_touch_config_t touch_config = {
        .x_max = LCD_H,
        .y_max = LCD_V,
        .rst_gpio_num = GPIO_NUM_18,
        .int_gpio_num = GPIO_NUM_NC, /* polled by touch_read() */
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    esp_lcd_touch_handle_t touch = NULL;
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_ft5x06(touch_io, &touch_config, &touch));
    (void)touch; /* reset + configuration only; reads go through touch_read() */
    const i2c_device_config_t touch_dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TOUCH_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(s_i2c, &touch_dev, &s_touch_dev) != ESP_OK) {
        s_touch_dev = NULL;
        ESP_LOGW(TAG, "touch device failed");
    }
    lvgl_port_lock(0);
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read);
    lv_indev_set_display(indev, disp);
    lvgl_port_unlock();
    ESP_LOGI(TAG, "display ready");
    return ESP_OK;
}
