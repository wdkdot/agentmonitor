#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t board_display_init(void);
bool board_display_lock(int timeout_ms);
void board_display_unlock(void);
esp_err_t board_display_set_brightness(uint8_t percent);
int64_t board_display_last_touch_us(void);

#ifdef __cplusplus
}
#endif
