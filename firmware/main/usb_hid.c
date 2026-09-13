#include "usb_hid.h"

#include <stdatomic.h>
#include <string.h>
#include "aiud_protocol.h"
#include "class/hid/hid_device.h"
#include "esp_log.h"
#include "esp_system.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"

static const char *TAG = "usage_usb";
static QueueHandle_t s_snapshot_queue;

#define USB_DESCRIPTOR_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN)
#define BOOTLOADER_COMMAND "AIUD-BOOTLOADER"
#define CAPTURE_COMMAND "AIUD-SNAPSHOT"

static atomic_bool s_capture_requested;

static const uint8_t s_hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_GENERIC_INOUT(AIUD_PACKET_SIZE)
};

static const char *s_string_descriptor[] = {
    (char[]){0x09, 0x04},
    "AI Usage Display",
    "AI Usage Monitor",
    "AIUD-DEV",
    "Usage data",
};

static const uint8_t s_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, USB_DESCRIPTOR_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_INOUT_DESCRIPTOR(0, 4, HID_ITF_PROTOCOL_NONE,
                             sizeof(s_hid_report_descriptor),
                             0x01, 0x81, AIUD_PACKET_SIZE, 5),
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t requested_length)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)requested_length;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t size)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    if (size >= sizeof(BOOTLOADER_COMMAND) - 1 &&
        memcmp(buffer, BOOTLOADER_COMMAND,
               sizeof(BOOTLOADER_COMMAND) - 1) == 0) {
        ESP_LOGW(TAG, "restarting into ROM download mode");
        REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
        esp_restart();
        return;
    }
    if (size >= sizeof(CAPTURE_COMMAND) - 1 &&
        memcmp(buffer, CAPTURE_COMMAND,
               sizeof(CAPTURE_COMMAND) - 1) == 0) {
        atomic_store(&s_capture_requested, true);
        return;
    }
    aiud_snapshot_t snapshot;
    const aiud_parse_result_t result = aiud_parse_packet(buffer, size, &snapshot);
    if (result != AIUD_PARSE_OK) {
        ESP_LOGW(TAG, "ignored HID report: %s", aiud_parse_result_name(result));
        return;
    }
    if (s_snapshot_queue != NULL) {
        xQueueOverwrite(s_snapshot_queue, &snapshot);
    }
}

bool usage_usb_capture_requested(void)
{
    return atomic_exchange(&s_capture_requested, false);
}

static esp_err_t send_input_report(const uint8_t report[AIUD_PACKET_SIZE])
{
    for (unsigned wait_ms = 0; wait_ms < 3000; ++wait_ms) {
        if (!tud_mounted()) return ESP_ERR_INVALID_STATE;
        if (tud_hid_ready()) {
            return tud_hid_report(0, report, AIUD_PACKET_SIZE)
                       ? ESP_OK : ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t usage_usb_send_capture(const void *pixels, size_t size,
                                 uint16_t width, uint16_t height)
{
    if (pixels == NULL || size == 0 || size > UINT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t report[AIUD_PACKET_SIZE] = {0};
    memcpy(report, "AIUDSNAP", 8);
    report[8] = (uint8_t)width;
    report[9] = (uint8_t)(width >> 8);
    report[10] = (uint8_t)height;
    report[11] = (uint8_t)(height >> 8);
    const uint32_t data_size = (uint32_t)size;
    for (unsigned i = 0; i < 4; ++i) {
        report[12 + i] = (uint8_t)(data_size >> (i * 8));
    }
    report[16] = 1; /* RGB565, big-endian bytes (LV_COLOR_16_SWAP). */
    esp_err_t error = send_input_report(report);
    if (error != ESP_OK) return error;

    const uint8_t *source = pixels;
    for (size_t offset = 0; offset < size; offset += sizeof(report)) {
        const size_t remaining = size - offset;
        const size_t chunk = remaining < sizeof(report) ? remaining : sizeof(report);
        memset(report, 0, sizeof(report));
        memcpy(report, source + offset, chunk);
        error = send_input_report(report);
        if (error != ESP_OK) return error;
    }
    ESP_LOGI(TAG, "screen capture sent: %ux%u, %u bytes",
             width, height, (unsigned)size);
    return ESP_OK;
}

esp_err_t usage_usb_init(QueueHandle_t snapshot_queue)
{
    if (snapshot_queue == NULL) return ESP_ERR_INVALID_ARG;
    s_snapshot_queue = snapshot_queue;

    tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG();
    config.descriptor.device = NULL;
    config.descriptor.full_speed_config = s_configuration_descriptor;
    config.descriptor.string = s_string_descriptor;
    config.descriptor.string_count = sizeof(s_string_descriptor) /
                                     sizeof(s_string_descriptor[0]);
#if TUD_OPT_HIGH_SPEED
    config.descriptor.high_speed_config = s_configuration_descriptor;
#endif
    esp_err_t error = tinyusb_driver_install(&config);
    if (error == ESP_OK) ESP_LOGI(TAG, "vendor HID ready");
    return error;
}
