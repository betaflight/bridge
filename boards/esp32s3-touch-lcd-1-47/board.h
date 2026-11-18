#pragma once

#include "driver/gpio.h"

#define BOARD_NAME              "esp32s3-touch-lcd-1-47"

// 1.47-inch 172x320 IPS JD9853 panel.
#define BSP_LCD_H_RES           172
#define BSP_LCD_V_RES           320
#define BSP_LCD_X_OFFSET        34
#define BSP_LCD_Y_OFFSET        0
#define BSP_LCD_SPI_HOST        SPI2_HOST
#define BSP_LCD_SPI_MOSI        GPIO_NUM_39
#define BSP_LCD_SPI_CLK         GPIO_NUM_38
#define BSP_LCD_SPI_CS          GPIO_NUM_21
#define BSP_LCD_DC              GPIO_NUM_45
#define BSP_LCD_RST             GPIO_NUM_40
#define BSP_LCD_BACKLIGHT       GPIO_NUM_46
#define BSP_LCD_BACKLIGHT_ON    1

// 1S LiPo on the battery connector through a 3:1 divider.
#define BOARD_ADC_GPIOS         { GPIO_NUM_12 }
#define BOARD_ADC_DIVIDER_RATIO 3
