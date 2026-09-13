/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Derived from the panel driver shipped with the validated Waveshare demo.
 */
#include "esp_lcd_sh8601.h"

#include <stdlib.h>
#include <sys/cdefs.h>
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LCD_OPCODE_WRITE_CMD   0x02ULL
#define LCD_OPCODE_WRITE_COLOR 0x32ULL

static const char *TAG = "sh8601";

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    int x_gap;
    int y_gap;
    uint8_t fb_bits_per_pixel;
    uint8_t madctl_val;
    uint8_t colmod_val;
    const sh8601_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
    struct {
        unsigned int use_qspi_interface : 1;
        unsigned int reset_level : 1;
    } flags;
} sh8601_panel_t;

static esp_err_t tx_param(sh8601_panel_t *panel, int command,
                          const void *parameters, size_t size)
{
    if (panel->flags.use_qspi_interface) {
        command = (command & 0xff) << 8;
        command |= LCD_OPCODE_WRITE_CMD << 24;
    }
    return esp_lcd_panel_io_tx_param(panel->io, command, parameters, size);
}

static esp_err_t tx_color(sh8601_panel_t *panel, int command,
                          const void *pixels, size_t size)
{
    if (panel->flags.use_qspi_interface) {
        command = (command & 0xff) << 8;
        command |= LCD_OPCODE_WRITE_COLOR << 24;
    }
    return esp_lcd_panel_io_tx_color(panel->io, command, pixels, size);
}

static esp_err_t panel_del(esp_lcd_panel_t *base)
{
    sh8601_panel_t *panel = __containerof(base, sh8601_panel_t, base);
    if (panel->reset_gpio_num >= 0) gpio_reset_pin(panel->reset_gpio_num);
    free(panel);
    return ESP_OK;
}

