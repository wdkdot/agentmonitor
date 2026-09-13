#include "app_settings.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";
static const char *NAMESPACE = "display";
static app_settings_t s_settings = {
    .brightness_percent = 65,
    .auto_dim_enabled = false,
    .auto_dim_seconds = 300,
    .return_home_on_idle = true,
};

esp_err_t app_settings_init(void)
{
    esp_err_t error = nvs_flash_init();
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "NVS unavailable, using defaults: %s",
                 esp_err_to_name(error));
        return error;
    }

    nvs_handle_t handle;
    error = nvs_open(NAMESPACE, NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (error != ESP_OK) return error;

    uint8_t brightness;
    uint8_t dim_enabled;
    uint16_t dim_seconds;
    uint8_t return_home;
    if (nvs_get_u8(handle, "brightness", &brightness) == ESP_OK &&
        brightness >= 10 && brightness <= 100) {
        s_settings.brightness_percent = brightness;
    }
    if (nvs_get_u16(handle, "dim_seconds", &dim_seconds) == ESP_OK &&
        (dim_seconds == 300 || dim_seconds == 1800 || dim_seconds == 3600 ||
         dim_seconds == 10800)) {
        s_settings.auto_dim_seconds = dim_seconds;
    }
    if (nvs_get_u8(handle, "dim_enabled", &dim_enabled) == ESP_OK) {
        s_settings.auto_dim_enabled = dim_enabled != 0;
    }
    if (nvs_get_u8(handle, "return_home", &return_home) == ESP_OK) {
        s_settings.return_home_on_idle = return_home != 0;
    }
    nvs_close(handle);
    return ESP_OK;
}

const app_settings_t *app_settings_get(void)
{
    return &s_settings;
}

static void save_u8(const char *key, uint8_t value)
{
    nvs_handle_t handle;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_set_u8(handle, key, value) == ESP_OK) nvs_commit(handle);
    nvs_close(handle);
}

void app_settings_set_brightness(uint8_t percent)
{
    if (percent < 10) percent = 10;
    if (percent > 100) percent = 100;
    s_settings.brightness_percent = percent;
    save_u8("brightness", percent);
}

void app_settings_set_auto_dim_enabled(bool enabled)
{
    s_settings.auto_dim_enabled = enabled;
    save_u8("dim_enabled", enabled ? 1 : 0);
}

void app_settings_set_auto_dim(uint16_t seconds)
{
    if (seconds != 300 && seconds != 1800 && seconds != 3600 &&
        seconds != 10800) {
        seconds = 300;
    }
    s_settings.auto_dim_seconds = seconds;
    nvs_handle_t handle;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_set_u16(handle, "dim_seconds", seconds) == ESP_OK) nvs_commit(handle);
    nvs_close(handle);
}

void app_settings_set_return_home(bool enabled)
{
    s_settings.return_home_on_idle = enabled;
    save_u8("return_home", enabled ? 1 : 0);
}
