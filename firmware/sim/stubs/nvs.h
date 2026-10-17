#pragma once
/* Host stub: one in-memory u8 per key, enough for screen.c. */
#include <stdint.h>
#include <string.h>
typedef int esp_err_t;
typedef int nvs_handle_t;
#ifndef ESP_OK
#define ESP_OK 0
#endif
#define NVS_READONLY 0
#define NVS_READWRITE 1
extern int sim_nvs_value; /* -1 = unset */
static inline esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h) { (void)mode; *h = 1; return strcmp(ns, "screen") ? -1 : ESP_OK; }
static inline esp_err_t nvs_get_u8(nvs_handle_t h, const char *k, uint8_t *v) { (void)h; (void)k; if (sim_nvs_value < 0) return -1; *v = (uint8_t)sim_nvs_value; return ESP_OK; }
static inline esp_err_t nvs_set_u8(nvs_handle_t h, const char *k, uint8_t v) { (void)h; (void)k; sim_nvs_value = v; return ESP_OK; }
static inline esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
static inline void nvs_close(nvs_handle_t h) { (void)h; }
