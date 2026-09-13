#include "display_bsp.h"

#include <assert.h>
#include "board_config.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_sh8601.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ft3168.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "display";
static SemaphoreHandle_t s_lvgl_mutex;
static esp_lcd_panel_io_handle_t s_panel_io;
static lv_color_t *s_rotation_buffer;
static size_t s_rotation_buffer_pixels;
static int64_t s_last_touch_us;

static const sh8601_lcd_init_cmd_t s_init_commands[] = {
    {0x11, (uint8_t[]){0x00}, 0, 80},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 1},
    {0x63, (uint8_t[]){0xFF}, 1, 1},
    {0x51, (uint8_t[]){0x00}, 1, 1},
    {0x29, (uint8_t[]){0x00}, 0, 10},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
};

static bool flush_complete(esp_lcd_panel_io_handle_t panel_io,
                           esp_lcd_panel_io_event_data_t *event,
                           void *context)
{
    (void)panel_io;
    (void)event;
    lv_disp_flush_ready((lv_disp_drv_t *)context);
    return false;
}

static void flush_display(lv_disp_drv_t *driver, const lv_area_t *area,
                          lv_color_t *pixels)
{
    esp_lcd_panel_handle_t panel = driver->user_data;
    const int source_width = lv_area_get_width(area);
    const int source_height = lv_area_get_height(area);
    const size_t pixel_count = (size_t)source_width * source_height;
    assert(s_rotation_buffer != NULL);
    assert(pixel_count <= s_rotation_buffer_pixels);

    // Rotate the logical 456 x 280 landscape area into the panel's native
    // 280 x 456 memory order. Keeping this buffer DMA-capable avoids LVGL's
    // transient software-rotation buffer on the asynchronous QSPI path.
    for (int source_y = 0; source_y < source_height; ++source_y) {
        for (int source_x = 0; source_x < source_width; ++source_x) {
            const int destination_y = source_x;
            const int destination_x = source_height - source_y - 1;
            s_rotation_buffer[destination_y * source_height + destination_x] =
                pixels[source_y * source_width + source_x];
        }
    }

    // The visible 280-column window starts at column 0x14 in SH8601 GRAM.
    // Keep this in the flush callback, matching the validated Waveshare demo.
    const int x_start = BOARD_LCD_NATIVE_WIDTH - area->y2 - 1 + 0x14;
    const int x_end = BOARD_LCD_NATIVE_WIDTH - area->y1 + 0x14;
    const int y_start = area->x1;
    const int y_end = area->x2 + 1;
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, x_start, y_start,
                                               x_end, y_end,
                                               s_rotation_buffer));
}

static void align_flush_area(lv_disp_drv_t *driver, lv_area_t *area)
{
    (void)driver;
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
}

