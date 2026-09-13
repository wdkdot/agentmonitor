#include "ft3168.h"

#include "board_config.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "ft3168";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_touch;

esp_err_t ft3168_init(void)
{
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = BOARD_TOUCH_PORT,
        .sda_io_num = BOARD_TOUCH_PIN_SDA,
        .scl_io_num = BOARD_TOUCH_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &s_bus), TAG,
                        "create I2C bus");

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_TOUCH_ADDRESS,
        .scl_speed_hz = 300000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &device_config, &s_touch),
                        TAG, "add touch device");

    const uint8_t normal_mode[] = {0x00, 0x00};
    return i2c_master_transmit(s_touch, normal_mode, sizeof(normal_mode), 20);
}

static esp_err_t read_register(uint8_t reg, uint8_t *data, size_t length)
{
    return i2c_master_transmit_receive(s_touch, &reg, 1, data, length, 20);
}

bool ft3168_read(uint16_t *x, uint16_t *y)
{
    uint8_t touches = 0;
    if (s_touch == NULL || read_register(0x02, &touches, 1) != ESP_OK ||
        (touches & 0x0f) != 1) {
        return false;
    }

    uint8_t point[4] = {0};
    if (read_register(0x03, point, sizeof(point)) != ESP_OK) {
        return false;
    }

    uint16_t raw_x = ((uint16_t)(point[0] & 0x0f) << 8) | point[1];
    uint16_t raw_y = ((uint16_t)(point[2] & 0x0f) << 8) | point[3];
    *x = raw_x < BOARD_LCD_NATIVE_WIDTH ? raw_x : BOARD_LCD_NATIVE_WIDTH - 1;
    *y = raw_y < BOARD_LCD_NATIVE_HEIGHT ? raw_y : BOARD_LCD_NATIVE_HEIGHT - 1;
    return true;
}
