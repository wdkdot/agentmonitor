idf_component_register(
    SRCS
        "app_main.c"
        "usb_hid.c"
        "usage_ui.c"
        "service_logos.c"
        "app_settings.c"
    INCLUDE_DIRS "."
    REQUIRES aiud_protocol board_v2 esp_timer nvs_flash
    PRIV_REQUIRES esp_tinyusb lvgl
)
