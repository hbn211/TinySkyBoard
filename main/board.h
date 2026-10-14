#pragma once

#include <stdint.h>
#include "lvgl.h"

// Waveshare ESP32-C6-LCD-1.47 (ST7789, 172x320)
#define LCD_HOST        SPI2_HOST
#define LCD_PIN_MOSI    6
#define LCD_PIN_SCLK    7
#define LCD_PIN_CS      14
#define LCD_PIN_DC      15
#define LCD_PIN_RST     21
#define LCD_PIN_BL      22
#define LCD_H_RES       172
#define LCD_V_RES       320
#define LCD_X_GAP       34   // the 172 px panel sits centered in the ST7789's 240 px RAM

#define RGB_LED_PIN     8

// USB position seen from the front with the text upright -> LVGL rotation.
// Order: USB at the bottom, left, top, right. Rotation 0 is the panel's native one (USB at the bottom).
// If "left" and "right" come out swapped on the board, swap the 1 and the 3.
static const uint8_t BOARD_USB_TO_ROT[4] = {0, 1, 2, 3};
#define BOOT_BTN_PIN    9

lv_display_t *board_display_init(void);
void board_display_set_rotation(lv_display_rotation_t rot);  // call with the LVGL lock held
void board_backlight_set(uint8_t percent);
void board_led_init(void);
void board_led_set(uint8_t r, uint8_t g, uint8_t b);
