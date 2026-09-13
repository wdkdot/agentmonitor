#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

esp_err_t usage_usb_init(QueueHandle_t snapshot_queue);
bool usage_usb_capture_requested(void);
esp_err_t usage_usb_send_capture(const void *pixels, size_t size,
                                 uint16_t width, uint16_t height);
