#pragma once

// Waveshare ESP32-S3-Touch-AMOLED-1.64 V2, verified by the graphics demo.
#define BOARD_LCD_NATIVE_WIDTH  280
#define BOARD_LCD_NATIVE_HEIGHT 456
#define BOARD_LCD_LOGICAL_WIDTH 456
#define BOARD_LCD_LOGICAL_HEIGHT 280

#define BOARD_LCD_HOST          SPI2_HOST
#define BOARD_LCD_PIN_CS        46
#define BOARD_LCD_PIN_PCLK      10
#define BOARD_LCD_PIN_DATA0     11
#define BOARD_LCD_PIN_DATA1     12
#define BOARD_LCD_PIN_DATA2     13
#define BOARD_LCD_PIN_DATA3     14
#define BOARD_LCD_PIN_RST       21

#define BOARD_TOUCH_PORT        I2C_NUM_0
#define BOARD_TOUCH_ADDRESS     0x38
#define BOARD_TOUCH_PIN_SCL     48
#define BOARD_TOUCH_PIN_SDA     47

// Landscape rendering is rotated into a dedicated native-orientation DMA
// buffer. 24 logical lines keep each buffer close to the validated demo's
// 280 x 48-pixel allocation while leaving enough internal DMA memory.
#define BOARD_LVGL_BUFFER_LINES 24
#define BOARD_LVGL_TICK_MS      2
