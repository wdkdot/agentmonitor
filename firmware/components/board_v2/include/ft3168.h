#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t ft3168_init(void);
bool ft3168_read(uint16_t *x, uint16_t *y);