static void read_touch(lv_indev_drv_t *driver, lv_indev_data_t *data)
{
    (void)driver;
    uint16_t x;
    uint16_t y;
    if (ft3168_read(&x, &y)) {
        s_last_touch_us = esp_timer_get_time();
        data->point.x = y;
        data->point.y = BOARD_LCD_NATIVE_WIDTH - x - 1;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

int64_t board_display_last_touch_us(void)
{
    return s_last_touch_us;
}

static void tick_lvgl(void *context)
{
    (void)context;
    lv_tick_inc(BOARD_LVGL_TICK_MS);
}

bool board_display_lock(int timeout_ms)
{
    if (s_lvgl_mutex == NULL) {
        return false;
    }
    TickType_t timeout = timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(s_lvgl_mutex, timeout) == pdTRUE;
}

void board_display_unlock(void)
{
    assert(s_lvgl_mutex != NULL);
    xSemaphoreGive(s_lvgl_mutex);
}

static void lvgl_task(void *context)
{
    (void)context;
    for (;;) {
        uint32_t delay_ms = 20;
        if (board_display_lock(-1)) {
            delay_ms = lv_timer_handler();
            board_display_unlock();
        }
        if (delay_ms < 1) delay_ms = 1;
        if (delay_ms > 100) delay_ms = 100;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

esp_err_t board_display_set_brightness(uint8_t percent)
{
    if (s_panel_io == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (percent > 100) percent = 100;
    const uint8_t level = (uint8_t)((percent * 255U) / 100U);
    uint32_t command = ((uint32_t)0x02 << 24) | ((uint32_t)0x51 << 8);
    return esp_lcd_panel_io_tx_param(s_panel_io, command, &level, 1);
}

esp_err_t board_display_init(void)
{
    ESP_RETURN_ON_ERROR(ft3168_init(), TAG, "initialize touch");

    const spi_bus_config_t bus_config = SH8601_PANEL_BUS_QSPI_CONFIG(
        BOARD_LCD_PIN_PCLK, BOARD_LCD_PIN_DATA0, BOARD_LCD_PIN_DATA1,
        BOARD_LCD_PIN_DATA2, BOARD_LCD_PIN_DATA3,
        BOARD_LCD_NATIVE_WIDTH * BOARD_LCD_NATIVE_HEIGHT * sizeof(lv_color_t));
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BOARD_LCD_HOST, &bus_config,
                                           SPI_DMA_CH_AUTO),
                        TAG, "initialize LCD bus");

    static lv_disp_drv_t display_driver;
    const esp_lcd_panel_io_spi_config_t io_config =
        SH8601_PANEL_IO_QSPI_CONFIG(BOARD_LCD_PIN_CS, flush_complete,
                                    &display_driver);
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_HOST,
                                 &io_config, &s_panel_io),
        TAG, "create panel IO");

    sh8601_vendor_config_t vendor_config = {
        .init_cmds = s_init_commands,
        .init_cmds_size = sizeof(s_init_commands) / sizeof(s_init_commands[0]),
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BOARD_LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_sh8601(s_panel_io, &panel_config,
                                                 &panel),
                        TAG, "create SH8601 panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "reset panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "initialize panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG,
                        "enable panel");

    lv_init();
    const size_t pixel_count =
        BOARD_LCD_LOGICAL_WIDTH * BOARD_LVGL_BUFFER_LINES;
    lv_color_t *buffer_a = heap_caps_malloc(pixel_count * sizeof(lv_color_t),
                                             MALLOC_CAP_DMA);
    lv_color_t *buffer_b = heap_caps_malloc(pixel_count * sizeof(lv_color_t),
                                             MALLOC_CAP_DMA);
    s_rotation_buffer = heap_caps_malloc(pixel_count * sizeof(lv_color_t),
                                          MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_rotation_buffer_pixels = pixel_count;
    ESP_RETURN_ON_FALSE(buffer_a != NULL && buffer_b != NULL &&
                        s_rotation_buffer != NULL, ESP_ERR_NO_MEM,
                        TAG, "allocate LVGL DMA buffers");

    static lv_disp_draw_buf_t draw_buffer;
    lv_disp_draw_buf_init(&draw_buffer, buffer_a, buffer_b, pixel_count);
    lv_disp_drv_init(&display_driver);
    display_driver.hor_res = BOARD_LCD_LOGICAL_WIDTH;
    display_driver.ver_res = BOARD_LCD_LOGICAL_HEIGHT;
    display_driver.flush_cb = flush_display;
    display_driver.rounder_cb = align_flush_area;
    display_driver.draw_buf = &draw_buffer;
    display_driver.user_data = panel;
    lv_disp_t *display = lv_disp_drv_register(&display_driver);
    ESP_RETURN_ON_FALSE(display != NULL, ESP_FAIL, TAG, "register display");

    static lv_indev_drv_t input_driver;
    lv_indev_drv_init(&input_driver);
    input_driver.type = LV_INDEV_TYPE_POINTER;
    input_driver.disp = display;
    input_driver.read_cb = read_touch;
    ESP_RETURN_ON_FALSE(lv_indev_drv_register(&input_driver) != NULL, ESP_FAIL,
                        TAG, "register touch");

    const esp_timer_create_args_t tick_args = {
        .callback = tick_lvgl,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer;
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &tick_timer), TAG,
                        "create LVGL tick");
    ESP_RETURN_ON_ERROR(
        esp_timer_start_periodic(tick_timer, BOARD_LVGL_TICK_MS * 1000), TAG,
        "start LVGL tick");

    s_lvgl_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lvgl_mutex != NULL, ESP_ERR_NO_MEM, TAG,
                        "create LVGL mutex");
    BaseType_t created = xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL,
                                                 2, NULL, 1);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "create LVGL task");
    return board_display_set_brightness(65);
}