static esp_err_t panel_reset(esp_lcd_panel_t *base)
{
    sh8601_panel_t *panel = __containerof(base, sh8601_panel_t, base);
    if (panel->reset_gpio_num >= 0) {
        gpio_set_level(panel->reset_gpio_num, panel->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(panel->reset_gpio_num, !panel->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(150));
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(tx_param(panel, LCD_CMD_SWRESET, NULL, 0), TAG,
                        "software reset");
    vTaskDelay(pdMS_TO_TICKS(80));
    return ESP_OK;
}

static const sh8601_lcd_init_cmd_t s_default_init[] = {
    {0x44, (uint8_t[]){0x01, 0xD1}, 2, 0},
    {0x35, (uint8_t[]){0x00}, 0, 0},
    {0x53, (uint8_t[]){0x20}, 1, 25},
};

static esp_err_t panel_init(esp_lcd_panel_t *base)
{
    sh8601_panel_t *panel = __containerof(base, sh8601_panel_t, base);
    ESP_RETURN_ON_ERROR(tx_param(panel, LCD_CMD_MADCTL,
                                 (uint8_t[]){panel->madctl_val}, 1),
                        TAG, "set MADCTL");
    ESP_RETURN_ON_ERROR(tx_param(panel, LCD_CMD_COLMOD,
                                 (uint8_t[]){panel->colmod_val}, 1),
                        TAG, "set COLMOD");

    const sh8601_lcd_init_cmd_t *commands = panel->init_cmds != NULL
        ? panel->init_cmds : s_default_init;
    const uint16_t count = panel->init_cmds != NULL
        ? panel->init_cmds_size
        : sizeof(s_default_init) / sizeof(s_default_init[0]);
    for (uint16_t i = 0; i < count; ++i) {
        ESP_RETURN_ON_ERROR(tx_param(panel, commands[i].cmd, commands[i].data,
                                     commands[i].data_bytes),
                            TAG, "send init command 0x%02x", commands[i].cmd);
        if (commands[i].delay_ms != 0) {
            vTaskDelay(pdMS_TO_TICKS(commands[i].delay_ms));
        }
    }
    return ESP_OK;
}

static esp_err_t panel_draw(esp_lcd_panel_t *base, int x_start, int y_start,
                            int x_end, int y_end, const void *pixels)
{
    sh8601_panel_t *panel = __containerof(base, sh8601_panel_t, base);
    ESP_RETURN_ON_FALSE(x_start < x_end && y_start < y_end,
                        ESP_ERR_INVALID_ARG, TAG, "invalid draw region");
    x_start += panel->x_gap;
    x_end += panel->x_gap;
    y_start += panel->y_gap;
    y_end += panel->y_gap;

    const uint8_t columns[] = {
        (uint8_t)(x_start >> 8), (uint8_t)x_start,
        (uint8_t)((x_end - 1) >> 8), (uint8_t)(x_end - 1),
    };
    const uint8_t rows[] = {
        (uint8_t)(y_start >> 8), (uint8_t)y_start,
        (uint8_t)((y_end - 1) >> 8), (uint8_t)(y_end - 1),
    };
    ESP_RETURN_ON_ERROR(tx_param(panel, LCD_CMD_CASET, columns, sizeof(columns)),
                        TAG, "set columns");
    ESP_RETURN_ON_ERROR(tx_param(panel, LCD_CMD_RASET, rows, sizeof(rows)),
                        TAG, "set rows");
    const size_t size = (x_end - x_start) * (y_end - y_start) *
                        panel->fb_bits_per_pixel / 8;
    return tx_color(panel, LCD_CMD_RAMWR, pixels, size);
}

static esp_err_t panel_invert(esp_lcd_panel_t *base, bool invert)
{
    sh8601_panel_t *panel = __containerof(base, sh8601_panel_t, base);
    return tx_param(panel, invert ? LCD_CMD_INVON : LCD_CMD_INVOFF, NULL, 0);
}

static esp_err_t panel_mirror(esp_lcd_panel_t *base, bool mirror_x,
                              bool mirror_y)
{
    sh8601_panel_t *panel = __containerof(base, sh8601_panel_t, base);
    if (mirror_y) return ESP_ERR_NOT_SUPPORTED;
    if (mirror_x) panel->madctl_val |= BIT(6);
    else panel->madctl_val &= ~BIT(6);
    return tx_param(panel, LCD_CMD_MADCTL, &panel->madctl_val, 1);
}

static esp_err_t panel_swap_xy(esp_lcd_panel_t *base, bool swap)
{
    (void)base;
    (void)swap;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t panel_set_gap(esp_lcd_panel_t *base, int x_gap, int y_gap)
{
    sh8601_panel_t *panel = __containerof(base, sh8601_panel_t, base);
    panel->x_gap = x_gap;
    panel->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_display(esp_lcd_panel_t *base, bool enabled)
{
    sh8601_panel_t *panel = __containerof(base, sh8601_panel_t, base);
    return tx_param(panel, enabled ? LCD_CMD_DISPON : LCD_CMD_DISPOFF, NULL, 0);
}

esp_err_t esp_lcd_new_panel_sh8601(const esp_lcd_panel_io_handle_t io,
                                   const esp_lcd_panel_dev_config_t *config,
                                   esp_lcd_panel_handle_t *result)
{
    ESP_RETURN_ON_FALSE(io != NULL && config != NULL && result != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    sh8601_panel_t *panel = calloc(1, sizeof(*panel));
    ESP_RETURN_ON_FALSE(panel != NULL, ESP_ERR_NO_MEM, TAG,
                        "allocate panel");

    esp_err_t error = ESP_OK;
    if (config->reset_gpio_num >= 0) {
        const gpio_config_t reset_config = {
            .pin_bit_mask = 1ULL << config->reset_gpio_num,
            .mode = GPIO_MODE_OUTPUT,
        };
        error = gpio_config(&reset_config);
        if (error != ESP_OK) goto fail;
    }

    if (config->rgb_ele_order == LCD_RGB_ELEMENT_ORDER_BGR) {
        panel->madctl_val |= LCD_CMD_BGR_BIT;
    } else if (config->rgb_ele_order != LCD_RGB_ELEMENT_ORDER_RGB) {
        error = ESP_ERR_NOT_SUPPORTED;
        goto fail;
    }
    if (config->bits_per_pixel == 16) {
        panel->colmod_val = 0x55;
        panel->fb_bits_per_pixel = 16;
    } else {
        error = ESP_ERR_NOT_SUPPORTED;
        goto fail;
    }

    panel->io = io;
    panel->reset_gpio_num = config->reset_gpio_num;
    panel->flags.reset_level = config->flags.reset_active_high;
    const sh8601_vendor_config_t *vendor = config->vendor_config;
    if (vendor != NULL) {
        panel->init_cmds = vendor->init_cmds;
        panel->init_cmds_size = vendor->init_cmds_size;
        panel->flags.use_qspi_interface = vendor->flags.use_qspi_interface;
    }
    panel->base.del = panel_del;
    panel->base.reset = panel_reset;
    panel->base.init = panel_init;
    panel->base.draw_bitmap = panel_draw;
    panel->base.invert_color = panel_invert;
    panel->base.mirror = panel_mirror;
    panel->base.swap_xy = panel_swap_xy;
    panel->base.set_gap = panel_set_gap;
    panel->base.disp_on_off = panel_display;
    *result = &panel->base;
    return ESP_OK;

fail:
    if (config->reset_gpio_num >= 0) gpio_reset_pin(config->reset_gpio_num);
    free(panel);
    return error;
}
