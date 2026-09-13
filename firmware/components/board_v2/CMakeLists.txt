idf_component_register(
    SRCS
        "display_bsp.c"
        "esp_lcd_sh8601.c"
        "ft3168.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd esp_timer esp_driver_gpio esp_driver_i2c esp_driver_spi lvgl
)
