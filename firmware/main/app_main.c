#include "aiud_protocol.h"
#include "app_settings.h"
#include "display_bsp.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "usage_ui.h"
#include "usb_hid.h"

static const char *TAG = "app";

void app_main(void)
{
    QueueHandle_t snapshots = xQueueCreate(1, sizeof(aiud_snapshot_t));
    ESP_ERROR_CHECK_WITHOUT_ABORT(snapshots != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    if (snapshots == NULL) return;

    ESP_ERROR_CHECK_WITHOUT_ABORT(app_settings_init());
    ESP_ERROR_CHECK(board_display_init());
    ESP_ERROR_CHECK_WITHOUT_ABORT(board_display_set_brightness(
        app_settings_get()->brightness_percent));
    if (board_display_lock(-1)) {
        usage_ui_init(snapshots);
        board_display_unlock();
    }

    ESP_ERROR_CHECK(usage_usb_init(snapshots));
    ESP_LOGI(TAG, "AI Usage Monitor ready");
}
