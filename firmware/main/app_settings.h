#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint8_t brightness_percent;
    bool auto_dim_enabled;
    uint16_t auto_dim_seconds;
    bool return_home_on_idle;
} app_settings_t;

esp_err_t app_settings_init(void);
const app_settings_t *app_settings_get(void);
void app_settings_set_brightness(uint8_t percent);
void app_settings_set_auto_dim_enabled(bool enabled);
void app_settings_set_auto_dim(uint16_t seconds);
void app_settings_set_return_home(bool enabled);
