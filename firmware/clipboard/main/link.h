#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void link_start(void);
void link_rx(const uint8_t *data, size_t length);
void link_set_dtr(bool asserted);
void link_send(uint8_t type, const uint8_t *payload, uint32_t length);
