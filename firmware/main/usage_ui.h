#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

void usage_ui_init(QueueHandle_t snapshot_queue);
