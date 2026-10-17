#pragma once
/* Host stub: records the backlight duty for sim_screen.c. */
#include <stdint.h>
typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#endif
typedef enum { LEDC_LOW_SPEED_MODE } ledc_mode_t;
typedef enum { LEDC_CHANNEL_0 } ledc_channel_t;
typedef enum { LEDC_TIMER_0 } ledc_timer_t;
typedef enum { LEDC_TIMER_10_BIT = 10 } ledc_timer_bit_t;
typedef enum { LEDC_AUTO_CLK } ledc_clk_cfg_t;
typedef struct { ledc_mode_t speed_mode; ledc_timer_bit_t duty_resolution; ledc_timer_t timer_num; uint32_t freq_hz; ledc_clk_cfg_t clk_cfg; } ledc_timer_config_t;
typedef struct { int gpio_num; ledc_mode_t speed_mode; ledc_channel_t channel; ledc_timer_t timer_sel; uint32_t duty; int hpoint; } ledc_channel_config_t;
extern uint32_t sim_backlight;
static inline esp_err_t ledc_timer_config(const ledc_timer_config_t *c) { (void)c; return ESP_OK; }
static inline esp_err_t ledc_channel_config(const ledc_channel_config_t *c) { sim_backlight = c->duty; return ESP_OK; }
static inline esp_err_t ledc_set_duty(ledc_mode_t m, ledc_channel_t c, uint32_t d) { (void)m; (void)c; sim_backlight = d; return ESP_OK; }
static inline esp_err_t ledc_update_duty(ledc_mode_t m, ledc_channel_t c) { (void)m; (void)c; return ESP_OK; }
