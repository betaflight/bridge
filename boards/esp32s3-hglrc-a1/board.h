#pragma once

#include "driver/gpio.h"

#define BOARD_NAME              "esp32s3-hglrc-a1"

// Onboard WS2812 NeoPixel — indicates FC/OTG-USB comms.
#define BOARD_RGB_LED_GPIO      2

// 1.47-inch 172x320 IPS ST7789 panel.
#define BSP_LCD_H_RES           172
#define BSP_LCD_V_RES           320
#define BSP_LCD_X_OFFSET        34
#define BSP_LCD_Y_OFFSET        0
#define BSP_LCD_SPI_HOST        SPI3_HOST
// GPIO19/GPIO20 are reserved for the native USB OTG D-/D+ pair used by the
// flight-controller USB host.
#define BSP_LCD_SPI_MOSI        GPIO_NUM_16
#define BSP_LCD_SPI_CLK         GPIO_NUM_18
#define BSP_LCD_SPI_CS          GPIO_NUM_5
#define BSP_LCD_DC              GPIO_NUM_17
#define BSP_LCD_RST             GPIO_NUM_21
#define BSP_LCD_BACKLIGHT       GPIO_NUM_11
// The backlight is switched by a PNP transistor: driving the pin low lights it.
#define BSP_LCD_BACKLIGHT_ON    0

// Battery sense on the XT60 (VIN) and XT30 (VCC) inputs; the higher is shown.
#define BOARD_ADC_GPIOS         { GPIO_NUM_4, GPIO_NUM_6 }
#define BOARD_ADC_DIVIDER_RATIO 11
